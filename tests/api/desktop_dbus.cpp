// The desktop's services on Linux (API 0.5.3), from the desktop's side of the
// session bus: this test owns org.kde.StatusNotifierWatcher,
// org.freedesktop.Notifications and org.freedesktop.portal.Desktop on a
// connection and thread of its own, as a desktop would, and drives the
// engine through the C API:
//   - the tray: registered with the watcher; its StatusNotifierItem's
//     properties (id, tooltip, menu, icon pixels); Activate to `activated`;
//     its menu's layout over com.canonical.dbusmenu (mnemonics, submenus,
//     check marks); a clicked event to `menu_item`; NewToolTip on a change;
//   - notifications: Notify's arguments; ActionInvoked to `activated`, for a
//     button and for the notification itself; CloseNotification on remove;
//   - global shortcuts through the GlobalShortcuts portal: the session made,
//     the shortcut bound with its trigger, Activated to the callback.
//
// Usage: desktop_dbus   (needs a session bus: dbus-run-session -- desktop_dbus)

#include "config.h"
#include "accessibility/DBus.h"
#include "zerog.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <unistd.h>
#include <wtf/Function.h>
#include <wtf/Lock.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

using namespace ZeroG::DBus;

namespace {

int failures;

// For printf.
std::string text(const String& string)
{
    auto utf8 = string.utf8();
    return { byteCast<char>(utf8.data()), utf8.length() };
}

void check(bool ok, const char* format, auto... arguments)
{
    std::printf("%s ", ok ? "ok  " : "FAIL");
    std::printf(format, arguments...);
    std::printf("\n");
    if (!ok)
        ++failures;
}

zg_runtime* runtime;

// The engine's loop until `done`, up to `milliseconds`.
bool waitFor(const std::function<bool()>& done, int milliseconds = 3000)
{
    for (int waited = 0; waited < milliseconds; waited += 5) {
        zg_runtime_update(runtime);
        if (done())
            return true;
        usleep(5000);
    }
    return done();
}

// ---- The desktop ----

class Desktop {
public:
    const Library& library { *Library::get() };
    Connection* connection { nullptr };

    Lock lock;
    String itemService WTF_GUARDED_BY_LOCK(lock);
    String itemSender WTF_GUARDED_BY_LOCK(lock);
    String notifyAppName WTF_GUARDED_BY_LOCK(lock);
    String notifySummary WTF_GUARDED_BY_LOCK(lock);
    String notifyBody WTF_GUARDED_BY_LOCK(lock);
    Vector<String> notifyActions WTF_GUARDED_BY_LOCK(lock);
    uint32_t closedID WTF_GUARDED_BY_LOCK(lock) { 0 };
    String shortcutsSession WTF_GUARDED_BY_LOCK(lock);
    String boundShortcut WTF_GUARDED_BY_LOCK(lock);
    String boundTrigger WTF_GUARDED_BY_LOCK(lock);
    std::atomic<int> newToolTips { 0 };
    std::atomic<bool> ready { false };

    bool start()
    {
        Error error;
        library.error_init(&error);
        connection = library.bus_get_private(SessionBus, &error);
        library.error_free(&error);
        if (!connection)
            return false;
        library.connection_set_exit_on_disconnect(connection, false);
        for (auto* name : { "org.kde.StatusNotifierWatcher", "org.freedesktop.Notifications", "org.freedesktop.portal.Desktop" }) {
            library.error_init(&error);
            library.bus_request_name(connection, name, 0, &error);
            library.error_free(&error);
        }
        library.error_init(&error);
        library.bus_add_match(connection, "type='signal',interface='org.kde.StatusNotifierItem'", &error);
        library.error_free(&error);
        std::thread([this] { run(); }).detach();
        ready = true;
        return true;
    }

    // `task` on the desktop's thread, waited for.
    void onThread(Function<void()>&& task)
    {
        std::atomic<bool> done { false };
        {
            Locker locker { lock };
            m_tasks.append([&] {
                task();
                done = true;
            });
        }
        waitFor([&] { return done.load(); }, 10000);
    }

