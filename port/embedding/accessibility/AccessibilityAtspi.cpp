// AT-SPI for the application's windows (docs/design/input-ime-clipboard-accessibility.md, "Accessibility"), written for
// ZeroG over libdbus loaded at run time (DBus.h): no GLib, no D-Bus at build
// time. The application is the root object (/org/a11y/atspi/accessible/root),
// embedded in the AT-SPI registry; each window is a frame under it, and the
// window's page is under the frame. Objects export the interfaces screen
// readers use most: Accessible, Component, Action, Value, Text and
// EditableText, with the properties of each.
//
// Threading: the connection to the accessibility bus has a thread of its own,
// which reads calls and sends their replies; each call is answered on the
// engine thread, which owns WebCore's tree. Events are sent from the engine
// thread (libdbus connections may be used from several threads). The bus
// thread sleeps in poll() on the connection's descriptors (libdbus's watches)
// and an eventfd that wakes it when they change: no wake-ups while idle.

#include "config.h"

#if !OS(WINDOWS)

#include "accessibility/Accessibility.h"
#include "accessibility/DBus.h"

#include "View.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <wtf/Condition.h>
#include <wtf/Lock.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Threading.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace ZeroG::Accessibility {

using namespace WebCore;
using DBus::Message;
using DBus::Reader;
using DBus::Writer;

namespace {

constexpr auto rootPath = "/org/a11y/atspi/accessible/root";
constexpr auto pathPrefix = "/org/a11y/atspi/accessible/"_s;
constexpr auto nullPath = "/org/a11y/atspi/null";

constexpr auto accessibleInterface = "org.a11y.atspi.Accessible"_s;
constexpr auto applicationInterface = "org.a11y.atspi.Application"_s;
constexpr auto componentInterface = "org.a11y.atspi.Component"_s;
constexpr auto actionInterface = "org.a11y.atspi.Action"_s;
constexpr auto valueInterface = "org.a11y.atspi.Value"_s;
constexpr auto textInterface = "org.a11y.atspi.Text"_s;
constexpr auto editableTextInterface = "org.a11y.atspi.EditableText"_s;

const DBus::Library& dbus()
{
    return *DBus::Library::get();
}

// at-spi2-core's AtspiRole values, and their names.
struct AtspiRole {
    uint32_t value;
    const char* name;
};

AtspiRole atspiRole(Role role)
{
    switch (role) {
    case Role::Document:
        return { 95, "document web" };
    case Role::Group:
        return { 85, "section" };
    case Role::Text:
        return { 116, "static" };
    case Role::Paragraph:
        return { 73, "paragraph" };
    case Role::Heading:
        return { 83, "heading" };
    case Role::Link:
        return { 88, "link" };
    case Role::Button:
        return { 43, "push button" };
    case Role::ToggleButton:
        return { 62, "toggle button" };
    case Role::CheckBox:
        return { 7, "check box" };
    case Role::RadioButton:
        return { 44, "radio button" };
    case Role::RadioGroup:
        return { 99, "grouping" };
    case Role::Switch:
        return { 130, "switch" };
    case Role::TextField:
        return { 79, "entry" };
    case Role::PasswordField:
        return { 40, "password text" };
    case Role::TextArea:
        return { 61, "text" };
    case Role::ComboBox:
        return { 11, "combo box" };
    case Role::ListBox:
        return { 98, "list box" };
    case Role::ListBoxOption:
    case Role::ListItem:
        return { 32, "list item" };
    case Role::List:
        return { 31, "list" };
    case Role::Image:
        return { 27, "image" };
    case Role::Table:
        return { 55, "table" };
    case Role::Row:
        return { 90, "table row" };
    case Role::Cell:
        return { 56, "table cell" };
    case Role::ColumnHeader:
        return { 57, "table column header" };
    case Role::RowHeader:
        return { 58, "table row header" };
    case Role::Slider:
        return { 51, "slider" };
    case Role::SpinButton:
        return { 52, "spin button" };
    case Role::ProgressBar:
        return { 42, "progress bar" };
    case Role::Menu:
        return { 33, "menu" };
    case Role::MenuBar:
        return { 34, "menu bar" };
    case Role::MenuItem:
        return { 35, "menu item" };
    case Role::TabList:
        return { 38, "page tab list" };
    case Role::Tab:
        return { 37, "page tab" };
    case Role::TabPanel:
        return { 39, "panel" };
    case Role::Dialog:
        return { 16, "dialog" };
    case Role::Alert:
        return { 101, "notification" };
    case Role::Toolbar:
        return { 63, "tool bar" };
    case Role::Tree:
        return { 65, "tree" };
    case Role::TreeItem:
        return { 91, "tree item" };
    case Role::Separator:
        return { 50, "separator" };
    case Role::Landmark:
        return { 110, "landmark" };
    case Role::Form:
        return { 87, "form" };
    case Role::ScrollBar:
        return { 48, "scroll bar" };
    case Role::Label:
        return { 29, "label" };
    case Role::Figure:
        return { 99, "grouping" };
    case Role::Article:
        return { 109, "article" };
    case Role::Status:
        return { 54, "status bar" };
    case Role::Unknown:
        break;
    }
    return { 67, "unknown" };
}

constexpr AtspiRole applicationRole { 75, "application" };
constexpr AtspiRole frameRole { 23, "frame" };

// at-spi2-core's AtspiStateType bits.
enum class AtspiState : uint64_t {
    Active = 1llu << 1,
    Checked = 1llu << 4,
    Collapsed = 1llu << 5,
    Defunct = 1llu << 6,
    Editable = 1llu << 7,
    Enabled = 1llu << 8,
    Expandable = 1llu << 9,
    Expanded = 1llu << 10,
    Focusable = 1llu << 11,
    Focused = 1llu << 12,
    MultiLine = 1llu << 17,
    Pressed = 1llu << 20,
    Resizable = 1llu << 21,
    Selectable = 1llu << 22,
    Selected = 1llu << 23,
    Sensitive = 1llu << 24,
    Showing = 1llu << 25,
    SingleLine = 1llu << 26,
    Visible = 1llu << 30,
    Indeterminate = 1llu << 32,
    Required = 1llu << 33,
    Checkable = 1llu << 41,
    ReadOnly = 1llu << 43,
};

uint64_t atspiStates(const Object& object)
{
    auto objectStates = states(object);
    uint64_t result = 0;
    auto add = [&](AtspiState state) {
        result |= static_cast<uint64_t>(state);
    };
    if (objectStates.contains(State::Enabled)) {
        add(AtspiState::Enabled);
        add(AtspiState::Sensitive);
    }
    if (objectStates.contains(State::Focusable))
        add(AtspiState::Focusable);
    if (objectStates.contains(State::Focused))
        add(AtspiState::Focused);
    if (objectStates.contains(State::Checkable))
        add(AtspiState::Checkable);
    if (objectStates.contains(State::Checked))
        add(AtspiState::Checked);
    if (objectStates.contains(State::Mixed))
        add(AtspiState::Indeterminate);
    if (objectStates.contains(State::Selected))
        add(AtspiState::Selected);
    if (role(object) == Role::ListBoxOption || role(object) == Role::Tab)
        add(AtspiState::Selectable);
    if (objectStates.contains(State::Expandable)) {
        add(AtspiState::Expandable);
        add(objectStates.contains(State::Expanded) ? AtspiState::Expanded : AtspiState::Collapsed);
    }
    if (objectStates.contains(State::Pressed))
        add(AtspiState::Pressed);
    if (objectStates.contains(State::Editable)) {
        add(AtspiState::Editable);
        add(objectStates.contains(State::MultiLine) ? AtspiState::MultiLine : AtspiState::SingleLine);
    }
    if (objectStates.contains(State::ReadOnly))
        add(AtspiState::ReadOnly);
    if (objectStates.contains(State::Required))
        add(AtspiState::Required);
    if (objectStates.contains(State::Visible)) {
        add(AtspiState::Visible);
        if (!objectStates.contains(State::Offscreen))
            add(AtspiState::Showing);
    }
    return result;
}

bool hasText(const Object& object)
{
    switch (role(object)) {
    case Role::TextField:
    case Role::PasswordField:
    case Role::TextArea:
    case Role::Text:
    case Role::Heading:
    case Role::Paragraph:
    case Role::Label:
    case Role::Link:
        return true;
    default:
        return object.isNonNativeTextControl();
    }
}

// The application's name for screen readers: the program's, as GTK's g_get_prgname.
String programName()
{
    static NeverDestroyed<String> name = [] {
        char buffer[64] = { };
        if (auto* file = fopen("/proc/self/comm", "r")) {
            if (!fgets(buffer, sizeof(buffer), file))
                buffer[0] = '\0';
            fclose(file);
        }
        auto result = String::fromUTF8(buffer);
        return result.endsWith('\n') ? result.left(result.length() - 1) : result;
    }();
    return name.get();
}

class AtspiBridge;

// What an object path names.
struct Target {
    enum class Kind : uint8_t { Application, Window, Object };
    Kind kind;
    AtspiBridge* bridge { nullptr };
    Object* object { nullptr };
};

// A property's value: its D-Bus signature and how to write it.
struct PropertyValue {
    const char* signature;
    Function<void(Writer&)> write;
};

// A call waiting for the engine thread's answer.
struct PendingCall : ThreadSafeRefCounted<PendingCall> {
    Lock lock;
    Condition condition;
    Message* reply WTF_GUARDED_BY_LOCK(lock) { nullptr };
    bool done WTF_GUARDED_BY_LOCK(lock) { false };
    bool abandoned WTF_GUARDED_BY_LOCK(lock) { false };
};

// The process's AT-SPI connection: the application, its windows, and the thread
// that talks to the accessibility bus.
class Service {
public:
    // Null where no AT-SPI can be reached: no libdbus, no session bus, or
    // NO_AT_BRIDGE=1 (the variable GTK's bridge honors too).
    static Service* get();

