// Notifications on Linux: org.freedesktop.Notifications on the session bus
// (SessionBus.cpp). A tag maps to the server's id, to replace and close; the
// ActionInvoked signal ("default" for the notification itself, the button's
// index otherwise) comes back to the engine thread.

#include "config.h"
#include "desktop/Desktop.h"

#include "desktop/SessionBus.h"
#include <cstring>
#include <wtf/CrossThreadCopier.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace ZeroG::Notifications {

using DBus::Message;
using DBus::Reader;
using DBus::Writer;

namespace {

struct State {
    Lock lock;
    // Engine thread.
    Config config;
    bool configured { false };
    unsigned untagged { 0 };
    // Both threads.
    HashMap<String, uint32_t> ids WTF_GUARDED_BY_LOCK(lock); // tag → server id
};

State& state()
{
    static NeverDestroyed<State> instance;
    return instance;
}

String tagFor(uint32_t id)
{
    auto& s = state();
    Locker locker { s.lock };
    for (auto& [tag, serverID] : s.ids) {
        if (serverID == id)
            return tag.isolatedCopy();
    }
    return { };
}

void listen(SessionBus& bus)
{
    bus.addSignalHandler([&bus](Message* signal) {
        auto& library = bus.library();
        const char* interface = library.message_get_interface(signal);
        const char* member = library.message_get_member(signal);
        if (!interface || strcmp(interface, "org.freedesktop.Notifications") || !member)
            return;
        Reader reader(signal);
        uint32_t id = reader.uint32();
        auto tag = tagFor(id);
        if (tag.isEmpty())
            return;
        if (!strcmp(member, "ActionInvoked")) {
            reader.next();
            auto key = reader.string();
            int action = key == "default"_s ? -1 : parseInteger<int>(key).value_or(-1);
            callOnMainThread([tag = tag.isolatedCopy(), action] {
                if (auto& activated = state().config.activated)
                    activated(tag, action);
            });
        } else if (!strcmp(member, "NotificationClosed")) {
            auto& s = state();
            Locker locker { s.lock };
            s.ids.remove(tag);
        }
    });
    bus.post([&bus](DBus::Connection* connection) {
        bus.addMatch(connection, "type='signal',interface='org.freedesktop.Notifications'");
    });
}

} // namespace

Result configure(Config&& config, String& error)
{
    auto* bus = SessionBus::get();
    if (!bus) {
        error = "no session bus (or no libdbus): no notifications"_s;
        return Result::Unsupported;
    }
    auto& s = state();
    if (!s.configured)
        listen(*bus);
    s.config = WTF::move(config);
    s.configured = true;
    return Result::Ok;
}

Result show(const String& title, const String& body, const String& tag, const Vector<String>& actions, String& error)
{
    auto& s = state();
    auto* bus = SessionBus::get();
    if (!s.configured || !bus) {
        error = "the application did not say who it is (zg_runtime_set_notifications)"_s;
        return Result::Failed;
    }
    String key = tag.isEmpty() ? makeString("zerog-"_s, ++s.untagged) : tag;
    bus->post([bus, appName = s.config.appName.isolatedCopy(), appId = s.config.appId.isolatedCopy(), icon = s.config.iconPath.isolatedCopy(),
        title = title.isolatedCopy(), body = body.isolatedCopy(), key = key.isolatedCopy(), actions = crossThreadCopy(actions)](DBus::Connection* connection) {
        auto& library = bus->library();
        uint32_t replaces = 0;
        {
            Locker locker { state().lock };
            replaces = state().ids.get(key);
        }
        auto* call = library.message_new_method_call("org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify");
        Writer writer(call);
        writer.string(appName);
        writer.uint32(replaces);
        writer.string(icon);
        writer.string(title);
        writer.string(body);
        writer.container(DBus::TypeArray, "s", [&](Writer& array) {
            array.string("default");
            array.string("");
            for (size_t i = 0; i < actions.size(); ++i) {
                array.string(String::number(i));
                array.string(actions[i]);
            }
        });
        writer.container(DBus::TypeArray, "{sv}", [&](Writer& hints) {
            // The .desktop file the notification is from (the app id's).
            hints.container(DBus::TypeDictEntry, nullptr, [&](Writer& pair) {
                pair.string("desktop-entry");
                pair.container(DBus::TypeVariant, "s", [&](Writer& value) { value.string(appId); });
            });
        });
        writer.int32(-1);
        if (auto* reply = bus->call(connection, call, 5000)) {
            uint32_t id = Reader(reply).uint32();
            library.message_unref(reply);
            Locker locker { state().lock };
            state().ids.set(key, id);
        }
        library.message_unref(call);
    });
    return Result::Ok;
}

Result remove(const String& tag, String& error)
{
    auto* bus = SessionBus::get();
    if (!state().configured || !bus) {
        error = "the application did not say who it is (zg_runtime_set_notifications)"_s;
        return Result::Failed;
    }
    bus->post([bus, tag = tag.isolatedCopy()](DBus::Connection* connection) {
        uint32_t id = 0;
        {
            Locker locker { state().lock };
            id = state().ids.take(tag);
        }
        if (!id)
            return;
        auto& library = bus->library();
        auto* call = library.message_new_method_call("org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "CloseNotification");
        Writer(call).uint32(id);
        library.connection_send(connection, call, nullptr);
        library.message_unref(call);
    });
    return Result::Ok;
}

} // namespace ZeroG::Notifications
