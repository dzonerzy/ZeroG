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

#include <WebCore/CertificateInfo.h>
#include <WebCore/CurlContext.h>
#include <WebCore/CurlRequest.h>
#include <WebCore/CurlRequestClient.h>
#include <WebCore/CurlResponse.h>
#include <WebCore/HTTPHeaderNames.h>
#include <WebCore/NetworkLoadMetrics.h>
#include <WebCore/ResourceLoader.h>
#include <wtf/FileSystem.h>

namespace ZeroG {

using namespace WebCore;

// BoringSSL does not read the system's trust store by itself. On Windows WebCore
// asks curl for the native store; elsewhere the engine points curl at the
// distribution's CA bundle.
void initializeCertificates()
{
#if !OS(WINDOWS)
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        static constexpr ASCIILiteral bundles[] = {
            "/etc/ssl/certs/ca-certificates.crt"_s, // Debian, Ubuntu, Arch, Alpine
            "/etc/pki/tls/certs/ca-bundle.crt"_s, // Fedora, RHEL
            "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem"_s, // RHEL 7+
            "/etc/ssl/ca-bundle.pem"_s, // openSUSE
            "/etc/ssl/cert.pem"_s, // others
        };
        for (auto bundle : bundles) {
            if (FileSystem::fileExists(String { bundle })) {
                CurlContext::singleton().sslHandle().setCACertPath(String { bundle });
                return;
            }
        }
    });
#endif
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
    static Ref<NetworkTask> create(ResourceLoader& loader) { return adoptRef(*new NetworkTask(loader)); }

    void ref() const final { LoadTask::ref(); }
    void deref() const final { LoadTask::deref(); }

private:
    explicit NetworkTask(ResourceLoader& loader)
        : m_loader(&loader)
        , m_session(sessionForFrame(loader.frame()))
        , m_request(loader.request())
    {
    }

    ResourceLoader* loader()
    {
        if (m_loader && m_loader->reachedTerminalState())
            m_loader = nullptr;
        return m_loader.get();
    }

    void start() final
    {
        if (!loader())
            return;
        initializeCertificates();
        send();
    }

    void stop() final
    {
        m_loader = nullptr;
        cancelRequest();
    }

    void send()
    {
        m_curlRequest = CurlRequest::create(withCookies(m_session, m_request), *this);
        m_curlRequest->resume();
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
        if (RefPtr loader = this->loader())
            loader->didSendData(bytesSent, totalBytesToBeSent);
    }

    void curlDidReceiveResponse(CurlRequest&, CurlResponse&& curlResponse) final
    {
        Ref protectedThis { *this };
        RefPtr loader = this->loader();
        if (!loader)
            return;

        if (m_request.allowCookies())
            m_session->storeCookies(m_request.firstPartyForCookies(), curlResponse.url, curlResponse.headers);
        m_response = ResourceResponse { curlResponse };
        m_response.setDeprecatedNetworkLoadMetrics(Box<NetworkLoadMetrics>::create(WTF::move(curlResponse.networkLoadMetrics)));
        if (isRedirection(m_response)) {
            redirect(*loader);
            return;
        }

        loader->didReceiveResponse(ResourceResponse { m_response }, [this, protectedThis = Ref { *this }] {
            // WebCore keeps the load: let curl deliver the body.
            if (this->loader() && m_curlRequest)
                m_curlRequest->completeDidReceiveResponse();
        });
    }

    void curlDidReceiveData(CurlRequest&, Ref<SharedBuffer>&& buffer) final
    {
        if (RefPtr loader = this->loader())
            loader->didReceiveData(buffer, buffer->size(), DataPayloadType::DataPayloadBytes);
    }

    void curlDidComplete(CurlRequest&, NetworkLoadMetrics&& metrics) final
    {
        Ref protectedThis { *this };
        RefPtr loader = std::exchange(m_loader, nullptr);
        m_curlRequest = nullptr;
        if (loader && !loader->reachedTerminalState())
            loader->didFinishLoading(metrics);
    }

    void curlDidFailWithError(CurlRequest&, ResourceError&& error, CertificateInfo&&) final
    {
        Ref protectedThis { *this };
        RefPtr loader = std::exchange(m_loader, nullptr);
        m_curlRequest = nullptr;
        if (loader && !loader->reachedTerminalState())
            loader->didFail(error);
    }

    // ---- Redirects ----

    void redirect(ResourceLoader& loader)
    {
        if (++m_redirectCount > maximumRedirects) {
            fail(loader, failedError(m_response.url(), "Too many redirects"_s));
            return;
        }
        auto redirected = redirectedRequest(m_request, m_response);
        if (!redirected) {
            fail(loader, redirected.error());
            return;
        }
        auto request = WTF::move(*redirected);

        // The current request answered with the redirect; it is done.
        cancelRequest();
        loader.willSendRequest(WTF::move(request), m_response, [this, protectedThis = Ref { *this }](ResourceRequest&& newRequest) {
            // A null request: WebCore refused the redirect and cancels the load itself.
            if (newRequest.isNull() || !this->loader())
                return;
            m_request = WTF::move(newRequest);
            send();
        });
    }

    void fail(ResourceLoader& loader, const ResourceError& error)
    {
        Ref protectedLoader { loader };
        m_loader = nullptr;
        cancelRequest();
        loader.didFail(error);
    }

    RefPtr<ResourceLoader> m_loader;
    const Ref<Session> m_session;
    ResourceRequest m_request;
    ResourceResponse m_response;
    RefPtr<CurlRequest> m_curlRequest;
    unsigned m_redirectCount { 0 };
};

} // anonymous namespace

Ref<LoadTask> createNetworkTask(ResourceLoader& loader)
{
    return NetworkTask::create(loader);
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
        SynchronousTransfer transfer;
        auto result = transferSynchronously(withCookies(session, request), transfer);
        if (!result) {
            error = result.error();
            return false;
        }
        if (request.allowCookies())
            session.storeCookies(request.firstPartyForCookies(), request.url(), transfer.headers);
        if (!isRedirection(*result)) {
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