    void add(AtspiBridge&);
    void remove(AtspiBridge&);

    // Object:<member> events (signature siiva{sv}).
    void emit(const String& path, ASCIILiteral interface, ASCIILiteral member, const char* detail, int detail1, int detail2, const char* dataSignature, NOESCAPE const Function<void(Writer&)>& data);

    String objectPath(const Object& object) const { return makeString(pathPrefix, object.objectID().toUInt64()); }
    String windowPath(const AtspiBridge&) const;

private:
    Service() = default;
    void run();
    bool connect();
    void answer(Message*);
    // libdbus's watches: the descriptors the bus thread polls.
    static DBus::Bool addWatch(DBus::Watch*, void* service);
    static void removeWatch(DBus::Watch*, void* service);
    static void watchToggled(DBus::Watch*, void* service);
    void wakeUp();

    Lock m_watchLock;
    Vector<DBus::Watch*> m_watches WTF_GUARDED_BY_LOCK(m_watchLock);
    int m_wakeUpFD { -1 };

    // Engine thread.
    Message* handle(Message*);
    std::optional<Target> resolve(const String& path);
    void writeReference(Writer&, const String& path);
    void writeReference(Writer&, const Target&);
    void writeNullReference(Writer&);
    String pathOf(const Target&);
    Vector<Target> children(const Target&);
    std::optional<Target> parent(const Target&);
    String name(const Target&);
    Vector<ASCIILiteral> interfaces(const Target&);
    std::optional<PropertyValue> property(const Target&, const String& interface, const String& name);
    bool invoke(const Target&, const String& interface, const String& member, Message* call, Message* reply);

