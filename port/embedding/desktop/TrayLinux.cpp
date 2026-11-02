// The system tray on Linux: a StatusNotifierItem (org.kde.StatusNotifierItem,
// the freedesktop.org specification KDE, Xfce, Cinnamon, MATE and GNOME's
// AppIndicator extension host), its menu over com.canonical.dbusmenu, on the
// session bus (SessionBus.cpp). The item registers with the
// StatusNotifierWatcher, and again whenever one starts. Hosts' calls are
// answered on the bus thread from a copy of the tray's state; clicks go to
// the engine thread.

#include "config.h"
#include "desktop/Desktop.h"

#include "desktop/SessionBus.h"
#include <cstring>
#include <limits>
#include <unistd.h>
#include <wtf/Lock.h>
#include <wtf/MainThread.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace ZeroG {

using DBus::Message;
using DBus::Reader;
using DBus::Writer;

namespace {

constexpr const char* s_itemInterface = "org.kde.StatusNotifierItem";
constexpr const char* s_menuInterface = "com.canonical.dbusmenu";
constexpr const char* s_propertiesInterface = "org.freedesktop.DBus.Properties";
constexpr int s_syntheticIDs = 0x100000; // submenus and separators

struct State {
    Lock lock;
    String id WTF_GUARDED_BY_LOCK(lock);
    String tooltip WTF_GUARDED_BY_LOCK(lock);
    TrayIcon icon WTF_GUARDED_BY_LOCK(lock);
    std::optional<Menu> menu WTF_GUARDED_BY_LOCK(lock);
    uint32_t revision WTF_GUARDED_BY_LOCK(lock) { 1 };
    std::optional<TrayRect> lastActivation WTF_GUARDED_BY_LOCK(lock);
};

// What the engine thread's tray gives the bus thread's objects to call it.
struct Link {
    Tray::Callbacks callbacks; // engine thread
    bool alive { true };       // engine thread
};

// A menu item as dbusmenu sees it.
struct Node {
    int id { 0 };
    const Menu::Item* item { nullptr }; // null: the root
    Vector<Node> children;
};

void collect(Node& parent, const Vector<Menu::Item>& items, int& nextSynthetic)
{
    for (auto& item : items) {
        Node node;
        node.item = &item;
        node.id = item.kind == Menu::Item::Kind::Action ? static_cast<int>(item.id) : nextSynthetic++;
        if (item.kind == Menu::Item::Kind::Submenu)
            collect(node, item.submenu, nextSynthetic);
        parent.children.append(WTF::move(node));
    }
}

Node tree(const std::optional<Menu>& menu)
{
    Node root;
    int nextSynthetic = s_syntheticIDs;
    if (menu)
        collect(root, menu->items, nextSynthetic);
    return root;
}

const Node* find(const Node& node, int id)
{
    if (node.id == id)
        return &node;
    for (auto& child : node.children) {
        if (auto* found = find(child, id))
            return found;
    }
    return nullptr;
}

// "&File" is "_File" for dbusmenu, "&&" a literal '&', '_' doubled.
String mnemonicLabel(const String& label)
{
    StringBuilder result;
    for (unsigned i = 0; i < label.length(); ++i) {
        auto c = label[i];
        if (c == '&' && i + 1 < label.length() && label[i + 1] == '&') {
            result.append('&');
            ++i;
        } else if (c == '&')
            result.append('_');
        else if (c == '_')
            result.append("__"_s);
        else
            result.append(c);
    }
    return result.toString();
}

void entry(Writer& dict, const char* key, const char* signature, NOESCAPE const Function<void(Writer&)>& value)
{
    dict.container(DBus::TypeDictEntry, nullptr, [&](Writer& pair) {
        pair.string(key);
        pair.container(DBus::TypeVariant, signature, value);
    });
}

bool wanted(const Vector<String>& names, ASCIILiteral name)
{
    return names.isEmpty() || names.contains(String(name));
}

void writeItemProperties(Writer& dict, const Node& node, const Vector<String>& names)
{
    if (!node.item) {
        if (wanted(names, "children-display"_s))
            entry(dict, "children-display", "s", [](Writer& value) { value.string("submenu"); });
        return;
    }
    auto& item = *node.item;
    if (item.kind == Menu::Item::Kind::Separator) {
        if (wanted(names, "type"_s))
            entry(dict, "type", "s", [](Writer& value) { value.string("separator"); });
        return;
    }
    if (wanted(names, "label"_s))
        entry(dict, "label", "s", [&](Writer& value) { value.string(mnemonicLabel(item.label)); });
    if (!item.enabled && wanted(names, "enabled"_s))
        entry(dict, "enabled", "b", [](Writer& value) { value.boolean(false); });
    if (item.kind == Menu::Item::Kind::Submenu) {
        if (wanted(names, "children-display"_s))
            entry(dict, "children-display", "s", [](Writer& value) { value.string("submenu"); });
    } else if (item.checkbox) {
        if (wanted(names, "toggle-type"_s))
            entry(dict, "toggle-type", "s", [](Writer& value) { value.string("checkmark"); });
        if (wanted(names, "toggle-state"_s))
            entry(dict, "toggle-state", "i", [&](Writer& value) { value.int32(item.checked ? 1 : 0); });
    }
}

// (ia{sv}av)
void writeLayout(Writer& writer, const Node& node, int depth, const Vector<String>& names)
{
    writer.container(DBus::TypeStruct, nullptr, [&](Writer& layout) {
        layout.int32(node.id);
        layout.container(DBus::TypeArray, "{sv}", [&](Writer& dict) {
            writeItemProperties(dict, node, names);
        });
        layout.container(DBus::TypeArray, "v", [&](Writer& children) {
            if (!depth)
                return;
            for (auto& child : node.children) {
                children.container(DBus::TypeVariant, "(ia{sv}av)", [&](Writer& variant) {
                    writeLayout(variant, child, depth - 1, names);
                });
            }
        });
    });
}

Vector<String> readStrings(Reader array)
{
    Vector<String> result;
    while (array.type() == DBus::TypeString) {
        result.append(array.string());
        if (!array.next())
            break;
    }
    return result;
}

// a(iiay): ARGB32 in network byte order.
void writePixmaps(Writer& writer, const TrayIcon& icon)
{
    writer.container(DBus::TypeArray, "(iiay)", [&](Writer& array) {
        if (!icon.width || !icon.height)
            return;
        array.container(DBus::TypeStruct, nullptr, [&](Writer& pixmap) {
            pixmap.int32(icon.width);
            pixmap.int32(icon.height);
            Vector<uint8_t> argb(icon.rgba.size());
            for (size_t i = 0; i + 3 < icon.rgba.size(); i += 4) {
                argb[i] = icon.rgba[i + 3];
                argb[i + 1] = icon.rgba[i];
                argb[i + 2] = icon.rgba[i + 1];
                argb[i + 3] = icon.rgba[i + 2];
            }
            pixmap.bytes(argb.span());
        });
    });
}

class TrayLinux final : public Tray {
public:
    TrayLinux(SessionBus& bus, const String& id, TrayIcon&& icon, const String& tooltip, Callbacks&& callbacks)
        : m_bus(bus)
        , m_state(std::make_shared<State>())
        , m_link(std::make_shared<Link>())
    {
        static unsigned nextNumber = 1;
        m_number = nextNumber++;
        m_link->callbacks = WTF::move(callbacks);
        {
            Locker locker { m_state->lock };
            m_state->id = id;
            m_state->tooltip = tooltip;
            m_state->icon = WTF::move(icon);
        }
        // The first item at the conventional name and path; others registered
        // by their paths on this connection.
        m_itemPath = m_number == 1 ? "/StatusNotifierItem"_s : makeString("/StatusNotifierItem"_s, m_number);
        m_menuPath = makeString("/MenuBar"_s, m_number);
        m_busName = makeString("org.kde.StatusNotifierItem-"_s, getpid(), '-', m_number);

        auto state = m_state;
        auto link = m_link;
        auto menuPath = m_menuPath;
        m_bus.addObject(m_itemPath, [&bus = m_bus, state, link, menuPath](Message* call) {
            return answerItem(bus, *state, link, menuPath, call);
        });
        m_bus.addObject(m_menuPath, [&bus = m_bus, state, link](Message* call) {
            return answerMenu(bus, *state, link, call);
        });
        // The watcher, now and whenever it starts again.
        auto service = m_number == 1 ? m_busName : m_itemPath;
        auto busName = m_busName;
        bool first = m_number == 1;
        m_signalHandler = m_bus.addSignalHandler([&bus = m_bus, service](Message* signal) {
            auto& library = bus.library();
            const char* member = library.message_get_member(signal);
            if (!member || strcmp(member, "NameOwnerChanged"))
                return;
            Reader reader(signal);
            if (reader.string() != "org.kde.StatusNotifierWatcher"_s)
                return;
            reader.next();
            reader.next();
            if (!reader.string().isEmpty())
                registerWithWatcher(bus, service);
        });
        m_bus.post([&bus = m_bus, service, busName, first](DBus::Connection* connection) {
            if (first) {
                DBus::Error error;
                bus.library().error_init(&error);
                bus.library().bus_request_name(connection, DBus::data(busName.utf8()), 0, &error);
                bus.library().error_free(&error);
            }
            bus.addMatch(connection, "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.kde.StatusNotifierWatcher'");
            registerWithWatcher(bus, service);
        });
    }

