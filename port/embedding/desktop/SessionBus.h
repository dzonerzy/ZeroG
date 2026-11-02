#pragma once

// The session bus for the desktop's services on Linux (the tray's
// StatusNotifierItem, org.freedesktop.Notifications, the GlobalShortcuts
// portal): a connection of its own on a thread of its own, as the AT-SPI
// bridge has its own (accessibility/AccessibilityAtspi.cpp), over libdbus
// loaded at run time. Objects and signal handlers run on the bus thread,
// over state they share with the engine thread under locks; what reaches
// the application goes to the engine thread (callOnMainThread).

#include "accessibility/DBus.h"
#include <memory>
#include <wtf/Function.h>
#include <wtf/Lock.h>
#include <wtf/Seconds.h>
#include <wtf/Vector.h>

namespace ZeroG {

class SessionBus {
    WTF_MAKE_NONCOPYABLE(SessionBus);
public:
    // Null without libdbus or a session bus (DBUS_SESSION_BUS_ADDRESS).
    static SessionBus* get();

    const DBus::Library& library() const { return *m_library; }

    // `task` on the bus thread, once connected (not at all if it cannot).
    void post(Function<void(DBus::Connection*)>&& task);
    // `task` on the bus thread, the calling thread waiting for its answer up
    // to `timeout`; false without one (no bus, or too late).
    bool postAndWait(Function<bool(DBus::Connection*)>&& task, Seconds timeout);

    // Method calls to `path` go to `handler` on the bus thread, which returns
    // the reply (null: an UnknownMethod error). From any thread.
    using ObjectHandler = Function<DBus::Message*(DBus::Message*)>;
    void addObject(const String& path, ObjectHandler&&);
    void removeObject(const String& path);

    // Every signal comes to every handler (each filters), on the bus thread.
    // Match rules (addMatch) say which reach the connection.
    using SignalHandler = Function<void(DBus::Message*)>;
    unsigned addSignalHandler(SignalHandler&&);
    void removeSignalHandler(unsigned);

    // Bus thread: a match rule, and the connection's unique name.
    void addMatch(DBus::Connection*, const char* rule);
    String uniqueName() const;

    // Bus thread: a blocking call (null: no reply, `error` says why).
    DBus::Message* call(DBus::Connection*, DBus::Message*, int timeoutMilliseconds, String* error = nullptr);

private:
    explicit SessionBus(const DBus::Library&);
    void run();
    void wakeUp();
    void dispatch(DBus::Connection*, DBus::Message*);
    static DBus::Bool addWatch(DBus::Watch*, void*);
    static void removeWatch(DBus::Watch*, void*);
    static void watchToggled(DBus::Watch*, void*);

    const DBus::Library* m_library;
    int m_wakeUpFD { -1 };

    mutable Lock m_lock;
    Vector<Function<void(DBus::Connection*)>> m_tasks WTF_GUARDED_BY_LOCK(m_lock);
    Vector<std::pair<String, std::shared_ptr<ObjectHandler>>> m_objects WTF_GUARDED_BY_LOCK(m_lock);
    Vector<std::pair<unsigned, std::shared_ptr<SignalHandler>>> m_signalHandlers WTF_GUARDED_BY_LOCK(m_lock);
    unsigned m_nextHandler WTF_GUARDED_BY_LOCK(m_lock) { 1 };
    String m_uniqueName WTF_GUARDED_BY_LOCK(m_lock);
    bool m_failed WTF_GUARDED_BY_LOCK(m_lock) { false };

    Lock m_watchLock;
    Vector<DBus::Watch*> m_watches WTF_GUARDED_BY_LOCK(m_watchLock);
};

} // namespace ZeroG
