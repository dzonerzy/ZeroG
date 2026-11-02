#include "config.h"
#include "desktop/SessionBus.h"

#include <cstdlib>
#include <memory>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <wtf/Condition.h>
#include <wtf/Threading.h>

namespace ZeroG {

SessionBus* SessionBus::get()
{
    static SessionBus* bus = []() -> SessionBus* {
        const char* address = getenv("DBUS_SESSION_BUS_ADDRESS");
        if (!address || !*address)
            return nullptr;
        auto* library = DBus::Library::get();
        if (!library)
            return nullptr;
        auto* bus = new SessionBus(*library);
        Thread::create("ZeroG session bus"_s, [bus] {
            bus->run();
        })->detach();
        return bus;
    }();
    return bus;
}

SessionBus::SessionBus(const DBus::Library& library)
    : m_library(&library)
    , m_wakeUpFD(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK))
{
}

void SessionBus::post(Function<void(DBus::Connection*)>&& task)
{
    {
        Locker locker { m_lock };
        if (m_failed)
            return;
        m_tasks.append(WTF::move(task));
    }
    wakeUp();
}

bool SessionBus::postAndWait(Function<bool(DBus::Connection*)>&& task, Seconds timeout)
{
    struct Answer {
        Lock lock;
        Condition condition;
        bool done WTF_GUARDED_BY_LOCK(lock) { false };
        bool result WTF_GUARDED_BY_LOCK(lock) { false };
    };
    auto answer = std::make_shared<Answer>();
    post([answer, task = WTF::move(task)](DBus::Connection* connection) {
        bool result = task(connection);
        Locker locker { answer->lock };
        answer->result = result;
        answer->done = true;
        answer->condition.notifyOne();
    });
    Locker locker { answer->lock };
    auto deadline = MonotonicTime::now() + timeout;
    while (!answer->done && MonotonicTime::now() < deadline)
        answer->condition.waitUntil(answer->lock, deadline);
    return answer->done && answer->result;
}

void SessionBus::addObject(const String& path, ObjectHandler&& handler)
{
    Locker locker { m_lock };
    m_objects.append({ path.isolatedCopy(), std::make_shared<ObjectHandler>(WTF::move(handler)) });
}

void SessionBus::removeObject(const String& path)
{
    Locker locker { m_lock };
    m_objects.removeFirstMatching([&](auto& object) {
        return object.first == path;
    });
}

unsigned SessionBus::addSignalHandler(SignalHandler&& handler)
{
    Locker locker { m_lock };
    unsigned id = m_nextHandler++;
    m_signalHandlers.append({ id, std::make_shared<SignalHandler>(WTF::move(handler)) });
    return id;
}

void SessionBus::removeSignalHandler(unsigned id)
{
    Locker locker { m_lock };
    m_signalHandlers.removeFirstMatching([id](auto& handler) {
        return handler.first == id;
    });
}

void SessionBus::addMatch(DBus::Connection* connection, const char* rule)
{
    DBus::Error error;
    m_library->error_init(&error);
    m_library->bus_add_match(connection, rule, &error);
    m_library->error_free(&error);
}

String SessionBus::uniqueName() const
{
    Locker locker { m_lock };
    return m_uniqueName.isolatedCopy();
}

DBus::Message* SessionBus::call(DBus::Connection* connection, DBus::Message* message, int timeoutMilliseconds, String* errorText)
{
    DBus::Error error;
    m_library->error_init(&error);
    auto* reply = m_library->connection_send_with_reply_and_block(connection, message, timeoutMilliseconds, &error);
    if (!reply && errorText)
        *errorText = String::fromUTF8(error.name ? error.name : "no reply");
    m_library->error_free(&error);
    return reply;
}

void SessionBus::wakeUp()
{
    uint64_t one = 1;
    [[maybe_unused]] auto written = write(m_wakeUpFD, &one, sizeof(one));
}