    ~TrayLinux()
    {
        m_link->alive = false;
        m_bus.removeSignalHandler(m_signalHandler);
        m_bus.removeObject(m_itemPath);
        m_bus.removeObject(m_menuPath);
        // Hosts drop an item whose name or connection goes; with one
        // connection for every item, an emptied item is the way to go away.
        auto itemPath = m_itemPath;
        auto busName = m_busName;
        bool first = m_number == 1;
        m_bus.post([&bus = m_bus, itemPath, busName, first](DBus::Connection* connection) {
            if (!first)
                return;
            auto* release = bus.library().message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ReleaseName");
            Writer(release).string(busName);
            bus.library().connection_send(connection, release, nullptr);
            bus.library().message_unref(release);
        });
    }

    void setIcon(TrayIcon&& icon) final
    {
        {
            Locker locker { m_state->lock };
            m_state->icon = WTF::move(icon);
        }
        emit(m_itemPath, s_itemInterface, "NewIcon");
    }

    void setTooltip(const String& tooltip) final
    {
        {
            Locker locker { m_state->lock };
            m_state->tooltip = tooltip;
        }
        emit(m_itemPath, s_itemInterface, "NewToolTip");
        emit(m_itemPath, s_itemInterface, "NewTitle");
    }

    void setMenu(std::optional<Menu>&& menu) final
    {
        {
            Locker locker { m_state->lock };
            m_state->menu = WTF::move(menu);
        }
        layoutUpdated();
    }

