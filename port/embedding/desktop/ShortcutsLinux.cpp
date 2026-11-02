// Global shortcuts on Linux. X11: key grabs on the root window, over an X11
// connection of its own on a thread of its own (libX11 loaded at run time,
// as SDL loads it), so that SDL's connection and event loop are left alone.
// Wayland, which lets no application grab keys: the GlobalShortcuts portal
// (org.freedesktop.portal.GlobalShortcuts) on the session bus, where the
// compositor offers it; the user confirms the bindings there.
// ZEROG_GLOBAL_SHORTCUTS=portal chooses the portal under X11 too (tests).

#include "config.h"
#include "desktop/Desktop.h"

#include "desktop/SessionBus.h"
#include <SDL3/SDL.h>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <wtf/Condition.h>
#include <wtf/HashMap.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Threading.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace ZeroG::Shortcuts {

namespace {

// ---- Keys ----

// An SDL keycode as an X keysym, and its name (the XDG shortcuts
// specification's, for the portal's preferred triggers).
std::optional<std::pair<unsigned long, String>> keysym(uint32_t key)
{
    if ((key >= 'a' && key <= 'z') || (key >= '0' && key <= '9'))
        return std::pair { static_cast<unsigned long>(key), makeString(static_cast<char>(key)) };
    for (int i = 0; i < 12; ++i) {
        if (key == SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F1 + i))
            return std::pair { 0xFFBEul + i, makeString('F', i + 1) };
        if (key == SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F13 + i))
            return std::pair { 0xFFCAul + i, makeString('F', i + 13) };
    }
    static constexpr std::tuple<uint32_t, unsigned long, ASCIILiteral> named[] = {
        { SDLK_SPACE, 0x20, "space"_s }, { SDLK_RETURN, 0xFF0D, "Return"_s }, { SDLK_ESCAPE, 0xFF1B, "Escape"_s },
        { SDLK_TAB, 0xFF09, "Tab"_s }, { SDLK_BACKSPACE, 0xFF08, "BackSpace"_s }, { SDLK_DELETE, 0xFFFF, "Delete"_s },
        { SDLK_INSERT, 0xFF63, "Insert"_s }, { SDLK_HOME, 0xFF50, "Home"_s }, { SDLK_END, 0xFF57, "End"_s },
        { SDLK_PAGEUP, 0xFF55, "Page_Up"_s }, { SDLK_PAGEDOWN, 0xFF56, "Page_Down"_s }, { SDLK_UP, 0xFF52, "Up"_s },
        { SDLK_DOWN, 0xFF54, "Down"_s }, { SDLK_LEFT, 0xFF51, "Left"_s }, { SDLK_RIGHT, 0xFF53, "Right"_s },
        { SDLK_PRINTSCREEN, 0xFF61, "Print"_s }, { SDLK_PAUSE, 0xFF13, "Pause"_s },
    };
    for (auto [keycode, symbol, name] : named) {
        if (key == keycode)
            return std::pair { symbol, String(name) };
    }
    // Latin-1 punctuation: the keysym is the character.
    if (key > 0x20 && key < 0x7F)
        return std::pair { static_cast<unsigned long>(key), makeString(static_cast<char>(key)) };
    return std::nullopt;
}

// ---- X11 ----

constexpr unsigned ShiftMask = 1 << 0;
constexpr unsigned LockMask = 1 << 1;
constexpr unsigned ControlMask = 1 << 2;
constexpr unsigned Mod1Mask = 1 << 3; // Alt
constexpr unsigned Mod2Mask = 1 << 4; // Num Lock
constexpr unsigned Mod4Mask = 1 << 6; // Super
constexpr int KeyPress = 2;
constexpr int BadAccess = 10;
constexpr int GrabModeAsync = 1;

// Xlib's public structures, as far as used.
struct XKeyEvent {
    int type;
    unsigned long serial;
    int sendEvent;
    void* display;
    unsigned long window, root, subwindow;
    unsigned long time;
    int x, y, xRoot, yRoot;
    unsigned state;
    unsigned keycode;
    int sameScreen;
};
union XEvent {
    int type;
    XKeyEvent key;
    long pad[24];
};
struct XErrorEvent {
    int type;
    void* display;
    unsigned long resourceID;
    unsigned long serial;
    unsigned char errorCode;
    unsigned char requestCode;
    unsigned char minorCode;
};