    Vector<AtspiBridge*> m_windows;
    int m_applicationId { 0 };

    // Set by the bus thread once connected.
    mutable Lock m_lock;
    DBus::Connection* m_connection WTF_GUARDED_BY_LOCK(m_lock) { nullptr };
    String m_uniqueName WTF_GUARDED_BY_LOCK(m_lock);
    String m_registryName WTF_GUARDED_BY_LOCK(m_lock);
    String m_registryPath WTF_GUARDED_BY_LOCK(m_lock);
};

class AtspiBridge final : public Bridge {
public:
    AtspiBridge(View& view, Service& service, const String& title)
        : Bridge(view)
        , m_service(service)
        , m_id(++s_lastId)
        , m_title(title)
    {
        m_service.add(*this);
    }

    ~AtspiBridge()
    {
        m_service.remove(*this);
    }

    unsigned id() const { return m_id; }
    const String& title() const { return m_title; }
    void titleChanged(const String& title) final
    {
        m_title = title;
        m_service.emit(m_service.windowPath(*this), "org.a11y.atspi.Event.Object"_s, "PropertyChange"_s, "accessible-name", 0, 0, "s", [&](Writer& writer) {
            writer.string(title);
        });
    }

    void event(Object& object, Event event) final
    {
        auto path = m_service.objectPath(object);
        auto stateChanged = [&](const char* state, bool on) {
            m_service.emit(path, "org.a11y.atspi.Event.Object"_s, "StateChanged"_s, state, on, 0, "i", [](Writer& writer) {
                writer.int32(0);
            });
        };
        switch (event) {
        case Event::FocusChanged:
            stateChanged("focused", true);
            break;
        case Event::StateChanged: {
            auto objectStates = states(object);
            if (objectStates.contains(State::Checkable)) {
                stateChanged("checked", objectStates.contains(State::Checked));
                stateChanged("indeterminate", objectStates.contains(State::Mixed));
            }
            if (objectStates.contains(State::Expandable))
                stateChanged("expanded", objectStates.contains(State::Expanded));
            if (role(object) == Role::ToggleButton)
                stateChanged("pressed", objectStates.contains(State::Pressed));
            stateChanged("enabled", objectStates.contains(State::Enabled));
            break;
        }
        case Event::ValueChanged:
            if (auto range = rangeValue(object)) {
                m_service.emit(path, "org.a11y.atspi.Event.Object"_s, "PropertyChange"_s, "accessible-value", 0, 0, "d", [&](Writer& writer) {
                    writer.float64(range->current);
                });
            }
            break;
        case Event::NameChanged:
            m_service.emit(path, "org.a11y.atspi.Event.Object"_s, "PropertyChange"_s, "accessible-name", 0, 0, "s", [&](Writer& writer) {
                writer.string(name(object));
            });
            break;
        case Event::ChildrenChanged: {
            auto objectChildren = children(object);
            if (objectChildren.isEmpty())
                break;
            auto childPath = m_service.objectPath(objectChildren.last().get());
            m_service.emit(path, "org.a11y.atspi.Event.Object"_s, "ChildrenChanged"_s, "add", static_cast<int>(objectChildren.size() - 1), 0, "(so)", [&](Writer& writer) {
                writer.container(DBus::TypeStruct, nullptr, [&](Writer& reference) {
                    reference.string(s_uniqueName);
                    reference.objectPath(DBus::data(childPath.utf8()));
                });
            });
            break;
        }
        case Event::LoadComplete:
            m_service.emit(path, "org.a11y.atspi.Event.Document"_s, "LoadComplete"_s, "", 0, 0, "i", [](Writer& writer) {
                writer.int32(0);
            });
            break;
        }
    }

    void objectDetached(AXID id) final
    {
        m_service.emit(makeString(pathPrefix, id.toUInt64()), "org.a11y.atspi.Event.Object"_s, "StateChanged"_s, "defunct", 1, 0, "i", [](Writer& writer) {
            writer.int32(0);
        });
    }

