// Accessibility on Linux (ZeroG.md, "Accessibilità"): an AT-SPI client, on a
// thread and a D-Bus connection of its own as a screen reader would be, finds
// the application through the AT-SPI registry, walks to the page's controls by
// name and uses them: a button's action, a text field's text read and set, a
// check box's action and state, a slider's value; grabbing the focus on the
// text field sends its focused event.
//
// Usage: accessibility   (needs a display and an accessibility bus:
//        dbus-run-session -- xvfb-run -a accessibility, with at-spi2-core)

#include "config.h"
#include "Embedding.h"
#include "accessibility/DBus.h"
#include "window/Window.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>
#include <wtf/text/WTFString.h>

using namespace WebCore;
using namespace ZeroG::DBus;

namespace {

constexpr auto page = R"HTML(<!DOCTYPE html>
<html><body>
  <button id="button" onclick="window.clicks = (window.clicks || 0) + 1">Press me</button>
  <input id="field" aria-label="Your name" value="abc">
  <input id="box" type="checkbox" aria-label="Agree">
  <input id="volume" type="range" min="0" max="10" value="3" aria-label="Volume">
</body></html>)HTML";

#define check(condition, ...) [&] {                    \
        bool result = (condition);                     \
        std::printf("%s ", result ? "ok  " : "FAIL");  \
        std::printf(__VA_ARGS__);                      \
        std::printf("\n");                             \
        return result;                                 \
    }()

bool runUntil(ZeroG::Window& window, Seconds timeout, const std::function<bool()>& condition)
{
    auto deadline = MonotonicTime::now() + timeout;
    while (!condition()) {
        if (MonotonicTime::now() > deadline || !window.runOnce(10_ms))
            return false;
    }
    return true;
}

struct Reference {
    String name;
    String path;
};

class Client {
public:
    explicit Client(Connection* connection)
        : m_library(*Library::get())
        , m_connection(connection)
    {
    }

    // A method call; null on error.
    Message* call(const Reference& target, const char* interface, const char* member, const std::function<void(Writer&)>& arguments = { })
    {
        auto* message = m_library.message_new_method_call(data(target.name.utf8()), data(target.path.utf8()), interface, member);
        if (arguments) {
            Writer writer(message);
            arguments(writer);
        }
        Error error;
        m_library.error_init(&error);
        auto* reply = m_library.connection_send_with_reply_and_block(m_connection, message, 5000, &error);
        m_library.message_unref(message);
        m_library.error_free(&error);
        return reply;
    }

    Vector<Reference> children(const Reference& target)
    {
        Vector<Reference> result;
        auto* reply = call(target, "org.a11y.atspi.Accessible", "GetChildren");
        if (!reply)
            return result;
        Reader reader(reply);
        auto array = reader.recurse();
        while (array.type() == TypeStruct) {
            auto reference = array.recurse();
            auto name = reference.string();
            reference.next();
            result.append({ name, reference.string() });
            array.next();
        }
        m_library.message_unref(reply);
        return result;
    }

    // A property's variant, read by `read`.
    template<typename Read>
    bool property(const Reference& target, const char* interface, const char* name, Read&& read)
    {
        auto* reply = call(target, "org.freedesktop.DBus.Properties", "Get", [&](Writer& writer) {
            writer.string(interface);
            writer.string(name);
        });
        if (!reply)
            return false;
        Reader reader(reply);
        auto variant = reader.recurse();
        read(variant);
        m_library.message_unref(reply);
        return true;
    }

    String name(const Reference& target)
    {
        String result;
        property(target, "org.a11y.atspi.Accessible", "Name", [&](Reader& value) {
            result = value.string();
        });
        return result;
    }

    uint32_t role(const Reference& target)
    {
        uint32_t result = 0;
        if (auto* reply = call(target, "org.a11y.atspi.Accessible", "GetRole")) {
            result = Reader(reply).uint32();
            m_library.message_unref(reply);
        }
        return result;
    }

    uint64_t state(const Reference& target)
    {
        uint64_t result = 0;
        if (auto* reply = call(target, "org.a11y.atspi.Accessible", "GetState")) {
            Reader reader(reply);
            auto words = reader.recurse();
            uint64_t low = words.uint32();
            words.next();
            uint64_t high = words.uint32();
            result = low | high << 32;
            m_library.message_unref(reply);
        }
        return result;
    }

