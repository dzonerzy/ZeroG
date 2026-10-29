// Window mode (ZeroG.md, "Modalità" and "Ciclo di un frame"): the engine owns a
// native window, presents with the GPU at the display's refresh rate, and
// requestAnimationFrame follows the presentations. The presented image must match a
// CPU render of the page, and resizing the window must resize the page.
//
// Usage: window   (needs a display: a desktop, or Xvfb on Linux)

#include "config.h"
#include "Embedding.h"
#include "Window.h"

#include <WebCore/ColorSpace.h>
#include <WebCore/ImageBuffer.h>
#include <WebCore/Page.h>
#include <WebCore/PixelBuffer.h>
#include <WebCore/PixelBufferFormat.h>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <wtf/text/StringToIntegerConversion.h>
#include <wtf/text/WTFString.h>

using namespace WebCore;

namespace {

constexpr auto page = R"HTML(<!DOCTYPE html>
<html><head><style>
  html, body { margin: 0; background: rgb(240, 240, 255); }
  #box { position: absolute; top: 20px; left: 0; width: 50px; height: 50px; background: rgb(255, 0, 0); }
  p { position: absolute; top: 120px; left: 10px; margin: 0; font: 24px sans-serif; }
</style></head><body>
  <div id="box"></div>
  <p>window mode</p>
  <script>
    window.callbacks = 0;
    let start;
    function step(t) {
      if (start === undefined)
        start = t;
      ++callbacks;
      document.getElementById("box").style.left = (Math.round((t - start) / 16) * 5) % 350 + "px";
      requestAnimationFrame(step);
    }
    requestAnimationFrame(step);
  </script>
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
        if (MonotonicTime::now() > deadline || !window.runOnce(50_ms))
            return false;
    }
    return true;
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool ok = true;

    auto window = ZeroG::Window::create({ "ZeroG window test"_s, { 400, 200 }, true, false, true });
    if (!check(!!window, "window with a GPU %s", ZeroG::Window::lastError().utf8().data()))
        return 1;
    std::printf("     GPU: %s, refresh %.1f Hz, scale %.2f\n", ZeroG::gpuRenderer().utf8().data(), 1 / window->refreshInterval().seconds(), window->view().scale());
    window->view().loadHTML(String::fromUTF8(page));

    // One second of animation. requestAnimationFrame runs once per display
    // refresh; frames are presented when something changed (the box moves every
    // 16ms of animation time), each swap waiting for vsync.
    auto start = MonotonicTime::now();
    runUntil(*window, 1_s, [&] { return false; });
    auto elapsed = MonotonicTime::now() - start;
    auto callbacks = parseInteger<uint64_t>(window->view().evaluateScript("callbacks"_s)).value_or(0);
    double rate = callbacks / elapsed.seconds();
    double refresh = 1 / window->refreshInterval().seconds();
    ok &= check(rate <= refresh * 1.1 && rate >= refresh * 0.4, "rAF at the display's rate: %.0f per second at %.0f Hz", rate, refresh);
    ok &= check(window->presentedFrames() >= 30 && window->presentedFrames() <= callbacks, "presented %u frames for the animation's changes", window->presentedFrames());

    // The presented image against a CPU render, with the animation stopped.
    window->view().evaluateScript("step = () => { }; 0"_s);
    runUntil(*window, 1_s, [&] { return false; });
    window->view().invalidate({ { }, window->view().size() });
    window->captureNextFrame();
    Vector<uint8_t> pixels;
    IntSize size;
    ok &= check(runUntil(*window, 5_s, [&] { return window->takeCapture(pixels, size); }), "captured a presented frame (%dx%d)", size.width(), size.height());
    RefPtr reference = ZeroG::paint(window->view().page(), ZeroG::Rendering::CPU, window->view().scale());
    RefPtr expected = reference ? reference->getPixelBuffer({ AlphaPremultiplication::Unpremultiplied, PixelFormat::RGBA8, ColorSpace::SRGB() }, { { }, window->view().size() }) : nullptr;
    if (expected && !pixels.isEmpty()) {
        auto expectedSize = expected->size();
        unsigned different = 0;
        for (int y = 0; y < std::min(size.height(), expectedSize.height()); ++y) {
            for (int x = 0; x < std::min(size.width(), expectedSize.width()); ++x) {
                auto* a = pixels.span().data() + (static_cast<size_t>(y) * size.width() + x) * 4;
                auto* b = expected->bytes().data() + (static_cast<size_t>(y) * expectedSize.width() + x) * 4;
                if (std::abs(a[0] - b[0]) > 48 || std::abs(a[1] - b[1]) > 48 || std::abs(a[2] - b[2]) > 48)
                    ++different;
            }
        }
        ok &= check(different <= static_cast<unsigned>(size.width() * size.height() / 100), "presented frame matches a CPU render (%u pixels differ)", different);
    } else
        ok &= check(false, "CPU reference");

    // Resizing the window resizes the page.
    window->setSize({ 500, 300 });
    ok &= check(runUntil(*window, 5_s, [&] { return window->view().size() == IntSize(500, 300); }), "page resized with the window: %dx%d",
        window->view().size().width(), window->view().size().height());
    auto innerWidth = window->view().evaluateScript("innerWidth + 'x' + innerHeight"_s);
    ok &= check(innerWidth == "500x300"_s, "window.innerWidth/innerHeight: %s", innerWidth.utf8().data());

    std::printf(ok ? "PASS\n" : "FAILED\n");
    return ok ? 0 : 1;
}
