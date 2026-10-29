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

namespace {

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
        auto request = m_request;
        if (request.allowCookies() && !request.hasHTTPHeaderField(HTTPHeaderName::Cookie)) {
            auto cookies = m_session->cookieHeader(request.firstPartyForCookies(), request.url());
            if (!cookies.isEmpty())
                request.addHTTPHeaderField(HTTPHeaderName::Cookie, cookies);
        }
        m_curlRequest = CurlRequest::create(request, *this);
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
        if (isRedirection()) {
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

    bool isRedirection() const
    {
        auto status = m_response.httpStatusCode();
        if (status < 300 || status >= 400 || status == 300 || status == 304 || status == 305 || status == 306)
            return false;
        return !m_response.httpHeaderField(HTTPHeaderName::Location).isEmpty();
    }

    bool shouldRedirectAsGET(const ResourceRequest& request, bool crossOrigin) const
    {
        auto& method = request.httpMethod();
        if (method == "GET"_s || method == "HEAD"_s)
            return false;
        if (m_response.isSeeOther())
            return true;
        if ((m_response.isMovedPermanently() || m_response.isFound()) && method == "POST"_s)
            return true;
        return crossOrigin && method == "DELETE"_s;
    }

    void redirect(ResourceLoader& loader)
    {
        static constexpr unsigned maximumRedirects = 20;
        if (++m_redirectCount > maximumRedirects) {
            fail(loader, failedError(m_response.url(), "Too many redirects"_s));
            return;
        }

        URL redirectedURL { m_response.url(), m_response.httpHeaderField(HTTPHeaderName::Location) };
        // A server cannot send the page into the application's schemes or files.
        if (!redirectedURL.protocolIsInHTTPFamily()) {
            fail(loader, ZeroG::cannotShowURLError(redirectedURL));
            return;
        }

        ResourceRequest request = m_request;
        if (!redirectedURL.hasFragmentIdentifier() && request.url().hasFragmentIdentifier())
            redirectedURL.setFragmentIdentifier(request.url().fragmentIdentifier());
        bool crossOrigin = !protocolHostAndPortAreEqual(request.url(), redirectedURL);
        request.setURL(WTF::move(redirectedURL));
        if (shouldRedirectAsGET(request, crossOrigin)) {
            request.setHTTPMethod("GET"_s);
            request.setHTTPBody(nullptr);
            request.clearHTTPContentType();
        }
        request.removeCredentials();
        if (crossOrigin) {
            request.clearHTTPAuthorization();
            request.clearHTTPOrigin();
        }

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

bool loadNetworkResourceSynchronously(const ResourceRequest& request, ResourceError& error, ResourceResponse&, Vector<uint8_t>&)
{
    // Synchronous XMLHttpRequest to the network would block the engine thread on
    // curl's; it is deprecated on the web and not supported here.
    error = failedError(request.url(), "Synchronous network loads are not supported"_s);
    return false;
}

} // namespace ZeroG