struct Xlib {
    void* (*openDisplay)(const char*);
    unsigned long (*defaultRootWindow)(void*);
    unsigned char (*keysymToKeycode)(void*, unsigned long);
    int (*grabKey)(void*, int, unsigned, unsigned long, int, int, int);
    int (*ungrabKey)(void*, int, unsigned, unsigned long);
    int (*pending)(void*);
    int (*nextEvent)(void*, XEvent*);
    int (*connectionNumber)(void*);
    int (*sync)(void*, int);
    int (*flush)(void*);
    int (*(*setErrorHandler)(int (*)(void*, XErrorEvent*)))(void*, XErrorEvent*);

    static const Xlib* get()
    {
        static std::optional<Xlib> library = []() -> std::optional<Xlib> {
            void* handle = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
            if (!handle)
                return std::nullopt;
            Xlib x;
            bool complete = true;
            auto load = [&](auto& function, const char* name) {
                function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(dlsym(handle, name));
                complete &= !!function;
            };
            load(x.openDisplay, "XOpenDisplay");
            load(x.defaultRootWindow, "XDefaultRootWindow");
            load(x.keysymToKeycode, "XKeysymToKeycode");
            load(x.grabKey, "XGrabKey");
            load(x.ungrabKey, "XUngrabKey");
            load(x.pending, "XPending");
            load(x.nextEvent, "XNextEvent");
            load(x.connectionNumber, "XConnectionNumber");
            load(x.sync, "XSync");
            load(x.flush, "XFlush");
            load(x.setErrorHandler, "XSetErrorHandler");
            if (!complete)
                return std::nullopt;
            return x;
        }();
        return library ? &*library : nullptr;
    }
};

class X11Grabs {
public:
    static X11Grabs* get()
    {
        static X11Grabs* grabs = []() -> X11Grabs* {
            auto* xlib = Xlib::get();
            if (!xlib || !getenv("DISPLAY"))
                return nullptr;
            auto* grabs = new X11Grabs(*xlib);
            Thread::create("ZeroG X11 shortcuts"_s, [grabs] {
                grabs->run();
            })->detach();
            return grabs;
        }();
        return grabs;
    }

    // From the engine thread, answered by the X thread.
    Result add(unsigned id, unsigned long symbol, unsigned modifiers, String& error)
    {
        Result result = Result::Unsupported;
        request([&] {
            if (!m_display) {
                error = "no X11 display"_s;
                return;
            }
            unsigned keycode = m_xlib.keysymToKeycode(m_display, symbol);
            if (!keycode) {
                error = "no such key on this keyboard"_s;
                result = Result::Invalid;
                return;
            }
            for (auto& grab : m_grabs) {
                if (grab.id == id || (grab.keycode == keycode && grab.modifiers == modifiers)) {
                    error = "the same id or keys again"_s;
                    result = Result::Taken;
                    return;
                }
            }
            // Another client's grab of the keys is a BadAccess error, which
            // comes back once the server has the requests (XSync).
            s_grabFailed = false;
            auto previous = m_xlib.setErrorHandler(onError);
            auto root = m_xlib.defaultRootWindow(m_display);
            for (unsigned locks : { 0u, LockMask, Mod2Mask, LockMask | Mod2Mask })
                m_xlib.grabKey(m_display, keycode, modifiers | locks, root, false, GrabModeAsync, GrabModeAsync);
            m_xlib.sync(m_display, false);
            m_xlib.setErrorHandler(previous);
            if (s_grabFailed) {
                for (unsigned locks : { 0u, LockMask, Mod2Mask, LockMask | Mod2Mask })
                    m_xlib.ungrabKey(m_display, keycode, modifiers | locks, root);
                m_xlib.flush(m_display);
                error = "the keys are taken (another application grabbed them)"_s;
                result = Result::Taken;
                return;
            }
            m_grabs.append({ id, keycode, modifiers });
            result = Result::Ok;
        });
        return result;
    }