    void setChecked(unsigned id, bool checked) final
    {
        {
            Locker locker { m_state->lock };
            if (m_state->menu)
                updateMenuItem(m_state->menu->items, id, checked, std::nullopt);
        }
        layoutUpdated();
    }

    void setEnabled(unsigned id, bool enabled) final
    {
        {
            Locker locker { m_state->lock };
            if (m_state->menu)
                updateMenuItem(m_state->menu->items, id, std::nullopt, enabled);
        }
        layoutUpdated();
    }

    std::optional<TrayRect> rect() final
    {
        Locker locker { m_state->lock };
        return m_state->lastActivation;
    }

private:
    static void registerWithWatcher(SessionBus& bus, const String& service)
    {
        bus.post([&bus, service](DBus::Connection* connection) {
            auto* request = bus.library().message_new_method_call("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem");
            Writer(request).string(service);
            // No watcher yet: the item waits for one (NameOwnerChanged).
            if (auto* reply = bus.call(connection, request, 2000))
                bus.library().message_unref(reply);
            bus.library().message_unref(request);
        });
    }

    void emit(const String& path, const char* interface, const char* member)
    {
        m_bus.post([&bus = m_bus, path, interface, member](DBus::Connection* connection) {
            auto* signal = bus.library().message_new_signal(DBus::data(path.utf8()), interface, member);
            bus.library().connection_send(connection, signal, nullptr);
            bus.library().message_unref(signal);
        });
    }

