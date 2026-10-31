#pragma once

// The resource loader (ZeroG.md, "Loader delle risorse"). WebKit's legacy
// ResourceHandle path is gone for curl ports, so loads go the way WebKit2's
// WebLoaderStrategy sends them, without the network process: data: URLs are
// decoded by WebCore itself, custom schemes (app://, the application's own) are
// answered by a SchemeHandler on the engine thread, http and https go to curl.
// Every callback into WebCore happens on the engine thread.

#include "Download.h"
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/ResourceResponse.h>
#include <WebCore/SharedBuffer.h>
#include <expected>
#include <memory>
#include <pal/SessionID.h>
#include <span>
#include <wtf/Deque.h>
#include <wtf/Function.h>
#include <wtf/HashMap.h>
#include <wtf/RefCounted.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

namespace WebCore {
class BlobRegistry;
class CacheStorageProvider;
class CookieJar;
class CookieJarDB;
class CredentialStorage;
class DatabaseProvider;
class LoaderStrategy;
class StorageNamespaceProvider;
class LocalFrame;
class NetworkLoadMetrics;
class ResourceLoader;
class UserContentProvider;
}

namespace ZeroG {

class Download;
class HTTPCache;
class SchemeHandler;
class ServiceWorkerSession;
class View;

// A load in flight, kept by the loader strategy until WebCore removes its loader.
class LoadTask : public RefCounted<LoadTask> {
public:
    virtual ~LoadTask() = default;
    virtual void start() = 0;
    virtual void stop() = 0;
    // The page's load becomes a download: the rest of the response goes to it.
    virtual bool convertToDownload(Download&) = 0;
};

// A load for the engine itself (a service worker's script update, a navigation
// preload), not for a page. Calls arrive as a page's would.
class LoadClient : public RefCounted<LoadClient> {
public:
    virtual ~LoadClient() = default;
    // `continueLoad(true)` for the body.
    virtual void didReceiveResponse(const WebCore::ResourceResponse&, CompletionHandler<void(bool)>&& continueLoad) = 0;
    // A redirect: the request to follow, or a null one, after which nothing
    // more comes (the client ends the load itself).
    virtual void willSendRequest(WebCore::ResourceRequest&&, const WebCore::ResourceResponse& redirectResponse, CompletionHandler<void(WebCore::ResourceRequest&&)>&&) = 0;
    virtual void didReceiveData(Ref<WebCore::SharedBuffer>&&) = 0;
    virtual void didFinish() = 0;
    virtual void didFail(const WebCore::ResourceError&) = 0;
};

// Where a task's results go: the page's ResourceLoader, a Download (from the
// start, or after the page's load was converted), or the engine's own client.
class LoadTarget {
public:
    LoadTarget() = default;
    explicit LoadTarget(WebCore::ResourceLoader&);
    explicit LoadTarget(Download&);
    explicit LoadTarget(LoadClient&);
    ~LoadTarget();

    // Null once WebCore's loader reached its end.
    WebCore::ResourceLoader* loader();
    Download* download() const { return m_download.get(); }
    bool isActive() { return loader() || m_download || m_client; }
    void clear();
    void convertToDownload(Download&);

    // `continueLoad(true)` once the body may come; false: the target refused it
    // (a download without a destination) and the task must stop.
    void didReceiveResponse(WebCore::ResourceResponse&&, CompletionHandler<void(bool)>&& continueLoad);
    void willSendRequest(WebCore::ResourceRequest&&, const WebCore::ResourceResponse& redirectResponse, CompletionHandler<void(WebCore::ResourceRequest&&)>&&);
    void didSendData(unsigned long long bytesSent, unsigned long long totalBytesToBeSent);
    void didReceiveData(Ref<WebCore::SharedBuffer>&&);
    void didFinish(const WebCore::NetworkLoadMetrics&);
    void didFail(const WebCore::ResourceError&);

private:
    RefPtr<WebCore::ResourceLoader> m_loader;
    RefPtr<Download> m_download;
    RefPtr<LoadClient> m_client;
};

// One load answered by a SchemeHandler. Calls arrive in order: a response, its
// data, then finish or fail. Once the page stops the load (navigation away,
// cancellation) every call is ignored, so a handler may answer late safely.
class SchemeTask final : public LoadTask {
public:
    static Ref<SchemeTask> create(WebCore::ResourceLoader&, SchemeHandler&);
    // A download from the scheme (Download.h).
    static Ref<SchemeTask> create(Download&, const WebCore::ResourceRequest&, SchemeHandler&);

    const WebCore::ResourceRequest& request() const { return m_request; }
    bool isStopped() { return !m_target.isActive(); }

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
    bool convertToDownload(Download&) final;

private:
    SchemeTask(LoadTarget&&, const WebCore::ResourceRequest&, SchemeHandler&);
    void runOrQueue(Function<void()>&&);
    void processNextTask();

    LoadTarget m_target;
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
    // Cookies persist in an SQLite file at `cookieDatabasePath`; local storage,
    // IndexedDB and the HTTP cache under `storageDirectory` (LocalStorage/,
    // IndexedDB/, HTTPCache/). Empty keeps them in memory for the session's
    // lifetime, with no HTTP cache. A zero capacity turns the HTTP cache off.
    static constexpr uint64_t defaultHTTPCacheCapacity = 64 * 1024 * 1024;
    static Ref<Session> create(const String& cookieDatabasePath = { }, const String& storageDirectory = { }, uint64_t httpCacheCapacity = defaultHTTPCacheCapacity)
    {
        return adoptRef(*new Session(cookieDatabasePath, storageDirectory, httpCacheCapacity));
    }
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

