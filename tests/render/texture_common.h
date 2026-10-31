#pragma once

// What the Windows texture-mode tests share (texture_d3d12.cpp, texture_d3d9.cpp,
// texture_vulkan_win.cpp): a page whose box moves every frame, a reader thread
// that acquires frames while the engine renders, reads them with the
// application's own API and checks none is torn, and the last frame compared
// with a CPU render of the page.

#include "config.h"
#include "Embedding.h"
#include "View.h"

#include <WebCore/ColorSpace.h>
#include <WebCore/ImageBuffer.h>
#include <WebCore/Page.h>
#include <WebCore/PixelBuffer.h>
#include <WebCore/PixelBufferFormat.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>
#include <wtf/text/WTFString.h>

namespace TextureTest {

constexpr int width = 400;
constexpr int height = 200;
constexpr int frames = 60;

constexpr auto page = R"HTML(<!DOCTYPE html>
<html><head><style>
  html, body { margin: 0; background: rgb(240, 240, 255); }
  #box { position: absolute; top: 20px; left: 0; width: 50px; height: 50px; background: rgb(255, 0, 0); }
  p { position: absolute; top: 120px; left: 10px; margin: 0; font: 24px sans-serif; }
</style></head><body>
  <div id="box"></div>
  <p>texture mode</p>
  <script>
    let start;
    function step(t) {
      if (start === undefined)
        start = t;
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

struct Readback {
    std::vector<uint8_t> pixels; // RGBA8, rows top to bottom
    unsigned width { 0 };
    unsigned height { 0 };

    // From rows of BGRA (D3D9's A8R8G8B8) or RGBA.
    void assign(const void* data, size_t pitch, unsigned w, unsigned h, bool bgra)
    {
        width = w;
        height = h;
        pixels.resize(static_cast<size_t>(w) * h * 4);
        for (unsigned y = 0; y < h; ++y) {
            auto* row = static_cast<const uint8_t*>(data) + y * pitch;
            auto* out = pixels.data() + static_cast<size_t>(y) * w * 4;
            if (!bgra) {
                std::memcpy(out, row, static_cast<size_t>(w) * 4);
                continue;
            }
            for (unsigned x = 0; x < w; ++x) {
                out[x * 4] = row[x * 4 + 2];
                out[x * 4 + 1] = row[x * 4 + 1];
                out[x * 4 + 2] = row[x * 4];
                out[x * 4 + 3] = row[x * 4 + 3];
            }
        }
    }
};

// The box is 50px of pure red on row 45: exactly one run of that length.
inline bool boxIsWhole(const Readback& frame)
{
    unsigned run = 0, runs = 0, longest = 0;
    auto* row = frame.pixels.data() + 45 * frame.width * 4;
    for (unsigned x = 0; x <= frame.width; ++x) {
        bool red = x < frame.width && row[x * 4] == 255 && row[x * 4 + 1] == 0 && row[x * 4 + 2] == 0;
        if (red)
            ++run;
        else if (run) {
            ++runs;
            longest = std::max(longest, run);
            run = 0;
        }
    }
    return runs == 1 && longest == 50;
}

// Reads an acquired frame on the reader's thread with the application's API.
using ReadFrame = std::function<bool(const ZeroG::Frame&, Readback&)>;

// The engine renders `frames` frames on the application's clock while the
// reader thread acquires, reads (`read`) and releases them; then the page stops
// and its last frame, read on the reader thread too (the device may be
// single-threaded), is compared with a CPU render. `onReader` runs first on the
// reader thread.
inline bool run(ZeroG::View& view, const ReadFrame& read, const std::function<void()>& onReader = { })
{
    bool ok = true;
    view.loadHTML(String::fromUTF8(page));

    std::atomic<bool> stop { false };
    std::atomic<int> acquired { 0 }, torn { 0 }, failed { 0 };
    std::atomic<bool> readLast { false }, lastDone { false }, hasLast { false };
    Readback last;
    std::thread reader([&] {
        if (onReader)
            onReader();
        Readback readback;
        while (!stop) {
            ZeroG::Frame frame;
            if (!view.surface().acquire(frame)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (read(frame, readback)) {
                ++acquired;
                if (!boxIsWhole(readback))
                    ++torn;
            } else
                ++failed;
            view.surface().release(frame);
        }
        // The last frame on this thread too: the device may be single-threaded.
        while (!readLast)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        for (int i = 0; i < 200 && !view.surface().hasNewFrame(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        ZeroG::Frame frame;
        if (view.surface().acquire(frame)) {
            hasLast = read(frame, last);
            view.surface().release(frame);
        }
        lastDone = true;
    });

    auto frameTime = MonotonicTime::now();
    int published = 0;
    for (int i = 0; i < frames; ++i) {
        if (view.render(frameTime))
            ++published;
        frameTime += 16_ms;
        while (MonotonicTime::now() < frameTime)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop = true;

    ok &= check(published >= frames * 9 / 10, "engine asked for %d frames in %d updates", published, frames);

    // The page stops; render until it is still, then the reader takes the last frame.
    view.evaluateScript("step = () => { }; 0"_s);
    for (int i = 0; i < 60 && (i < 3 || view.hasPendingRenderingUpdate()); ++i) {
        frameTime += 16_ms;
        view.render(frameTime);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    readLast = true;
    reader.join();

    ok &= check(acquired > frames / 4, "reader read %d frames", acquired.load());
    ok &= check(!failed, "no failed readbacks (%d)", failed.load());
    ok &= check(!torn, "no torn frames (%d)", torn.load());
    ok &= check(hasLast, "read the last frame");

    RefPtr reference = ZeroG::paint(view.page(), ZeroG::Rendering::CPU);
    RefPtr expected = reference ? reference->getPixelBuffer({ WebCore::AlphaPremultiplication::Unpremultiplied, WebCore::PixelFormat::RGBA8, WebCore::ColorSpace::SRGB() }, { { }, view.size() }) : nullptr;
    if (hasLast && expected && last.pixels.size() == expected->bytes().size()) {
        unsigned different = 0;
        auto bytes = expected->bytes();
        for (size_t i = 0; i < last.pixels.size(); i += 4) {
            for (int c = 0; c < 4; ++c) {
                if (std::abs(last.pixels[i + c] - bytes[i + c]) > 48) {
                    ++different;
                    break;
                }
            }
        }
        ok &= check(different <= width * height / 100, "last frame matches a CPU render (%u pixels differ)", different);
    } else
        ok &= check(false, "CPU reference");
    return ok;
}

} // namespace TextureTest