    void layoutUpdated()
    {
        uint32_t revision;
        {
            Locker locker { m_state->lock };
            revision = ++m_state->revision;
        }
        m_bus.post([&bus = m_bus, path = m_menuPath, revision](DBus::Connection* connection) {
            auto* signal = bus.library().message_new_signal(DBus::data(path.utf8()), s_menuInterface, "LayoutUpdated");
            Writer writer(signal);
            writer.uint32(revision);
            writer.int32(0);
            bus.library().connection_send(connection, signal, nullptr);
            bus.library().message_unref(signal);
        });
    }

    static bool is(const DBus::Library& library, Message* call, const char* interface, const char* member)
    {
        const char* callInterface = library.message_get_interface(call);
        const char* callMember = library.message_get_member(call);
        return callMember && !strcmp(callMember, member) && (!callInterface || !strcmp(callInterface, interface));
    }

    static void writeItemProperty(Writer& value, State& state, const String& menuPath, const String& name) WTF_REQUIRES_LOCK(state.lock)
    {
        if (name == "Category"_s)
            value.container(DBus::TypeVariant, "s", [](Writer& v) { v.string("ApplicationStatus"); });
        else if (name == "Id"_s)
            value.container(DBus::TypeVariant, "s", [&](Writer& v) { v.string(state.id); });
        else if (name == "Title"_s)
            value.container(DBus::TypeVariant, "s", [&](Writer& v) { v.string(state.tooltip.isEmpty() ? state.id : state.tooltip); });
        else if (name == "Status"_s)
            value.container(DBus::TypeVariant, "s", [](Writer& v) { v.string("Active"); });
        else if (name == "WindowId"_s)
            value.container(DBus::TypeVariant, "i", [](Writer& v) { v.int32(0); });
        else if (name == "IconName"_s || name == "OverlayIconName"_s || name == "AttentionIconName"_s || name == "AttentionMovieName"_s || name == "IconThemePath"_s)
            value.container(DBus::TypeVariant, "s", [](Writer& v) { v.string(""); });
        else if (name == "IconPixmap"_s)
            value.container(DBus::TypeVariant, "a(iiay)", [&](Writer& v) { writePixmaps(v, state.icon); });
        else if (name == "OverlayIconPixmap"_s || name == "AttentionIconPixmap"_s)
            value.container(DBus::TypeVariant, "a(iiay)", [](Writer& v) { writePixmaps(v, { }); });
        else if (name == "ToolTip"_s) {
            value.container(DBus::TypeVariant, "(sa(iiay)ss)", [&](Writer& v) {
                v.container(DBus::TypeStruct, nullptr, [&](Writer& tip) {
                    tip.string("");
                    writePixmaps(tip, { });
                    tip.string(state.tooltip);
                    tip.string("");
                });
            });
        } else if (name == "ItemIsMenu"_s)
            value.container(DBus::TypeVariant, "b", [](Writer& v) { v.boolean(false); });
        else if (name == "Menu"_s)
            value.container(DBus::TypeVariant, "o", [&](Writer& v) { v.objectPath(DBus::data(menuPath.utf8())); });
    }