    bool remove(unsigned id)
    {
        bool removed = false;
        request([&] {
            for (size_t i = 0; i < m_grabs.size(); ++i) {
                if (m_grabs[i].id != id)
                    continue;
                auto root = m_xlib.defaultRootWindow(m_display);
                for (unsigned locks : { 0u, LockMask, Mod2Mask, LockMask | Mod2Mask })
                    m_xlib.ungrabKey(m_display, m_grabs[i].keycode, m_grabs[i].modifiers | locks, root);
                m_xlib.flush(m_display);
                m_grabs.removeAt(i);
                removed = true;
                return;
            }
        });
        return removed;
    }

private:
    explicit X11Grabs(const Xlib& xlib)
        : m_xlib(xlib)
        , m_wakeUpFD(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK))
    {
    }

    struct Grab {
        unsigned id;
        unsigned keycode;
        unsigned modifiers;
    };

    struct RequestType {
        Function<void()> work;
        bool done { false };
    };

    static int onError(void*, XErrorEvent* error)
    {
        if (error->errorCode == BadAccess)
            s_grabFailed = true;
        return 0;
    }

    // `work` on the X thread; the caller waits for it.
    void request(Function<void()>&& work)
    {
        auto pending = std::make_shared<RequestType>();
        pending->work = WTF::move(work);
        {
            Locker locker { m_lock };
            m_requests.append(pending);
        }
        uint64_t one = 1;
        [[maybe_unused]] auto written = write(m_wakeUpFD, &one, sizeof(one));
        Locker locker { m_lock };
        auto deadline = MonotonicTime::now() + 3_s;
        while (!pending->done && MonotonicTime::now() < deadline)
            m_condition.waitUntil(m_lock, deadline);
    }

    void run()
    {
        m_display = m_xlib.openDisplay(nullptr);
        int fd = m_display ? m_xlib.connectionNumber(m_display) : -1;
        for (;;) {
            Vector<std::shared_ptr<RequestType>> requests;
            {
                Locker locker { m_lock };
                requests = std::exchange(m_requests, { });
            }
            for (auto& request : requests) {
                request->work();
                Locker locker { m_lock };
                request->done = true;
            }
            if (!requests.isEmpty())
                m_condition.notifyAll();
            while (m_display && m_xlib.pending(m_display)) {
                XEvent event;
                m_xlib.nextEvent(m_display, &event);
                if (event.type != KeyPress)
                    continue;
                unsigned modifiers = event.key.state & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask);
                for (auto& grab : m_grabs) {
                    if (grab.keycode == event.key.keycode && grab.modifiers == modifiers) {
                        callOnMainThread([id = grab.id] {
                            pressed(id);
                        });
                    }
                }
            }
            pollfd descriptors[2] = { { m_wakeUpFD, POLLIN, 0 }, { fd, POLLIN, 0 } };
            if (poll(descriptors, fd >= 0 ? 2 : 1, -1) < 0)
                continue;
            if (descriptors[0].revents & POLLIN) {
                uint64_t count;
                [[maybe_unused]] auto readBytes = read(m_wakeUpFD, &count, sizeof(count));
            }
        }
    }

    const Xlib& m_xlib;
    int m_wakeUpFD { -1 };
    void* m_display { nullptr }; // X thread
    Vector<Grab> m_grabs; // X thread
    Lock m_lock;
    Condition m_condition;
    Vector<std::shared_ptr<RequestType>> m_requests WTF_GUARDED_BY_LOCK(m_lock);
    static inline bool s_grabFailed { false };
};

// ---- The GlobalShortcuts portal ----

using DBus::Message;
using DBus::Reader;
using DBus::Writer;

class Portal {
public:
    static Portal* get()
    {
        static Portal* portal = []() -> Portal* {
            auto* bus = SessionBus::get();
            if (!bus)
                return nullptr;
            // Whether the desktop has it at all (asked once).
            bool available = bus->postAndWait([bus](DBus::Connection* connection) {
                auto& library = bus->library();
                auto* call = library.message_new_method_call("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", "org.freedesktop.DBus.Properties", "Get");
                Writer writer(call);
                writer.string("org.freedesktop.portal.GlobalShortcuts");
                writer.string("version");
                auto* reply = bus->call(connection, call, 2000);
                library.message_unref(call);
                if (!reply)
                    return false;
                library.message_unref(reply);
                return true;
            }, 3_s);
            if (!available)
                return nullptr;
            auto* portal = new Portal(*bus);
            portal->start();
            return portal;
        }();
        return portal;
    }