    // The connection's name, for references in events (engine thread).
    static String s_uniqueName;

private:
    Service& m_service;
    unsigned m_id;
    String m_title;
    static unsigned s_lastId;
};

String AtspiBridge::s_uniqueName;
unsigned AtspiBridge::s_lastId;

Service* Service::get()
{
    static Service* service = []() -> Service* {
        if (const char* disabled = getenv("NO_AT_BRIDGE"); disabled && !strcmp(disabled, "1"))
            return nullptr;
        // The session bus says where the accessibility bus is; without one there
        // is no AT-SPI (and no autolaunching a bus for it).
        if (!getenv("AT_SPI_BUS_ADDRESS") && !getenv("DBUS_SESSION_BUS_ADDRESS"))
            return nullptr;
        if (!DBus::Library::get())
            return nullptr;
        auto* service = new Service;
        Thread::create("ZeroG AT-SPI"_s, [service] {
            service->run();
        })->detach();
        return service;
    }();
    return service;
}

String Service::windowPath(const AtspiBridge& bridge) const
{
    return makeString(pathPrefix, "window"_s, bridge.id());
}

void Service::add(AtspiBridge& bridge)
{
    m_windows.append(&bridge);
    auto path = windowPath(bridge);
    emit(String::fromLatin1(rootPath), "org.a11y.atspi.Event.Object"_s, "ChildrenChanged"_s, "add", static_cast<int>(m_windows.size() - 1), 0, "(so)", [&](Writer& writer) {
        writer.container(DBus::TypeStruct, nullptr, [&](Writer& reference) {
            reference.string(AtspiBridge::s_uniqueName);
            reference.objectPath(DBus::data(path.utf8()));
        });
    });
}

void Service::remove(AtspiBridge& bridge)
{
    auto index = m_windows.find(&bridge);
    if (index == notFound)
        return;
    m_windows.removeAt(index);
    auto path = windowPath(bridge);
    emit(String::fromLatin1(rootPath), "org.a11y.atspi.Event.Object"_s, "ChildrenChanged"_s, "remove", static_cast<int>(index), 0, "(so)", [&](Writer& writer) {
        writer.container(DBus::TypeStruct, nullptr, [&](Writer& reference) {
            reference.string(AtspiBridge::s_uniqueName);
            reference.objectPath(DBus::data(path.utf8()));
        });
    });
}

void Service::emit(const String& path, ASCIILiteral interface, ASCIILiteral member, const char* detail, int detail1, int detail2, const char* dataSignature, NOESCAPE const Function<void(Writer&)>& data)
{
    DBus::Connection* connection;
    {
        Locker locker { m_lock };
        connection = m_connection;
    }
    if (!connection)
        return; // not connected (yet): nobody hears
    auto* signal = dbus().message_new_signal(DBus::data(path.utf8()),interface.characters(), member.characters());
    if (!signal)
        return;
    Writer writer(signal);
    writer.string(detail);
    writer.int32(detail1);
    writer.int32(detail2);
    writer.container(DBus::TypeVariant, dataSignature, data);
    writer.container(DBus::TypeArray, "{sv}", [](Writer&) { });
    // Written now, or by the bus thread once the socket takes it (its write
    // watch turns on).
    dbus().connection_send(connection, signal, nullptr);
    dbus().message_unref(signal);
}

// ---- The bus thread ----

bool Service::connect()
{
    auto& library = dbus();
    DBus::Error error;
    library.error_init(&error);
    auto fail = [&] {
        library.error_free(&error);
        return false;
    };

    // The accessibility bus's address, from at-spi-bus-launcher (org.a11y.Bus on
    // the session bus) unless the environment gives it.
    String address;
    if (const char* given = getenv("AT_SPI_BUS_ADDRESS"); given && *given)
        address = String::fromUTF8(given);
    else {
        auto* session = library.bus_get_private(DBus::SessionBus, &error);
        if (!session)
            return fail();
        library.connection_set_exit_on_disconnect(session, false);
        auto* request = library.message_new_method_call("org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus", "GetAddress");
        auto* reply = library.connection_send_with_reply_and_block(session, request, 3000, &error);
        library.message_unref(request);
        if (reply) {
            address = Reader(reply).string();
            library.message_unref(reply);
        }
        library.connection_close(session);
        library.connection_unref(session);
        if (address.isEmpty())
            return fail();
    }

    auto* connection = library.connection_open_private(DBus::data(address.utf8()), &error);
    if (!connection)
        return fail();
    if (!library.bus_register(connection, &error)) {
        library.connection_close(connection);
        library.connection_unref(connection);
        return fail();
    }
    library.connection_set_exit_on_disconnect(connection, false);
    auto uniqueName = String::fromUTF8(library.bus_get_unique_name(connection));

    // The registry takes the application as one of the desktop's children and
    // says which object is its parent.
    auto* embed = library.message_new_method_call("org.a11y.atspi.Registry", rootPath, "org.a11y.atspi.Socket", "Embed");
    Writer(embed).container(DBus::TypeStruct, nullptr, [&](Writer& plug) {
        plug.string(uniqueName);
        plug.objectPath(rootPath);
    });
    auto* reply = library.connection_send_with_reply_and_block(connection, embed, 3000, &error);
    library.message_unref(embed);
    String registryName, registryPath;
    if (reply) {
        Reader reader(reply);
        auto socket = reader.recurse();
        registryName = socket.string();
        socket.next();
        registryPath = socket.string();
        library.message_unref(reply);
    }
    library.error_free(&error);

    callOnMainThread([uniqueName = uniqueName.isolatedCopy()] {
        AtspiBridge::s_uniqueName = uniqueName;
    });
    Locker locker { m_lock };
    m_connection = connection;
    m_uniqueName = WTF::move(uniqueName);
    m_registryName = WTF::move(registryName);
    m_registryPath = WTF::move(registryPath);
    return true;
}

DBus::Bool Service::addWatch(DBus::Watch* watch, void* data)
{
    auto& service = *static_cast<Service*>(data);
    {
        Locker locker { service.m_watchLock };
        service.m_watches.append(watch);
    }
    service.wakeUp();
    return true;
}

void Service::removeWatch(DBus::Watch* watch, void* data)
{
    auto& service = *static_cast<Service*>(data);
    {
        Locker locker { service.m_watchLock };
        service.m_watches.removeFirst(watch);
    }
    service.wakeUp();
}

void Service::watchToggled(DBus::Watch*, void* data)
{
    // A watch turned on or off (writing, when messages wait in the queue): the
    // bus thread polls again with it.
    static_cast<Service*>(data)->wakeUp();
}

void Service::wakeUp()
{
    uint64_t one = 1;
    [[maybe_unused]] auto written = write(m_wakeUpFD, &one, sizeof(one));
}

void Service::run()
{
    m_wakeUpFD = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m_wakeUpFD < 0 || !connect())
        return;
    DBus::Connection* connection;
    {
        Locker locker { m_lock };
        connection = m_connection;
    }
    auto& library = dbus();
    library.connection_set_watch_functions(connection, addWatch, removeWatch, watchToggled, this, nullptr);