    static Message* answerItem(SessionBus& bus, State& state, const std::shared_ptr<Link>& link, const String& menuPath, Message* call)
    {
        auto& library = bus.library();
        static constexpr ASCIILiteral properties[] = { "Category"_s, "Id"_s, "Title"_s, "Status"_s, "WindowId"_s, "IconName"_s, "IconPixmap"_s,
            "OverlayIconName"_s, "OverlayIconPixmap"_s, "AttentionIconName"_s, "AttentionIconPixmap"_s, "AttentionMovieName"_s, "ToolTip"_s,
            "ItemIsMenu"_s, "Menu"_s };
        if (is(library, call, s_propertiesInterface, "GetAll")) {
            auto* reply = library.message_new_method_return(call);
            Locker locker { state.lock };
            Writer(reply).container(DBus::TypeArray, "{sv}", [&](Writer& dict) {
                for (auto name : properties) {
                    dict.container(DBus::TypeDictEntry, nullptr, [&](Writer& pair) {
                        pair.string(name.characters());
                        writeItemProperty(pair, state, menuPath, String(name));
                    });
                }
            });
            return reply;
        }
        if (is(library, call, s_propertiesInterface, "Get")) {
            Reader reader(call);
            reader.next();
            auto name = reader.string();
            auto* reply = library.message_new_method_return(call);
            Locker locker { state.lock };
            Writer writer(reply);
            writeItemProperty(writer, state, menuPath, name);
            return reply;
        }
        if (is(library, call, s_itemInterface, "Activate")) {
            Reader reader(call);
            int x = reader.int32();
            reader.next();
            int y = reader.int32();
            {
                Locker locker { state.lock };
                state.lastActivation = TrayRect { x, y, 1, 1 };
            }
            callOnMainThread([link, x, y] {
                if (link->alive && link->callbacks.activated)
                    link->callbacks.activated(x, y);
            });
            return library.message_new_method_return(call);
        }
        // The host shows the menu itself (the Menu property); the rest asks nothing of the application.
        if (is(library, call, s_itemInterface, "ContextMenu") || is(library, call, s_itemInterface, "SecondaryActivate") || is(library, call, s_itemInterface, "Scroll"))
            return library.message_new_method_return(call);
        if (is(library, call, "org.freedesktop.DBus.Introspectable", "Introspect")) {
            auto* reply = library.message_new_method_return(call);
            Writer(reply).string("<node><interface name=\"org.kde.StatusNotifierItem\"><method name=\"Activate\"><arg type=\"i\" direction=\"in\"/><arg type=\"i\" direction=\"in\"/></method>"
                "<method name=\"ContextMenu\"><arg type=\"i\" direction=\"in\"/><arg type=\"i\" direction=\"in\"/></method><signal name=\"NewIcon\"/><signal name=\"NewToolTip\"/><signal name=\"NewTitle\"/>"
                "<property name=\"Id\" type=\"s\" access=\"read\"/><property name=\"Title\" type=\"s\" access=\"read\"/><property name=\"Status\" type=\"s\" access=\"read\"/>"
                "<property name=\"IconPixmap\" type=\"a(iiay)\" access=\"read\"/><property name=\"ToolTip\" type=\"(sa(iiay)ss)\" access=\"read\"/><property name=\"Menu\" type=\"o\" access=\"read\"/></interface></node>");
            return reply;
        }
        return nullptr;
    }