    bool booleanCall(const Reference& target, const char* interface, const char* member, const std::function<void(Writer&)>& arguments = { })
    {
        bool result = false;
        if (auto* reply = call(target, interface, member, arguments)) {
            result = Reader(reply).boolean();
            m_library.message_unref(reply);
        }
        return result;
    }

    String stringCall(const Reference& target, const char* interface, const char* member, const std::function<void(Writer&)>& arguments = { })
    {
        String result;
        if (auto* reply = call(target, interface, member, arguments)) {
            result = Reader(reply).string();
            m_library.message_unref(reply);
        }
        return result;
    }

private:
    const Library& m_library;
    Connection* m_connection;
};

String programName()
{
    char buffer[64] = { };
    if (auto* file = fopen("/proc/self/comm", "r")) {
        if (!fgets(buffer, sizeof(buffer), file))
            buffer[0] = '\0';
        fclose(file);
    }
    auto name = String::fromUTF8(buffer);
    return name.endsWith('\n') ? name.left(name.length() - 1) : name;
}

struct Results {
    bool connected { false };
    bool foundApplication { false };
    bool foundButton { false };
    uint32_t buttonRole { 0 };
    bool pressed { false };
    String fieldText;
    bool setField { false };
    bool focusEvent { false };
    bool toggled { false };
    double volume { -1 };
    double volumeMaximum { -1 };
    std::atomic<bool> done { false };
};

Connection* connectToAccessibilityBus(const Library& library)
{
    Error error;
    library.error_init(&error);
    auto* session = library.bus_get_private(SessionBus, &error);
    library.error_free(&error);
    if (!session)
        return nullptr;
    library.connection_set_exit_on_disconnect(session, false);
    auto* request = library.message_new_method_call("org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus", "GetAddress");
    auto* reply = library.connection_send_with_reply_and_block(session, request, 5000, &error);
    library.error_free(&error);
    library.message_unref(request);
    String address;
    if (reply) {
        address = Reader(reply).string();
        library.message_unref(reply);
    }
    library.connection_close(session);
    library.connection_unref(session);
    if (address.isEmpty())
        return nullptr;
    auto* connection = library.connection_open_private(data(address.utf8()), &error);
    library.error_free(&error);
    if (!connection)
        return nullptr;
    library.connection_set_exit_on_disconnect(connection, false);
    if (!library.bus_register(connection, &error)) {
        library.error_free(&error);
        library.connection_close(connection);
        library.connection_unref(connection);
        return nullptr;
    }
    return connection;
}