    // A blocking call to the engine's objects, from the desktop's thread.
    Message* call(const String& destination, const char* path, const char* interface, const char* member, const std::function<void(Writer&)>& arguments = { })
    {
        auto* message = library.message_new_method_call(data(destination.utf8()), path, interface, member);
        if (arguments) {
            Writer writer(message);
            arguments(writer);
        }
        Error error;
        library.error_init(&error);
        auto* reply = library.connection_send_with_reply_and_block(connection, message, 3000, &error);
        library.error_free(&error);
        library.message_unref(message);
        return reply;
    }

    void signal(const char* path, const char* interface, const char* member, const std::function<void(Writer&)>& arguments)
    {
        auto* message = library.message_new_signal(path, interface, member);
        Writer writer(message);
        arguments(writer);
        library.connection_send(connection, message, nullptr);
        library.connection_flush(connection);
        library.message_unref(message);
    }

private:
    Vector<Function<void()>> m_tasks WTF_GUARDED_BY_LOCK(lock);

    bool is(Message* message, const char* member)
    {
        const char* name = library.message_get_member(message);
        return name && !strcmp(name, member);
    }

    void reply(Message* call, const std::function<void(Writer&)>& arguments = { })
    {
        auto* message = library.message_new_method_return(call);
        if (arguments) {
            Writer writer(message);
            arguments(writer);
        }
        library.connection_send(connection, message, nullptr);
        library.message_unref(message);
    }

    void handle(Message* message)
    {
        int type = library.message_get_type(message);
        if (type == Signal) {
            if (is(message, "NewToolTip"))
                ++newToolTips;
            return;
        }
        if (type != MethodCall)
            return;
        auto sender = String::fromUTF8(library.message_get_sender(message));
        if (is(message, "RegisterStatusNotifierItem")) {
            Locker locker { lock };
            itemService = Reader(message).string();
            itemSender = sender;
            reply(message);
        } else if (is(message, "Notify")) {
            Reader reader(message);
            Locker locker { lock };
            notifyAppName = reader.string();
            reader.next(); // replaces_id
            reader.next(); // app_icon
            reader.next();
            notifySummary = reader.string();
            reader.next();
            notifyBody = reader.string();
            reader.next();
            notifyActions.clear();
            for (auto actions = reader.recurse(); actions.type() == TypeString;) {
                notifyActions.append(actions.string());
                if (!actions.next())
                    break;
            }
            reply(message, [](Writer& writer) { writer.uint32(42); });
        } else if (is(message, "CloseNotification")) {
            Locker locker { lock };
            closedID = Reader(message).uint32();
            reply(message);
        } else if (is(message, "Get")) {
            // The portal's version.
            reply(message, [](Writer& writer) {
                writer.container(TypeVariant, "u", [](Writer& value) { value.uint32(1); });
            });
        } else if (is(message, "CreateSession")) {
            // The Response comes from the request object the token names.
            String token;
            for (auto options = Reader(message).recurse(); options.type() == TypeDictEntry;) {
                auto pair = options.recurse();
                if (pair.string() == "handle_token"_s) {
                    pair.next();
                    token = pair.recurse().string();
                }
                if (!options.next())
                    break;
            }
            auto escaped = makeStringByReplacingAll(sender.substring(1), '.', '_');
            auto requestPath = makeString("/org/freedesktop/portal/desktop/request/"_s, escaped, '/', token);
            auto session = makeString("/org/freedesktop/portal/desktop/session/"_s, escaped, "/zerog_session"_s);
            {
                Locker locker { lock };
                shortcutsSession = session;
            }
            reply(message, [&](Writer& writer) { writer.objectPath(data(requestPath.utf8())); });
            library.connection_flush(connection);
            signal(data(requestPath.utf8()), "org.freedesktop.portal.Request", "Response", [&](Writer& writer) {
                writer.uint32(0);
                writer.container(TypeArray, "{sv}", [&](Writer& results) {
                    results.container(TypeDictEntry, nullptr, [&](Writer& pair) {
                        pair.string("session_handle");
                        pair.container(TypeVariant, "s", [&](Writer& value) { value.string(session); });
                    });
                });
            });
        } else if (is(message, "BindShortcuts")) {
            Reader reader(message);
            reader.next();
            for (auto shortcuts = reader.recurse(); shortcuts.type() == TypeStruct;) {
                auto shortcut = shortcuts.recurse();
                Locker locker { lock };
                boundShortcut = shortcut.string();
                shortcut.next();
                for (auto properties = shortcut.recurse(); properties.type() == TypeDictEntry;) {
                    auto pair = properties.recurse();
                    if (pair.string() == "preferred_trigger"_s) {
                        pair.next();
                        boundTrigger = pair.recurse().string();
                    }
                    if (!properties.next())
                        break;
                }
                if (!shortcuts.next())
                    break;
            }
            reply(message, [](Writer& writer) { writer.objectPath("/org/freedesktop/portal/desktop/request/bind"); });
        } else
            reply(message);
    }