DBus::Bool SessionBus::addWatch(DBus::Watch* watch, void* data)
{
    auto& bus = *static_cast<SessionBus*>(data);
    {
        Locker locker { bus.m_watchLock };
        bus.m_watches.append(watch);
    }
    bus.wakeUp();
    return true;
}

void SessionBus::removeWatch(DBus::Watch* watch, void* data)
{
    auto& bus = *static_cast<SessionBus*>(data);
    {
        Locker locker { bus.m_watchLock };
        bus.m_watches.removeFirst(watch);
    }
    bus.wakeUp();
}

void SessionBus::watchToggled(DBus::Watch*, void* data)
{
    static_cast<SessionBus*>(data)->wakeUp();
}

void SessionBus::dispatch(DBus::Connection* connection, DBus::Message* message)
{
    auto& library = *m_library;
    int type = library.message_get_type(message);
    if (type == DBus::MethodCall) {
        std::shared_ptr<ObjectHandler> handler;
        {
            Locker locker { m_lock };
            const char* path = library.message_get_path(message);
            for (auto& object : m_objects) {
                if (path && object.first == String::fromUTF8(path)) {
                    handler = object.second;
                    break;
                }
            }
        }
        auto* reply = handler ? (*handler)(message) : nullptr;
        if (!reply)
            reply = library.message_new_error(message, "org.freedesktop.DBus.Error.UnknownMethod", "no such object or method");
        library.connection_send(connection, reply, nullptr);
        library.message_unref(reply);
    } else if (type == DBus::Signal) {
        Vector<std::shared_ptr<SignalHandler>> handlers;
        {
            Locker locker { m_lock };
            for (auto& handler : m_signalHandlers)
                handlers.append(handler.second);
        }
        for (auto& handler : handlers)
            (*handler)(message);
    }
}

void SessionBus::run()
{
    auto& library = *m_library;
    DBus::Error error;
    library.error_init(&error);
    auto* connection = m_wakeUpFD >= 0 ? library.bus_get_private(DBus::SessionBus, &error) : nullptr;
    library.error_free(&error);
    if (!connection) {
        Locker locker { m_lock };
        m_failed = true;
        m_tasks.clear();
        return;
    }
    library.connection_set_exit_on_disconnect(connection, false);
    {
        Locker locker { m_lock };
        m_uniqueName = String::fromUTF8(library.bus_get_unique_name(connection));
    }
    library.connection_set_watch_functions(connection, addWatch, removeWatch, watchToggled, this, nullptr);

    auto runQueued = [&] {
        for (;;) {
            Vector<Function<void(DBus::Connection*)>> tasks;
            {
                Locker locker { m_lock };
                tasks = std::exchange(m_tasks, { });
            }
            if (tasks.isEmpty())
                break;
            for (auto& task : tasks)
                task(connection);
        }
        while (auto* message = library.connection_pop_message(connection)) {
            dispatch(connection, message);
            library.message_unref(message);
        }
        library.connection_flush(connection);
    };

    while (library.connection_get_is_connected(connection)) {
        runQueued();
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
            continue;
        if (descriptors.last().revents & POLLIN) {
            uint64_t count;
            [[maybe_unused]] auto readBytes = read(m_wakeUpFD, &count, sizeof(count));
        }
        for (size_t i = 0; i < polled.size(); ++i) {
            auto revents = descriptors[i].revents;
            if (!revents)
                continue;
            {
                Locker locker { m_watchLock };
                if (!m_watches.contains(polled[i]))
                    continue;
            }
            unsigned flags = (revents & POLLIN ? DBus::WatchReadable : 0) | (revents & POLLOUT ? DBus::WatchWritable : 0)
                | (revents & POLLERR ? DBus::WatchError : 0) | (revents & POLLHUP ? DBus::WatchHangup : 0);
            library.watch_handle(polled[i], flags);
        }
    }
    Locker locker { m_lock };
    m_failed = true;
    m_tasks.clear();
}

} // namespace ZeroG