    Result add(unsigned id, const String& trigger, const String& description)
    {
        {
            Locker locker { m_lock };
            for (auto& shortcut : m_shortcuts) {
                if (shortcut.id == id)
                    return Result::Taken;
            }
            m_shortcuts.append({ id, trigger.isolatedCopy(), description.isolatedCopy() });
        }
        bind();
        return Result::Ok;
    }

    bool remove(unsigned id)
    {
        bool removed;
        {
            Locker locker { m_lock };
            removed = m_shortcuts.removeFirstMatching([id](auto& shortcut) {
                return shortcut.id == id;
            });
        }
        if (removed)
            bind();
        return removed;
    }

private:
    explicit Portal(SessionBus& bus)
        : m_bus(bus)
    {
    }

    struct Shortcut {
        unsigned id;
        String trigger;
        String description;
    };

    // The request object a call's Response comes from (the portal's
    // documented path for a handle_token).
    static String requestPath(const String& uniqueName, const String& token)
    {
        auto sender = makeStringByReplacingAll(uniqueName.substring(1), '.', '_');
        return makeString("/org/freedesktop/portal/desktop/request/"_s, sender, '/', token);
    }

    static void tokenEntry(Writer& dict, const char* key, const String& value)
    {
        dict.container(DBus::TypeDictEntry, nullptr, [&](Writer& pair) {
            pair.string(key);
            pair.container(DBus::TypeVariant, "s", [&](Writer& variant) { variant.string(value); });
        });
    }

    void start()
    {
        m_bus.addSignalHandler([this](Message* signal) {
            auto& library = m_bus.library();
            const char* member = library.message_get_member(signal);
            const char* path = library.message_get_path(signal);
            if (!member || !path)
                return;
            if (!strcmp(member, "Response")) {
                bool session = false;
                {
                    Locker locker { m_lock };
                    session = String::fromUTF8(path) == m_sessionRequest;
                }
                if (!session)
                    return;
                // (u response, a{sv} results): results' session_handle.
                Reader reader(signal);
                if (reader.uint32())
                    return; // refused
                reader.next();
                for (auto results = reader.recurse(); results.type() == DBus::TypeDictEntry;) {
                    auto pair = results.recurse();
                    if (pair.string() == "session_handle"_s) {
                        pair.next();
                        auto handle = pair.recurse().string();
                        {
                            Locker locker { m_lock };
                            m_session = handle;
                        }
                        bind();
                    }
                    if (!results.next())
                        break;
                }
            } else if (!strcmp(member, "Activated")) {
                // (o session, s shortcut, t timestamp, a{sv})
                Reader reader(signal);
                reader.next();
                auto name = reader.string();
                if (!name.startsWith("zerog-"_s))
                    return;
                if (auto id = parseInteger<unsigned>(StringView(name).substring(6)))
                    callOnMainThread([id = *id] { pressed(id); });
            }
        });
        m_bus.post([this](DBus::Connection* connection) {
            auto& library = m_bus.library();
            m_bus.addMatch(connection, "type='signal',interface='org.freedesktop.portal.GlobalShortcuts',member='Activated'");
            m_bus.addMatch(connection, "type='signal',interface='org.freedesktop.portal.Request',member='Response'");
            String token = "zerog_session_request"_s;
            {
                Locker locker { m_lock };
                m_sessionRequest = requestPath(m_bus.uniqueName(), token);
            }
            auto* call = library.message_new_method_call("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", "org.freedesktop.portal.GlobalShortcuts", "CreateSession");
            Writer(call).container(DBus::TypeArray, "{sv}", [&](Writer& options) {
                tokenEntry(options, "handle_token", token);
                tokenEntry(options, "session_handle_token", "zerog_session"_s);
            });
            if (auto* reply = m_bus.call(connection, call, 5000))
                library.message_unref(reply);
            library.message_unref(call);
        });
    }