    static Message* answerMenu(SessionBus& bus, State& state, const std::shared_ptr<Link>& link, Message* call)
    {
        auto& library = bus.library();
        if (is(library, call, s_menuInterface, "GetLayout")) {
            Reader reader(call);
            int parent = reader.int32();
            reader.next();
            int depth = reader.int32();
            reader.next();
            auto names = readStrings(reader.recurse());
            auto* reply = library.message_new_method_return(call);
            Locker locker { state.lock };
            auto root = tree(state.menu);
            auto* node = find(root, parent);
            Writer writer(reply);
            writer.uint32(state.revision);
            writeLayout(writer, node ? *node : root, depth < 0 ? std::numeric_limits<int>::max() : depth, names);
            return reply;
        }
        if (is(library, call, s_menuInterface, "GetGroupProperties")) {
            Reader reader(call);
            Vector<int> ids;
            for (auto array = reader.recurse(); array.type() == DBus::TypeInt32;) {
                ids.append(array.int32());
                if (!array.next())
                    break;
            }
            reader.next();
            auto names = readStrings(reader.recurse());
            auto* reply = library.message_new_method_return(call);
            Locker locker { state.lock };
            auto root = tree(state.menu);
            Writer(reply).container(DBus::TypeArray, "(ia{sv})", [&](Writer& array) {
                for (int id : ids) {
                    auto* node = find(root, id);
                    if (!node)
                        continue;
                    array.container(DBus::TypeStruct, nullptr, [&](Writer& item) {
                        item.int32(id);
                        item.container(DBus::TypeArray, "{sv}", [&](Writer& dict) {
                            writeItemProperties(dict, *node, names);
                        });
                    });
                }
            });
            return reply;
        }
        if (is(library, call, s_menuInterface, "GetProperty")) {
            Reader reader(call);
            int id = reader.int32();
            reader.next();
            auto name = reader.string();
            auto* reply = library.message_new_method_return(call);
            Locker locker { state.lock };
            auto root = tree(state.menu);
            auto* node = find(root, id);
            Writer writer(reply);
            // The property alone, as a variant: through a one-entry dict's writer.
            writer.container(DBus::TypeVariant, "s", [&](Writer& value) {
                value.string(node && node->item ? mnemonicLabel(node->item->label) : String());
            });
            return reply;
        }
        if (is(library, call, s_menuInterface, "Event") || is(library, call, s_menuInterface, "EventGroup")) {
            bool group = is(library, call, s_menuInterface, "EventGroup");
            Vector<int> clicked;
            auto take = [&](Reader event) {
                int id = event.int32();
                event.next();
                if (event.string() == "clicked"_s)
                    clicked.append(id);
            };
            Reader reader(call);
            if (group) {
                for (auto array = reader.recurse(); array.type() == DBus::TypeStruct;) {
                    take(array.recurse());
                    if (!array.next())
                        break;
                }
            } else
                take(reader);
            {
                Locker locker { state.lock };
                auto root = tree(state.menu);
                clicked.removeAllMatching([&](int id) {
                    auto* node = find(root, id);
                    return !node || !node->item || node->item->kind != Menu::Item::Kind::Action || !node->item->enabled;
                });
            }
            for (int id : clicked) {
                callOnMainThread([link, id] {
                    if (link->alive && link->callbacks.menuItem)
                        link->callbacks.menuItem(static_cast<unsigned>(id));
                });
            }
            auto* reply = library.message_new_method_return(call);
            if (group)
                Writer(reply).container(DBus::TypeArray, "i", [](Writer&) { });
            return reply;
        }
        if (is(library, call, s_menuInterface, "AboutToShow")) {
            auto* reply = library.message_new_method_return(call);
            Writer(reply).boolean(false);
            return reply;
        }
        if (is(library, call, s_menuInterface, "AboutToShowGroup")) {
            auto* reply = library.message_new_method_return(call);
            Writer writer(reply);
            writer.container(DBus::TypeArray, "i", [](Writer&) { });
            writer.container(DBus::TypeArray, "i", [](Writer&) { });
            return reply;
        }
        if (is(library, call, s_propertiesInterface, "GetAll") || is(library, call, s_propertiesInterface, "Get")) {
            bool all = is(library, call, s_propertiesInterface, "GetAll");
            String only;
            if (!all) {
                Reader reader(call);
                reader.next();
                only = reader.string();
            }
            auto* reply = library.message_new_method_return(call);
            auto write = [&](Writer& value, const String& name) {
                if (name == "Version"_s)
                    value.container(DBus::TypeVariant, "u", [](Writer& v) { v.uint32(3); });
                else if (name == "TextDirection"_s)
                    value.container(DBus::TypeVariant, "s", [](Writer& v) { v.string("ltr"); });
                else if (name == "Status"_s)
                    value.container(DBus::TypeVariant, "s", [](Writer& v) { v.string("normal"); });
                else if (name == "IconThemePath"_s)
                    value.container(DBus::TypeVariant, "as", [](Writer& v) { v.container(DBus::TypeArray, "s", [](Writer&) { }); });
            };
            Writer writer(reply);
            if (all) {
                writer.container(DBus::TypeArray, "{sv}", [&](Writer& dict) {
                    for (auto name : { "Version"_s, "TextDirection"_s, "Status"_s, "IconThemePath"_s }) {
                        dict.container(DBus::TypeDictEntry, nullptr, [&](Writer& pair) {
                            pair.string(name.characters());
                            write(pair, String(name));
                        });
                    }
                });
            } else
                write(writer, only);
            return reply;
        }
        return nullptr;
    }

    SessionBus& m_bus;
    std::shared_ptr<State> m_state;
    std::shared_ptr<Link> m_link;
    unsigned m_number { 0 };
    unsigned m_signalHandler { 0 };
    String m_itemPath;
    String m_menuPath;
    String m_busName;
};

} // namespace

std::unique_ptr<Tray> Tray::create(const String& id, TrayIcon&& icon, const String& tooltip, Callbacks&& callbacks, String& error)
{
    auto* bus = SessionBus::get();
    if (!bus) {
        error = "no session bus (or no libdbus): no status notifier items"_s;
        return nullptr;
    }
    return std::make_unique<TrayLinux>(*bus, id, WTF::move(icon), tooltip, WTF::move(callbacks));
}

} // namespace ZeroG