    void run()
    {
        for (;;) {
            library.connection_read_write(connection, 10);
            while (auto* message = library.connection_pop_message(connection)) {
                handle(message);
                library.message_unref(message);
            }
            Vector<Function<void()>> tasks;
            {
                Locker locker { lock };
                tasks = std::exchange(m_tasks, { });
            }
            for (auto& task : tasks)
                task();
        }
    }
};

// ---- The engine's side ----

int activations, activatedX, activatedY;
unsigned chosenItem;
String notificationTag;
int notificationAction = -99;
unsigned pressedShortcut;

void onActivated(zg_tray*, int x, int y, void*)
{
    activatedX = x;
    activatedY = y;
    ++activations;
}

void onMenuItem(zg_tray*, unsigned id, void*)
{
    chosenItem = id;
}

void onNotification(zg_runtime*, const char* tag, int action, void*)
{
    notificationTag = String::fromUTF8(tag);
    notificationAction = action;
}

void onShortcut(zg_runtime*, unsigned id, void*)
{
    pressedShortcut = id;
}

// A dict of the item's properties: the value of `name` (a variant).
std::optional<Reader> property(Reader dict, const char* name)
{
    while (dict.type() == TypeDictEntry) {
        auto pair = dict.recurse();
        if (pair.string() == String::fromUTF8(name)) {
            pair.next();
            return pair.recurse();
        }
        if (!dict.next())
            break;
    }
    return std::nullopt;
}

} // namespace