    auto answerQueued = [&] {
        while (auto* message = library.connection_pop_message(connection)) {
            if (library.message_get_type(message) == DBus::MethodCall)
                answer(message);
            library.message_unref(message);
        }
    };
    // Calls that came while the registry embedded the application.
    answerQueued();

    while (library.connection_get_is_connected(connection)) {
        // The enabled watches, and the wake-up descriptor last.
        Vector<pollfd> descriptors;
        Vector<DBus::Watch*> polled;
        {
            Locker locker { m_watchLock };
            for (auto* watch : m_watches) {
                if (!library.watch_get_enabled(watch))
                    continue;
                unsigned flags = library.watch_get_flags(watch);
                short events = (flags & DBus::WatchReadable ? POLLIN : 0) | (flags & DBus::WatchWritable ? POLLOUT : 0);
                descriptors.append({ library.watch_get_unix_fd(watch), events, 0 });
                polled.append(watch);
            }
        }
        descriptors.append({ m_wakeUpFD, POLLIN, 0 });
        if (poll(descriptors.mutableSpan().data(), descriptors.size(), -1) < 0)
            continue; // a signal
        if (descriptors.last().revents & POLLIN) {
            uint64_t count;
            [[maybe_unused]] auto readBytes = read(m_wakeUpFD, &count, sizeof(count));
        }
        for (size_t i = 0; i < polled.size(); ++i) {
            auto revents = descriptors[i].revents;
            if (!revents)
                continue;
            {
                // Removed meanwhile (the connection closing): not to be handled.
                Locker locker { m_watchLock };
                if (!m_watches.contains(polled[i]))
                    continue;
            }
            unsigned flags = (revents & POLLIN ? DBus::WatchReadable : 0) | (revents & POLLOUT ? DBus::WatchWritable : 0)
                | (revents & POLLERR ? DBus::WatchError : 0) | (revents & POLLHUP ? DBus::WatchHangup : 0);
            library.watch_handle(polled[i], flags);
        }
        answerQueued();
    }
}

void Service::answer(Message* call)
{
    auto& library = dbus();
    auto pending = adoptRef(*new PendingCall);
    library.message_ref(call);
    callOnMainThread([this, pending, call] {
        auto* reply = handle(call);
        dbus().message_unref(call);
        Locker locker { pending->lock };
        if (pending->abandoned) {
            if (reply)
                dbus().message_unref(reply);
            return;
        }
        pending->reply = reply;
        pending->done = true;
        pending->condition.notifyOne();
    });

    Message* reply = nullptr;
    {
        Locker locker { pending->lock };
        // An engine thread busy for this long (a script that does not end) gets
        // the caller an error rather than a hung screen reader.
        auto deadline = MonotonicTime::now() + 5_s;
        while (!pending->done && MonotonicTime::now() < deadline)
            pending->condition.waitUntil(pending->lock, deadline);
        if (pending->done)
            reply = pending->reply;
        else
            pending->abandoned = true;
    }
    if (!reply)
        reply = library.message_new_error(call, "org.freedesktop.DBus.Error.Failed", "the application did not answer");
    DBus::Connection* connection;
    {
        Locker locker { m_lock };
        connection = m_connection;
    }
    library.connection_send(connection, reply, nullptr);
    library.message_unref(reply);
}

// ---- Calls, on the engine thread ----

std::optional<Target> Service::resolve(const String& path)
{
    if (StringView(path) == StringView::fromLatin1(rootPath))
        return Target { Target::Kind::Application };
    if (!path.startsWith(pathPrefix))
        return std::nullopt;
    auto rest = StringView(path).substring(pathPrefix.length());
    if (rest.startsWith("window"_s)) {
        auto id = parseInteger<unsigned>(rest.substring(6));
        for (auto* bridge : m_windows) {
            if (id && bridge->id() == *id)
                return Target { Target::Kind::Window, bridge };
        }
        return std::nullopt;
    }
    auto id = parseInteger<uint64_t>(rest);
    if (!id || !*id || *id == std::numeric_limits<uint64_t>::max())
        return std::nullopt;
    for (auto* bridge : m_windows) {
        if (auto* found = object(bridge->view(), AXID { *id }))
            return Target { Target::Kind::Object, bridge, found };
    }
    return std::nullopt;
}

String Service::pathOf(const Target& target)
{
    switch (target.kind) {
    case Target::Kind::Application:
        return String::fromLatin1(rootPath);
    case Target::Kind::Window:
        return windowPath(*target.bridge);
    case Target::Kind::Object:
        return objectPath(*target.object);
    }
    return { };
}

void Service::writeReference(Writer& writer, const String& path)
{
    String uniqueName;
    {
        Locker locker { m_lock };
        uniqueName = m_uniqueName;
    }
    writer.container(DBus::TypeStruct, nullptr, [&](Writer& reference) {
        reference.string(uniqueName);
        reference.objectPath(DBus::data(path.utf8()));
    });
}

void Service::writeReference(Writer& writer, const Target& target)
{
    writeReference(writer, pathOf(target));
}

void Service::writeNullReference(Writer& writer)
{
    writeReference(writer, String::fromLatin1(nullPath));
}

Vector<Target> Service::children(const Target& target)
{
    Vector<Target> result;
    switch (target.kind) {
    case Target::Kind::Application:
        for (auto* bridge : m_windows)
            result.append({ Target::Kind::Window, bridge });
        break;
    case Target::Kind::Window:
        if (auto* document = root(target.bridge->view()))
            result.append({ Target::Kind::Object, target.bridge, document });
        break;
    case Target::Kind::Object:
        for (auto& child : Accessibility::children(*target.object))
            result.append({ Target::Kind::Object, target.bridge, child.ptr() });
        break;
    }
    return result;
}

