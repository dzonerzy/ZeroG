// Phase 1 gate (ZeroG.md, "Roadmap"): HTML from a string, laid out and painted
// through Skia, saved as PNG, with pixels checked where the page puts known
// colors. With --gpu the page is also painted on the GPU (Skia Ganesh over EGL:
// ANGLE on Windows, the system driver on Linux), checked the same way and
// compared with the CPU image.
//
// Usage: first_render [--gpu] [out.png]   (the GPU image goes to out-gpu.png)

#include "config.h"
#include "Embedding.h"

#include <WebCore/ColorSpace.h>
#include <WebCore/ImageBuffer.h>
#include <WebCore/Page.h>
#include <WebCore/PixelBuffer.h>
#include <WebCore/PixelBufferFormat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <wtf/text/WTFString.h>

using namespace WebCore;

namespace {

constexpr int viewportWidth = 400;
constexpr int viewportHeight = 200;

// Three 100x100 boxes along the top: red and green from CSS, the third blue only if
// the inline script ran. Text goes below them.
constexpr auto page = R"HTML(<!DOCTYPE html>
<html><head><style>
  html, body { margin: 0; background: white; }
  .box { position: absolute; top: 0; width: 100px; height: 100px; }
  #red { left: 0; background: rgb(255, 0, 0); }
  #green { left: 100px; background: rgb(0, 255, 0); }
  #script { left: 200px; background: rgb(0, 0, 0); }
  p { position: absolute; top: 120px; left: 10px; margin: 0; font: 32px sans-serif; color: black; }
</style></head><body>
  <div class="box" id="red"></div>
  <div class="box" id="green"></div>
  <div class="box" id="script"></div>
  <p>ZeroG renders text</p>
  <script>document.getElementById("script").style.background = "rgb(0, 0, 255)";</script>
</body></html>)HTML";

struct RGBA {
    uint8_t r, g, b, a;
};

RGBA pixelAt(const PixelBuffer& pixels, int x, int y)
{
    auto bytes = pixels.bytes();
    size_t offset = (static_cast<size_t>(y) * pixels.size().width() + x) * 4;
    return { bytes[offset], bytes[offset + 1], bytes[offset + 2], bytes[offset + 3] };
}

bool expectPixel(const PixelBuffer& pixels, int x, int y, RGBA expected, const char* what)
{
    auto actual = pixelAt(pixels, x, y);
    bool ok = actual.r == expected.r && actual.g == expected.g && actual.b == expected.b && actual.a == expected.a;
    std::printf("%s %s at (%d, %d): got %u,%u,%u,%u expected %u,%u,%u,%u\n", ok ? "ok  " : "FAIL", what, x, y,
        actual.r, actual.g, actual.b, actual.a, expected.r, expected.g, expected.b, expected.a);
    return ok;
}

bool writePNG(ImageBuffer& buffer, const std::string& path)
{
    auto png = ZeroG::encodePNG(buffer);
    FILE* file = png.isEmpty() ? nullptr : std::fopen(path.c_str(), "wb");
    bool ok = file && std::fwrite(png.span().data(), 1, png.size(), file) == png.size();
    if (file)
        std::fclose(file);
    std::printf("%s wrote %s (%zu bytes)\n", ok ? "ok  " : "FAIL", path.c_str(), png.size());
    return ok;
}

RefPtr<PixelBuffer> readPixels(ImageBuffer& buffer)
{
    PixelBufferFormat format { AlphaPremultiplication::Unpremultiplied, PixelFormat::RGBA8, ColorSpace::SRGB() };
    return buffer.getPixelBuffer(format, { { }, { viewportWidth, viewportHeight } });
}