    // Every shortcut, bound again (the portal binds a session's whole list).
    void bind()
    {
        String session;
        Vector<Shortcut> shortcuts;
        {
            Locker locker { m_lock };
            session = m_session.isolatedCopy();
            for (auto& shortcut : m_shortcuts)
                shortcuts.append({ shortcut.id, shortcut.trigger.isolatedCopy(), shortcut.description.isolatedCopy() });
        }
        if (session.isEmpty())
            return; // once the session exists
        m_bus.post([this, session = WTF::move(session), shortcuts = WTF::move(shortcuts)](DBus::Connection* connection) {
            auto& library = m_bus.library();
            auto* call = library.message_new_method_call("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", "org.freedesktop.portal.GlobalShortcuts", "BindShortcuts");
            Writer writer(call);
            writer.objectPath(DBus::data(session.utf8()));
            writer.container(DBus::TypeArray, "(sa{sv})", [&](Writer& array) {
                for (auto& shortcut : shortcuts) {
                    array.container(DBus::TypeStruct, nullptr, [&](Writer& item) {
                        item.string(makeString("zerog-"_s, shortcut.id));
                        item.container(DBus::TypeArray, "{sv}", [&](Writer& properties) {
                            tokenEntry(properties, "description", shortcut.description);
                            tokenEntry(properties, "preferred_trigger", shortcut.trigger);
                        });
                    });
                }
            });
            writer.string("");
            writer.container(DBus::TypeArray, "{sv}", [&](Writer& options) {
                tokenEntry(options, "handle_token", makeString("zerog_bind_"_s, ++m_bindings));
            });
            if (auto* reply = m_bus.call(connection, call, 5000))
                library.message_unref(reply);
            library.message_unref(call);
        });
    }

    SessionBus& m_bus;
    Lock m_lock;
    Vector<Shortcut> m_shortcuts WTF_GUARDED_BY_LOCK(m_lock);
    String m_session WTF_GUARDED_BY_LOCK(m_lock);
    String m_sessionRequest WTF_GUARDED_BY_LOCK(m_lock);
    unsigned m_bindings { 0 }; // bus thread
};

bool usesPortal()
{
    if (const char* chosen = getenv("ZEROG_GLOBAL_SHORTCUTS"))
        return !strcmp(chosen, "portal");
    if (const char* driver = SDL_GetCurrentVideoDriver())
        return !strcmp(driver, "wayland");
    return getenv("WAYLAND_DISPLAY") && !getenv("DISPLAY");
}

// "CTRL+SHIFT+k": the XDG shortcuts specification's trigger.
String trigger(const Accelerator& accelerator, const String& keyName)
{
    StringBuilder result;
    if (accelerator.modifiers & SDL_KMOD_CTRL)
        result.append("CTRL+"_s);
    if (accelerator.modifiers & SDL_KMOD_ALT)
        result.append("ALT+"_s);
    if (accelerator.modifiers & SDL_KMOD_SHIFT)
        result.append("SHIFT+"_s);
    if (accelerator.modifiers & SDL_KMOD_GUI)
        result.append("LOGO+"_s);
    result.append(keyName);
    return result.toString();
}

} // namespace

Result add(unsigned id, const Accelerator& accelerator, const String& text, String& error)
{
    auto symbol = keysym(accelerator.key);
    if (!symbol) {
        error = makeString("no key for \""_s, text, '"');
        return Result::Invalid;
    }
    if (usesPortal()) {
        auto* portal = Portal::get();
        if (!portal) {
            error = "this desktop has no GlobalShortcuts portal"_s;
            return Result::Unsupported;
        }
        auto result = portal->add(id, trigger(accelerator, symbol->second), text);
        if (result == Result::Taken)
            error = "this id is taken"_s;
        return result;
    }
    auto* grabs = X11Grabs::get();
    if (!grabs) {
        error = "no X11 display"_s;
        return Result::Unsupported;
    }
    unsigned modifiers = 0;
    if (accelerator.modifiers & SDL_KMOD_CTRL)
        modifiers |= ControlMask;
    if (accelerator.modifiers & SDL_KMOD_SHIFT)
        modifiers |= ShiftMask;
    if (accelerator.modifiers & SDL_KMOD_ALT)
        modifiers |= Mod1Mask;
    if (accelerator.modifiers & SDL_KMOD_GUI)
        modifiers |= Mod4Mask;
    return grabs->add(id, symbol->first, modifiers, error);
}

bool remove(unsigned id)
{
    if (usesPortal()) {
        auto* portal = Portal::get();
        return portal && portal->remove(id);
    }
    auto* grabs = X11Grabs::get();
    return grabs && grabs->remove(id);
}

} // namespace ZeroG::Shortcuts
