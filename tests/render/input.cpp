// Input (texture and CPU modes; window mode feeds the same View calls): mouse
// clicks and hover, focus, typing and editing commands in a text field, keyboard
// event properties, wheel scrolling. Each is checked from the page's side.
//
// Usage: input

#include "config.h"
#include "Embedding.h"
#include "View.h"

#include <WebCore/WindowsKeyboardCodes.h>
#include <cstdio>
#include <thread>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

using namespace WebCore;

namespace {

constexpr auto page = R"HTML(<!DOCTYPE html>
<html><head><style>
  body { margin: 0; height: 3000px; background: white; }
  #button { position: absolute; left: 20px; top: 20px; width: 100px; height: 40px; }
  #field { position: absolute; left: 20px; top: 100px; width: 200px; height: 30px; font: 16px sans-serif; }
  #hover { position: absolute; left: 300px; top: 20px; width: 50px; height: 50px; background: rgb(0, 0, 255); }
  #hover:hover { background: rgb(255, 0, 0); }
</style></head><body>
  <button id="button">Click</button>
  <input id="field">
  <div id="hover"></div>
  <script>
    window.events = [];
    document.getElementById("button").addEventListener("click", e => events.push(`click ${e.clientX},${e.clientY} ${e.detail}`));
    addEventListener("keydown", e => events.push(`keydown ${e.key} ${e.code} ${e.keyCode}`));
    addEventListener("wheel", e => events.push(`wheel ${e.deltaY}`));
  </script>
</body></html>)HTML";

#define check(condition, ...) [&] {                    \
        bool result = (condition);                     \
        std::printf("%s ", result ? "ok  " : "FAIL");  \
        std::printf(__VA_ARGS__);                      \
        std::printf("\n");                             \
        return result;                                 \
    }()

using Modifier = PlatformEvent::Modifier;

void click(ZeroG::View& view, FloatPoint point, int clickCount = 1)
{
    view.mouseMove(point, ZeroG::View::MouseButton::None, { });
    view.mouseDown(point, ZeroG::View::MouseButton::Left, clickCount, { });
    view.mouseUp(point, ZeroG::View::MouseButton::Left, clickCount, { });
}

// A key that types `text` (empty for keys that do not).
void press(ZeroG::View& view, const String& key, const String& code, int keyCode, const String& text = { }, ZeroG::View::Modifiers modifiers = { })
{
    ZeroG::View::KeyEvent event { key, code, keyCode, false, false, modifiers };
    view.keyDown(event);
    if (!text.isEmpty())
        view.insertText(text, modifiers);
    view.keyUp(event);
}

void type(ZeroG::View& view, const String& text)
{
    for (auto character : StringView(text).codeUnits()) {
        auto letter = String::fromLatin1(std::span(reinterpret_cast<const char*>(&character), 1));
        auto upper = static_cast<char>(toASCIIUpper(character));
        press(view, letter, makeString("Key"_s, upper), upper, letter);
    }
}

uint32_t pixel(ZeroG::View& view, int x, int y)
{
    ZeroG::Frame frame;
    if (!view.surface().acquire(frame))
        return 0;
    auto* row = static_cast<const uint8_t*>(frame.pixels) + y * frame.stride;
    uint32_t bgra = row[x * 4] | row[x * 4 + 1] << 8 | row[x * 4 + 2] << 16;
    view.surface().release(frame);
    return bgra; // 0xRRGGBB
}

String script(ZeroG::View& view, ASCIILiteral source)
{
    return view.evaluateScript(String(source));
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool ok = true;

    auto view = ZeroG::View::create({ { 400, 300 }, 1, false, true, nullptr });
    view->loadHTML(String::fromUTF8(page));
    view->setFocused(true);
    auto frameTime = MonotonicTime::now();
    // One application frame: engine work (timers drive smooth scrolling), then a
    // render, 16ms apart in real time.
    auto render = [&] {
        frameTime += 16_ms;
        while (MonotonicTime::now() < frameTime)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ZeroG::runPendingWork();
        view->render(frameTime);
    };
    render();

    // Mouse.
    click(*view, { 70, 40 });
    auto events = script(*view, "events.join('|')"_s);
    ok &= check(events.contains("click 70,40 1"_s), "click on the button: %s", events.utf8().data());
    click(*view, { 70, 40 }, 2);
    ok &= check(script(*view, "events.filter(e => e.startsWith('click')).pop()"_s) == "click 70,40 2"_s, "double click count");

    view->mouseMove({ 325, 45 }, ZeroG::View::MouseButton::None, { });
    render();
    auto hovered = pixel(*view, 325, 45);
    ok &= check(hovered == 0xff0000, ":hover on the hovered box (%06x)", hovered);
    view->mouseLeave();
    render();
    auto left = pixel(*view, 325, 45);
    ok &= check(left == 0x0000ff, ":hover ends when the mouse leaves (%06x)", left);

    // Focus and typing.
    click(*view, { 120, 115 });
    ok &= check(script(*view, "document.activeElement.id"_s) == "field"_s, "click focuses the text field");
    type(*view, "hello"_s);
    ok &= check(script(*view, "field.value"_s) == "hello"_s, "typed text: '%s'", script(*view, "field.value"_s).utf8().data());
    ok &= check(script(*view, "events.includes('keydown h KeyH 72')"_s) == "true"_s, "keydown key, code and keyCode");
    press(*view, "Backspace"_s, "Backspace"_s, VK_BACK);
    ok &= check(script(*view, "field.value"_s) == "hell"_s, "Backspace deletes: '%s'", script(*view, "field.value"_s).utf8().data());
    press(*view, "ArrowLeft"_s, "ArrowLeft"_s, VK_LEFT);
    press(*view, "ArrowLeft"_s, "ArrowLeft"_s, VK_LEFT);
    type(*view, "X"_s);
    ok &= check(script(*view, "field.value"_s) == "heXll"_s, "arrow keys move the caret: '%s'", script(*view, "field.value"_s).utf8().data());
    press(*view, "a"_s, "KeyA"_s, 'A', { }, Modifier::ControlKey);
    type(*view, "new"_s);
    ok &= check(script(*view, "field.value"_s) == "new"_s, "Ctrl+A selects all, typing replaces it: '%s'", script(*view, "field.value"_s).utf8().data());
    press(*view, "z"_s, "KeyZ"_s, 'Z', { }, Modifier::ControlKey);
    auto undone = script(*view, "field.value"_s);
    ok &= check(undone != "new"_s, "Ctrl+Z undoes: '%s'", undone.utf8().data());
    press(*view, "z"_s, "KeyZ"_s, 'Z', { }, { Modifier::ControlKey, Modifier::ShiftKey });
    ok &= check(script(*view, "field.value"_s) == "new"_s, "Ctrl+Shift+Z redoes: '%s'", script(*view, "field.value"_s).utf8().data());

    // Wheel: 300 pixels down.
    view->wheel({ 200, 200 }, { 0, -300 }, { });
    for (int i = 0; i < 120 && script(*view, "scrollY"_s) != "300"_s; ++i)
        render();
    ok &= check(script(*view, "scrollY"_s) == "300"_s, "wheel scrolls the page: scrollY %s", script(*view, "scrollY"_s).utf8().data());
    ok &= check(script(*view, "events.some(e => e.startsWith('wheel ') && parseFloat(e.slice(6)) > 0)"_s) == "true"_s, "wheel event with deltaY > 0");

    std::printf(ok ? "PASS\n" : "FAILED\n");
    return ok ? 0 : 1;
}
