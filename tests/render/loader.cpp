// The resource loader: an app:// page served from a MemoryFS (stylesheet, module
// scripts, fetch, range requests, archives, percent-encoded paths, directory
// redirects, synchronous XMLHttpRequest), data: URLs, the write-once rules, and
// http over curl against a server inside the test (fetch with CORS, redirects, a
// main-frame navigation).
//
// Usage: loader

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "config.h"
#include "Embedding.h"
#include "MemoryFS.h"
#include "View.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>
#include <wtf/SHA1.h>
#include <wtf/text/Base64.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>
#include <zlib.h>

using namespace WebCore;

namespace {

#define check(condition, ...) [&] {                    \
        bool result = (condition);                     \
        std::printf("%s ", result ? "ok  " : "FAIL");  \
        std::printf(__VA_ARGS__);                      \
        std::printf("\n");                             \
        return result;                                 \
    }()

std::span<const uint8_t> bytes(const char* text)
{
    return { reinterpret_cast<const uint8_t*>(text), std::strlen(text) };
}

constexpr auto indexPage = R"HTML(<!DOCTYPE html>
<html><head>
  <title>Loader test</title>
  <link rel="stylesheet" href="style.css">
  <script type="module" src="app.js"></script>
</head><body><div id="box"></div></body></html>)HTML";

// ---- An archive in tools/assets.py's format ----

void appendLittleEndian(Vector<uint8_t>& out, uint64_t value, size_t size)
{
    for (size_t i = 0; i < size; ++i)
        out.append(static_cast<uint8_t>(value >> (8 * i)));
}

Vector<uint8_t> makeArchive(const Vector<std::pair<String, String>>& files)
{
    struct Entry {
        std::string path;
        Vector<uint8_t> stored;
        size_t size;
        bool compressed;
    };
    Vector<Entry> entries;
    for (auto& [path, content] : files) {
        auto utf8 = content.utf8();
        auto text = byteCast<uint8_t>(utf8.span());
        uLongf length = compressBound(text.size());
        Vector<uint8_t> compressed(length);
        compress(compressed.mutableSpan().data(), &length, text.data(), text.size());
        compressed.shrink(length);
        bool useCompressed = length < text.size();
        auto pathUTF8 = path.utf8();
        Entry entry { std::string(byteCast<char>(pathUTF8.data()), pathUTF8.length()), useCompressed ? WTF::move(compressed) : Vector<uint8_t>(text), text.size(), useCompressed };
        entries.append(WTF::move(entry));
    }

    Vector<uint8_t> out;
    out.append(std::span { reinterpret_cast<const uint8_t*>("ZGAR"), 4 });
    appendLittleEndian(out, 1, 4);
    appendLittleEndian(out, entries.size(), 4);
    appendLittleEndian(out, 0, 4);
    size_t offset = 16 + entries.size() * 40;
    Vector<uint8_t> tail;
    for (auto& entry : entries) {
        size_t pathOffset = offset + tail.size();
        tail.append(std::span { reinterpret_cast<const uint8_t*>(entry.path.data()), entry.path.size() });
        size_t dataOffset = offset + tail.size();
        tail.append(entry.stored.span());
        appendLittleEndian(out, dataOffset, 8);
        appendLittleEndian(out, entry.stored.size(), 8);
        appendLittleEndian(out, entry.size, 8);
        appendLittleEndian(out, pathOffset, 4);
        appendLittleEndian(out, entry.path.size(), 4);
        appendLittleEndian(out, 0, 4); // MIME offset
        appendLittleEndian(out, 0, 2); // no MIME: from the extension
        out.append(entry.compressed ? 1 : 0);
        out.append(0);
    }
    out.appendVector(tail);
    return out;
}

// ---- A small HTTP server on 127.0.0.1 ----

#if defined(_WIN32)
using Socket = SOCKET;
void closeSocket(Socket socket) { closesocket(socket); }
#else
using Socket = int;
constexpr Socket INVALID_SOCKET = -1;
void closeSocket(Socket socket) { close(socket); }
#endif

// A loopback port nothing listens on (bound, then released).
int unusedPort()
{
    Socket probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address { };
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    socklen_t length = sizeof(address);
    getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length);
    closeSocket(probe);
    return ntohs(address.sin_port);
}

