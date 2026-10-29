// Phase 1 gate (ZeroG.md, "Roadmap"): HTML from a string, laid out and painted on
// the CPU through Skia, saved as PNG, with pixels checked where the page puts
// known colors. Usage: first_render [out.png]

#include "config.h"
#include "Embedding.h"

#include <WebCore/ColorSpace.h>
#include <WebCore/ImageBuffer.h>
#include <WebCore/Page.h>
#include <WebCore/PixelBuffer.h>
#include <WebCore/PixelBufferFormat.h>
#include <cstdio>
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

} // namespace

int main(int argc, char** argv)
{
    const char* outputPath = argc > 1 ? argv[1] : "first_render.png";

    ZeroG::initializeProcess();
    Ref webPage = ZeroG::createPage({ { viewportWidth, viewportHeight }, true });
    ZeroG::loadHTML(webPage, String::fromUTF8(page));

    auto scriptResult = ZeroG::evaluateScript(webPage, "6 * 7"_s);
    std::printf("%s script evaluation: %s\n", scriptResult == "42"_s ? "ok  " : "FAIL", scriptResult.utf8().data());
    auto inlineStyle = ZeroG::evaluateScript(webPage, "document.getElementById('script').style.background"_s);
    std::printf("     inline script set background: '%s'\n", inlineStyle.utf8().data());

    RefPtr buffer = ZeroG::paint(webPage);
    if (!buffer) {
        std::printf("FAIL could not create the image buffer\n");
        return 1;
    }

    auto png = ZeroG::encodePNG(*buffer);
    if (png.isEmpty()) {
        std::printf("FAIL PNG encoding returned no data\n");
        return 1;
    }
    FILE* file = std::fopen(outputPath, "wb");
    if (!file || std::fwrite(png.span().data(), 1, png.size(), file) != png.size()) {
        std::printf("FAIL could not write %s\n", outputPath);
        return 1;
    }
    std::fclose(file);
    std::printf("wrote %s (%zu bytes)\n", outputPath, png.size());

    PixelBufferFormat format { AlphaPremultiplication::Unpremultiplied, PixelFormat::RGBA8, ColorSpace::SRGB() };
    RefPtr pixels = buffer->getPixelBuffer(format, { { }, { viewportWidth, viewportHeight } });
    if (!pixels) {
        std::printf("FAIL could not read the pixels back\n");
        return 1;
    }

    bool ok = true;
    ok &= expectPixel(*pixels, 50, 50, { 255, 0, 0, 255 }, "red box");
    ok &= expectPixel(*pixels, 150, 50, { 0, 255, 0, 255 }, "green box");
    ok &= expectPixel(*pixels, 250, 50, { 0, 0, 255, 255 }, "box colored by script");
    ok &= expectPixel(*pixels, 350, 50, { 255, 255, 255, 255 }, "background");

    // The text band must contain dark glyph pixels, so a font was found and drawn.
    unsigned dark = 0;
    for (int y = 120; y < 170; ++y) {
        for (int x = 0; x < viewportWidth; ++x) {
            auto pixel = pixelAt(*pixels, x, y);
            if (pixel.r < 128 && pixel.g < 128 && pixel.b < 128)
                ++dark;
        }
    }
    bool hasText = dark > 200;
    std::printf("%s text: %u dark pixels\n", hasText ? "ok  " : "FAIL", dark);
    ok &= hasText;

    std::printf(ok ? "PASS\n" : "FAILED\n");
    return ok ? 0 : 1;
}
