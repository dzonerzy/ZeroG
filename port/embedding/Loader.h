#pragma once

// The resource loader (ZeroG.md, "Loader delle risorse"). WebKit's legacy
// ResourceHandle path is gone for curl ports, so loads go the way WebKit2's
// WebLoaderStrategy sends them, without the network process: data: URLs are
// decoded by WebCore itself, custom schemes (app://, the application's own) are
// answered by a SchemeHandler on the engine thread, http and https go to curl.
// Every callback into WebCore happens on the engine thread.

#include <WebCore/ResourceError.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/ResourceResponse.h>
#include <WebCore/SharedBuffer.h>
#include <memory>
#include <pal/SessionID.h>
#include <wtf/Deque.h>
#include <wtf/Function.h>
#include <wtf/HashMap.h>
#include <wtf/RefCounted.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

namespace WebCore {
class CookieJar;
class CookieJarDB;
class LoaderStrategy;
class LocalFrame;
class ResourceLoader;
}

namespace ZeroG {

class SchemeHandler;

// A load in flight, kept by the loader strategy until WebCore removes its loader.
class LoadTask : public RefCounted<LoadTask> {
public:
    virtual ~LoadTask() = default;
    virtual void start() = 0;
    virtual void stop() = 0;
};

// One load answered by a SchemeHandler. Calls arrive in order: a response, its
// data, then finish or fail. Once the page stops the load (navigation away,
// cancellation) every call is ignored, so a handler may answer late safely.
class SchemeTask final : public LoadTask {
public:
    static Ref<SchemeTask> create(WebCore::ResourceLoader&, SchemeHandler&);

    const WebCore::ResourceRequest& request() const { return m_request; }
    bool isStopped() const { return !m_loader; }

    void didReceiveResponse(WebCore::ResourceResponse&&);
    void didReceiveData(Ref<WebCore::SharedBuffer>&&);
    void didFinish();
    void didFail(const WebCore::ResourceError&);

    // Redirects within the scheme; once WebCore accepts it, the handler's start()
    // runs again with the new request.
    void redirect(const URL&, int status = 301);

    // Convenience: a complete response with the whole body.
    void respond(int status, const String& mimeType, Ref<WebCore::SharedBuffer>&& body, const Vector<std::pair<String, String>>& headers = { });

    // From the loader strategy.
    void start() final;
    void stop() final;

private:
    SchemeTask(WebCore::ResourceLoader&, SchemeHandler&);
    WebCore::ResourceLoader* loader();
    void runOrQueue(Function<void()>&&);
    void processNextTask();

    RefPtr<WebCore::ResourceLoader> m_loader;
    const Ref<SchemeHandler> m_handler;
    WebCore::ResourceRequest m_request;
    bool m_waitingForResponsePolicy { false };
    Deque<Function<void()>> m_queuedTasks;
};

// Answers the loads of one URL scheme. Thread-safe reference counting so the
// application may hold one from any thread; start and stop run on the engine
// thread.
class SchemeHandler : public ThreadSafeRefCounted<SchemeHandler> {
public:
    virtual ~SchemeHandler() = default;
    // May answer now or later; later answers must be made on the engine thread.
    virtual void start(SchemeTask&) = 0;
    // The page no longer wants the load.
    virtual void stop(SchemeTask&) { }
    // Synchronous loads (synchronous XMLHttpRequest). Handlers that can only
    // answer asynchronously keep the default, which fails the load.
    virtual bool loadSynchronously(const WebCore::ResourceRequest&, WebCore::ResourceResponse&, Vector<uint8_t>&) { return false; }
};

// Per-session loading state, one per zg_context: scheme handlers (pages of
// different sessions may answer the same scheme differently) and cookies.
class Session : public RefCounted<Session> {
public:
    // Cookies persist in an SQLite file at `cookieDatabasePath`; empty keeps them
    // in memory for the session's lifetime.
    static Ref<Session> create(const String& cookieDatabasePath = { }) { return adoptRef(*new Session(cookieDatabasePath)); }
    static Session& defaultSession();
    // Its resources leave WebCore's memory cache with it.
    ~Session();
    // The session's owner is gone (zg_context_destroy): drop the scheme handlers
    // and the session's cached resources now, though pages being torn down may
    // still hold the session itself.
    void close();

    // WebCore's session: its memory cache entries are the session's own, so two
    // sessions serving the same app:// URL never see each other's files.
    PAL::SessionID sessionID() const { return m_sessionID; }

    enum class RegisterResult : uint8_t { Registered, InvalidScheme, BuiltinScheme, AlreadyRegistered };
    // The scheme is registered with WebCore (process-wide) as secure, CORS-enabled
    // and handled by a scheme handler.
    RegisterResult registerScheme(const String& scheme, Ref<SchemeHandler>&&);
    SchemeHandler* schemeHandler(StringView scheme) const;
    bool hasScheme(StringView scheme) const { return schemeHandler(scheme); }

    // Cookies, as WebKit's curl network session keeps them.
    WebCore::CookieJarDB& cookieDatabase();
    // The Cookie header for a request (HttpOnly cookies included), and the
    // Set-Cookie headers among a response's raw header lines.
    String cookieHeader(const URL& firstParty, const URL&);
    void storeCookies(const URL& firstParty, const URL&, const Vector<String>& headerLines);

private:
    explicit Session(const String& cookieDatabasePath);
    const PAL::SessionID m_sessionID;
    HashMap<String, Ref<SchemeHandler>> m_schemeHandlers;
    std::unique_ptr<WebCore::CookieJarDB> m_cookieDatabase;
};

// The cookie jar of a page in a session: document.cookie and the DOM's cookie APIs.
Ref<WebCore::CookieJar> createCookieJar(Session&);

// The session of the view a frame belongs to.
Session& sessionForFrame(WebCore::LocalFrame*);

// http and https over curl (LoaderCurl.cpp).
Ref<LoadTask> createNetworkTask(WebCore::ResourceLoader&);
bool loadNetworkResourceSynchronously(Session&, const WebCore::ResourceRequest&, WebCore::ResourceError&, WebCore::ResourceResponse&, Vector<uint8_t>&);

// Errors handed to WebCore, in WebKit's domains so its loader classifies them.
WebCore::ResourceError cancelledError(const URL&);
WebCore::ResourceError cannotShowURLError(const URL&);
WebCore::ResourceError blockedError(const URL&);
WebCore::ResourceError notFoundError(const URL&);
WebCore::ResourceError failedError(const URL&, const String& description);

// The loader strategy (PlatformStrategiesZeroG).
WebCore::LoaderStrategy* createLoaderStrategy();

} // namespace ZeroG
