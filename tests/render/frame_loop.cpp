// The frame loop of texture and CPU modes (docs/design/graphics-presentation.md, "Frame cycle"): the
// application gives each frame's time, requestAnimationFrame follows it, and each
// frame repaints only what changed. The incrementally updated buffers must match a
// full repaint of the page.
//
// Usage: frame_loop

#include "config.h"
#include "Embedding.h"
#include "View.h"

#include <WebCore/ColorSpace.h>
#include <WebCore/ImageBuffer.h>
#include <WebCore/Page.h>
#include <WebCore/PixelBuffer.h>
#include <WebCore/PixelBufferFormat.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

using namespace WebCore;

namespace {

constexpr int width = 400;
constexpr int height = 200;
constexpr int frames = 20;
constexpr Seconds frameInterval = 16_ms;

// A 50x50 box moving 10px per 16ms frame, driven by requestAnimationFrame
// timestamps, over a static background with text.
constexpr auto page = R"HTML(<!DOCTYPE html>
<html><head><style>
  html, body { margin: 0; background: rgb(240, 240, 255); }
  #box { position: absolute; top: 20px; left: 0; width: 50px; height: 50px; background: rgb(200, 30, 30); }
  p { position: absolute; top: 120px; left: 10px; margin: 0; font: 24px sans-serif; }
</style></head><body>
  <div id="box"></div>
  <p>frame loop</p>
  <script>
    window.timestamps = [];
    let start;
    function step(t) {
      if (start === undefined)
        start = t;
      timestamps.push(t);
      document.getElementById("box").style.left = Math.round((t - start) / 16) * 10 + "px";
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

// The surface's BGRA premultiplied frame against a full repaint of the page.
bool matchesFullRepaint(ZeroG::View& view, const ZeroG::Frame& frame)
{
    RefPtr reference = ZeroG::paint(view.page(), ZeroG::Rendering::CPU, view.scale());
    if (!reference)
        return false;
    // ImageBuffer rects are logical; the pixel buffer comes back at physical size.
    RefPtr pixels = reference->getPixelBuffer({ AlphaPremultiplication::Premultiplied, PixelFormat::BGRA8, ColorSpace::SRGB() }, { { }, view.size() });
    if (!pixels || pixels->size() != IntSize(frame.width, frame.height))
        return check(false, "reference has the frame's size");
    unsigned different = 0;
    auto expected = pixels->bytes();
    auto* actual = static_cast<const uint8_t*>(frame.pixels);
    for (unsigned y = 0; y < frame.height; ++y) {
        if (std::memcmp(actual + y * frame.stride, expected.data() + y * frame.width * 4, frame.width * 4))
            ++different;
    }
    return check(!different, "frame matches a full repaint (%u rows differ)", different);
}

bool runAtScale(float scale)
{
    std::printf("-- scale %.1f\n", scale);
    auto view = ZeroG::View::create({ { width, height }, scale, false, true, nullptr });
    view->loadHTML(String::fromUTF8(page));

    bool ok = true;
    auto physicalArea = static_cast<uint64_t>(std::ceil(width * scale)) * static_cast<uint64_t>(std::ceil(height * scale));
    // An application's frame clock: monotonic, increasing, and not ahead of now
    // (the engine clamps later times to now).
    auto frameTime = MonotonicTime::now();
    auto waitFor = [](MonotonicTime time) {
        while (MonotonicTime::now() < time)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };

    ok &= check(view->render(frameTime), "first frame published");
    ok &= check(view->lastPaintedArea() == physicalArea, "first frame paints the whole view (%llu of %llu)",
        static_cast<unsigned long long>(view->lastPaintedArea()), static_cast<unsigned long long>(physicalArea));

    ZeroG::Frame frame;
    ok &= check(view->surface().acquire(frame), "acquire the first frame");
    ok &= check(frame.dirty == IntRect(0, 0, frame.width, frame.height), "first frame is dirty everywhere");
    ok &= matchesFullRepaint(*view, frame);
    view->surface().release(frame);
    ok &= check(!view->surface().acquire(frame), "no newer frame to acquire");

    uint64_t largestPaint = 0;
    for (int i = 1; i <= frames; ++i) {
        frameTime += frameInterval;
        waitFor(frameTime);
        if (!view->render(frameTime)) {
            ok &= check(false, "frame %d published", i);
            break;
        }
        std::printf("%llu ", static_cast<unsigned long long>(view->lastPaintedArea()));
        // Frame 1 reuses the first buffer; frame 2 creates the second one and
        // paints it whole.
        if (i > 2)
            largestPaint = std::max(largestPaint, view->lastPaintedArea());
    }
    std::printf("pixels painted per frame\n");
    ok &= check(largestPaint * 10 < physicalArea, "animation frames repaint little (largest %llu of %llu)",
        static_cast<unsigned long long>(largestPaint), static_cast<unsigned long long>(physicalArea));

    auto left = view->evaluateScript("document.getElementById('box').style.left"_s);
    ok &= check(left == makeString(frames * 10, "px"_s), "rAF followed the frame clock: left %s", left.utf8().data());
    auto spacing = view->evaluateScript("timestamps.length + ' ' + timestamps.slice(1).every((t, i) => Math.abs(t - timestamps[i] - 16) <= 1)"_s);
    ok &= check(spacing == makeString(frames + 1, " true"_s), "rAF ran every frame, 16ms apart (%s)", spacing.utf8().data());

    bool acquired = view->surface().acquire(frame);
    ok &= check(acquired, "acquire the newest frame");
    if (acquired) {
        ok &= check(!frame.dirty.isEmpty() && frame.dirty.width() < static_cast<int>(frame.width), "dirty rect covers the animation: %d,%d %dx%d",
            frame.dirty.x(), frame.dirty.y(), frame.dirty.width(), frame.dirty.height());
        ok &= matchesFullRepaint(*view, frame);
        view->surface().release(frame);
    }

    // Stop the animation: the callback already registered runs once more and
    // registers the no-op, which runs the frame after. Then nothing is left to do
    // and no frame is published.
    view->evaluateScript("step = () => { }; 0"_s);
    for (int i = 0; i < 2; ++i) {
        frameTime += frameInterval;
        waitFor(frameTime);
        view->render(frameTime);
    }
    frameTime += frameInterval;
    waitFor(frameTime);
    ok &= check(!view->render(frameTime), "idle page publishes no frame");
    return ok;
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool ok = runAtScale(1);
    ok &= runAtScale(1.5);
    std::printf(ok ? "PASS\n" : "FAILED\n");
    return ok ? 0 : 1;
}
