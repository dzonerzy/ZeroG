// Texture mode on Windows (ZeroG.md, "Render thread separato"): the application
// owns a D3D11 device and a render thread. The engine renders the page with Skia
// over ANGLE into shared textures; the render thread acquires the newest frame,
// copies it on its own device and releases it, while the engine keeps rendering.
// Every frame read must be whole (no tearing), and the last one must match a CPU
// render of the page.
//
// Usage: texture_d3d11 [buffer count, default 3] [cpu|noread]
//   cpu     the same with a CPU surface (diagnostics)
//   noread  no reads during rendering, only the last frame
// The first torn frame is written to torn.rgba (RGBA8, 400x200).

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
#include <vector>
#include <d3d11_1.h>
#include <thread>
#include <wrl/client.h>
#include <wtf/text/WTFString.h>

using Microsoft::WRL::ComPtr;
using namespace WebCore;

namespace {

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
};

// Render thread side: copy the acquired texture into CPU memory with the host's
// own device and context.
bool readFrame(ID3D11Device* device, ID3D11DeviceContext* context, const ZeroG::Frame& frame, Readback& out)
{
    if (frame.pixels) {
        // CPU surface (diagnostics): BGRA rows.
        out.width = frame.width;
        out.height = frame.height;
        out.pixels.resize(out.width * out.height * 4);
        for (unsigned y = 0; y < out.height; ++y) {
            auto* row = static_cast<const uint8_t*>(frame.pixels) + y * frame.stride;
            for (unsigned x = 0; x < out.width; ++x) {
                auto* pixel = out.pixels.data() + (y * out.width + x) * 4;
                pixel[0] = row[x * 4 + 2];
                pixel[1] = row[x * 4 + 1];
                pixel[2] = row[x * 4];
                pixel[3] = row[x * 4 + 3];
            }
        }
        return true;
    }
    auto* texture = static_cast<ID3D11Texture2D*>(frame.texture);
    D3D11_TEXTURE2D_DESC description;
    texture->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    description.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &staging)))
        return false;
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return false;
    out.width = description.Width;
    out.height = description.Height;
    out.pixels.resize(out.width * out.height * 4);
    for (unsigned y = 0; y < out.height; ++y)
        std::memcpy(out.pixels.data() + y * out.width * 4, static_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch, out.width * 4);
    context->Unmap(staging.Get(), 0);
    return true;
}

// The box is 50px of pure red on row 45: exactly one run of that length.
bool boxIsWhole(const Readback& frame)
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

} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    unsigned bufferCount = argc > 1 ? std::atoi(argv[1]) : 3;
    bool ok = true;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) {
        std::printf("FAIL no D3D11 device\n");
        return 1;
    }

    ZeroG::initializeProcess();
    ok &= check(ZeroG::initializeGPU(), "GPU: %s", ZeroG::gpuRenderer().utf8().data());
    bool cpu = argc > 2 && !std::strcmp(argv[2], "cpu");
    auto surface = cpu ? ZeroG::createCPUSurface(bufferCount) : ZeroG::createD3D11Surface(device.Get(), bufferCount);
    if (!check(!!surface, "D3D11 surface on the host's device"))
        return 1;
    auto view = ZeroG::View::create({ { width, height }, 1, false, true, WTF::move(surface) });
    view->loadHTML(String::fromUTF8(page));

    // The application's render thread.
    std::atomic<bool> stop { false };
    std::atomic<int> acquired { 0 }, torn { 0 }, failed { 0 };
    bool concurrentReads = !(argc > 2 && !std::strcmp(argv[2], "noread"));
    std::thread renderThread([&] {
        Readback readback;
        while (!stop && concurrentReads) {
            ZeroG::Frame frame;
            if (!view->surface().acquire(frame)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (readFrame(device.Get(), context.Get(), frame, readback)) {
                ++acquired;
                if (!boxIsWhole(readback) && !torn++) {
                    if (FILE* file = std::fopen("torn.rgba", "wb")) {
                        std::fwrite(readback.pixels.data(), 1, readback.pixels.size(), file);
                        std::fclose(file);
                    }
                }
            } else
                ++failed;
            view->surface().release(frame);
        }
    });

    // The engine thread: one frame every 16ms of the application's clock.
    auto frameTime = MonotonicTime::now();
    int published = 0;
    for (int i = 0; i < frames; ++i) {
        if (view->render(frameTime))
            ++published;
        frameTime += 16_ms;
        while (MonotonicTime::now() < frameTime)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop = true;
    renderThread.join();

    ok &= check(published == frames, "engine published %d of %d frames", published, frames);
    ok &= check(acquired > frames / 4, "render thread read %d frames", acquired.load());
    ok &= check(!failed, "no failed readbacks (%d)", failed.load());
    ok &= check(!torn, "no torn frames (%d)", torn.load());

    // The last frame against a CPU render of the same page.
    view->evaluateScript("step = () => { }; 0"_s);
    for (int i = 0; i < 3; ++i) {
        frameTime += 16_ms;
        view->render(frameTime);
    }
    ZeroG::Frame frame;
    Readback last;
    bool hasLast = view->surface().acquire(frame);
    if (hasLast) {
        hasLast = readFrame(device.Get(), context.Get(), frame, last);
        view->surface().release(frame);
    }
    ok &= check(hasLast, "read the last frame");
    RefPtr reference = ZeroG::paint(view->page(), ZeroG::Rendering::CPU);
    RefPtr expected = reference ? reference->getPixelBuffer({ AlphaPremultiplication::Unpremultiplied, PixelFormat::RGBA8, ColorSpace::SRGB() }, { { }, view->size() }) : nullptr;
    if (hasLast && expected) {
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

    std::printf(ok ? "PASS\n" : "FAILED\n");
    return ok ? 0 : 1;
}
