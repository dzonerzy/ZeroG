// The Web Inspector (dev and debug profiles): the frontend opens in a view the
// embedder creates, connects to the page and shows its console; it closes
// itself and with the inspected view; the remote inspector's server lists the
// views to a client on its port.

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
#include "View.h"
#include "inspector/InspectorZeroG.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

using namespace WebCore;

namespace {

#define check(condition, ...) [&] {                    \
        bool result = (condition);                     \
        std::printf("%s ", result ? "ok  " : "FAIL");  \
        std::printf(__VA_ARGS__);                      \
        std::printf("\n");                             \
        return result;                                 \
    }()

// Opens the frontend in a CPU view, as an embedder would in a window.
class Client final : public ZeroG::ViewClient {
public:
    ZeroG::View* createInspectorView(ZeroG::Session& session) final
    {
        ZeroG::View::Configuration configuration;
        configuration.size = { 1000, 700 };
        configuration.session = &session;
        frontend = ZeroG::View::create(WTF::move(configuration));
        frontend->setClient(&frontendClient);
        ++opened;
        return frontend.get();
    }

    void closeInspectorView(ZeroG::View& view) final
    {
        if (&view == frontend.get()) {
            ++closed;
            frontend = nullptr;
        }
    }

    void bringInspectorViewToFront(ZeroG::View&) final { ++broughtToFront; }

    class FrontendClient final : public ZeroG::ViewClient {
        void consoleMessage(JSC::MessageSource, JSC::MessageLevel, const String& message, unsigned, unsigned, const String&, const String&) final
        {
            std::printf("     frontend console: %s\n", message.utf8().data());
        }
    } frontendClient;

    std::unique_ptr<ZeroG::View> frontend;
    int opened { 0 };
    int closed { 0 };
    int broughtToFront { 0 };
};