bool checkPixels(const PixelBuffer& pixels)
{
    bool ok = true;
    ok &= expectPixel(pixels, 50, 50, { 255, 0, 0, 255 }, "red box");
    ok &= expectPixel(pixels, 150, 50, { 0, 255, 0, 255 }, "green box");
    ok &= expectPixel(pixels, 250, 50, { 0, 0, 255, 255 }, "box colored by script");
    ok &= expectPixel(pixels, 350, 50, { 255, 255, 255, 255 }, "background");

    // The text band must contain dark glyph pixels, so a font was found and drawn.
    unsigned dark = 0;
    for (int y = 120; y < 170; ++y) {
        for (int x = 0; x < viewportWidth; ++x) {
            auto pixel = pixelAt(pixels, x, y);
            if (pixel.r < 128 && pixel.g < 128 && pixel.b < 128)
                ++dark;
        }
    }
    bool hasText = dark > 200;
    std::printf("%s text: %u dark pixels\n", hasText ? "ok  " : "FAIL", dark);
    return ok && hasText;
}

// GPU and CPU rasterize glyph edges slightly differently; anything beyond that is
// a rendering difference.
bool compareImages(const PixelBuffer& cpu, const PixelBuffer& gpu)
{
    unsigned different = 0;
    for (int y = 0; y < viewportHeight; ++y) {
        for (int x = 0; x < viewportWidth; ++x) {
            auto a = pixelAt(cpu, x, y);
            auto b = pixelAt(gpu, x, y);
            if (std::abs(a.r - b.r) > 48 || std::abs(a.g - b.g) > 48 || std::abs(a.b - b.b) > 48 || std::abs(a.a - b.a) > 48)
                ++different;
        }
    }
    unsigned limit = viewportWidth * viewportHeight / 100;
    bool ok = different <= limit;
    std::printf("%s GPU vs CPU: %u pixels differ (limit %u)\n", ok ? "ok  " : "FAIL", different, limit);
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    bool gpu = false;
    std::string outputPath = "first_render.png";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--gpu"))
            gpu = true;
        else
            outputPath = argv[i];
    }

    ZeroG::initializeProcess();
    Ref webPage = ZeroG::createPage({ { viewportWidth, viewportHeight }, true });
    ZeroG::loadHTML(webPage, String::fromUTF8(page));

    auto scriptResult = ZeroG::evaluateScript(webPage, "6 * 7"_s);
    bool ok = scriptResult == "42"_s;
    std::printf("%s script evaluation: %s\n", ok ? "ok  " : "FAIL", scriptResult.utf8().data());

    std::printf("-- CPU\n");
    RefPtr cpuBuffer = ZeroG::paint(webPage, ZeroG::Rendering::CPU);
    RefPtr cpuPixels = cpuBuffer ? readPixels(*cpuBuffer) : nullptr;
    if (!cpuPixels) {
        std::printf("FAIL CPU paint\n");
        return 1;
    }
    ok &= writePNG(*cpuBuffer, outputPath);
    ok &= checkPixels(*cpuPixels);

    if (gpu) {
        std::printf("-- GPU\n");
        bool hasGPU = ZeroG::initializeGPU();
        std::printf("%s GPU display and Skia GL context: %s\n", hasGPU ? "ok  " : "FAIL", ZeroG::gpuRenderer().utf8().data());
        RefPtr gpuBuffer = hasGPU ? ZeroG::paint(webPage, ZeroG::Rendering::GPU) : nullptr;
        bool accelerated = gpuBuffer && gpuBuffer->renderingMode() == RenderingMode::Accelerated;
        std::printf("%s accelerated image buffer\n", accelerated ? "ok  " : "FAIL");
        RefPtr gpuPixels = accelerated ? readPixels(*gpuBuffer) : nullptr;
        if (!gpuPixels) {
            std::printf("FAILED\n");
            return 1;
        }
        auto gpuPath = outputPath;
        auto dot = gpuPath.rfind('.');
        gpuPath.insert(dot == std::string::npos ? gpuPath.size() : dot, "-gpu");
        ok &= writePNG(*gpuBuffer, gpuPath);
        ok &= checkPixels(*gpuPixels);
        ok &= compareImages(*cpuPixels, *gpuPixels);
    }

    std::printf(ok ? "PASS\n" : "FAILED\n");
    return ok ? 0 : 1;
}