int main()
{
    if (!getenv("DBUS_SESSION_BUS_ADDRESS") || !Library::get()) {
        std::printf("skip: no session bus or libdbus (run under dbus-run-session)\n");
        return 0;
    }
    // The portal under X11 or no display alike.
    setenv("ZEROG_GLOBAL_SHORTCUTS", "portal", 1);

    Desktop desktop;
    check(desktop.start(), "the desktop's services on the session bus");

    zg_runtime_config config = { sizeof config };
    runtime = zg_runtime_create(&config);
    check(runtime, "a runtime");

    // ---- The tray ----
    static uint8_t icon[16 * 16 * 4];
    for (size_t i = 0; i < sizeof icon; i += 4) {
        icon[i] = 255;
        icon[i + 3] = 255;
    }
    zg_tray_config trayConfig = { sizeof trayConfig };
    trayConfig.icon_rgba = icon;
    trayConfig.icon_width = trayConfig.icon_height = 16;
    trayConfig.tooltip = "Peer test";
    trayConfig.id = "zerog-test";
    zg_tray_callbacks callbacks = { sizeof callbacks };
    callbacks.activated = onActivated;
    callbacks.menu_item = onMenuItem;
    zg_tray* tray = zg_tray_create(runtime, &trayConfig, &callbacks);
    check(tray, "a tray: %s", tray ? "made" : zg_last_error());

    auto expectedService = makeString("org.kde.StatusNotifierItem-"_s, getpid(), "-1"_s);
    check(waitFor([&] {
        Locker locker { desktop.lock };
        return desktop.itemService == expectedService;
    }), "registered with the watcher as %s", expectedService.utf8().data());

    desktop.onThread([&] {
        auto* reply = desktop.call(expectedService, "/StatusNotifierItem", "org.freedesktop.DBus.Properties", "GetAll", [](Writer& writer) {
            writer.string("org.kde.StatusNotifierItem");
        });
        check(reply, "its properties");
        if (!reply)
            return;
        Reader reader(reply);
        auto dict = reader.recurse();
        auto id = property(dict, "Id");
        check(id && id->string() == "zerog-test"_s, "Id: %s", text(id ? id->string() : "none"_s).c_str());
        auto menu = property(dict, "Menu");
        check(menu && menu->string() == "/MenuBar1"_s, "Menu: %s", text(menu ? menu->string() : "none"_s).c_str());
        auto tooltip = property(dict, "ToolTip");
        String tooltipText;
        if (tooltip) {
            auto fields = tooltip->recurse();
            fields.next();
            fields.next();
            tooltipText = fields.string();
        }
        check(tooltipText == "Peer test"_s, "ToolTip: %s", tooltipText.utf8().data());
        auto pixmaps = property(dict, "IconPixmap");
        int width = 0, height = 0;
        uint32_t firstPixel = 0;
        if (pixmaps) {
            auto pixmap = pixmaps->recurse().recurse();
            width = pixmap.int32();
            pixmap.next();
            height = pixmap.int32();
            pixmap.next();
            auto bytes = pixmap.recurse();
            for (int i = 0; i < 4; ++i) {
                firstPixel = (firstPixel << 8) | bytes.byte();
                bytes.next();
            }
        }
        check(width == 16 && height == 16 && firstPixel == 0xFFFF0000, "IconPixmap: %dx%d, ARGB 0x%08x", width, height, firstPixel);
        desktop.library.message_unref(reply);
    });

    desktop.onThread([&] {
        if (auto* reply = desktop.call(expectedService, "/StatusNotifierItem", "org.kde.StatusNotifierItem", "Activate", [](Writer& writer) {
                writer.int32(10);
                writer.int32(20);
            }))
            desktop.library.message_unref(reply);
    });
    check(waitFor([] { return activations > 0; }) && activatedX == 10 && activatedY == 20, "Activate: activated at %d, %d", activatedX, activatedY);
    int x = 0, y = 0;
    check(zg_tray_get_rect(tray, &x, &y, nullptr, nullptr) == ZG_OK && x == 10 && y == 20, "its place: the last activation's");

    zg_menu* menu = zg_menu_create();
    zg_menu* more = zg_menu_create();
    zg_menu_add_item(menu, 31, "&Open", nullptr, 0);
    zg_menu_add_separator(menu);
    zg_menu_add_item(more, 32, "Pin", nullptr, ZG_MENU_ITEM_CHECKBOX | ZG_MENU_ITEM_CHECKED);
    zg_menu_add_submenu(menu, "More", more);
    check(zg_tray_set_menu(tray, menu) == ZG_OK, "its menu");
    zg_menu_destroy(more);
    zg_menu_destroy(menu);

    desktop.onThread([&] {
        auto* reply = desktop.call(expectedService, "/MenuBar1", "com.canonical.dbusmenu", "GetLayout", [](Writer& writer) {
            writer.int32(0);
            writer.int32(-1);
            writer.container(TypeArray, "s", [](Writer&) { });
        });
        check(reply, "its layout");
        if (!reply)
            return;
        Reader reader(reply);
        reader.next();
        auto root = reader.recurse();
        root.next();
        root.next();
        Vector<String> labels;
        String toggle;
        size_t children = 0;
        for (auto items = root.recurse(); items.type() == TypeVariant;) {
            ++children;
            auto node = items.recurse().recurse();
            node.next();
            if (auto label = property(node.recurse(), "label"))
                labels.append(label->string());
            if (auto type = property(node.recurse(), "type"))
                labels.append(makeString('[', type->string(), ']'));
            node.next();
            for (auto sub = node.recurse(); sub.type() == TypeVariant;) {
                auto subnode = sub.recurse().recurse();
                subnode.next();
                if (auto state = property(subnode.recurse(), "toggle-state"))
                    toggle = String::number(state->int32());
                if (!sub.next())
                    break;
            }
            if (!items.next())
                break;
        }
        check(children == 3 && labels.size() == 3 && labels[0] == "_Open"_s && labels[1] == "[separator]"_s && labels[2] == "More"_s,
            "three items: %s, %s, %s", text(labels.size() > 0 ? labels[0] : String()).c_str(), text(labels.size() > 1 ? labels[1] : String()).c_str(),
            text(labels.size() > 2 ? labels[2] : String()).c_str());
        check(toggle == "1"_s, "the submenu's check mark: %s", toggle.utf8().data());
        desktop.library.message_unref(reply);
    });

    desktop.onThread([&] {
        if (auto* reply = desktop.call(expectedService, "/MenuBar1", "com.canonical.dbusmenu", "Event", [](Writer& writer) {
                writer.int32(31);
                writer.string("clicked");
                writer.container(TypeVariant, "i", [](Writer& value) { value.int32(0); });
                writer.uint32(0);
            }))
            desktop.library.message_unref(reply);
    });
    check(waitFor([] { return chosenItem; }) && chosenItem == 31, "a clicked item: %u", chosenItem);

    check(zg_tray_set_tooltip(tray, "Changed") == ZG_OK && waitFor([&] { return desktop.newToolTips.load() > 0; }), "a new tooltip, signaled");
    zg_tray_destroy(tray);

    // ---- Notifications ----
    zg_notifications_config notifications = { sizeof notifications };
    notifications.app_id = "com.zerog.test";
    notifications.app_name = "ZeroG test";
    notifications.activated = onNotification;
    check(zg_runtime_set_notifications(runtime, &notifications) == ZG_OK, "notifications");
    const char* buttons[2] = { "Yes", "No" };
    zg_notification notification = { sizeof notification };
    notification.title = "Hello";
    notification.body = "World";
    notification.tag = "t1";
    notification.actions = buttons;
    notification.action_count = 2;
    check(zg_notification_show(runtime, &notification) == ZG_OK, "a notification");
    check(waitFor([&] {
        Locker locker { desktop.lock };
        return desktop.notifySummary == "Hello"_s;
    }), "Notify reached the server");
    {
        Locker locker { desktop.lock };
        check(desktop.notifyAppName == "ZeroG test"_s && desktop.notifyBody == "World"_s, "from %s, body %s", desktop.notifyAppName.utf8().data(), desktop.notifyBody.utf8().data());
        auto& actions = desktop.notifyActions;
        check(actions.size() == 6 && actions[0] == "default"_s && actions[2] == "0"_s && actions[3] == "Yes"_s && actions[4] == "1"_s && actions[5] == "No"_s,
            "its actions: %zu (default, then the buttons by index)", actions.size());
    }
    // The server's id is known once Notify answered.
    waitFor([] { return false; }, 200);
    desktop.onThread([&] {
        desktop.signal("/org/freedesktop/Notifications", "org.freedesktop.Notifications", "ActionInvoked", [](Writer& writer) {
            writer.uint32(42);
            writer.string("1");
        });
    });
    check(waitFor([] { return notificationAction == 1; }) && notificationTag == "t1"_s, "a button: activated(%s, %d)", notificationTag.utf8().data(), notificationAction);
    desktop.onThread([&] {
        desktop.signal("/org/freedesktop/Notifications", "org.freedesktop.Notifications", "ActionInvoked", [](Writer& writer) {
            writer.uint32(42);
            writer.string("default");
        });
    });
    check(waitFor([] { return notificationAction == -1; }), "the notification itself: action %d", notificationAction);
    check(zg_notification_remove(runtime, "t1") == ZG_OK && waitFor([&] {
        Locker locker { desktop.lock };
        return desktop.closedID == 42;
    }), "removed: CloseNotification");

    // ---- Global shortcuts, through the portal ----
    check(zg_runtime_set_shortcut_callback(runtime, onShortcut, nullptr) == ZG_OK, "a shortcut callback");
    zg_result result = zg_shortcut_register(runtime, 5, "Ctrl+Shift+K");
    check(result == ZG_OK, "a shortcut, through the portal: %s", result ? zg_last_error() : "registered");
    check(waitFor([&] {
        Locker locker { desktop.lock };
        return desktop.boundShortcut == "zerog-5"_s;
    }), "bound in the portal's session");
    {
        Locker locker { desktop.lock };
        check(desktop.boundTrigger == "CTRL+SHIFT+k"_s, "its trigger: %s", desktop.boundTrigger.utf8().data());
    }
    String session;
    {
        Locker locker { desktop.lock };
        session = desktop.shortcutsSession;
    }
    desktop.onThread([&] {
        // (o session, s shortcut, t timestamp, a{sv}): what the engine reads
        // is the session and the shortcut.
        desktop.signal("/org/freedesktop/portal/desktop", "org.freedesktop.portal.GlobalShortcuts", "Activated", [&](Writer& writer) {
            writer.objectPath(data(session.utf8()));
            writer.string("zerog-5");
            writer.container(TypeArray, "{sv}", [](Writer&) { });
        });
    });
    check(waitFor([] { return pressedShortcut == 5; }), "Activated: shortcut %u", pressedShortcut);

    zg_runtime_destroy(runtime);
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