class HTTPServer {
public:
    HTTPServer()
    {
#if defined(_WIN32)
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
        m_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address { };
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        bind(m_listener, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        listen(m_listener, 16);
        socklen_t length = sizeof(address);
        getsockname(m_listener, reinterpret_cast<sockaddr*>(&address), &length);
        m_port = ntohs(address.sin_port);
        m_thread = std::thread([this] { run(); });
    }

    ~HTTPServer()
    {
        m_stopping = true;
        // Wake accept() with a connection of our own.
        Socket waker = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address { };
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(m_port);
        connect(waker, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        closeSocket(waker);
        m_thread.join();
        {
            // WebSockets still open end with the server.
            std::lock_guard lock { m_lock };
            for (auto socket : m_open)
                shutdown(socket, 2);
        }
        for (auto& connection : m_connections)
            connection.join();
        closeSocket(m_listener);
    }

    int port() const { return m_port; }
    int requestCount() const { return m_requests; }

private:
    void run()
    {
        while (true) {
            Socket client = accept(m_listener, nullptr, nullptr);
            if (m_stopping) {
                if (client != INVALID_SOCKET)
                    closeSocket(client);
                return;
            }
            if (client == INVALID_SOCKET)
                continue;
            std::lock_guard lock { m_lock };
            m_open.push_back(client);
            m_connections.emplace_back([this, client] {
                serve(client);
                std::lock_guard lock { m_lock };
                m_open.erase(std::find(m_open.begin(), m_open.end(), client));
                closeSocket(client);
            });
        }
    }

    void serve(Socket client)
    {
        {
            std::string request;
            char buffer[4096];
            while (request.find("\r\n\r\n") == std::string::npos) {
                auto received = recv(client, buffer, sizeof(buffer), 0);
                if (received <= 0)
                    break;
                request.append(buffer, received);
            }
            if (request.find("\r\nUpgrade: websocket\r\n") != std::string::npos) {
                ++m_requests;
                webSocket(client, request);
                return;
            }
            // The body, if any.
            auto headerEnd = request.find("\r\n\r\n");
            auto lengthHeader = request.find("\r\nContent-Length: ");
            if (headerEnd != std::string::npos && lengthHeader != std::string::npos && lengthHeader < headerEnd) {
                size_t length = std::stoul(request.substr(lengthHeader + 18));
                while (request.size() < headerEnd + 4 + length) {
                    auto received = recv(client, buffer, sizeof(buffer), 0);
                    if (received <= 0)
                        break;
                    request.append(buffer, received);
                }
            }
            ++m_requests;
            auto pathStart = request.find(' ') + 1;
            auto path = request.substr(pathStart, request.find(' ', pathStart) - pathStart);
            auto response = respond(path, request);
            send(client, response.data(), static_cast<int>(response.size()), 0);
        }
    }

    static std::string header(const std::string& request, const std::string& name)
    {
        auto start = request.find("\r\n" + name + ": ");
        if (start == std::string::npos)
            return { };
        start += name.size() + 4;
        return request.substr(start, request.find("\r\n", start) - start);
    }

    static void sendFrame(Socket client, uint8_t opCode, const std::string& payload)
    {
        std::string frame;
        frame += static_cast<char>(0x80 | opCode);
        if (payload.size() < 126)
            frame += static_cast<char>(payload.size());
        else if (payload.size() < 65536) {
            frame += static_cast<char>(126);
            frame += static_cast<char>(payload.size() >> 8);
            frame += static_cast<char>(payload.size());
        } else {
            frame += static_cast<char>(127);
            for (int i = 7; i >= 0; --i)
                frame += static_cast<char>(static_cast<uint64_t>(payload.size()) >> (8 * i));
        }
        frame += payload;
        send(client, frame.data(), static_cast<int>(frame.size()), 0);
    }

    // An echo server: a welcome message with the handshake's cookies, then each
    // text message back as "echo: ...", binary as is, pings answered, and the
    // close handshake returned.
    void webSocket(Socket client, const std::string& request)
    {
        auto pathStart = request.find(' ') + 1;
        auto path = request.substr(pathStart, request.find(' ', pathStart) - pathStart);
        if (path == "/ws-reject") {
            auto rejected = response("403 Forbidden", "text/plain", "no");
            send(client, rejected.data(), static_cast<int>(rejected.size()), 0);
            return;
        }
        auto key = header(request, "Sec-WebSocket-Key");
        SHA1 sha1;
        auto accepted = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        sha1.addBytes(std::span { reinterpret_cast<const uint8_t*>(accepted.data()), accepted.size() });
        SHA1::Digest digest;
        sha1.computeHash(digest);
        auto accept = base64EncodeToString(digest).utf8();
        std::string handshake = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "
            + std::string(byteCast<char>(accept.data()), accept.length()) + "\r\n";
        if (header(request, "Sec-WebSocket-Protocol").find("chat") != std::string::npos)
            handshake += "Sec-WebSocket-Protocol: chat\r\n";
        handshake += "Set-Cookie: fromsocket=4; Path=/\r\n\r\n";
        send(client, handshake.data(), static_cast<int>(handshake.size()), 0);
        sendFrame(client, 1, "welcome, cookies: " + header(request, "Cookie"));

        std::string data;
        char buffer[65536];
        while (true) {
            // A frame from the client: always masked.
            while (true) {
                if (data.size() >= 2) {
                    uint64_t length = static_cast<uint8_t>(data[1]) & 0x7f;
                    size_t offset = 2;
                    if (length == 126 && data.size() >= 4) {
                        length = static_cast<uint8_t>(data[2]) << 8 | static_cast<uint8_t>(data[3]);
                        offset = 4;
                    } else if (length == 127 && data.size() >= 10) {
                        length = 0;
                        for (int i = 0; i < 8; ++i)
                            length = length << 8 | static_cast<uint8_t>(data[2 + i]);
                        offset = 10;
                    }
                    if ((length < 126 || offset > 2) && data.size() >= offset + 4 + length)
                        break;
                }
                auto received = recv(client, buffer, sizeof(buffer), 0);
                if (received <= 0)
                    return;
                data.append(buffer, received);
            }
            uint8_t opCode = data[0] & 0x0f;
            uint64_t length = static_cast<uint8_t>(data[1]) & 0x7f;
            size_t offset = 2;
            if (length == 126) {
                length = static_cast<uint8_t>(data[2]) << 8 | static_cast<uint8_t>(data[3]);
                offset = 4;
            } else if (length == 127) {
                length = 0;
                for (int i = 0; i < 8; ++i)
                    length = length << 8 | static_cast<uint8_t>(data[2 + i]);
                offset = 10;
            }
            std::string payload = data.substr(offset + 4, length);
            for (size_t i = 0; i < payload.size(); ++i)
                payload[i] ^= data[offset + (i % 4)];
            data.erase(0, offset + 4 + length);

            if (opCode == 1)
                sendFrame(client, 1, "echo: " + payload);
            else if (opCode == 2)
                sendFrame(client, 2, payload);
            else if (opCode == 9)
                sendFrame(client, 10, payload);
            else if (opCode == 8) {
                sendFrame(client, 8, payload);
                return;
            }
        }
    }

    static std::string response(const std::string& status, const std::string& type, const std::string& body, const std::string& extraHeaders = { })
    {
        return "HTTP/1.1 " + status + "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size())
            + "\r\nConnection: close\r\n" + extraHeaders + "\r\n" + body;
    }

    std::string respond(const std::string& path, const std::string& request)
    {
        if (path == "/echo-body") {
            auto headerEnd = request.find("\r\n\r\n");
            return response("200 OK", "text/plain", headerEnd == std::string::npos ? "" : request.substr(headerEnd + 4));
        }
        // Basic authentication: user / secret.
        if (path == "/protected" || path == "/protected-other") {
            if (header(request, "Authorization") == "Basic dXNlcjpzZWNyZXQ=")
                return response("200 OK", "text/html", "<!DOCTYPE html><title>welcome</title>welcome");
            auto realm = path == "/protected" ? "zerog" : "other";
            return response("401 Unauthorized", "text/html", "<!DOCTYPE html><title>denied</title>denied", std::string("WWW-Authenticate: Basic realm=\"") + realm + "\"\r\n");
        }
        if (path == "/set-cookie")
            return response("200 OK", "text/plain", "set", "Set-Cookie: visible=1; Path=/\r\nSet-Cookie: hidden=2; Path=/; HttpOnly\r\n");
        if (path == "/echo-cookie") {
            std::string cookies;
            auto start = request.find("\r\nCookie: ");
            if (start != std::string::npos) {
                start += 10;
                cookies = request.substr(start, request.find("\r\n", start) - start);
            }
            return response("200 OK", "text/plain", cookies);
        }
        if (path == "/page.html")
            return response("200 OK", "text/html; charset=utf-8", "<!DOCTYPE html><title>From HTTP</title><p>remote");
        if (path == "/api.json")
            return response("200 OK", "application/json", "{\"remote\":true}", "Access-Control-Allow-Origin: *\r\n");
        if (path == "/private.json")
            return response("200 OK", "application/json", "{\"secret\":1}");
        if (path == "/redirect")
            return response("302 Found", "text/plain", "", "Location: /api.json\r\nAccess-Control-Allow-Origin: *\r\n");
        if (path == "/to-app")
            return response("302 Found", "text/plain", "", "Location: app://localhost/data.json\r\nAccess-Control-Allow-Origin: *\r\n");
        return response("404 Not Found", "text/plain", "missing");
    }

    Socket m_listener { INVALID_SOCKET };
    int m_port { 0 };
    std::thread m_thread;
    std::mutex m_lock;
    std::vector<Socket> m_open;
    std::vector<std::thread> m_connections;
    std::atomic<bool> m_stopping { false };
    std::atomic<int> m_requests { 0 };
};

// ---- The page ----

class Client final : public ZeroG::ViewClient {
public:
    void loadFinished(const URL& url, const ResourceError& error) final
    {
        finishedURL = url;
        loadFailed = !error.isNull();
        if (loadFailed)
            std::printf("     load of %s failed: %s (%s %d)\n", url.string().utf8().data(), error.localizedDescription().utf8().data(), error.domain().utf8().data(), error.errorCode());
        ++loads;
    }

    void consoleMessage(JSC::MessageSource, JSC::MessageLevel, const String& message, unsigned, unsigned, const String&) final
    {
        std::printf("     console: %s\n", message.utf8().data());
    }

    // Answers HTTP challenges from a queue; none left: no credential.
    void authenticationChallenge(const AuthenticationChallenge& challenge, CompletionHandler<void(std::optional<Credential>&&)>&& completionHandler) final
    {
        challenges.append(makeString(challenge.protectionSpace().realm(), ' ', challenge.previousFailureCount()));
        if (credentials.isEmpty()) {
            completionHandler(std::nullopt);
            return;
        }
        auto [user, password] = credentials.takeFirst();
        completionHandler(Credential { user, password, CredentialPersistence::ForSession });
    }

    Vector<String> challenges;
    Deque<std::pair<String, String>> credentials;
    URL finishedURL;
    bool loadFailed { false };
    int loads { 0 };
};

template<typename Condition>
bool runUntil(Condition&& condition, std::chrono::milliseconds timeout = std::chrono::seconds(10))
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        ZeroG::runPendingWork();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

bool load(ZeroG::View& view, Client& client, const String& url)
{
    int loads = client.loads;
    view.loadURL(URL { url });
    if (!runUntil([&] { return client.loads > loads; }, std::chrono::seconds(30))) {
        std::printf("     load of %s timed out\n", url.utf8().data());
        return false;
    }
    return !client.loadFailed;
}

// Evaluates an async expression and waits for its value as a string.
String await(ZeroG::View& view, const String& expression)
{
    view.evaluateScript(makeString("window.__result = undefined; (async () => { try { window.__result = String(await ("_s, expression,
        ")); } catch (e) { window.__result = 'error: ' + e; } })(); undefined"_s));
    String result;
    bool settled = runUntil([&] {
        result = view.evaluateScript("typeof window.__result === 'string' ? window.__result : '\\u0000'"_s);
        return result != String(std::span<const char16_t> { u"\0", 1 });
    });
    return settled ? result : "timeout"_s;
}

} // anonymous namespace

int main()
{
    // Unbuffered: a crash still shows how far the test got.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    ZeroG::initializeProcess();
    bool passed = true;

    auto fs = ZeroG::MemoryFS::create();
    fs->addFile("/index.html"_s, bytes(indexPage));
    fs->addFile("/style.css"_s, bytes("#box { width: 123px; height: 10px; }"));
    fs->addFile("/app.js"_s, bytes("import { value } from './lib/value.mjs'; window.moduleValue = value;"));
    fs->addFile("/lib/value.mjs"_s, bytes("export const value = 42;"));
    fs->addFile("/data.json"_s, bytes("{\"answer\":42}"));
    fs->addFile("/sub/index.html"_s, bytes("<!DOCTYPE html><title>sub</title>"));
    fs->addFile("/frames.html"_s, bytes("<!DOCTYPE html><title>outer</title><iframe src='inner.html'></iframe>"));
    fs->addFile("/inner.html"_s, bytes("<!DOCTYPE html><title>inner</title><script>parent.innerLoaded = location.href;</script>"));
    fs->addFile(String::fromUTF8("/\xC3\xBCn\xC3\xAF code.txt"), bytes("unicode"));
    Vector<uint8_t> media(1000);
    for (size_t i = 0; i < media.size(); ++i)
        media[i] = static_cast<uint8_t>(i);
    bool released = false;
    fs->addFile("/media.bin"_s, media.span(), "application/octet-stream"_s, [&released] { released = true; });

    // Write-once and path rules.
    passed &= check(fs->addFile("/index.html"_s, bytes("again")) == ZeroG::MemoryFS::AddResult::AlreadyExists, "a path is written once");
    passed &= check(fs->addFile("/../etc/passwd"_s, bytes("x")) == ZeroG::MemoryFS::AddResult::InvalidPath, "'..' is refused");
    passed &= check(fs->addFile("/a//b"_s, bytes("x")) == ZeroG::MemoryFS::AddResult::InvalidPath, "empty segments are refused");
    passed &= check(fs->addFile("relative.txt"_s, bytes("x")) == ZeroG::MemoryFS::AddResult::InvalidPath, "relative paths are refused");
    passed &= check(fs->addFile("/dir/"_s, bytes("x")) == ZeroG::MemoryFS::AddResult::InvalidPath, "directories are not files");

    // Archives.
    StringBuilder longText;
    for (int i = 0; i < 200; ++i)
        longText.append("compressible "_s);
    auto archive = makeArchive({ { "/a.txt"_s, "stored"_s }, { "/b.txt"_s, longText.toString() } });
    auto mounted = fs->mountArchive("/pkg"_s, archive.span());
    passed &= check(mounted && *mounted == 2, "archive mounted (2 files, one compressed)");
    auto again = fs->mountArchive("/pkg"_s, archive.span());
    passed &= check(!again && again.error().kind == ZeroG::MemoryFS::MountError::Kind::Conflict, "mounting over existing paths fails: %s", (again ? String { } : again.error().message).utf8().data());
    auto garbage = fs->mountArchive("/other"_s, bytes("not an archive at all"));
    passed &= check(!garbage && !fs->contains("/other/a.txt"_s), "a malformed archive is refused: %s", (garbage ? String { } : garbage.error().message).utf8().data());

    // Schemes.
    auto session = ZeroG::Session::create();
    using RegisterResult = ZeroG::Session::RegisterResult;
    passed &= check(session->registerScheme("app"_s, fs.copyRef()) == RegisterResult::Registered, "app:// registered");
    passed &= check(session->registerScheme("app"_s, fs.copyRef()) == RegisterResult::AlreadyRegistered, "a scheme is registered once per session");
    passed &= check(session->registerScheme("https"_s, fs.copyRef()) == RegisterResult::BuiltinScheme, "built-in schemes cannot be taken");
    passed &= check(session->registerScheme("1app"_s, fs.copyRef()) == RegisterResult::InvalidScheme, "invalid scheme names are refused");

    HTTPServer server;
    auto http = makeString("http://127.0.0.1:"_s, server.port());

    {
        ZeroG::View::Configuration configuration;
        configuration.session = session.copyRef();
        auto view = ZeroG::View::create(WTF::move(configuration));
        Client client;
        view->setClient(&client);

        passed &= check(load(*view, client, "app://localhost/index.html"_s), "app://localhost/index.html loads");
        passed &= check(view->evaluateScript("document.title"_s) == "Loader test"_s, "title from the page");
        passed &= check(view->evaluateScript("location.origin"_s) == "app://localhost"_s, "origin: %s", view->evaluateScript("location.origin"_s).utf8().data());
        passed &= check(view->evaluateScript("String(isSecureContext && typeof crypto.subtle === 'object')"_s) == "true"_s, "secure context with crypto.subtle");
        passed &= check(view->evaluateScript("getComputedStyle(document.getElementById('box')).width"_s) == "123px"_s, "stylesheet applied");
        runUntil([&] { return view->evaluateScript("String(window.moduleValue)"_s) == "42"_s; });
        passed &= check(view->evaluateScript("String(window.moduleValue)"_s) == "42"_s, "module script and its import ran");

        auto json = await(*view, "fetch('/data.json').then(r => r.json()).then(j => j.answer)"_s);
        passed &= check(json == "42"_s, "fetch JSON: %s", json.utf8().data());
        auto type = await(*view, "fetch('/data.json').then(r => r.headers.get('content-type'))"_s);
        passed &= check(type == "application/json; charset=utf-8"_s, "MIME type from the extension: %s", type.utf8().data());
        auto missing = await(*view, "fetch('/missing.txt').then(r => r.status)"_s);
        passed &= check(missing == "404"_s, "missing file: %s", missing.utf8().data());
        auto range = await(*view, "fetch('/media.bin', { headers: { Range: 'bytes=10-19' } }).then(async r => "
            "`${r.status} ${r.headers.get('content-range')} ${[...new Uint8Array(await r.arrayBuffer())].join(',')}`)"_s);
        passed &= check(range == "206 bytes 10-19/1000 10,11,12,13,14,15,16,17,18,19"_s, "range request: %s", range.utf8().data());
        auto suffix = await(*view, "fetch('/media.bin', { headers: { Range: 'bytes=-3' } }).then(async r => [...new Uint8Array(await r.arrayBuffer())].join(','))"_s);
        passed &= check(suffix == "229,230,231"_s, "suffix range: %s", suffix.utf8().data());
        auto unsatisfiable = await(*view, "fetch('/media.bin', { headers: { Range: 'bytes=5000-' } }).then(r => r.status)"_s);
        passed &= check(unsatisfiable == "416"_s, "unsatisfiable range: %s", unsatisfiable.utf8().data());
        auto stored = await(*view, "fetch('/pkg/a.txt').then(r => r.text())"_s);
        passed &= check(stored == "stored"_s, "stored archive entry: %s", stored.utf8().data());
        auto inflated = await(*view, "fetch('/pkg/b.txt').then(r => r.text()).then(t => t.length)"_s);
        passed &= check(inflated == String::number(longText.length()), "compressed archive entry: %s chars", inflated.utf8().data());
        auto unicode = await(*view, "fetch('/%C3%BCn%C3%AF%20code.txt').then(r => r.text())"_s);
        passed &= check(unicode == "unicode"_s, "percent-encoded path: %s", unicode.utf8().data());
        auto post = await(*view, "fetch('/data.json', { method: 'POST', body: 'x' }).then(r => r.status)"_s);
        passed &= check(post == "405"_s, "only GET and HEAD: %s", post.utf8().data());
        auto data = await(*view, "fetch('data:text/plain,hello').then(r => r.text())"_s);
        passed &= check(data == "hello"_s, "data: URL: %s", data.utf8().data());
        auto sync = view->evaluateScript("(() => { const x = new XMLHttpRequest(); x.open('GET', '/data.json', false); x.send(); return x.status + ' ' + x.responseText; })()"_s);
        passed &= check(sync == "200 {\"answer\":42}"_s, "synchronous XMLHttpRequest: %s", sync.utf8().data());
        // Blobs.
        auto blobText = await(*view, "new Blob(['hello ', 'blob'], { type: 'text/plain' }).text()"_s);
        passed &= check(blobText == "hello blob"_s, "Blob.text(): %s", blobText.utf8().data());
        auto sliced = await(*view, "new Blob(['hello blob']).slice(6).text()"_s);
        passed &= check(sliced == "blob"_s, "a slice: %s", sliced.utf8().data());
        auto reader = await(*view, "new Promise(resolve => { const r = new FileReader(); r.onload = () => resolve(r.result); r.readAsText(new Blob(['read me'])); })"_s);
        passed &= check(reader == "read me"_s, "FileReader: %s", reader.utf8().data());
        auto objectURL = await(*view, "(async () => { const url = URL.createObjectURL(new Blob(['{\"from\":\"blob\"}'], { type: 'application/json' }));"
            "const r = await fetch(url); const partial = await (await fetch(url, { headers: { Range: 'bytes=2-5' } })).text();"
            "const sync = (() => { const x = new XMLHttpRequest(); x.open('GET', url, false); x.send(); return x.responseText; })();"
            "URL.revokeObjectURL(url); const revoked = await fetch(url).then(() => 'still there', () => 'gone');"
            "return [r.headers.get('content-type'), (await r.json()).from, partial, sync, revoked].join(' '); })()"_s);
        passed &= check(objectURL == "application/json blob from {\"from\":\"blob\"} gone"_s, "object URL (fetch, range, synchronous XHR, revoke): %s", objectURL.utf8().data());

        auto unknown = await(*view, "fetch('nowhere://x/y').then(() => 'loaded')"_s);
        passed &= check(unknown.startsWith("error:"_s), "unregistered scheme fails: %s", unknown.utf8().data());

        // http over curl, from the app's page.
        auto remote = await(*view, makeString("fetch('"_s, http, "/api.json').then(r => r.json()).then(j => j.remote)"_s));
        passed &= check(remote == "true"_s, "fetch over http with CORS: %s", remote.utf8().data());
        // Cross-origin, WebCore restarts the request at the redirect's target;
        // patch 0017 keeps Response.redirected true.
        auto redirected = await(*view, makeString("fetch('"_s, http, "/redirect').then(async r => `${r.redirected} ${new URL(r.url).pathname} ${(await r.json()).remote}`)"_s));
        passed &= check(redirected == "true /api.json true"_s, "http redirect: %s", redirected.utf8().data());
        auto direct = await(*view, makeString("fetch('"_s, http, "/api.json').then(r => r.redirected)"_s));
        passed &= check(direct == "false"_s, "no redirect, not redirected: %s", direct.utf8().data());
        auto blocked = await(*view, makeString("fetch('"_s, http, "/private.json').then(r => r.status)"_s));
        passed &= check(blocked.startsWith("error:"_s), "no CORS header, no read: %s", blocked.utf8().data());
        auto intoApp = await(*view, makeString("fetch('"_s, http, "/to-app').then(r => r.status)"_s));
        passed &= check(intoApp.startsWith("error:"_s), "a server cannot redirect into app://: %s", intoApp.utf8().data());

        // Iframes.
        passed &= check(load(*view, client, "app://localhost/frames.html"_s), "a page with an iframe loads");
        runUntil([&] { return view->evaluateScript("String(window.innerLoaded)"_s) != "undefined"_s; });
        passed &= check(view->evaluateScript("String(window.innerLoaded)"_s) == "app://localhost/inner.html"_s, "the iframe loaded and ran: %s", view->evaluateScript("String(window.innerLoaded)"_s).utf8().data());
        passed &= check(view->evaluateScript("frames[0].document.title"_s) == "inner"_s, "its document: %s", view->evaluateScript("frames[0].document.title"_s).utf8().data());

        // A single-page application's client-side routes.
        fs->setFallback("/index.html"_s);
        passed &= check(load(*view, client, "app://localhost/settings/profile"_s), "a client-side route loads");
        passed &= check(view->evaluateScript("document.title + ' ' + location.pathname"_s) == "Loader test /settings/profile"_s,
            "with the fallback page at its own URL: %s", view->evaluateScript("document.title + ' ' + location.pathname"_s).utf8().data());
        auto missingScript = await(*view, "fetch('/missing/app.js').then(r => r.status)"_s);
        passed &= check(missingScript == "404"_s, "a missing file with an extension is still a 404: %s", missingScript.utf8().data());

        // Directories.
        passed &= check(load(*view, client, "app://localhost/sub"_s), "directory without its slash loads");
        passed &= check(view->evaluateScript("location.href"_s) == "app://localhost/sub/"_s && view->evaluateScript("document.title"_s) == "sub"_s,
            "redirected to its index.html: %s", view->evaluateScript("location.href"_s).utf8().data());

        // A main-frame navigation over http.
        passed &= check(load(*view, client, makeString(http, "/page.html"_s)), "http page loads");
        passed &= check(view->evaluateScript("document.title"_s) == "From HTTP"_s, "its title: %s", view->evaluateScript("document.title"_s).utf8().data());
        passed &= check(view->evaluateScript("String(isSecureContext)"_s) == "true"_s, "loopback http is a secure context");

        // Cookies: from responses, to requests, HttpOnly hidden from scripts.
        await(*view, "fetch('/set-cookie').then(r => r.text())"_s);
        auto visible = view->evaluateScript("document.cookie"_s);
        passed &= check(visible == "visible=1"_s, "Set-Cookie stored, HttpOnly hidden from document.cookie: '%s'", visible.utf8().data());
        view->evaluateScript("document.cookie = 'script=3; path=/'"_s);
        auto sent = await(*view, "fetch('/echo-cookie').then(r => r.text())"_s);
        passed &= check(sent.contains("visible=1"_s) && sent.contains("hidden=2"_s) && sent.contains("script=3"_s), "cookies sent with requests: '%s'", sent.utf8().data());

        // WebSockets.
        auto ws = makeString("ws://127.0.0.1:"_s, server.port());
        auto echo = await(*view, makeString("new Promise(resolve => { const log = []; const ws = new WebSocket('"_s, ws, "/echo', ['chat']);"
            "ws.binaryType = 'arraybuffer';"
            "ws.onopen = () => { log.push('open ' + ws.protocol); ws.send('hello'); ws.send(new Uint8Array([1, 2, 3])); ws.send('x'.repeat(100000)); };"
            "ws.onmessage = e => { log.push(typeof e.data === 'string' ? (e.data.length > 1000 ? 'long ' + e.data.length : e.data) : 'binary ' + [...new Uint8Array(e.data)].join(','));"
            "  if (log.length === 5) ws.close(1000, 'bye'); };"
            "ws.onerror = () => log.push('error');"
            "ws.onclose = e => { log.push(`close ${e.code} ${e.reason} ${e.wasClean}`); resolve(log.join('|')); }; })"_s));
        passed &= check(echo.startsWith("open chat|welcome, cookies: "_s) && echo.contains("hidden=2"_s)
            && echo.endsWith("|echo: hello|binary 1,2,3|long 100006|close 1000 bye true"_s), "WebSocket echo: %s", echo.utf8().data());
        auto blobMessage = await(*view, makeString("new Promise(resolve => { const ws = new WebSocket('"_s, ws, "/echo'); ws.binaryType = 'arraybuffer'; let first = true;"
            "ws.onopen = () => ws.send(new Blob([new Uint8Array([7, 8, 9])]));"
            "ws.onmessage = e => { if (first) { first = false; return; } resolve([...new Uint8Array(e.data)].join(',')); ws.close(); }; })"_s));
        passed &= check(blobMessage == "7,8,9"_s, "a Blob sent over a WebSocket: %s", blobMessage.utf8().data());
        auto socketCookie = view->evaluateScript("document.cookie"_s);
        passed &= check(socketCookie.contains("fromsocket=4"_s), "the handshake's Set-Cookie is stored: %s", socketCookie.utf8().data());
        auto rejected = await(*view, makeString("new Promise(resolve => { const log = []; const ws = new WebSocket('"_s, ws, "/ws-reject');"
            "ws.onerror = () => log.push('error'); ws.onclose = e => { log.push(`close ${e.code} ${e.wasClean}`); resolve(log.join('|')); }; })"_s));
        passed &= check(rejected == "error|close 1006 false"_s, "a refused handshake fails: %s", rejected.utf8().data());
        auto unreachable = await(*view, makeString("new Promise(resolve => { const log = []; const ws = new WebSocket('ws://127.0.0.1:"_s, unusedPort(), "/');"
            "ws.onerror = () => log.push('error'); ws.onclose = e => { log.push(`close ${e.code}`); resolve(log.join('|')); }; })"_s));
        passed &= check(unreachable == "error|close 1006"_s, "an unreachable server fails: %s", unreachable.utf8().data());

        // Synchronous XMLHttpRequest over http: on its own curl handle.
        constexpr auto syncXHR = "(method, url, body) => { const x = new XMLHttpRequest(); x.open(method, url, false); x.send(body); "
            "return `${x.status} ${new URL(x.responseURL).pathname} ${x.responseText}`; }"_s;
        auto syncGet = view->evaluateScript(makeString('(', syncXHR, ")('GET', '/api.json')"_s));
        passed &= check(syncGet == "200 /api.json {\"remote\":true}"_s, "synchronous XHR over http: %s", syncGet.utf8().data());
        auto syncRedirect = view->evaluateScript(makeString('(', syncXHR, ")('GET', '/redirect')"_s));
        passed &= check(syncRedirect == "200 /api.json {\"remote\":true}"_s, "follows redirects: %s", syncRedirect.utf8().data());
        auto syncPost = view->evaluateScript(makeString('(', syncXHR, ")('POST', '/echo-body', 'posted body')"_s));
        passed &= check(syncPost == "200 /echo-body posted body"_s, "sends a body: %s", syncPost.utf8().data());
        auto syncCookies = view->evaluateScript(makeString('(', syncXHR, ")('GET', '/echo-cookie')"_s));
        passed &= check(syncCookies.contains("hidden=2"_s), "and cookies: %s", syncCookies.utf8().data());

        // HTTP authentication: a wrong password, then the right one.
        client.credentials.append({ "user"_s, "wrong"_s });
        client.credentials.append({ "user"_s, "secret"_s });
        passed &= check(load(*view, client, makeString(http, "/protected"_s)) && view->evaluateScript("document.title"_s) == "welcome"_s,
            "a protected page loads with the right credential: %s", view->evaluateScript("document.title"_s).utf8().data());
        auto challenges = makeStringByJoining(client.challenges.span(), "|"_s);
        passed &= check(challenges == "zerog 0|zerog 1"_s, "asked twice, the second time after a failure: %s", challenges.utf8().data());
        auto again = await(*view, "fetch('/protected').then(r => r.status)"_s);
        passed &= check(again == "200"_s && client.challenges.size() == 2, "the session keeps the credential: %s, %zu challenges", again.utf8().data(), client.challenges.size());
        passed &= check(load(*view, client, makeString(http, "/protected-other"_s)) && view->evaluateScript("document.title"_s) == "denied"_s,
            "no credential given: the server's 401 page: %s", view->evaluateScript("document.title"_s).utf8().data());

        int loads = client.loads;
        view->loadURL(URL { makeString(http, "/nothing-here"_s) });
        runUntil([&] { return client.loads > loads; });
        passed &= check(client.loads > loads, "a 404 page still finishes loading");

        // https on the real network, where the system's trust store is used (the
        // test server above is plain http). Opt-in: ZEROG_ONLINE_TESTS=1.
        if (auto* online = std::getenv("ZEROG_ONLINE_TESTS"); online && *online == '1') {
            passed &= check(load(*view, client, "https://example.com/"_s), "https://example.com/ loads (certificate verified)");
            passed &= check(view->evaluateScript("document.title"_s).contains("Example"_s), "its title: %s", view->evaluateScript("document.title"_s).utf8().data());
            auto secure = await(*view, "new Promise(resolve => { const ws = new WebSocket('wss://ws.postman-echo.com/raw');"
                "ws.onopen = () => ws.send('over tls'); ws.onmessage = e => { resolve(e.data); ws.close(); };"
                "ws.onerror = () => resolve('error'); })"_s);
            passed &= check(secure == "over tls"_s, "wss:// echo (certificate verified): %s", secure.utf8().data());
        }
    }

    // Registered fonts: one session's pages see them, ahead of system fonts.
    {
        auto readFile = [](const char* path) {
            Vector<uint8_t> data;
            if (auto* file = std::fopen(path, "rb")) {
                uint8_t buffer[65536];
                while (auto read = std::fread(buffer, 1, sizeof buffer, file))
                    data.append(std::span { buffer, read });
                std::fclose(file);
            }
            return data;
        };
        auto ahem = readFile(ZEROG_TEST_FONTS "/resources/fonts/ahem.ttf");
        auto roboto = readFile(ZEROG_TEST_FONTS "/modules/canvaskit/tests/assets/Roboto-Regular.woff2");

        auto fontSession = ZeroG::Session::create();
        auto fontFiles = ZeroG::MemoryFS::create();
        fontFiles->addFile("/fonts.html"_s, bytes("<!DOCTYPE html><span id='a' style='font: 20px Ahem'>XXX</span><br><span id='r' style='font: 20px Roboto'>X</span>"));
        fontSession->registerScheme("app"_s, fontFiles.copyRef());
        auto registered = fontSession->registerFont(ahem.span());
        passed &= check(registered && *registered == "Ahem"_s, "register a TrueType font: %s", (registered ? *registered : registered.error()).utf8().data());
        auto garbage = fontSession->registerFont(bytes("not a font"));
        passed &= check(!garbage, "data that is not a font is refused: %s", (garbage ? String { } : garbage.error()).utf8().data());

        ZeroG::View::Configuration configuration;
        configuration.session = fontSession.copyRef();
        auto view = ZeroG::View::create(WTF::move(configuration));
        Client client;
        view->setClient(&client);
        passed &= check(load(*view, client, "app://localhost/fonts.html"_s), "a page of the session loads");
        // Ahem's glyphs are 1em squares: three of them at 20px are 60px wide.
        // Web fonts load when used; wait for it, then lay out.
        auto width = await(*view, "document.fonts.load('20px Ahem').then(() => Math.round(document.getElementById('a').getBoundingClientRect().width))"_s);
        passed &= check(width == "60"_s, "the page uses the registered font: %s px", width.utf8().data());

        // Registered while the page is open: the page sees it too. WOFF2 is read as sfnt.
        auto robotoFamily = fontSession->registerFont(roboto.span());
        passed &= check(robotoFamily && *robotoFamily == "Roboto"_s, "register a WOFF2 font: %s", (robotoFamily ? *robotoFamily : robotoFamily.error()).utf8().data());
        auto loaded = await(*view, "document.fonts.load('20px Roboto').then(faces => faces.length + ' ' + document.fonts.check('20px Roboto'))"_s);
        passed &= check(loaded == "1 true"_s, "an open page gets a font registered later: %s", loaded.utf8().data());
        // Another session's pages do not see them.
        auto otherFiles = ZeroG::MemoryFS::create();
        otherFiles->addFile("/fonts.html"_s, bytes("<!DOCTYPE html><span id='a' style='font: 20px Ahem, monospace'>XXX</span>"));
        auto otherSession = ZeroG::Session::create();
        otherSession->registerScheme("app"_s, otherFiles.copyRef());
        ZeroG::View::Configuration otherConfiguration;
        otherConfiguration.session = otherSession.copyRef();
        auto otherView = ZeroG::View::create(WTF::move(otherConfiguration));
        Client otherClient;
        otherView->setClient(&otherClient);
        load(*otherView, otherClient, "app://localhost/fonts.html"_s);
        auto otherWidth = await(*otherView, "document.fonts.load('20px Ahem').then(() => Math.round(document.getElementById('a').getBoundingClientRect().width))"_s);
        passed &= check(otherWidth != "60"_s, "another session's page does not: %s px", otherWidth.utf8().data());
    }

    passed &= check(!released, "an external file stays while the file system lives");

    {
        bool releasedOnce = false;
        RefPtr other = ZeroG::MemoryFS::create();
        other->addFile("/x.bin"_s, media.span(), { }, [&releasedOnce] { releasedOnce = true; });
        bool notAdded = false;
        other->addFile("/x.bin"_s, media.span(), { }, [&notAdded] { notAdded = true; });
        passed &= check(!notAdded, "a refused external file is not released");
        other = nullptr;
        passed &= check(releasedOnce, "an external file is released with its file system");
    }

    std::printf("%s\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