template<typename Condition>
bool runUntil(Condition&& condition, ZeroG::View& view, Client& client, std::chrono::milliseconds timeout = std::chrono::seconds(30))
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        ZeroG::runPendingWork();
        view.render(MonotonicTime::now());
        if (client.frontend)
            client.frontend->render(MonotonicTime::now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

#if defined(_WIN32)
using Socket = SOCKET;
void closeSocket(Socket socket) { closesocket(socket); }
#else
using Socket = int;
void closeSocket(Socket socket) { close(socket); }
#endif

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

// A client of the remote inspector's protocol (RemoteInspectorMessageParser: a
// big-endian 32-bit length, then a JSON event): asks for the target list and
// keeps what the server sends.
class RemoteClient {
public:
    explicit RemoteClient(int port)
    {
        m_thread = std::thread([this, port] { run(port); });
    }

    ~RemoteClient()
    {
        m_stopping = true;
        if (m_socket != static_cast<Socket>(-1))
            shutdown(m_socket, 2);
        m_thread.join();
    }

    std::string received()
    {
        std::lock_guard lock { m_lock };
        return m_received;
    }

private:
    void run(int port)
    {
        m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address { };
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        if (connect(m_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)))
            return;
        std::string event = "{\"event\":\"SetupInspectorClient\"}";
        uint32_t size = htonl(static_cast<uint32_t>(event.size()));
        std::string message(reinterpret_cast<const char*>(&size), 4);
        message += event;
        send(m_socket, message.data(), static_cast<int>(message.size()), 0);
        char buffer[65536];
        while (!m_stopping) {
            auto length = recv(m_socket, buffer, sizeof(buffer), 0);
            if (length <= 0)
                break;
            std::lock_guard lock { m_lock };
            m_received.append(buffer, length);
        }
        closeSocket(m_socket);
    }

    Socket m_socket { static_cast<Socket>(-1) };
    std::thread m_thread;
    std::atomic<bool> m_stopping { false };
    std::mutex m_lock;
    std::string m_received;
};

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    ZeroG::initializeProcess();
#if defined(_WIN32)
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
    bool passed = true;

    if (!ZeroG::ViewInspector::isAvailable()) {
        std::printf("     this profile has no Web Inspector\n");
        auto view = ZeroG::View::create({ });
        passed &= check(!view->inspector() && !view->showInspector(), "no inspector, nothing opens");
        std::printf("%s\n", passed ? "PASS" : "FAIL");
        return passed ? 0 : 1;
    }

    // The remote inspector's server, before the views it lists.
    int port = unusedPort();
    passed &= check(ZeroG::startRemoteInspectorServer("127.0.0.1"_s, port), "the remote inspector listens on 127.0.0.1:%d", port);

    Client client;
    auto view = ZeroG::View::create({ });
    view->setClient(&client);
    view->loadHTML("<!DOCTYPE html><title>Inspected page</title><p id=p>hello"_s, URL { "https://inspected.example/"_s });
    view->render(MonotonicTime::now());

    // The frontend loads and connects: its page target is the inspected page.
    passed &= check(view->showInspector() && client.frontend && view->inspector()->frontendView() == client.frontend.get(), "the inspector opens in the embedder's view");
    auto frontendState = [&] {
        return client.frontend ? client.frontend->evaluateScript("typeof WI === 'object' && WI.pageTarget ? WI.pageTarget.identifier : ''"_s) : String { };
    };
    bool connected = runUntil([&] { return !frontendState().isEmpty(); }, *view, client);
    passed &= check(connected, "the frontend connects to the page target: %s", frontendState().utf8().data());

    if (connected) {
        // The page's console reaches the frontend's model.
        client.frontend->evaluateScript("window.__messages = []; WI.consoleManager.addEventListener(WI.ConsoleManager.Event.MessageAdded, e => __messages.push(e.data.message.messageText)); 0"_s);
        view->evaluateScript("console.log('hello inspector')"_s);
        auto messages = [&] { return client.frontend ? client.frontend->evaluateScript("__messages.join('|')"_s) : String { }; };
        passed &= check(runUntil([&] { return messages().contains("hello inspector"_s); }, *view, client), "a console.log in the page shows in the frontend: %s", messages().utf8().data());

        // The DOM: the frontend asks the page for its document.
        auto title = [&] { return client.frontend ? client.frontend->evaluateScript("WI.networkManager.mainFrame ? WI.networkManager.mainFrame.mainResource.url : ''"_s) : String { }; };
        passed &= check(runUntil([&] { return title().contains("inspected.example"_s); }, *view, client), "and its main frame: %s", title().utf8().data());
    }

    // Shown again: brought to the front, not opened twice.
    int broughtToFront = client.broughtToFront;
    view->showInspector();
    passed &= check(client.opened == 1 && client.broughtToFront == broughtToFront + 1, "showing it again brings it to the front");

    // The frontend closes itself (its close button): the view goes after its script.
    if (client.frontend)
        client.frontend->evaluateScript("InspectorFrontendHost.closeWindow(); 0"_s);
    passed &= check(runUntil([&] { return !client.frontend; }, *view, client, std::chrono::seconds(5)) && client.closed == 1 && !view->inspector()->frontendView(), "the frontend closes itself");
    passed &= check(view->evaluateScript("document.title"_s) == "Inspected page"_s, "the page goes on");

    // Reopened, then the embedder destroys the frontend's view (its window closed).
    passed &= check(view->showInspector() && client.opened == 2, "it opens again");
    client.frontend = nullptr;
    passed &= check(!view->inspector()->frontendView(), "the embedder closing it is seen");
    runUntil([] { return false; }, *view, client, std::chrono::milliseconds(200));

    // The remote inspector lists the view (and the frontend's own view, if open).
    {
        RemoteClient remote(port);
        bool listed = runUntil([&] { return remote.received().find("SetTargetList") != std::string::npos && remote.received().find("Inspected page") != std::string::npos; }, *view, client, std::chrono::seconds(10));
        auto received = remote.received();
        for (auto& character : received) {
            if (static_cast<unsigned char>(character) < 0x20)
                character = '.';
        }
        passed &= check(listed, "a remote client gets the target list with the page: %s", received.substr(0, 400).c_str());
        // The backend's protocol comes first, so the remote frontend speaks this engine's.
        passed &= check(received.find("\"event\":\"BackendCommands\",\"message\":\"\"") == std::string::npos && received.find("InspectorBackend") != std::string::npos,
            "with the backend's protocol (%zu bytes received)", received.size());
    }

    // The inspected view goes with its inspector open: the frontend closes.
    view->showInspector();
    runUntil([&] { return !frontendState().isEmpty(); }, *view, client);
    view = nullptr;
    passed &= check(!client.frontend && client.closed == 2, "destroying the inspected view closes the inspector");

    std::printf("%s\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
