/*
 * Copyright (C) 2019-2025 Apple Inc. All rights reserved.
 * Copyright (C) 2022 Sony Interactive Entertainment Inc.
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

// Derived, at the pinned WebKit commit, from WebKit's WebSocketChannel
// (Source/WebKit/WebProcess/Network: the page's side, buffered amount, closing)
// and WebSocketTaskCurl (Source/WebKit/NetworkProcess/curl: handshake, frames
// and the close handshake over a CurlStream), joined into one channel that runs
// on the engine thread (docs/PROVENANCE.md).

#include "config.h"
#include "WebSocketChannelZeroG.h"

#include "Loader.h"
#include <JavaScriptCore/ConsoleTypes.h>
#include <WebCore/Blob.h>
#include <WebCore/CookieJarDB.h>
#include <WebCore/CurlContext.h>
#include <WebCore/CurlStreamScheduler.h>
#include <WebCore/Document.h>
#include <WebCore/DocumentInlines.h>
#include <WebCore/EmptyClients.h>
#include <WebCore/ExceptionCode.h>
#include <WebCore/FrameDestructionObserverInlines.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/NetworkSendQueue.h>
#include <WebCore/SharedBuffer.h>
#include <WebCore/ThreadableWebSocketChannel.h>
#include <WebCore/WebSocketChannelClient.h>
#include <WebCore/WebSocketDeflateFramer.h>
#include <WebCore/WebSocketFrame.h>
#include <WebCore/WebSocketHandshake.h>
#include <WebCore/WebTransportSession.h>
#include <wtf/CheckedArithmetic.h>
#include <wtf/text/MakeString.h>

namespace ZeroG {

using namespace WebCore;

namespace {

// Cookies are kept for http(s) URLs: ws:// shares http://'s, wss:// https://'s.
URL cookieURL(const URL& url)
{
    URL httpURL = url;
    httpURL.setProtocol(url.protocolIs("wss"_s) ? "https"_s : "http"_s);
    return httpURL;
}

class WebSocketChannelZeroG final : public RefCounted<WebSocketChannelZeroG>, public ThreadableWebSocketChannel, public CurlStream::Client, public CanMakeWeakPtr<WebSocketChannelZeroG> {
public:
    static Ref<WebSocketChannelZeroG> create(Document& document, WebSocketChannelClient& client)
    {
        return adoptRef(*new WebSocketChannelZeroG(document, client));
    }

    ~WebSocketChannelZeroG()
    {
        destroyStream();
    }

    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

private:
    WebSocketChannelZeroG(Document& document, WebSocketChannelClient& client)
        : m_document(document)
        , m_client(client)
        , m_progressIdentifier(WebSocketChannelIdentifier::generate())
    {
        m_messageQueue = NetworkSendQueue::create(document, [weakThis = WeakPtr { *this }](auto& utf8) {
            if (RefPtr protectedThis = weakThis.get())
                protectedThis->sendMessage(WebSocketFrame::OpCodeText, byteCast<uint8_t>(utf8.span()));
        }, [weakThis = WeakPtr { *this }](auto data) {
            if (RefPtr protectedThis = weakThis.get())
                protectedThis->sendMessage(WebSocketFrame::OpCodeBinary, data);
        }, [weakThis = WeakPtr { *this }](ExceptionCode code) {
            if (RefPtr protectedThis = weakThis.get())
                protectedThis->fail(makeString("Failed to load Blob: exception code = "_s, static_cast<int>(code)));
            return NetworkSendQueue::Continue::No;
        });
    }

    enum class State : uint8_t { Connecting, Handshaking, Opened, Closing, Closed };

    // ---- ThreadableWebSocketChannel: the page's side ----

    ConnectStatus connect(const URL& url, const String& protocol) final
    {
        RefPtr document = m_document.get();
        if (!document)
            return ConnectStatus::KO;
        auto request = webSocketConnectRequest(*document, url);
        if (!request)
            return ConnectStatus::KO;
        if (request->url() != url) {
            if (RefPtr client = m_client.get())
                client->didUpgradeURL();
        }

        m_request = WTF::move(*request);
        m_url = m_request.url();
        m_protocol = protocol;
        m_session = sessionForFrame(document->frame());
        initializeCertificates();
        m_streamID = CurlContext::singleton().streamScheduler().createStream(m_url, *this, CurlStream::ServerTrustEvaluation::Enable, CurlStream::LocalhostAlias::Disable);
        m_state = State::Connecting;
        return ConnectStatus::OK;
    }

    String subprotocol() final { return m_subprotocol.isNull() ? emptyString() : m_subprotocol; }
    String extensions() final { return m_extensions.isNull() ? emptyString() : m_extensions; }

    void send(UTF8CString&& message) final
    {
        if (increaseBufferedAmount(message.length()))
            m_messageQueue->enqueue(WTF::move(message));
    }

    void send(const JSC::ArrayBuffer& data, size_t byteOffset, size_t byteLength) final
    {
        if (increaseBufferedAmount(byteLength))
            m_messageQueue->enqueue(data, byteOffset, byteLength);
    }

    void send(Blob& blob) final
    {
        if (!blob.size()) {
            send(JSC::ArrayBuffer::create(0, 1), 0, 0);
            return;
        }
        if (increaseBufferedAmount(blob.size()))
            m_messageQueue->enqueue(blob);
    }

    void close(int code, const String& reason) final
    {
        Ref protectedThis { *this };
        m_isClosing = true;
        if (RefPtr client = m_client.get())
            client->didStartClosingHandshake();

        if (m_state == State::Closed)
            return;
        if (m_state == State::Connecting || m_state == State::Handshaking) {
            didClose(CloseEventCodeAbnormalClosure, { });
            return;
        }
        sendClosingHandshakeIfNeeded(code, reason);
    }

    void fail(String&& reason) final
    {
        Ref protectedThis { *this };
        logErrorMessage(reason);
        if (RefPtr client = m_client.get())
            client->didReceiveMessageError(String { reason });
        if (m_isClosing)
            return;
        if (m_state == State::Opened)
            sendClosingHandshakeIfNeeded(CloseEventCodeGoingAway, reason);
        didClose(CloseEventCodeAbnormalClosure, { });
    }

    void disconnect() final
    {
        m_client = nullptr;
        m_document = nullptr;
        m_messageQueue->clear();
        if (m_state == State::Opened)
            sendClosingHandshakeIfNeeded(CloseEventCodeGoingAway, { });
        destroyStream();
        m_state = State::Closed;
    }

    void suspend() final { }
    void resume() final { }

    WebSocketChannelIdentifier progressIdentifier() const final { return m_progressIdentifier; }
    bool hasCreatedHandshake() const final { return !m_handshakeRequest.isNull(); }
    bool isConnected() const final { return m_state == State::Opened; }
    ResourceRequest clientHandshakeRequest(const CookieGetter&) const final { return m_handshakeRequest; }
    const ResourceResponse& serverHandshakeResponse() const final { return m_handshakeResponse; }

    bool increaseBufferedAmount(size_t length)
    {
        if (!length)
            return true;
        CheckedSize amount = m_bufferedAmount;
        amount += length;
        if (amount.hasOverflowed()) [[unlikely]] {
            fail("Failed to send WebSocket frame: buffer has no more space"_s);
            return false;
        }
        m_bufferedAmount = amount;
        if (RefPtr client = m_client.get())
            client->didUpdateBufferedAmount(m_bufferedAmount);
        return true;
    }

    void decreaseBufferedAmount(size_t length)
    {
        if (!length)
            return;
        m_bufferedAmount -= std::min(length, m_bufferedAmount);
        if (RefPtr client = m_client.get())
            client->didUpdateBufferedAmount(m_bufferedAmount);
    }

    // A message leaves the page's queue: framed and handed to the stream.
    void sendMessage(WebSocketFrame::OpCode opCode, std::span<const uint8_t> data)
    {
        if (m_state == State::Opened && !sendFrame(opCode, data))
            fail("Failed to send WebSocket frame."_s);
        decreaseBufferedAmount(data.size());
    }

    void logErrorMessage(const String& message)
    {
        RefPtr document = m_document.get();
        if (!document)
            return;
        auto consoleMessage = m_url.isNull()
            ? makeString("WebSocket connection failed: "_s, message)
            : makeString("WebSocket connection to '"_s, m_url.string(), "' failed: "_s, message);
        document->addConsoleMessage(MessageSource::Network, MessageLevel::Error, consoleMessage);
    }

    // ---- CurlStream::Client: the network side ----

    void didOpen(CurlStreamID) final
    {
        if (m_state != State::Connecting)
            return;
        m_state = State::Handshaking;

        m_handshake = makeUnique<WebSocketHandshake>(m_url, m_protocol, m_request.httpUserAgent(), m_request.httpHeaderField(HTTPHeaderName::Origin), m_request.allowCookies(), false);
        m_handshake->reset();
        m_handshake->addExtensionProcessor(m_deflateFramer.createExtensionProcessor());
        m_handshake->setClientHandshakeRequestHeaders(m_request.httpHeaderFields());

        String cookies;
        if (m_request.allowCookies() && m_session)
            cookies = m_session->cookieHeader(m_request.firstPartyForCookies(), cookieURL(m_url));

        // The handshake ends with an empty line; the Cookie header goes before it.
        auto message = m_handshake->clientHandshakeMessage();
        Vector<uint8_t> bytes(byteCast<uint8_t>(message.span()));
        if (!cookies.isEmpty()) {
            auto cookieHeader = makeString("Cookie: "_s, cookies, "\r\n"_s).utf8();
            bytes.shrink(bytes.size() - 2);
            bytes.append(byteCast<uint8_t>(cookieHeader.span()));
            bytes.append(std::span { reinterpret_cast<const uint8_t*>("\r\n"), 2 });
        }
        sendBytes(bytes.span());

        m_handshakeRequest = m_handshake->clientHandshakeRequest([&cookies](const URL&) { return cookies; });
    }

    void didSendData(CurlStreamID, size_t) final { }

    void didReceiveData(CurlStreamID, const SharedBuffer& buffer) final
    {
        Ref protectedThis { *this };
        if (m_state == State::Connecting || m_state == State::Closed)
            return;
        // The server closed the connection.
        if (!buffer.size()) {
            didClose(CloseEventCodeAbnormalClosure, { });
            return;
        }
        if (m_shouldDiscardReceivedData || m_receivedClosingHandshake)
            return;

        m_receiveBuffer.append(buffer.span());

        auto opened = validateOpeningHandshake();
        if (!opened) {
            didFail(String { opened.error() });
            return;
        }
        if (!*opened)
            return;

        if (auto error = receiveFrames())
            didFail(WTF::move(*error));
    }

    void didFail(CurlStreamID, CURLcode errorCode, CertificateInfo&& certificate) final
    {
        if (errorCode == CURLE_PEER_FAILED_VERIFICATION) {
            auto reason = certificate.verificationErrorDescription();
            didFail(makeString("WebSocket network error: certificate not trusted"_s, reason.isEmpty() ? ""_s : ": "_s, reason));
            return;
        }
        didFail(makeString("WebSocket network error: "_s, String::fromLatin1(curl_easy_strerror(errorCode)), " (curl error "_s, static_cast<unsigned>(errorCode), ')'));
    }

    void skipReceivedBuffer(size_t length)
    {
        m_receiveBuffer.removeAt(0, std::min(length, m_receiveBuffer.size()));
    }

    // Whether the server's handshake is complete (and valid).
    std::expected<bool, String> validateOpeningHandshake()
    {
        if (m_didCompleteOpeningHandshake)
            return true;
        if (m_state != State::Handshaking || !m_handshake || m_handshake->mode() != WebSocketHandshake::Incomplete) {
            m_handshake = nullptr;
            return makeUnexpected("Unexpected handshaking condition"_str);
        }

        auto headerLength = m_handshake->readServerHandshake(m_receiveBuffer.span());
        if (headerLength <= 0)
            return false;
        skipReceivedBuffer(headerLength);

        if (m_handshake->mode() != WebSocketHandshake::Connected) {
            auto reason = m_handshake->failureReason();
            m_handshakeResponse = m_handshake->serverHandshakeResponse();
            m_handshake = nullptr;
            return makeUnexpected(reason);
        }

        auto setCookie = m_handshake->serverSetCookie();
        if (!setCookie.isEmpty() && m_session && m_request.allowCookies())
            m_session->cookieDatabase().setCookie(m_request.firstPartyForCookies(), cookieURL(m_url), setCookie, CookieJarDB::Source::Network);

        m_state = State::Opened;
        m_didCompleteOpeningHandshake = true;
        m_handshakeResponse = m_handshake->serverHandshakeResponse();
        auto subprotocol = m_handshake->serverWebSocketProtocol();
        auto extensions = m_handshake->acceptedExtensions();
        m_handshake = nullptr;

        if (!m_isClosing) {
            m_subprotocol = WTF::move(subprotocol);
            m_extensions = WTF::move(extensions);
            if (RefPtr client = m_client.get())
                client->didConnect();
        }
        return true;
    }

    std::optional<String> receiveFrames()
    {
        while (m_receiveBuffer.size() && !m_shouldDiscardReceivedData && !m_receivedClosingHandshake && (m_state == State::Opened || m_state == State::Closing)) {
            WebSocketFrame frame;
            const uint8_t* frameEnd;
            String errorString;
            auto parseResult = WebSocketFrame::parseFrame(m_receiveBuffer.mutableSpan(), frame, frameEnd, errorString);
            if (parseResult == WebSocketFrame::FrameIncomplete)
                return std::nullopt;
            if (parseResult == WebSocketFrame::FrameError)
                return errorString;

            auto inflateResult = m_deflateFramer.inflate(frame);
            if (!inflateResult->succeeded())
                return inflateResult->failureReason();
            if (auto error = validateFrame(frame))
                return error;

            size_t frameLength = frameEnd - m_receiveBuffer.span().data();
            if (!frame.final || frame.opCode == WebSocketFrame::OpCodeContinuation) {
                if (frame.opCode != WebSocketFrame::OpCodeContinuation) {
                    m_hasContinuousFrame = true;
                    m_continuousFrameOpCode = frame.opCode;
                }
                m_continuousFrameData.append(frame.payload);
                if (frame.final) {
                    auto data = std::exchange(m_continuousFrameData, { });
                    m_hasContinuousFrame = false;
                    skipReceivedBuffer(frameLength);
                    if (auto error = processFrame(m_continuousFrameOpCode, data.span()))
                        return error;
                    continue;
                }
                skipReceivedBuffer(frameLength);
                continue;
            }
            // The payload points into the receive buffer: process it first.
            Vector<uint8_t> payload(frame.payload);
            auto opCode = frame.opCode;
            skipReceivedBuffer(frameLength);
            if (auto error = processFrame(opCode, payload.span()))
                return error;
        }
        return std::nullopt;
    }

    std::optional<String> processFrame(WebSocketFrame::OpCode opCode, std::span<const uint8_t> data)
    {
        switch (opCode) {
        case WebSocketFrame::OpCodeText: {
            String message = data.size() ? String::fromUTF8(data) : emptyString();
            if (message.isNull())
                return "Could not decode a text frame as UTF-8."_str;
            if (RefPtr client = m_client.get(); client && !m_isClosing)
                client->didReceiveMessage(WTF::move(message));
            break;
        }
        case WebSocketFrame::OpCodeBinary:
            if (RefPtr client = m_client.get(); client && !m_isClosing)
                client->didReceiveBinaryData(Vector<uint8_t> { data });
            break;
        case WebSocketFrame::OpCodeClose: {
            int code = CloseEventCodeNoStatusRcvd;
            if (data.size() == 1)
                return "Received a broken close frame containing an invalid size body."_str;
            if (data.size() >= 2) {
                code = data[0] << 8 | data[1];
                if (code == CloseEventCodeNoStatusRcvd || code == CloseEventCodeAbnormalClosure || code == CloseEventCodeTLSHandshake)
                    return "Received a broken close frame containing a reserved status code."_str;
            }
            auto reason = data.size() >= 3 ? String::fromUTF8(data.subspan(2)) : emptyString();
            m_receivedClosingHandshake = true;
            sendClosingHandshakeIfNeeded(code, reason);
            didClose(code, reason);
            break;
        }
        case WebSocketFrame::OpCodePing:
            if (!sendFrame(WebSocketFrame::OpCodePong, data))
                return "Failed to send WebSocket frame."_str;
            break;
        case WebSocketFrame::OpCodeContinuation:
        case WebSocketFrame::OpCodePong:
        case WebSocketFrame::OpCodeInvalid:
            break;
        }
        return std::nullopt;
    }

    std::optional<String> validateFrame(const WebSocketFrame& frame)
    {
        if (WebSocketFrame::isReservedOpCode(frame.opCode))
            return makeString("Unrecognized frame opcode: "_s, static_cast<unsigned>(frame.opCode));
        if (frame.reserved2 || frame.reserved3)
            return makeString("One or more reserved bits are on: reserved2 = "_s, static_cast<unsigned>(frame.reserved2), ", reserved3 = "_s, static_cast<unsigned>(frame.reserved3));
        if (frame.masked)
            return "A server must not mask any frames that it sends to the client."_str;
        if (WebSocketFrame::isControlOpCode(frame.opCode) && !frame.final)
            return makeString("Received fragmented control frame: opcode = "_s, static_cast<unsigned>(frame.opCode));
        if (WebSocketFrame::isControlOpCode(frame.opCode) && WebSocketFrame::needsExtendedLengthField(frame.payload.size()))
            return makeString("Received control frame having too long payload: "_s, frame.payload.size(), " bytes"_s);
        if (m_hasContinuousFrame && frame.opCode != WebSocketFrame::OpCodeContinuation && !WebSocketFrame::isControlOpCode(frame.opCode))
            return "Received new data frame but previous continuous frame is unfinished."_str;
        if (!m_hasContinuousFrame && frame.opCode == WebSocketFrame::OpCodeContinuation)
            return "Received unexpected continuation frame."_str;
        return std::nullopt;
    }

    void sendClosingHandshakeIfNeeded(int code, const String& reason)
    {
        if (m_didSendClosingHandshake)
            return;
        Vector<uint8_t> payload;
        if (!m_receivedClosingHandshake && code != CloseEventCodeNotSpecified) {
            payload.append(static_cast<uint8_t>(static_cast<unsigned short>(code) >> 8));
            payload.append(static_cast<uint8_t>(code));
            payload.append(byteCast<uint8_t>(reason.utf8().span()));
        }
        sendFrame(WebSocketFrame::OpCodeClose, payload.span());
        m_state = State::Closing;
        m_didSendClosingHandshake = true;
    }

    bool sendFrame(WebSocketFrame::OpCode opCode, std::span<const uint8_t> data)
    {
        if (m_didSendClosingHandshake)
            return true;
        WebSocketFrame frame(opCode, true, false, true, data);
        auto deflateResult = m_deflateFramer.deflate(frame);
        if (!deflateResult->succeeded()) {
            didFail(deflateResult->failureReason());
            return false;
        }
        Vector<uint8_t> frameData;
        frame.makeFrameData(frameData);
        sendBytes(frameData.span());
        return true;
    }

    void sendBytes(std::span<const uint8_t> bytes)
    {
        if (m_streamID == invalidCurlStreamID)
            return;
        auto buffer = makeUniqueArray<uint8_t>(bytes.size());
        memcpySpan(unsafeMakeSpan(buffer.get(), bytes.size()), bytes);
        CurlContext::singleton().streamScheduler().send(m_streamID, WTF::move(buffer), bytes.size());
    }

    // The connection failed: Hybi-10 7.1.7, nothing more is processed.
    void didFail(String&& reason)
    {
        Ref protectedThis { *this };
        if (m_receivedDidFail)
            return;
        m_receivedDidFail = true;
        m_shouldDiscardReceivedData = true;
        m_receiveBuffer.clear();
        m_deflateFramer.didFail();
        m_hasContinuousFrame = false;
        m_continuousFrameData.clear();

        logErrorMessage(reason);
        if (RefPtr client = m_client.get())
            client->didReceiveMessageError(WTF::move(reason));
        didClose(CloseEventCodeAbnormalClosure, { });
    }

    void didClose(int code, const String& reason)
    {
        Ref protectedThis { *this };
        destroyStream();
        if (m_state == State::Closed)
            return;
        m_state = State::Closed;

        // The client may go away while it handles the close.
        RunLoop::mainSingleton().dispatch([weakThis = WeakPtr { *this }, code, reason] {
            RefPtr protectedThis = weakThis.get();
            if (!protectedThis)
                return;
            RefPtr client = protectedThis->m_client.get();
            if (!client)
                return;
            bool receivedClosingHandshake = code != CloseEventCodeAbnormalClosure;
            if (receivedClosingHandshake)
                client->didStartClosingHandshake();
            client->didClose(protectedThis->m_bufferedAmount,
                (protectedThis->m_isClosing || receivedClosingHandshake) ? WebSocketChannelClient::ClosingHandshakeComplete : WebSocketChannelClient::ClosingHandshakeIncomplete,
                code, reason);
        });
    }

    void destroyStream()
    {
        if (m_streamID == invalidCurlStreamID)
            return;
        CurlContext::singleton().streamScheduler().destroyStream(m_streamID);
        m_streamID = invalidCurlStreamID;
    }

    WeakPtr<Document, WeakPtrImplWithEventTargetData> m_document;
    ThreadSafeWeakPtr<WebSocketChannelClient> m_client;
    RefPtr<NetworkSendQueue> m_messageQueue;
    RefPtr<Session> m_session;
    const WebSocketChannelIdentifier m_progressIdentifier;

    ResourceRequest m_request;
    URL m_url;
    String m_protocol;
    String m_subprotocol;
    String m_extensions;
    ResourceRequest m_handshakeRequest;
    ResourceResponse m_handshakeResponse;

    State m_state { State::Connecting };
    CurlStreamID m_streamID { invalidCurlStreamID };
    std::unique_ptr<WebSocketHandshake> m_handshake;
    WebSocketDeflateFramer m_deflateFramer;
    Vector<uint8_t> m_receiveBuffer;
    Vector<uint8_t> m_continuousFrameData;
    WebSocketFrame::OpCode m_continuousFrameOpCode { WebSocketFrame::OpCodeInvalid };
    size_t m_bufferedAmount { 0 };
    bool m_hasContinuousFrame { false };
    bool m_isClosing { false };
    bool m_didCompleteOpeningHandshake { false };
    bool m_didSendClosingHandshake { false };
    bool m_receivedClosingHandshake { false };
    bool m_receivedDidFail { false };
    bool m_shouldDiscardReceivedData { false };
};

class SocketProviderZeroG final : public SocketProvider {
    RefPtr<ThreadableWebSocketChannel> createWebSocketChannel(Document& document, WebSocketChannelClient& client, IsInitiatedByWorker) final
    {
        return WebSocketChannelZeroG::create(document, client);
    }

    // No WebTransport yet.
    Ref<WebTransportSession> createWebTransportSession(ScriptExecutionContext& context, WebTransportSessionClient& client) final
    {
        return emptySocketProvider()->createWebTransportSession(context, client);
    }
};

} // anonymous namespace

Ref<SocketProvider> createSocketProvider()
{
    static NeverDestroyed<Ref<SocketProvider>> provider { adoptRef(*new SocketProviderZeroG) };
    return provider.get();
}

} // namespace ZeroG