    // Fonts the application registers (Fonts.cpp): every page of the session
    // sees their families, ahead of system fonts of the same name, as @font-face
    // rules of a user style sheet. Returns the family name.
    std::expected<String, String> registerFont(std::span<const uint8_t>);
    // The pages' user content: the registered fonts' style sheet.
    WebCore::UserContentProvider& userContentProvider();

    // HTTP credentials the application gave for the session's protection spaces;
    // in memory only, for the session's lifetime.
    WebCore::CredentialStorage& credentials();

    // Cookies, as WebKit's curl network session keeps them.
    WebCore::CookieJarDB& cookieDatabase();
    // The Cookie header for a request (HttpOnly cookies included), and the
    // Set-Cookie headers among a response's raw header lines.
    String cookieHeader(const URL& firstParty, const URL&);
    void storeCookies(const URL& firstParty, const URL&, const Vector<String>& headerLines);

    // Web storage and IndexedDB of the session's pages (port/embedding/storage).
    WebCore::StorageNamespaceProvider& storageNamespaceProvider();
    WebCore::DatabaseProvider& databaseProvider();
    // The Cache API's storage (storage/CacheStorageZeroG.h), under CacheStorage/.
    WebCore::CacheStorageProvider& cacheStorageProvider();
    // The HTTP disk cache (HTTPCache.h); null without a directory.
    HTTPCache* httpCache() const { return m_httpCache.get(); }
    // Service workers (serviceworkers/ServiceWorkersZeroG.h), registrations
    // under ServiceWorkers/; null in the minimal profile and once closed.
    ServiceWorkerSession* serviceWorkers();

private:
    Session(const String& cookieDatabasePath, const String& storageDirectory, uint64_t httpCacheCapacity);
    const PAL::SessionID m_sessionID;
    const String m_storageDirectory;
    const RefPtr<HTTPCache> m_httpCache;
    RefPtr<WebCore::StorageNamespaceProvider> m_storageNamespaceProvider;
    RefPtr<WebCore::DatabaseProvider> m_databaseProvider;
    RefPtr<WebCore::CacheStorageProvider> m_cacheStorageProvider;
    RefPtr<ServiceWorkerSession> m_serviceWorkers;
    bool m_isClosed { false };
    HashMap<String, Ref<SchemeHandler>> m_schemeHandlers;
    std::unique_ptr<WebCore::CookieJarDB> m_cookieDatabase;
    RefPtr<WebCore::UserContentProvider> m_userContentProvider;
    std::unique_ptr<WebCore::CredentialStorage> m_credentials;
};

// The cookie jar of a page in a session: document.cookie and the DOM's cookie APIs.
Ref<WebCore::CookieJar> createCookieJar(Session&);

// The session of the view a frame belongs to.
Session& sessionForFrame(WebCore::LocalFrame*);

// http and https over curl (LoaderCurl.cpp). Certificates are set up once,
// before the first TLS connection (WebSockets included).
void initializeCertificates();
// Root certificates trusted on top of the system's (PEM, any number); returns how
// many were read. Process-wide, thread-safe.
size_t addTrustedCertificates(std::span<const uint8_t> pem);
Ref<LoadTask> createNetworkTask(WebCore::ResourceLoader&);
Ref<LoadTask> createNetworkTask(Download&, Session&, const WebCore::ResourceRequest&);
Ref<LoadTask> createNetworkTask(LoadClient&, Session&, const WebCore::ResourceRequest&);
bool loadNetworkResourceSynchronously(Session&, const WebCore::ResourceRequest&, WebCore::ResourceError&, WebCore::ResourceResponse&, Vector<uint8_t>&);

// Errors handed to WebCore, in WebKit's domains so its loader classifies them.
WebCore::ResourceError cancelledError(const URL&);
WebCore::ResourceError cannotShowURLError(const URL&);
WebCore::ResourceError blockedError(const URL&);
WebCore::ResourceError notFoundError(const URL&);
WebCore::ResourceError failedError(const URL&, const String& description);
// A load WebCore stopped for a policy decision (it became a download, for example).
bool isInterruptedForPolicyChangeError(const WebCore::ResourceError&);

// The loader strategy (PlatformStrategiesZeroG).
WebCore::LoaderStrategy* createLoaderStrategy();

// Blobs (LoaderBlob.cpp): the process's blob registry, and loads of blob: URLs.
WebCore::BlobRegistry* createBlobRegistry();
Ref<LoadTask> createBlobTask(WebCore::ResourceLoader&);
Ref<LoadTask> createBlobTask(Download&, const WebCore::ResourceRequest&, WebCore::LocalFrame*);

// Downloads (Download.h): a page's load becomes one, or one starts anew.
bool convertLoadToDownload(WebCore::ResourceLoader&, Download&);
void startDownload(View&, const WebCore::ResourceRequest&, const String& suggestedFilename = { });
bool loadBlobSynchronously(WebCore::LocalFrame*, const WebCore::ResourceRequest&, WebCore::ResourceError&, WebCore::ResourceResponse&, Vector<uint8_t>&);

} // namespace ZeroG
