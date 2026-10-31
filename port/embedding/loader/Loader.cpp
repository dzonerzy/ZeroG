/*
 * Copyright (C) 2010-2025 Apple Inc. All rights reserved.
 * Copyright (C) 2017 Igalia S.L.
 * Copyright (C) 2018-2020 Sony Interactive Entertainment Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

// Derived, at the pinned WebKit commit, from WebKit's WebLoaderStrategy (load
// dispatch), WebURLSchemeTaskProxy (scheme tasks), WebErrors (error domains and
// codes) and NetworkStorageSessionCurl (cookies) (docs/PROVENANCE.md).

#include "config.h"
#include "Loader.h"

#include "Download.h"
#include "HTTPCache.h"
#include "View.h"
#include "serviceworkers/ServiceWorkersZeroG.h"
#include "storage/CacheStorageZeroG.h"
#include "storage/DatabaseProviderZeroG.h"
#include "storage/WebStorageNamespaceProvider.h"
#include <wtf/FileSystem.h>
#include <WebCore/Cookie.h>
#include <WebCore/CookieJar.h>
#include <WebCore/CookieJarDB.h>
#include <WebCore/CredentialStorage.h>
#include <WebCore/DataURLDecoder.h>
#include <WebCore/Document.h>
#include <WebCore/StorageSessionProvider.h>
#include <WebCore/DocumentPage.h>
#include <WebCore/FrameLoader.h>
#include <WebCore/HTTPHeaderNames.h>
#include <WebCore/HTTPParsers.h>
#include <WebCore/LegacySchemeRegistry.h>
#include <WebCore/LoaderStrategy.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/MemoryCache.h>
#include <WebCore/NetworkLoadMetrics.h>
#include <WebCore/Page.h>
#include <WebCore/ResourceLoader.h>
#include <WebCore/SubresourceLoader.h>
#include <WebCore/UserContentProvider.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>

namespace ZeroG {

using namespace WebCore;

// ---- Errors ----

// WebKit's policy error domain and codes (WebKit's WebErrors.cpp), which
// WebCore's loader and the inspector recognize.
static constexpr auto policyErrorDomain = "WebKitErrorDomain"_s;
enum PolicyError : int {
    CannotShowMIMEType = 100,
    CannotShowURL = 101,
    FrameLoadInterruptedByPolicyChange = 102,
    CannotUseRestrictedPort = 103,
    PlugInWillHandleLoad = 204,
};

ResourceError cancelledError(const URL& url)
{
    return { errorDomainWebKitInternal, 0, url, "Load cancelled"_s, ResourceError::Type::Cancellation };
}

ResourceError cannotShowURLError(const URL& url)
{
    return { policyErrorDomain, CannotShowURL, url, "The URL can't be shown"_s };
}

ResourceError blockedError(const URL& url)
{
    return { policyErrorDomain, CannotUseRestrictedPort, url, "Not allowed to use restricted network port"_s, ResourceError::Type::AccessControl };
}

ResourceError notFoundError(const URL& url)
{
    return { errorDomainWebKitNetwork, 404, url, "Resource not found"_s };
}

ResourceError failedError(const URL& url, const String& description)
{
    return { errorDomainWebKitNetwork, 0, url, description };
}

bool isInterruptedForPolicyChangeError(const ResourceError& error)
{
    return error.domain() == policyErrorDomain && error.errorCode() == FrameLoadInterruptedByPolicyChange;
}

// ---- Load targets ----

LoadTarget::LoadTarget(ResourceLoader& loader)
    : m_loader(&loader)
{
}

LoadTarget::LoadTarget(Download& download)
    : m_download(&download)
{
}

LoadTarget::~LoadTarget() = default;

ResourceLoader* LoadTarget::loader()
{
    if (m_loader && m_loader->reachedTerminalState())
        m_loader = nullptr;
    return m_loader.get();
}

void LoadTarget::clear()
{
    m_loader = nullptr;
    m_download = nullptr;
}

void LoadTarget::convertToDownload(Download& download)
{
    m_loader = nullptr;
    m_download = &download;
}

void LoadTarget::didReceiveResponse(ResourceResponse&& response, CompletionHandler<void(bool)>&& continueLoad)
{
    if (RefPtr loader = this->loader()) {
        loader->didReceiveResponse(WTF::move(response), [continueLoad = WTF::move(continueLoad)] mutable {
            continueLoad(true);
        });
        return;
    }
    if (RefPtr download = m_download) {
        bool accepted = download->start(response);
        if (!accepted)
            m_download = nullptr;
        continueLoad(accepted);
        return;
    }
    continueLoad(false);
}

void LoadTarget::willSendRequest(ResourceRequest&& request, const ResourceResponse& redirectResponse, CompletionHandler<void(ResourceRequest&&)>&& completionHandler)
{
    if (RefPtr loader = this->loader()) {
        loader->willSendRequest(WTF::move(request), redirectResponse, WTF::move(completionHandler));
        return;
    }
    // A download follows its redirects.
    completionHandler(m_download ? WTF::move(request) : ResourceRequest { });
}

void LoadTarget::didSendData(unsigned long long bytesSent, unsigned long long totalBytesToBeSent)
{
    if (RefPtr loader = this->loader())
        loader->didSendData(bytesSent, totalBytesToBeSent);
}

void LoadTarget::didReceiveData(Ref<SharedBuffer>&& data)
{
    if (RefPtr loader = this->loader())
        loader->didReceiveData(data, data->size(), DataPayloadType::DataPayloadBytes);
    else if (RefPtr download = m_download)
        download->didReceiveData(data->span());
}

// The target is forgotten first: finishing removes the loader from the strategy,
// which stops its task.
void LoadTarget::didFinish(const NetworkLoadMetrics& metrics)
{
    RefPtr loader = this->loader();
    RefPtr download = m_download;
    clear();
    if (loader)
        loader->didFinishLoading(metrics);
    else if (download)
        download->didFinish();
}

void LoadTarget::didFail(const ResourceError& error)
{
    RefPtr loader = this->loader();
    RefPtr download = m_download;
    clear();
    if (loader)
        loader->didFail(error);
    else if (download)
        download->didFail(error);
}

// ---- Scheme tasks ----

Ref<SchemeTask> SchemeTask::create(ResourceLoader& loader, SchemeHandler& handler)
{
    return adoptRef(*new SchemeTask(LoadTarget { loader }, loader.request(), handler));
}

Ref<SchemeTask> SchemeTask::create(Download& download, const ResourceRequest& request, SchemeHandler& handler)
{
    return adoptRef(*new SchemeTask(LoadTarget { download }, request, handler));
}

SchemeTask::SchemeTask(LoadTarget&& target, const ResourceRequest& request, SchemeHandler& handler)
    : m_target(WTF::move(target))
    , m_handler(handler)
    , m_request(request)
{
}

void SchemeTask::start()
{
    if (!m_target.isActive())
        return;
    Ref protectedThis { *this };
    m_handler->start(*this);
}

void SchemeTask::stop()
{
    if (!m_target.isActive())
        return;
    m_target.clear();
    m_queuedTasks.clear();
    Ref protectedThis { *this };
    m_handler->stop(*this);
}

bool SchemeTask::convertToDownload(Download& download)
{
    if (!m_target.isActive())
        return false;
    m_target.convertToDownload(download);
    // The page's response policy will not complete for it any more.
    m_waitingForResponsePolicy = false;
    processNextTask();
    return true;
}

// While WebCore decides what to do with a response, data and completion wait,
// as in WebKit's WebURLSchemeTaskProxy.
void SchemeTask::runOrQueue(Function<void()>&& task)
{
    if (m_waitingForResponsePolicy) {
        m_queuedTasks.append(WTF::move(task));
        return;
    }
    task();
}

void SchemeTask::processNextTask()
{
    while (!m_waitingForResponsePolicy && !m_queuedTasks.isEmpty())
        m_queuedTasks.takeFirst()();
}

void SchemeTask::didReceiveResponse(ResourceResponse&& response)
{
    runOrQueue([this, protectedThis = Ref { *this }, response = WTF::move(response)] mutable {
        if (!m_target.isActive())
            return;
        m_waitingForResponsePolicy = true;
        m_target.didReceiveResponse(WTF::move(response), [this, protectedThis = Ref { *this }](bool accepted) {
            m_waitingForResponsePolicy = false;
            if (!accepted) {
                stop();
                return;
            }
            processNextTask();
        });
    });
}

void SchemeTask::didReceiveData(Ref<SharedBuffer>&& data)
{
    runOrQueue([this, protectedThis = Ref { *this }, data = WTF::move(data)] mutable {
        m_target.didReceiveData(WTF::move(data));
    });
}

void SchemeTask::didFinish()
{
    runOrQueue([this, protectedThis = Ref { *this }] {
        m_target.didFinish(NetworkLoadMetrics { });
    });
}

void SchemeTask::didFail(const ResourceError& error)
{
    runOrQueue([this, protectedThis = Ref { *this }, error] {
        m_target.didFail(error);
    });
}

static ASCIILiteral statusText(int);

void SchemeTask::redirect(const URL& location, int status)
{
    runOrQueue([this, protectedThis = Ref { *this }, location, status] {
        if (!m_target.isActive())
            return;
        if (location.protocol() != m_request.url().protocol()) {
            m_target.didFail(ZeroG::cannotShowURLError(location));
            return;
        }
        ResourceResponse response { URL { m_request.url() }, String { }, 0, String { } };
        response.setHTTPStatusCode(status);
        response.setHTTPStatusText(statusText(status));
        response.setHTTPHeaderField(HTTPHeaderName::Location, location.string());
        auto request = m_request;
        request.setURL(URL { location });
        m_waitingForResponsePolicy = true;
        m_target.willSendRequest(WTF::move(request), response, [this, protectedThis = Ref { *this }](ResourceRequest&& newRequest) {
            m_waitingForResponsePolicy = false;
            m_queuedTasks.clear();
            // A null request: WebCore refused the redirect and cancels the load.
            if (newRequest.isNull() || !m_target.isActive())
                return;
            m_request = WTF::move(newRequest);
            m_handler->start(*this);
        });
    });
}

static ASCIILiteral statusText(int status)
{
    switch (status) {
    case 200: return "OK"_s;
    case 204: return "No Content"_s;
    case 206: return "Partial Content"_s;
    case 301: return "Moved Permanently"_s;
    case 302: return "Found"_s;
    case 304: return "Not Modified"_s;
    case 400: return "Bad Request"_s;
    case 403: return "Forbidden"_s;
    case 404: return "Not Found"_s;
    case 405: return "Method Not Allowed"_s;
    case 416: return "Range Not Satisfiable"_s;
    case 500: return "Internal Server Error"_s;
    default: return ""_s;
    }
}

void SchemeTask::respond(int status, const String& mediaType, Ref<SharedBuffer>&& body, const Vector<std::pair<String, String>>& headers)
{
    auto mimeType = extractMIMETypeFromMediaType(mediaType);
    auto charset = extractCharsetFromMediaType(mediaType).toString();
    ResourceResponse response { URL { m_request.url() }, WTF::move(mimeType), static_cast<long long>(body->size()), WTF::move(charset) };
    response.setHTTPStatusCode(status);
    response.setHTTPStatusText(statusText(status));
    if (!mediaType.isEmpty())
        response.setHTTPHeaderField(HTTPHeaderName::ContentType, mediaType);
    response.setHTTPHeaderField(HTTPHeaderName::ContentLength, String::number(body->size()));
    for (auto& [name, value] : headers)
        response.setHTTPHeaderField(name, value);
    didReceiveResponse(WTF::move(response));
    if (body->size())
        didReceiveData(WTF::move(body));
    didFinish();
}

// ---- Sessions ----

static bool s_creatingDefaultSession;

Session& Session::defaultSession()
{
    static NeverDestroyed<Ref<Session>> session { [] {
        s_creatingDefaultSession = true;
        auto session = Session::create();
        s_creatingDefaultSession = false;
        return session;
    }() };
    return session.get();
}

// Never WebKit's ephemeral IDs: they mean private browsing (no console output,
// among others). What a session keeps on disk is decided here, by its cookie
// database.
static PAL::SessionID newSessionID()
{
    if (s_creatingDefaultSession)
        return PAL::SessionID::defaultSessionID();
    return PAL::SessionID::generatePersistentSessionID();
}

Session::Session(const String& cookieDatabasePath, const String& storageDirectory, uint64_t httpCacheCapacity)
    : m_sessionID(newSessionID())
    , m_storageDirectory(storageDirectory)
    , m_httpCache(storageDirectory.isEmpty() || !httpCacheCapacity ? nullptr
        : RefPtr { HTTPCache::create(FileSystem::pathByAppendingComponent(storageDirectory, "HTTPCache"_s), httpCacheCapacity) })
    // ":memory:" is SQLite's in-memory database, as WebKit's ephemeral sessions use.
    , m_cookieDatabase(makeUnique<CookieJarDB>(cookieDatabasePath.isEmpty() ? ":memory:"_s : cookieDatabasePath))
{
}

// Made on first use: a session whose pages never touch storage opens no files.
StorageNamespaceProvider& Session::storageNamespaceProvider()
{
    if (!m_storageNamespaceProvider) {
        m_storageNamespaceProvider = WebKit::WebStorageNamespaceProvider::create(m_storageDirectory.isEmpty()
            ? String() : FileSystem::pathByAppendingComponent(m_storageDirectory, "LocalStorage"_s));
    }
    return *m_storageNamespaceProvider;
}

CacheStorageProvider& Session::cacheStorageProvider()
{
    if (!m_cacheStorageProvider) {
        m_cacheStorageProvider = createCacheStorageProvider(m_storageDirectory.isEmpty()
            ? String() : FileSystem::pathByAppendingComponent(m_storageDirectory, "CacheStorage"_s));
    }
    return *m_cacheStorageProvider;
}

DatabaseProvider& Session::databaseProvider()
{
    if (!m_databaseProvider) {
        m_databaseProvider = DatabaseProviderZeroG::create(m_storageDirectory.isEmpty()
            ? String() : FileSystem::pathByAppendingComponent(m_storageDirectory, "IndexedDB"_s));
    }
    return *m_databaseProvider;
}

ServiceWorkerSession* Session::serviceWorkers()
{
#if defined(ZEROG_FULL_WEB)
    if (!m_serviceWorkers && !m_isClosed) {
        m_serviceWorkers = ServiceWorkerSession::create(*this, m_storageDirectory.isEmpty()
            ? String() : FileSystem::pathByAppendingComponent(m_storageDirectory, "ServiceWorkers"_s));
    }
#endif
    return m_serviceWorkers.get();
}

Session::~Session()
{
    // Cached responses keep their files alive; a session's go with it.
    MemoryCache::singleton().evictResources(m_sessionID);
}

CredentialStorage& Session::credentials()
{
    if (!m_credentials)
        m_credentials = std::make_unique<CredentialStorage>();
    return *m_credentials;
}

void Session::close()
{
    m_isClosed = true;
    // The workers hold the session; they stop and let it go.
    if (RefPtr serviceWorkers = std::exchange(m_serviceWorkers, nullptr))
        serviceWorkers->close();
    m_schemeHandlers.clear();
    MemoryCache::singleton().evictResources(m_sessionID);
}

CookieJarDB& Session::cookieDatabase()
{
    m_cookieDatabase->open();
    return *m_cookieDatabase;
}

// As WebKit's NetworkStorageSessionCurl: name=value pairs for the URL, HttpOnly
// ones only for HTTP, secure ones only over secure transports.
static String cookieString(CookieJarDB& database, const URL& firstParty, const URL& url, bool forHTTPHeader)
{
    auto httpOnly = forHTTPHeader ? std::nullopt : std::optional<bool> { false };
    auto secure = CookieJar::shouldIncludeSecureCookies(url) == IncludeSecureCookies::Yes ? std::nullopt : std::optional<bool> { false };
    StringBuilder cookies;
    if (auto result = database.searchCookies(firstParty, url, httpOnly, secure, std::nullopt)) {
        for (auto& cookie : *result) {
            if (!cookies.isEmpty())
                cookies.append("; "_s);
            if (!cookie.name.isEmpty())
                cookies.append(cookie.name, '=');
            cookies.append(cookie.value);
        }
    }
    return cookies.toString();
}

String Session::cookieHeader(const URL& firstParty, const URL& url)
{
    return cookieString(cookieDatabase(), firstParty, url, true);
}

void Session::storeCookies(const URL& firstParty, const URL& url, const Vector<String>& headerLines)
{
    // Each Set-Cookie header is one cookie. ResourceResponse joins repeated
    // headers with commas, which also appear in Expires dates, so read the raw
    // lines, as WebKit's NetworkDataTaskCurl does.
    static constexpr auto setCookie = "set-cookie:"_s;
    for (auto& line : headerLines) {
        if (!line.startsWithIgnoringASCIICase(setCookie))
            continue;
        auto value = line.substring(setCookie.length()).trim(isASCIIWhitespace<char16_t>);
        cookieDatabase().setCookie(firstParty, url, value, CookieJarDB::Source::Network);
    }
}

namespace {

class NoStorageSessionProvider final : public StorageSessionProvider {
    CookieStorageSession* storageSession() const final { return nullptr; }
};

class CookieJarZeroG final : public CookieJar {
public:
    explicit CookieJarZeroG(Session& session)
        : CookieJar(adoptRef(*new NoStorageSessionProvider))
        , m_session(session)
    {
    }

private:
    String cookies(Document& document, const URL& url) const final
    {
        return cookieString(m_session->cookieDatabase(), document.firstPartyForCookies(), url, false);
    }

    void setCookies(Document& document, const URL& url, const String& cookieString) final
    {
        m_session->cookieDatabase().setCookie(document.firstPartyForCookies(), url, cookieString, CookieJarDB::Source::Script);
    }

    bool cookiesEnabled(Document&) final { return true; }
    void remoteCookiesEnabled(const Document&, CompletionHandler<void(bool)>&& completionHandler) const final { completionHandler(true); }

    std::pair<String, SecureCookiesAccessed> cookieRequestHeaderFieldValue(const URL& firstParty, const SameSiteInfo&, const URL& url, IncludeSecureCookies) const final
    {
        return { cookieString(m_session->cookieDatabase(), firstParty, url, true), SecureCookiesAccessed::No };
    }

    bool getRawCookies(Document& document, const URL& url, Vector<Cookie>& cookies) const final
    {
        auto result = m_session->cookieDatabase().searchCookies(document.firstPartyForCookies(), url, std::nullopt, std::nullopt, std::nullopt);
        if (!result)
            return false;
        cookies = WTF::move(*result);
        return true;
    }

    void deleteCookie(const Document&, const URL& url, const String& name, CompletionHandler<void()>&& completionHandler) final
    {
        m_session->cookieDatabase().deleteCookie(url.string(), name);
        completionHandler();
    }

    const Ref<Session> m_session;
};

} // anonymous namespace

Ref<CookieJar> createCookieJar(Session& session)
{
    return adoptRef(*new CookieJarZeroG(session));
}

static bool isValidScheme(const String& scheme)
{
    if (scheme.isEmpty() || !isASCIILower(scheme[0]))
        return false;
    for (auto character : StringView { scheme }.codeUnits()) {
        if (!isASCIILower(character) && !isASCIIDigit(character) && character != '+' && character != '-' && character != '.')
            return false;
    }
    return true;
}

Session::RegisterResult Session::registerScheme(const String& scheme, Ref<SchemeHandler>&& handler)
{
    if (!isValidScheme(scheme))
        return RegisterResult::InvalidScheme;
    if (LegacySchemeRegistry::isBuiltinScheme(scheme) || scheme == "file"_s || scheme == "ws"_s || scheme == "wss"_s)
        return RegisterResult::BuiltinScheme;
    if (m_schemeHandlers.contains(scheme))
        return RegisterResult::AlreadyRegistered;

    // ZeroG.md, "Schema custom app://": secure (so crypto.subtle and other
    // secure-context APIs work) and CORS-enabled (fetch). Process-wide in WebCore.
    LegacySchemeRegistry::registerURLSchemeAsSecure(scheme);
    LegacySchemeRegistry::registerURLSchemeAsCORSEnabled(scheme);
    LegacySchemeRegistry::registerURLSchemeAsHandledBySchemeHandler(scheme);
    m_schemeHandlers.add(scheme, WTF::move(handler));
    return RegisterResult::Registered;
}

SchemeHandler* Session::schemeHandler(StringView scheme) const
{
    auto iterator = m_schemeHandlers.find<StringViewHashTranslator>(scheme);
    return iterator == m_schemeHandlers.end() ? nullptr : iterator->value.ptr();
}

Session& sessionForFrame(LocalFrame* frame)
{
    if (frame) {
        if (RefPtr page = frame->page()) {
            if (auto* view = View::fromPage(*page))
                return view->session();
        }
#if defined(ZEROG_FULL_WEB)
        // A service worker's page.
        if (auto* session = sessionForServiceWorkerFrameLoaderClient(frame->loader().client()))
            return *session;
#endif
    }
    return Session::defaultSession();
}

// ---- The loader strategy ----

namespace {

// Blobs, the session's schemes, http and https.
RefPtr<LoadTask> createDirectTask(ResourceLoader& loader)
{
    auto& url = loader.request().url();
    if (url.protocolIsBlob())
        return createBlobTask(loader);
    if (auto* handler = sessionForFrame(loader.frame()).schemeHandler(url.protocol()))
        return SchemeTask::create(loader, *handler);
    if (url.protocolIsInHTTPFamily())
        return createNetworkTask(loader);
    return nullptr;
}

class LoaderStrategyZeroG final : public LoaderStrategy {
public:
    // A load leaving the page (a conversion to a download): WebCore's loader is
    // about to fail, and must not stop the task with it.
    RefPtr<LoadTask> takeTask(ResourceLoader& loader)
    {
        auto identifier = loader.identifier();
        return identifier ? m_tasks.take(*identifier) : nullptr;
    }

private:
    void loadResource(LocalFrame& frame, CachedResource& resource, ResourceRequest&& request, const ResourceLoaderOptions& options, CompletionHandler<void(RefPtr<SubresourceLoader>&&)>&& completionHandler) final
    {
        SubresourceLoader::create(frame, resource, WTF::move(request), options, [this, completionHandler = WTF::move(completionHandler)](RefPtr<SubresourceLoader>&& loader) mutable {
            if (loader)
                schedule(*loader);
            completionHandler(WTF::move(loader));
        });
    }

    void schedule(ResourceLoader& loader)
    {
        auto& url = loader.request().url();
        // WebCore decodes data: URLs itself.
        if (url.protocolIsData()) {
            loader.start();
            return;
        }

        // A service worker answers first, for what it controls; what it does
        // not answer goes on as below.
        RefPtr<LoadTask> task;
        if (auto* serviceWorkers = sessionForFrame(loader.frame()).serviceWorkers()) {
            task = serviceWorkers->createLoadTask(loader, [loader = Ref { loader }] {
                return createDirectTask(loader);
            });
        }
        if (!task)
            task = createDirectTask(loader);

        if (!task) {
            RunLoop::mainSingleton().dispatch([loader = Ref { loader }] {
                if (!loader->reachedTerminalState())
                    loader->didFail(ZeroG::cannotShowURLError(loader->request().url()));
            });
            return;
        }

        m_tasks.set(*loader.identifier(), *task);
        // Answers always come after loadResource returns, as from a network process.
        RunLoop::mainSingleton().dispatch([task = task.releaseNonNull()] {
            task->start();
        });
    }

    void loadResourceSynchronously(FrameLoader& frameLoader, ResourceLoaderIdentifier, const ResourceRequest& request, ClientCredentialPolicy, const FetchOptions&, const HTTPHeaderMap&, ResourceError& error, ResourceResponse& response, Vector<uint8_t>& data) final
    {
        auto& url = request.url();
        if (url.protocolIsData()) {
            auto result = DataURLDecoder::decode(url);
            if (!result) {
                error = failedError(url, "Invalid data URL"_s);
                return;
            }
            response = ResourceResponse::dataURLResponse(url, *result);
            data = WTF::move(result->data);
            return;
        }
        if (url.protocolIsBlob()) {
            loadBlobSynchronously(&frameLoader.frame(), request, error, response, data);
            return;
        }
        if (auto* handler = sessionForFrame(&frameLoader.frame()).schemeHandler(url.protocol())) {
            Ref protectedHandler { *handler };
            if (!handler->loadSynchronously(request, response, data))
                error = failedError(url, "This scheme does not answer synchronous loads"_s);
            return;
        }
        if (url.protocolIsInHTTPFamily()) {
            loadNetworkResourceSynchronously(sessionForFrame(&frameLoader.frame()), request, error, response, data);
            return;
        }
        error = ZeroG::cannotShowURLError(url);
    }

    void remove(ResourceLoader* loader) final
    {
        auto identifier = loader->identifier();
        if (!identifier)
            return;
        if (RefPtr task = m_tasks.take(*identifier))
            task->stop();
    }

    void pageLoadCompleted(Page&) final { }
    void browsingContextRemoved(LocalFrame&) final { }
    void setDefersLoading(ResourceLoader&, bool) final { }
    void crossOriginRedirectReceived(ResourceLoader*, const URL&) final { }
    void servePendingRequests(ResourceLoadPriority) final { }
    void suspendPendingRequests() final { }
    void resumePendingRequests() final { }
    void preconnectTo(FrameLoader&, ResourceRequest&&, StoredCredentialsPolicy, ShouldPreconnectAsFirstParty, PreconnectCompletionHandler&& completionHandler) final
    {
        if (completionHandler)
            completionHandler({ });
    }
    void setCaptureExtraNetworkLoadMetricsEnabled(bool) final { }
    bool isOnLine() const final { return true; }
    void addOnlineStateChangeListener(Function<void(bool)>&&) final { }
    void isResourceLoadFinished(CachedResource&, CompletionHandler<void(bool)>&& completionHandler) final { completionHandler(true); }

    ResourceError cancelledError(const ResourceRequest& request) const final { return ZeroG::cancelledError(request.url()); }
    ResourceError blockedError(const ResourceRequest& request) const final { return ZeroG::blockedError(request.url()); }
    bool isBlockedError(const ResourceError& error) const final { return error.domain() == policyErrorDomain && error.errorCode() == CannotUseRestrictedPort; }
    ResourceError blockedByContentBlockerError(const ResourceRequest& request) const final { return { policyErrorDomain, CannotShowURL, request.url(), "The URL was blocked by a content blocker"_s, ResourceError::Type::AccessControl }; }
    ResourceError cannotShowURLError(const ResourceRequest& request) const final { return ZeroG::cannotShowURLError(request.url()); }
    ResourceError interruptedForPolicyChangeError(const ResourceRequest& request) const final { return { policyErrorDomain, FrameLoadInterruptedByPolicyChange, request.url(), "Frame load interrupted"_s }; }
#if ENABLE(CONTENT_FILTERING)
    ResourceError blockedByContentFilterError(const ResourceRequest& request) const final { return { policyErrorDomain, CannotShowURL, request.url(), "The URL was blocked by a content filter"_s, ResourceError::Type::AccessControl }; }
#endif
    ResourceError cannotShowMIMETypeError(const ResourceResponse& response) const final { return { policyErrorDomain, CannotShowMIMEType, response.url(), "Content with specified MIME type can't be shown"_s }; }
    ResourceError fileDoesNotExistError(const ResourceResponse& response) const final { return notFoundError(response.url()); }
    ResourceError httpsUpgradeRedirectLoopError(const ResourceRequest& request) const final { return failedError(request.url(), "HTTPS upgrade redirect loop detected"_s); }
    ResourceError httpNavigationWithHTTPSOnlyError(const ResourceRequest& request) const final { return failedError(request.url(), "HTTP navigation with HTTPS-only enabled"_s); }
    bool isHttpNavigationWithHTTPSOnlyError(const ResourceError&) const final { return false; }
    ResourceError pluginWillHandleLoadError(const ResourceResponse& response) const final { return { policyErrorDomain, PlugInWillHandleLoad, response.url(), "Plug-in handled load"_s }; }

    HashMap<ResourceLoaderIdentifier, Ref<LoadTask>> m_tasks;
};

} // anonymous namespace

static LoaderStrategyZeroG* s_loaderStrategy;

LoaderStrategy* createLoaderStrategy()
{
    s_loaderStrategy = new LoaderStrategyZeroG;
    return s_loaderStrategy;
}

// ---- Downloads ----

bool convertLoadToDownload(ResourceLoader& loader, Download& download)
{
    RefPtr task = s_loaderStrategy ? s_loaderStrategy->takeTask(loader) : nullptr;
    if (!task || !task->convertToDownload(download))
        return false;
    download.setTask(task.get());
    return true;
}

void startDownload(View& view, const ResourceRequest& request, const String& suggestedFilename)
{
    auto download = Download::create(view, request.url(), suggestedFilename);
    auto& url = request.url();
    RefPtr frame = view.page().localMainFrame();

    if (url.protocolIsData()) {
        auto result = DataURLDecoder::decode(url);
        if (!result) {
            download->didFail(failedError(url, "Invalid data URL"_s));
            return;
        }
        if (download->start(ResourceResponse::dataURLResponse(url, *result))) {
            download->didReceiveData(result->data.span());
            download->didFinish();
        }
        return;
    }

    RefPtr<LoadTask> task;
    if (url.protocolIsBlob())
        task = createBlobTask(download, request, frame.get());
    else if (auto* handler = view.session().schemeHandler(url.protocol()))
        task = SchemeTask::create(download, request, *handler);
    else if (url.protocolIsInHTTPFamily())
        task = createNetworkTask(download, view.session(), request);
    if (!task) {
        download->didFail(ZeroG::cannotShowURLError(url));
        return;
    }
    download->setTask(task.get());
    RunLoop::mainSingleton().dispatch([task = task.releaseNonNull()] {
        task->start();
    });
}

} // namespace ZeroG
