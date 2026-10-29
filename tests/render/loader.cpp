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

#include <atomic>
#include <chrono>
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
            std::string request;
            char buffer[4096];
            while (request.find("\r\n\r\n") == std::string::npos) {
                auto received = recv(client, buffer, sizeof(buffer), 0);
                if (received <= 0)
                    break;
                request.append(buffer, received);
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
            closeSocket(client);
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

        int loads = client.loads;
        view->loadURL(URL { makeString(http, "/nothing-here"_s) });
        runUntil([&] { return client.loads > loads; });
        passed &= check(client.loads > loads, "a 404 page still finishes loading");

        // https on the real network, where the system's trust store is used (the
        // test server above is plain http). Opt-in: ZEROG_ONLINE_TESTS=1.
        if (auto* online = std::getenv("ZEROG_ONLINE_TESTS"); online && *online == '1') {
            passed &= check(load(*view, client, "https://example.com/"_s), "https://example.com/ loads (certificate verified)");
            passed &= check(view->evaluateScript("document.title"_s).contains("Example"_s), "its title: %s", view->evaluateScript("document.title"_s).utf8().data());
        }
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