std::optional<Target> Service::parent(const Target& target)
{
    switch (target.kind) {
    case Target::Kind::Application:
        return std::nullopt; // the registry's socket
    case Target::Kind::Window:
        return Target { Target::Kind::Application };
    case Target::Kind::Object:
        if (auto* parentObject = Accessibility::parent(*target.object, target.bridge->view()))
            return Target { Target::Kind::Object, target.bridge, parentObject };
        return Target { Target::Kind::Window, target.bridge };
    }
    return std::nullopt;
}

String Service::name(const Target& target)
{
    switch (target.kind) {
    case Target::Kind::Application:
        return programName();
    case Target::Kind::Window:
        return target.bridge->title();
    case Target::Kind::Object:
        return Accessibility::name(*target.object);
    }
    return { };
}

Vector<ASCIILiteral> Service::interfaces(const Target& target)
{
    Vector<ASCIILiteral> result { accessibleInterface };
    switch (target.kind) {
    case Target::Kind::Application:
        result.append(applicationInterface);
        break;
    case Target::Kind::Window:
        result.append(componentInterface);
        break;
    case Target::Kind::Object: {
        auto& object = *target.object;
        result.append(componentInterface);
        if (!defaultActionName(object).isEmpty())
            result.append(actionInterface);
        if (rangeValue(object))
            result.append(valueInterface);
        if (hasText(object))
            result.append(textInterface);
        if (hasText(object) && canSetValue(object) && !rangeValue(object))
            result.append(editableTextInterface);
        break;
    }
    }
    return result;
}

std::optional<PropertyValue> Service::property(const Target& target, const String& interface, const String& property)
{
    auto string = [](String value) -> std::optional<PropertyValue> {
        return PropertyValue { "s", [value = WTF::move(value)](Writer& writer) {
            writer.string(value);
        } };
    };
    auto int32 = [](int value) -> std::optional<PropertyValue> {
        return PropertyValue { "i", [value](Writer& writer) {
            writer.int32(value);
        } };
    };
    auto float64 = [](double value) -> std::optional<PropertyValue> {
        return PropertyValue { "d", [value](Writer& writer) {
            writer.float64(value);
        } };
    };
    auto* object = target.object;

    if (interface == accessibleInterface) {
        if (property == "Name"_s)
            return string(name(target));
        if (property == "Description"_s)
            return string(object ? description(*object) : emptyString());
        if (property == "HelpText"_s || property == "Locale"_s || property == "AccessibleId"_s)
            return string(emptyString());
        if (property == "ChildCount"_s)
            return int32(children(target).size());
        if (property == "Parent"_s) {
            auto parentTarget = parent(target);
            if (parentTarget) {
                auto path = pathOf(*parentTarget);
                return PropertyValue { "(so)", [this, path](Writer& writer) {
                    writeReference(writer, path);
                } };
            }
            String registryName, registryPath;
            {
                Locker locker { m_lock };
                registryName = m_registryName;
                registryPath = m_registryPath;
            }
            return PropertyValue { "(so)", [this, registryName, registryPath](Writer& writer) {
                if (registryPath.isEmpty()) {
                    writeNullReference(writer);
                    return;
                }
                writer.container(DBus::TypeStruct, nullptr, [&](Writer& reference) {
                    reference.string(registryName);
                    reference.objectPath(DBus::data(registryPath.utf8()));
                });
            } };
        }
        return std::nullopt;
    }
    if (interface == applicationInterface && target.kind == Target::Kind::Application) {
        if (property == "ToolkitName"_s)
            return string("ZeroG"_s);
        if (property == "Version"_s)
            return string("0.3"_s);
        if (property == "AtspiVersion"_s)
            return string("2.1"_s);
        if (property == "Id"_s)
            return int32(m_applicationId);
        return std::nullopt;
    }
    if (!object)
        return std::nullopt;
    if (interface == actionInterface && property == "NActions"_s)
        return int32(defaultActionName(*object).isEmpty() ? 0 : 1);
    if (interface == valueInterface) {
        auto range = rangeValue(*object);
        if (!range)
            return std::nullopt;
        if (property == "CurrentValue"_s)
            return float64(range->current);
        if (property == "MinimumValue"_s)
            return float64(range->minimum);
        if (property == "MaximumValue"_s)
            return float64(range->maximum);
        if (property == "MinimumIncrement"_s)
            return float64((range->maximum - range->minimum) / 100);
        if (property == "Text"_s)
            return string(value(*object));
        return std::nullopt;
    }
    if (interface == textInterface) {
        if (property == "CharacterCount"_s)
            return int32(Accessibility::text(*object).length());
        if (property == "CaretOffset"_s) {
            auto selection = textSelection(*object);
            return int32(selection && states(*object).contains(State::Focused) ? static_cast<int>(selection->end) : -1);
        }
        return std::nullopt;
    }
    return std::nullopt;
}

