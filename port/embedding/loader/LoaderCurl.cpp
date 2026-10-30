/*
 * Copyright (C) 2018 Sony Interactive Entertainment Inc.
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

// http and https loads over WebCore's curl classes (ZeroG.md, "Rete, storage e
// risorse"): requests run on curl's network thread, and their responses come
// back to the engine thread in pieces, streamed into the page's loader. The
// response, redirect and cookie handling is derived, at the pinned WebKit
// commit, from WebKit's NetworkDataTaskCurl (docs/PROVENANCE.md).

#include "config.h"
#include "Loader.h"

#include "Embedding.h"
#include "HTTPCache.h"
#include "View.h"
#include <WebCore/AuthenticationChallenge.h>
#include <WebCore/CacheValidation.h>
#include <WebCore/CertificateInfo.h>
#include <WebCore/CredentialStorage.h>
#include <WebCore/DocumentPage.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/Page.h>
#include <WebCore/CurlContext.h>
#include <WebCore/CurlRequest.h>
#include <WebCore/CurlRequestClient.h>
#include <WebCore/CurlResponse.h>
#include <WebCore/HTTPHeaderNames.h>
#include <WebCore/NetworkLoadMetrics.h>
#include <WebCore/ResourceLoader.h>
#include <WebCore/SharedBuffer.h>
#include <wtf/FileSystem.h>
#include <wtf/RunLoop.h>

namespace ZeroG {

using namespace WebCore;

// The system's root certificates: curl's native store on Windows, the
// distribution's CA bundle elsewhere (CurlSSLHandleZeroG.cpp finds it). The
// application's own come on top (addTrustedCertificates).
void initializeCertificates()
{
#if !OS(WINDOWS)
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        if (std::holds_alternative<std::monostate>(CurlContext::singleton().sslHandle().getCACertInfo()))
            log(LogLevel::Warning, "No CA certificate bundle found in the standard locations: https and wss connections fail certificate verification, except to servers whose root the application added"_s);
    });
#endif
}

size_t addTrustedCertificates(std::span<const uint8_t> pem)
{
    return CurlContext::singleton().sslHandle().addTrustedCertificates(pem);
}

// A certificate the engine does not trust, said so (curl's own message is only
// "SSL peer certificate or SSH remote key was not OK").
ResourceError describeCertificateError(ResourceError&& error, const CertificateInfo& certificate)
{
    if (!error.isCertificationVerificationError())
        return WTF::move(error);
    auto reason = certificate.verificationErrorDescription();
    return ResourceError { error.domain(), error.errorCode(), error.failingURL(),
        makeString("certificate not trusted"_s, reason.isEmpty() ? ""_s : ": "_s, reason), error.type() };
}

namespace {

// The Cookie header of the session, unless the request carries one or must not.
ResourceRequest withCookies(Session& session, const ResourceRequest& original)
{
    auto request = original;
    if (request.allowCookies() && !request.hasHTTPHeaderField(HTTPHeaderName::Cookie)) {
        auto cookies = session.cookieHeader(request.firstPartyForCookies(), request.url());
        if (!cookies.isEmpty())
            request.addHTTPHeaderField(HTTPHeaderName::Cookie, cookies);
    }
    return request;
}

// ---- Redirects, as NetworkDataTaskCurl::willPerformHTTPRedirection ----

constexpr unsigned maximumRedirects = 20;

bool isRedirection(const ResourceResponse& response)
{
    auto status = response.httpStatusCode();
    if (status < 300 || status >= 400 || status == 300 || status == 304 || status == 305 || status == 306)
        return false;
    return !response.httpHeaderField(HTTPHeaderName::Location).isEmpty();
}

bool shouldRedirectAsGET(const ResourceRequest& request, const ResourceResponse& response, bool crossOrigin)
{
    auto& method = request.httpMethod();
    if (method == "GET"_s || method == "HEAD"_s)
        return false;
    if (response.isSeeOther())
        return true;
    if ((response.isMovedPermanently() || response.isFound()) && method == "POST"_s)
        return true;
    return crossOrigin && method == "DELETE"_s;
}

// The request a redirect response leads to.
std::expected<ResourceRequest, ResourceError> redirectedRequest(const ResourceRequest& current, const ResourceResponse& response)
{
    URL redirectedURL { response.url(), response.httpHeaderField(HTTPHeaderName::Location) };
    // A server cannot send the page into the application's schemes or files.
    if (!redirectedURL.protocolIsInHTTPFamily())
        return makeUnexpected(ZeroG::cannotShowURLError(redirectedURL));

    ResourceRequest request = current;
    if (!redirectedURL.hasFragmentIdentifier() && request.url().hasFragmentIdentifier())
        redirectedURL.setFragmentIdentifier(request.url().fragmentIdentifier());
    bool crossOrigin = !protocolHostAndPortAreEqual(request.url(), redirectedURL);
    request.setURL(WTF::move(redirectedURL));
    if (shouldRedirectAsGET(request, response, crossOrigin)) {
        request.setHTTPMethod("GET"_s);
        request.setHTTPBody(nullptr);
        request.clearHTTPContentType();
    }
    request.removeCredentials();
    if (crossOrigin) {
        request.clearHTTPAuthorization();
        request.clearHTTPOrigin();
    }
    return request;
}

class NetworkTask final : public LoadTask, public CurlRequestClient {
public:
    static Ref<NetworkTask> create(ResourceLoader& loader)
    {
        return adoptRef(*new NetworkTask(LoadTarget { loader }, sessionForFrame(loader.frame()), loader.request()));
    }
    static Ref<NetworkTask> create(Download& download, Session& session, const ResourceRequest& request)
    {
        return adoptRef(*new NetworkTask(LoadTarget { download }, session, request));
    }

    void ref() const final { LoadTask::ref(); }
    void deref() const final { LoadTask::deref(); }

private:
    NetworkTask(LoadTarget&& target, Session& session, const ResourceRequest& request)
        : m_target(WTF::move(target))
        , m_session(session)
        , m_request(request)
    {
    }

    void start() final
    {
        if (!m_target.isActive())
            return;
        initializeCertificates();
        send();
    }

    void stop() final
    {
        m_target.clear();
        cancelRequest();
    }

    bool convertToDownload(Download& download) final
    {
        if (!m_target.isActive() || !m_curlRequest)
            return false;
        m_target.convertToDownload(download);
        // The page's response policy will not complete for it any more.
        if (std::exchange(m_waitingForResponsePolicy, false))
            m_curlRequest->completeDidReceiveResponse();
        return true;
    }

    void send()
    {
        // The session's disk cache answers first when it can, or turns the
        // request into a conditional one (HTTPCache.h).
        m_revalidating = std::nullopt;
        m_storeResponse = false;
        auto request = m_request;
        RefPtr cache = m_session->httpCache();
        if (cache && HTTPCache::canUse(m_request) && m_request.cachePolicy() != ResourceRequestCachePolicy::ReloadIgnoringCacheData) {
            auto policy = m_request.cachePolicy();
            if (auto entry = cache->lookup(m_request)) {
                bool staleAllowed = policy == ResourceRequestCachePolicy::ReturnCacheDataElseLoad || policy == ResourceRequestCachePolicy::ReturnCacheDataDontLoad;
                if (staleAllowed || HTTPCache::isFresh(*entry, m_request)) {
                    serveFromCache(WTF::move(*entry));
                    return;
                }
                if (auto conditional = HTTPCache::conditionalRequest(m_request, *entry)) {
                    request = WTF::move(*conditional);
                    m_revalidating = WTF::move(entry);
                }
            } else if (policy == ResourceRequestCachePolicy::ReturnCacheDataDontLoad) {
                // fetch(..., { cache: 'only-if-cached' }) with nothing stored.
                RunLoop::mainSingleton().dispatch([this, protectedThis = Ref { *this }] {
                    fail(failedError(m_request.url(), "Not in the cache"_s));
                });
                return;
            }
        }

        m_curlRequest = CurlRequest::create(withCookies(m_session, request), *this);
        if (!m_credential.isEmpty()) {
            m_curlRequest->setAuthenticationScheme(m_authenticationScheme);
            m_curlRequest->setUserPass(m_credential.user(), m_credential.password());
        }
        m_curlRequest->resume();
    }

    // ---- HTTP authentication, as NetworkDataTaskCurl::tryHttpAuthentication ----

    void authenticate(AuthenticationChallenge&& challenge)
    {
        // One partition per session (the storage is the session's own); a null
        // String cannot be a hash key.
        auto& credentials = m_session->credentials();
        auto& space = challenge.protectionSpace();
        if (challenge.previousFailureCount())
            credentials.remove(emptyString(), space);
        else {
            // A credential the application gave before for this protection space.
            auto stored = credentials.get(emptyString(), space);
            if (!stored.isEmpty() && stored != m_credential) {
                restart(space, stored);
                return;
            }
        }

        auto* view = viewForLoader();
        if (!view) {
            deliverResponse();
            return;
        }
        view->client().authenticationChallenge(challenge, [this, protectedThis = Ref { *this }, challenge](std::optional<Credential>&& credential) {
            if (!m_target.isActive() || !m_curlRequest)
                return;
            // No credential: the server's own 401 response is the page.
            if (!credential || credential->isEmpty()) {
                deliverResponse();
                return;
            }
            m_session->credentials().set(emptyString(), *credential, challenge.protectionSpace(), challenge.failureResponse().url());
            restart(challenge.protectionSpace(), *credential);
        });
    }

    void restart(const ProtectionSpace& space, const Credential& credential)
    {
        cancelRequest();
        m_credential = credential;
        m_authenticationScheme = space.authenticationScheme();
        send();
    }

    View* viewForLoader()
    {
        RefPtr loader = m_target.loader();
        RefPtr frame = loader ? loader->frame() : nullptr;
        RefPtr page = frame ? frame->page() : nullptr;
        return page ? View::fromPage(*page) : nullptr;
    }

    void deliverResponse()
    {
        if (!m_target.isActive())
            return;
        m_waitingForResponsePolicy = true;
        m_target.didReceiveResponse(ResourceResponse { m_response }, [this, protectedThis = Ref { *this }](bool accepted) {
            if (!std::exchange(m_waitingForResponsePolicy, false))
                return;
            if (!accepted) {
                stop();
                return;
            }
            // The target keeps the load: let curl deliver the body.
            if (m_target.isActive() && m_curlRequest)
                m_curlRequest->completeDidReceiveResponse();
        });
    }

    // A stored response, delivered as a network one is: the response (with the
    // page's policy decision), the body, the end. Never from within start().
    void serveFromCache(HTTPCache::Entry&& entry)
    {
        m_response = WTF::move(entry.response);
        RunLoop::mainSingleton().dispatch([this, protectedThis = Ref { *this }, body = WTF::move(entry.body)] mutable {
            if (!m_target.isActive())
                return;
            m_waitingForResponsePolicy = true;
            m_target.didReceiveResponse(ResourceResponse { m_response }, [this, protectedThis = Ref { *this }, body = WTF::move(body)](bool accepted) mutable {
                if (!std::exchange(m_waitingForResponsePolicy, false))
                    return;
                if (!accepted) {
                    stop();
                    return;
                }
                if (m_target.isActive() && body->size())
                    m_target.didReceiveData(WTF::move(body));
                if (m_target.isActive()) {
                    NetworkLoadMetrics metrics;
                    m_target.didFinish(metrics);
                }
            });
        });
    }

    void cancelRequest()
    {
        if (RefPtr request = std::exchange(m_curlRequest, nullptr)) {
            request->invalidateClient();
            request->cancel();
        }
    }

    // ---- CurlRequestClient (engine thread) ----

    void curlDidSendData(CurlRequest&, unsigned long long bytesSent, unsigned long long totalBytesToBeSent) final
    {
        m_target.didSendData(bytesSent, totalBytesToBeSent);
    }

    void curlDidReceiveResponse(CurlRequest&, CurlResponse&& curlResponse) final
    {
        Ref protectedThis { *this };
        if (!m_target.isActive())
            return;
        RefPtr loader = m_target.loader();

        if (m_request.allowCookies())
            m_session->storeCookies(m_request.firstPartyForCookies(), curlResponse.url, curlResponse.headers);
        m_response = ResourceResponse { curlResponse };
        m_response.setDeprecatedNetworkLoadMetrics(Box<NetworkLoadMetrics>::create(WTF::move(curlResponse.networkLoadMetrics)));
        if (isRedirection(m_response)) {
            redirect();
            return;
        }
        // Page loads that may ask (not fetch with credentials: 'omit', for example).
        if (m_response.isUnauthorized() && curlResponse.availableHttpAuth && loader && loader->options().clientCredentialPolicy == ClientCredentialPolicy::MayAskClientForCredentials) {
            authenticate(AuthenticationChallenge { curlResponse, m_authenticationFailures++, m_response });
            return;
        }

        RefPtr cache = m_session->httpCache();
        if (auto entry = std::exchange(m_revalidating, std::nullopt)) {
            // Still valid: the stored body, with the headers the server sent now.
            if (m_response.httpStatusCode() == 304 && cache) {
                updateResponseHeadersAfterRevalidation(entry->response, m_response);
                entry->responseTime = WallTime::now();
                cache->update(m_request, *entry);
                cancelRequest();
                serveFromCache(WTF::move(*entry));
                return;
            }
        }
        m_storeResponse = cache && HTTPCache::canStore(m_request, m_response);
        if (m_storeResponse) {
            m_responseTime = WallTime::now();
            m_cacheBody.reset();
        }
        deliverResponse();
    }

    void curlDidReceiveData(CurlRequest&, Ref<SharedBuffer>&& buffer) final
    {
        if (m_storeResponse) {
            RefPtr cache = m_session->httpCache();
            // Too large for an entry: not kept.
            if (!cache || m_cacheBody.size() + buffer->size() > cache->capacity() / 8) {
                m_storeResponse = false;
                m_cacheBody.reset();
            } else
                m_cacheBody.append(buffer.get());
        }
        m_target.didReceiveData(WTF::move(buffer));
    }

    void curlDidComplete(CurlRequest&, NetworkLoadMetrics&& metrics) final
    {
        Ref protectedThis { *this };
        m_curlRequest = nullptr;
        if (std::exchange(m_storeResponse, false)) {
            if (RefPtr cache = m_session->httpCache())
                cache->store(m_request, m_response, m_cacheBody.takeBufferAsContiguous(), m_responseTime);
        }
        m_target.didFinish(metrics);
    }

    void curlDidFailWithError(CurlRequest&, ResourceError&& error, CertificateInfo&& certificate) final
    {
        Ref protectedThis { *this };
        m_curlRequest = nullptr;
        m_target.didFail(describeCertificateError(WTF::move(error), certificate));
    }

    // ---- Redirects ----

    void redirect()
    {
        if (++m_redirectCount > maximumRedirects) {
            fail(failedError(m_response.url(), "Too many redirects"_s));
            return;
        }
        auto redirected = redirectedRequest(m_request, m_response);
        if (!redirected) {
            fail(redirected.error());
            return;
        }
        auto request = WTF::move(*redirected);

        // The current request answered with the redirect; it is done.
        cancelRequest();
        m_target.willSendRequest(WTF::move(request), m_response, [this, protectedThis = Ref { *this }](ResourceRequest&& newRequest) {
            // A null request: WebCore refused the redirect and cancels the load itself.
            if (newRequest.isNull() || !m_target.isActive())
                return;
            m_request = WTF::move(newRequest);
            send();
        });
    }

    void fail(const ResourceError& error)
    {
        Ref protectedThis { *this };
        cancelRequest();
        m_target.didFail(error);
    }

    LoadTarget m_target;
    const Ref<Session> m_session;
    ResourceRequest m_request;
    ResourceResponse m_response;
    RefPtr<CurlRequest> m_curlRequest;
    unsigned m_redirectCount { 0 };
    unsigned m_authenticationFailures { 0 };
    bool m_waitingForResponsePolicy { false };
    // The disk cache: the entry a conditional request revalidates, and the body
    // of a response being kept.
    std::optional<HTTPCache::Entry> m_revalidating;
    bool m_storeResponse { false };
    SharedBufferBuilder m_cacheBody;
    WallTime m_responseTime;
    Credential m_credential;
    ProtectionSpace::AuthenticationScheme m_authenticationScheme { ProtectionSpace::AuthenticationScheme::Default };
};

} // anonymous namespace

Ref<LoadTask> createNetworkTask(ResourceLoader& loader)
{
    return NetworkTask::create(loader);
}

Ref<LoadTask> createNetworkTask(Download& download, Session& session, const ResourceRequest& request)
{
    return NetworkTask::create(download, session, request);
}

// ---- Synchronous loads (synchronous XMLHttpRequest) ----
//
// The scheduler's requests answer on the engine thread, which a synchronous load
// blocks; so the transfer runs here, on its own handle, set up as CurlRequest
// sets up its own (the handle's constructor applies the shared TLS, proxy and
// connection settings).

namespace {

struct SynchronousTransfer {
    Vector<String> headers;
    Vector<uint8_t> body;
    Vector<uint8_t> upload;
    size_t uploaded { 0 };

    static size_t header(char* data, size_t size, size_t count, void* user)
    {
        auto& transfer = *static_cast<SynchronousTransfer*>(user);
        auto line = String::fromLatin1(unsafeMakeSpan(data, size * count));
        // Each response (100 Continue, then the real one) starts over.
        if (startsWithLettersIgnoringASCIICase(line, "http/"_s))
            transfer.headers.clear();
        if (line != "\r\n"_s && line != "\n"_s)
            transfer.headers.append(WTF::move(line));
        return size * count;
    }

    static size_t write(char* data, size_t size, size_t count, void* user)
    {
        static_cast<SynchronousTransfer*>(user)->body.append(unsafeMakeSpan(reinterpret_cast<const uint8_t*>(data), size * count));
        return size * count;
    }

    static size_t read(char* buffer, size_t size, size_t count, void* user)
    {
        auto& transfer = *static_cast<SynchronousTransfer*>(user);
        size_t length = std::min(size * count, transfer.upload.size() - transfer.uploaded);
        std::memcpy(buffer, transfer.upload.span().subspan(transfer.uploaded).data(), length);
        transfer.uploaded += length;
        return length;
    }
};

std::expected<ResourceResponse, ResourceError> transferSynchronously(const ResourceRequest& request, SynchronousTransfer& transfer)
{
    CurlHandle handle;
    handle.setURL(request.url(), CurlHandle::LocalhostAlias::Disable);
    handle.appendRequestHeaders(request.httpHeaderFields());
    if (RefPtr body = request.httpBody())
        transfer.upload = body->flatten();

    auto& method = request.httpMethod();
    if (method == "GET"_s)
        handle.enableHttpGetRequest();
    else if (method == "HEAD"_s)
        handle.enableHttpHeadRequest();
    else {
        if (method == "POST"_s)
            handle.enableHttpPostRequest(transfer.upload.size());
        else {
            handle.setHttpCustomRequest(method);
            handle.enableHttpPutRequest(transfer.upload.size());
        }
        handle.setReadCallbackFunction(SynchronousTransfer::read, &transfer);
        if (!request.hasHTTPHeader(HTTPHeaderName::ContentType) && transfer.upload.isEmpty())
            handle.removeRequestHeader("Content-Type"_s);
        handle.removeRequestHeader("Expect"_s);
    }
    handle.setHeaderCallbackFunction(SynchronousTransfer::header, &transfer);
    handle.setWriteCallbackFunction(SynchronousTransfer::write, &transfer);
    handle.setTimeout(request.timeoutInterval() ? Seconds { request.timeoutInterval() } : CurlContext::singleton().defaultTimeoutInterval());

    if (auto result = handle.perform(); result != CURLE_OK)
        return makeUnexpected(ResourceError { result, request.url() });

    CurlResponse curlResponse;
    curlResponse.url = request.url();
    curlResponse.statusCode = handle.getResponseCode().value_or(0);
    curlResponse.httpVersion = handle.getHttpVersion().value_or(0);
    curlResponse.expectedContentLength = transfer.body.size();
    curlResponse.headers = transfer.headers;
    return ResourceResponse { curlResponse };
}

} // anonymous namespace

bool loadNetworkResourceSynchronously(Session& session, const ResourceRequest& original, ResourceError& error, ResourceResponse& response, Vector<uint8_t>& data)
{
    initializeCertificates();
    auto request = original;
    for (unsigned redirects = 0; ; ++redirects) {
        if (redirects > maximumRedirects) {
            error = failedError(request.url(), "Too many redirects"_s);
            return false;
        }
        // A fresh stored response answers without the network (no revalidation:
        // a synchronous load is the rare case).
        RefPtr cache = session.httpCache();
        if (cache && HTTPCache::canUse(request) && request.cachePolicy() != ResourceRequestCachePolicy::ReloadIgnoringCacheData) {
            if (auto entry = cache->lookup(request); entry && HTTPCache::isFresh(*entry, request)) {
                response = WTF::move(entry->response);
                data = Vector<uint8_t> { entry->body->span() };
                return true;
            }
        }
        SynchronousTransfer transfer;
        auto result = transferSynchronously(withCookies(session, request), transfer);
        if (!result) {
            error = result.error();
            return false;
        }
        if (request.allowCookies())
            session.storeCookies(request.firstPartyForCookies(), request.url(), transfer.headers);
        if (!isRedirection(*result)) {
            if (cache && HTTPCache::canStore(request, *result))
                cache->store(request, *result, SharedBuffer::create(transfer.body.span()), WallTime::now());
            response = WTF::move(*result);
            data = WTF::move(transfer.body);
            return true;
        }
        auto next = redirectedRequest(request, *result);
        if (!next) {
            error = next.error();
            return false;
        }
        request = WTF::move(*next);
    }
}

} // namespace ZeroG