void runClient(Results& results)
{
    auto& library = *Library::get();
    auto* connection = connectToAccessibilityBus(library);
    results.connected = !!connection;
    if (!connection) {
        results.done = true;
        return;
    }
    Client client(connection);

    // The application, among the desktop's children, once it embedded itself.
    Reference desktop { "org.a11y.atspi.Registry"_s, "/org/a11y/atspi/accessible/root"_s };
    Reference applicationReference;
    auto self = programName();
    for (int attempt = 0; attempt < 100 && applicationReference.path.isNull(); ++attempt) {
        for (auto& child : client.children(desktop)) {
            if (client.name(child) == self) {
                applicationReference = child;
                break;
            }
        }
        if (applicationReference.path.isNull())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    results.foundApplication = !applicationReference.path.isNull();

    // The controls, by name, anywhere under the application.
    std::optional<Reference> button, field, box, volume;
    if (results.foundApplication) {
        for (int attempt = 0; attempt < 50 && !(button && field && box && volume); ++attempt) {
            Vector<Reference> queue { applicationReference };
            for (size_t i = 0; i < queue.size() && i < 1000; ++i) {
                auto name = client.name(queue[i]);
                if (name == "Press me"_s)
                    button = queue[i];
                else if (name == "Your name"_s)
                    field = queue[i];
                else if (name == "Agree"_s)
                    box = queue[i];
                else if (name == "Volume"_s)
                    volume = queue[i];
                queue.appendVector(client.children(queue[i]));
            }
            if (!(button && field && box && volume))
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    if (button) {
        results.foundButton = true;
        results.buttonRole = client.role(*button);
        results.pressed = client.booleanCall(*button, "org.a11y.atspi.Action", "DoAction", [](Writer& writer) {
            writer.int32(0);
        });
    }
    if (field) {
        // The focus moves there: a screen reader hears of it.
        Error error;
        library.error_init(&error);
        library.bus_add_match(connection, "type='signal',interface='org.a11y.atspi.Event.Object',member='StateChanged'", &error);
        library.error_free(&error);
        client.booleanCall(*field, "org.a11y.atspi.Component", "GrabFocus");
        auto deadline = MonotonicTime::now() + 5_s;
        while (!results.focusEvent && MonotonicTime::now() < deadline && library.connection_read_write(connection, 50)) {
            while (auto* message = library.connection_pop_message(connection)) {
                if (library.message_get_type(message) == Signal && field->path == String::fromUTF8(library.message_get_path(message))) {
                    Reader reader(message);
                    auto detail = reader.string();
                    reader.next();
                    if (detail == "focused"_s && reader.int32() == 1)
                        results.focusEvent = true;
                }
                library.message_unref(message);
            }
        }

        results.fieldText = client.stringCall(*field, "org.a11y.atspi.Text", "GetText", [](Writer& writer) {
            writer.int32(0);
            writer.int32(-1);
        });
        results.setField = client.booleanCall(*field, "org.a11y.atspi.EditableText", "SetTextContents", [](Writer& writer) {
            writer.string("xyz");
        });
    }
    if (box) {
        client.booleanCall(*box, "org.a11y.atspi.Action", "DoAction", [](Writer& writer) {
            writer.int32(0);
        });
        results.toggled = client.state(*box) & (1llu << 4); // checked
    }
    if (volume) {
        client.property(*volume, "org.a11y.atspi.Value", "CurrentValue", [&](Reader& value) {
            results.volume = value.float64();
        });
        client.property(*volume, "org.a11y.atspi.Value", "MaximumValue", [&](Reader& value) {
            results.volumeMaximum = value.float64();
        });
    }

    library.connection_close(connection);
    library.connection_unref(connection);
    results.done = true;
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (!getenv("DBUS_SESSION_BUS_ADDRESS") || !Library::get()) {
        std::printf("SKIP no session bus or no libdbus: run under dbus-run-session, with at-spi2-core\n");
        return 0;
    }
    bool ok = true;

    auto window = ZeroG::Window::create({ "ZeroG accessibility test"_s, { 400, 200 }, true, false, true });
    if (!check(!!window, "window with a GPU %s", ZeroG::Window::lastError().utf8().data()))
        return 1;
    window->view().loadHTML(String::fromUTF8(page));
    window->view().setFocused(true);
    runUntil(*window, 500_ms, [] { return false; });

    // The client works while this thread runs the window's loop, where the
    // bridge answers its calls.
    Results results;
    std::thread client(runClient, std::ref(results));
    bool finished = runUntil(*window, 90_s, [&] { return results.done.load(); });
    client.join();
    ok &= check(finished, "the client finished");

    ok &= check(results.connected, "the client reaches the accessibility bus");
    ok &= check(results.foundApplication, "the application is among the registry's children");
    ok &= check(results.foundButton && results.buttonRole == 43, "the button is found by its name, as a push button (role %u)", results.buttonRole);
    ok &= check(results.pressed, "its action is done");
    runUntil(*window, 1_s, [&] { return window->view().evaluateScript("String(window.clicks)"_s) == "1"_s; });
    auto clicks = window->view().evaluateScript("String(window.clicks)"_s);
    ok &= check(clicks == "1"_s, "the page saw the click: %s", clicks.utf8().data());
    ok &= check(results.fieldText == "abc"_s, "the text field's text: '%s'", results.fieldText.utf8().data());
    auto field = window->view().evaluateScript("document.getElementById('field').value"_s);
    ok &= check(results.setField && field == "xyz"_s, "text set through AT-SPI reaches the page: '%s'", field.utf8().data());
    ok &= check(results.focusEvent, "grabbing the focus sends the field's focused event");
    auto checked = window->view().evaluateScript("String(document.getElementById('box').checked)"_s);
    ok &= check(results.toggled && checked == "true"_s, "the check box is checked: %s", checked.utf8().data());
    ok &= check(results.volume == 3 && results.volumeMaximum == 10, "the slider's value: %g of %g", results.volume, results.volumeMaximum);

    window = nullptr;
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