// Interfaces' methods; false: no such method.
bool Service::invoke(const Target& target, const String& interface, const String& member, Message* call, Message* reply)
{
    Reader arguments(call);
    Writer writer(reply);
    auto* object = target.object;

    if (interface == accessibleInterface) {
        if (member == "GetChildren"_s) {
            auto targetChildren = children(target);
            writer.container(DBus::TypeArray, "(so)", [&](Writer& array) {
                for (auto& child : targetChildren)
                    writeReference(array, child);
            });
            return true;
        }
        if (member == "GetChildAtIndex"_s) {
            auto index = arguments.int32();
            auto targetChildren = children(target);
            if (index >= 0 && static_cast<size_t>(index) < targetChildren.size())
                writeReference(writer, targetChildren[index]);
            else
                writeNullReference(writer);
            return true;
        }
        if (member == "GetIndexInParent"_s) {
            int index = -1;
            if (auto parentTarget = parent(target)) {
                auto siblings = children(*parentTarget);
                for (size_t i = 0; i < siblings.size(); ++i) {
                    if (siblings[i].kind == target.kind && siblings[i].bridge == target.bridge && siblings[i].object == target.object)
                        index = static_cast<int>(i);
                }
            }
            writer.int32(index);
            return true;
        }
        if (member == "GetRole"_s || member == "GetRoleName"_s || member == "GetLocalizedRoleName"_s) {
            auto targetRole = target.kind == Target::Kind::Application ? applicationRole : target.kind == Target::Kind::Window ? frameRole : atspiRole(role(*object));
            if (member == "GetRole"_s)
                writer.uint32(targetRole.value);
            else
                writer.string(targetRole.name);
            return true;
        }
        if (member == "GetState"_s) {
            uint64_t bits = 0;
            if (target.kind == Target::Kind::Window)
                bits = static_cast<uint64_t>(AtspiState::Enabled) | static_cast<uint64_t>(AtspiState::Sensitive) | static_cast<uint64_t>(AtspiState::Visible) | static_cast<uint64_t>(AtspiState::Showing) | static_cast<uint64_t>(AtspiState::Resizable) | static_cast<uint64_t>(AtspiState::Active);
            else if (object)
                bits = atspiStates(*object);
            writer.container(DBus::TypeArray, "u", [&](Writer& array) {
                array.uint32(bits & 0xffffffff);
                array.uint32(bits >> 32);
            });
            return true;
        }
        if (member == "GetRelationSet"_s) {
            writer.container(DBus::TypeArray, "(ua(so))", [](Writer&) { });
            return true;
        }
        if (member == "GetAttributes"_s) {
            writer.container(DBus::TypeArray, "{ss}", [&](Writer& array) {
                if (target.kind != Target::Kind::Application)
                    return;
                array.container(DBus::TypeDictEntry, nullptr, [](Writer& entry) {
                    entry.string("toolkit");
                    entry.string("ZeroG");
                });
            });
            return true;
        }
        if (member == "GetApplication"_s) {
            writeReference(writer, Target { Target::Kind::Application });
            return true;
        }
        if (member == "GetInterfaces"_s) {
            auto targetInterfaces = interfaces(target);
            writer.container(DBus::TypeArray, "s", [&](Writer& array) {
                for (auto name : targetInterfaces)
                    array.string(name.characters());
            });
            return true;
        }
        return false;
    }

    if (interface == applicationInterface && target.kind == Target::Kind::Application) {
        if (member == "GetLocale"_s) {
            writer.string("");
            return true;
        }
        return false;
    }

    if (interface == componentInterface && target.kind != Target::Kind::Application) {
        auto& view = target.bridge->view();
        float scale = view.scale();
        // Window coordinates (a Wayland window cannot know its place on the
        // screen): physical pixels from the window's top left.
        auto rect = enclosingIntRect(object ? bounds(*object) : FloatRect { { }, view.size() });
        rect.scale(scale);
        if (member == "GetExtents"_s) {
            writer.container(DBus::TypeStruct, nullptr, [&](Writer& extents) {
                extents.int32(rect.x());
                extents.int32(rect.y());
                extents.int32(rect.width());
                extents.int32(rect.height());
            });
            return true;
        }
        if (member == "GetPosition"_s || member == "GetSize"_s) {
            bool position = member == "GetPosition"_s;
            writer.container(DBus::TypeStruct, nullptr, [&](Writer& pair) {
                pair.int32(position ? rect.x() : rect.width());
                pair.int32(position ? rect.y() : rect.height());
            });
            return true;
        }
        if (member == "Contains"_s || member == "GetAccessibleAtPoint"_s) {
            int x = arguments.int32();
            arguments.next();
            int y = arguments.int32();
            if (member == "Contains"_s) {
                writer.boolean(rect.contains({ x, y }));
                return true;
            }
            auto* hit = hitTest(view, { static_cast<int>(x / scale), static_cast<int>(y / scale) });
            if (hit)
                writeReference(writer, Target { Target::Kind::Object, target.bridge, hit });
            else
                writeNullReference(writer);
            return true;
        }
        if (member == "GetLayer"_s) {
            writer.uint32(target.kind == Target::Kind::Window ? 7 : 3); // window, widget
            return true;
        }
        if (member == "GetMDIZOrder"_s) {
            writer.int16(0);
            return true;
        }
        if (member == "GrabFocus"_s) {
            writer.boolean(object && setFocus(*object));
            return true;
        }
        if (member == "GetAlpha"_s) {
            writer.float64(1);
            return true;
        }
        if (member == "ScrollTo"_s || member == "ScrollToPoint"_s || member == "SetExtents"_s || member == "SetPosition"_s || member == "SetSize"_s) {
            writer.boolean(false);
            return true;
        }
        return false;
    }

    if (!object)
        return false;

    if (interface == actionInterface) {
        auto actionName = defaultActionName(*object);
        if (member == "GetActions"_s) {
            writer.container(DBus::TypeArray, "(sss)", [&](Writer& array) {
                if (actionName.isEmpty())
                    return;
                array.container(DBus::TypeStruct, nullptr, [&](Writer& action) {
                    action.string(actionName);
                    action.string("");
                    action.string("");
                });
            });
            return true;
        }
        auto index = arguments.int32();
        bool valid = !index && !actionName.isEmpty();
        if (member == "DoAction"_s) {
            writer.boolean(valid && doDefaultAction(*object));
            return true;
        }
        if (member == "GetName"_s || member == "GetLocalizedName"_s) {
            writer.string(valid ? actionName : emptyString());
            return true;
        }
        if (member == "GetDescription"_s || member == "GetKeyBinding"_s) {
            writer.string("");
            return true;
        }
        return false;
    }

    if (interface == textInterface) {
        auto text = Accessibility::text(*object);
        if (member == "GetText"_s) {
            int start = arguments.int32();
            arguments.next();
            int end = arguments.int32();
            int length = text.length();
            if (end < 0 || end > length)
                end = length;
            start = std::clamp(start, 0, end);
            writer.string(text.substring(start, end - start));
            return true;
        }
        if (member == "GetCharacterAtOffset"_s) {
            int offset = arguments.int32();
            writer.int32(offset >= 0 && static_cast<unsigned>(offset) < text.length() ? text[offset] : 0);
            return true;
        }
        if (member == "SetCaretOffset"_s) {
            unsigned offset = std::max(0, arguments.int32());
            writer.boolean(setTextSelection(*object, { offset, offset }));
            return true;
        }
        return false;
    }

    if (interface == editableTextInterface) {
        if (member == "SetTextContents"_s) {
            writer.boolean(setValue(*object, arguments.string()));
            return true;
        }
        if (member == "InsertText"_s || member == "DeleteText"_s || member == "CutText"_s || member == "CopyText"_s || member == "PasteText"_s) {
            writer.boolean(false);
            return true;
        }
        return false;
    }

    return false;
}

Message* Service::handle(Message* call)
{
    auto& library = dbus();
    auto path = String::fromUTF8(library.message_get_path(call));
    auto interface = String::fromUTF8(library.message_get_interface(call));
    auto member = String::fromUTF8(library.message_get_member(call));

    if (interface == "org.freedesktop.DBus.Introspectable"_s && member == "Introspect"_s) {
        auto* reply = library.message_new_method_return(call);
        Writer(reply).string("<node/>");
        return reply;
    }

    auto target = resolve(path);
    if (!target)
        return library.message_new_error(call, "org.freedesktop.DBus.Error.UnknownObject", "no such object");

    if (interface == "org.freedesktop.DBus.Properties"_s) {
        Reader arguments(call);
        auto propertyInterface = arguments.string();
        arguments.next();
        if (member == "Get"_s) {
            auto value = property(*target, propertyInterface, arguments.string());
            if (!value)
                return library.message_new_error(call, "org.freedesktop.DBus.Error.UnknownProperty", "no such property");
            auto* reply = library.message_new_method_return(call);
            Writer(reply).container(DBus::TypeVariant, value->signature, value->write);
            return reply;
        }
        if (member == "GetAll"_s) {
            static constexpr ASCIILiteral accessibleProperties[] = { "Name"_s, "Description"_s, "Parent"_s, "ChildCount"_s, "Locale"_s, "AccessibleId"_s, "HelpText"_s };
            static constexpr ASCIILiteral applicationProperties[] = { "ToolkitName"_s, "Version"_s, "AtspiVersion"_s, "Id"_s };
            static constexpr ASCIILiteral valueProperties[] = { "CurrentValue"_s, "MinimumValue"_s, "MaximumValue"_s, "MinimumIncrement"_s, "Text"_s };
            static constexpr ASCIILiteral textProperties[] = { "CharacterCount"_s, "CaretOffset"_s };
            static constexpr ASCIILiteral actionProperties[] = { "NActions"_s };
            std::span<const ASCIILiteral> names;
            if (propertyInterface == accessibleInterface)
                names = accessibleProperties;
            else if (propertyInterface == applicationInterface)
                names = applicationProperties;
            else if (propertyInterface == valueInterface)
                names = valueProperties;
            else if (propertyInterface == textInterface)
                names = textProperties;
            else if (propertyInterface == actionInterface)
                names = actionProperties;
            auto* reply = library.message_new_method_return(call);
            Writer(reply).container(DBus::TypeArray, "{sv}", [&](Writer& array) {
                for (auto name : names) {
                    auto value = property(*target, propertyInterface, name);
                    if (!value)
                        continue;
                    array.container(DBus::TypeDictEntry, nullptr, [&](Writer& entry) {
                        entry.string(name.characters());
                        entry.container(DBus::TypeVariant, value->signature, value->write);
                    });
                }
            });
            return reply;
        }
        if (member == "Set"_s) {
            auto name = arguments.string();
            arguments.next();
            auto value = arguments.recurse();
            bool done = false;
            if (propertyInterface == applicationInterface && name == "Id"_s && target->kind == Target::Kind::Application) {
                m_applicationId = value.int32();
                done = true;
            } else if (propertyInterface == valueInterface && name == "CurrentValue"_s && target->object)
                done = setRangeValue(*target->object, value.float64());
            if (!done)
                return library.message_new_error(call, "org.freedesktop.DBus.Error.PropertyReadOnly", "cannot set the property");
            return library.message_new_method_return(call);
        }
        return library.message_new_error(call, "org.freedesktop.DBus.Error.UnknownMethod", "no such method");
    }

    auto* reply = library.message_new_method_return(call);
    if (invoke(*target, interface, member, call, reply))
        return reply;
    library.message_unref(reply);
    return library.message_new_error(call, "org.freedesktop.DBus.Error.UnknownMethod", "no such method");
}

} // namespace

std::unique_ptr<Bridge> createBridge(View& view, void*, const String& title)
{
    auto* service = Service::get();
    if (!service)
        return nullptr;
    return std::make_unique<AtspiBridge>(view, *service, title);
}

} // namespace ZeroG::Accessibility

#endif // !OS(WINDOWS)
