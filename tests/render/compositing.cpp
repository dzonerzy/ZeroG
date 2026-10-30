// GPU compositing (ZeroG.md, "Compositing", version 2): a GPU view's layers are
// composited on the compositor thread. CSS animations of transform and opacity
// go on producing frames there with no rendering update at all (render() is not
// called); a still frame matches the CPU render of the same page, its layers
// flattened.
//
// Usage: compositing   (a GPU without a window: ANGLE on Windows, EGL on Linux)

#include "config.h"
#include "Embedding.h"
#include "View.h"
#include "graphics/Compositor.h"
#include "graphics/Surface.h"

#include <GLES2/gl2.h>
#include <WebCore/ColorSpace.h>
#include <WebCore/GLContext.h>
#include <WebCore/ImageBuffer.h>
#include <WebCore/PixelBuffer.h>
#include <WebCore/PixelBufferFormat.h>
#include <WebCore/PlatformDisplay.h>
#include <chrono>
#include <cstdio>
#include <thread>
#include <wtf/text/WTFString.h>

using namespace WebCore;

namespace {

constexpr int viewWidth = 320;
constexpr int viewHeight = 200;

constexpr auto page = R"HTML(<!DOCTYPE html>
<html><head><style>
  html, body { margin: 0; background: rgb(250, 250, 240); }
  @keyframes slide { from { transform: translateX(0); } to { transform: translateX(200px); } }
  @keyframes fade { from { opacity: 1; } to { opacity: 0.2; } }
  #slider { position: absolute; left: 10px; top: 20px; width: 60px; height: 60px; background: rgb(0, 120, 255);
            animation: slide 1s linear infinite alternate; }
  #fader { position: absolute; left: 10px; top: 110px; width: 60px; height: 60px; background: rgb(255, 60, 0);
           animation: fade 1s linear infinite alternate; }
  #layer { position: absolute; left: 200px; top: 110px; width: 80px; height: 60px; background: rgb(20, 160, 60);
           will-change: transform; }
  .still #slider, .still #fader { animation: none; }
</style></head><body><div id="slider"></div><div id="fader"></div><div id="layer"></div></body></html>)HTML";

#define check(condition, ...) [&] {                    \
        bool result = (condition);                     \
        std::printf("%s ", result ? "ok  " : "FAIL");  \
        std::printf(__VA_ARGS__);                      \
        std::printf("\n");                             \
        return result;                                 \
    }()

// Reads a GPU frame (a texture in the engine's share group) back to RGBA rows,
// top first, on this thread's Skia GL context.
bool readFrame(const ZeroG::Frame& frame, Vector<uint8_t>& pixels)
{
    auto* context = PlatformDisplay::sharedDisplay().skiaGLContext();
    if (!context || !context->makeContextCurrent())
        return false;
    GLuint framebuffer = 0;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, static_cast<GLuint>(reinterpret_cast<uintptr_t>(frame.texture)), 0);
    bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (complete) {
        pixels.resize(static_cast<size_t>(frame.width) * frame.height * 4);
        glReadPixels(0, 0, frame.width, frame.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.mutableSpan().data());
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &framebuffer);
    return complete;
}

bool acquireAndRead(ZeroG::View& view, Vector<uint8_t>& pixels)
{
    ZeroG::Frame frame;
    for (int i = 0; i < 400 && !view.surface().acquire(frame); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!frame.handle)
        return false;
    bool read = readFrame(frame, pixels);
    view.surface().release(frame);
    return read;
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    ZeroG::initializeProcess();
    bool ok = check(ZeroG::initializeGPU(), "GPU: %s", ZeroG::gpuRenderer().utf8().data());
    if (!ok)
        return 1;
    auto surface = ZeroG::createGPUSurface(3);
    if (!check(!!surface, "a GPU surface"))
        return 1;
    auto view = ZeroG::View::create({ { viewWidth, viewHeight }, 1, false, true, WTF::move(surface) });
    ok &= check(!!view->compositor(), "the GPU view composites its layers");
    if (!view->compositor())
        return 1;
    view->loadHTML(String::fromUTF8(page));

    // The application's loop: the engine's tasks and timers, then a frame.
    auto frameTime = MonotonicTime::now();
    auto frame = [&] {
        ZeroG::runPendingWork();
        view->render(frameTime);
        frameTime += 16_ms;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    };

    // The page's first rendering updates: layers, tiles, the first frames.
    for (int i = 0; i < 20; ++i)
        frame();
    unsigned framesBefore = view->compositor()->compositedFrames();
    ok &= check(framesBefore > 0, "frames composited for the rendering updates: %u", framesBefore);

    // No render() for half a second: the animations run on the compositor thread.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    unsigned animationFrames = view->compositor()->compositedFrames() - framesBefore;
    ok &= check(animationFrames >= 15, "CSS animations go on with no rendering update: %u frames in 500 ms", animationFrames);

    // And they move: two frames apart differ where the slider is.
    Vector<uint8_t> first, second;
    bool readFirst = acquireAndRead(*view, first);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    bool readSecond = acquireAndRead(*view, second);
    ok &= check(readFirst && readSecond, "frames read back");
    if (readFirst && readSecond && first.size() == second.size()) {
        unsigned changed = 0;
        for (size_t i = 0; i < first.size(); i += 4)
            changed += first[i] != second[i] || first[i + 1] != second[i + 1] || first[i + 2] != second[i + 2];
        ok &= check(changed > 500, "the animation moves between frames (%u pixels changed)", changed);
    }

    // Still: the composited frame against the CPU render, layers flattened.
    view->evaluateScript("document.body.className = 'still'; 0"_s);
    for (int i = 0; i < 60 && (i < 5 || view->hasPendingRenderingUpdate()); ++i)
        frame();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    unsigned stillFrames = view->compositor()->compositedFrames();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ok &= check(view->compositor()->compositedFrames() - stillFrames <= 1, "a still page composites no more frames (%u in 300 ms)", view->compositor()->compositedFrames() - stillFrames);
    view->repaint();
    view->render(frameTime);
    Vector<uint8_t> still;
    bool readStill = acquireAndRead(*view, still);
    RefPtr reference = ZeroG::paint(view->page(), ZeroG::Rendering::CPU);
    RefPtr expected = reference ? reference->getPixelBuffer({ AlphaPremultiplication::Unpremultiplied, PixelFormat::RGBA8, ColorSpace::SRGB() }, { { }, view->size() }) : nullptr;
    if (readStill && expected && still.size() == expected->bytes().size()) {
        auto bytes = expected->bytes();
        unsigned different = 0;
        // Frames are stored top row first (Skia's top-left origin), as the reference.
        for (int y = 0; y < viewHeight; ++y) {
            for (int x = 0; x < viewWidth; ++x) {
                auto* a = still.span().data() + (static_cast<size_t>(y) * viewWidth + x) * 4;
                auto* b = bytes.data() + (static_cast<size_t>(y) * viewWidth + x) * 4;
                if (std::abs(a[0] - b[0]) > 48 || std::abs(a[1] - b[1]) > 48 || std::abs(a[2] - b[2]) > 48)
                    ++different;
            }
        }
        ok &= check(different <= viewWidth * viewHeight / 100, "a still composited frame matches the CPU render (%u pixels differ)", different);
    } else
        ok &= check(false, "a still frame and the CPU reference (%d, %d)", readStill, !!expected);

    // WebGL (standard, dev and debug builds): its drawing buffer is a layer the
    // compositor shows.
    {
        auto webGLSurface = ZeroG::createGPUSurface(3);
        auto webGLView = ZeroG::View::create({ { viewWidth, viewHeight }, 1, false, true, WTF::move(webGLSurface) });
        // Why a context could not be created, as WebCore says it.
        struct ConsoleClient final : public ZeroG::ViewClient {
            void consoleMessage(JSC::MessageSource, JSC::MessageLevel, const String& message, unsigned, unsigned, const String&, const String&) final
            {
                std::printf("     console: %s\n", message.utf8().data());
            }
        } consoleClient;
        webGLView->setClient(&consoleClient);
        webGLView->loadHTML("<!DOCTYPE html><body style='margin: 0; background: white'><canvas id=c width=100 height=100></canvas><script>"
            "c.addEventListener('webglcontextcreationerror', e => console.log('webglcontextcreationerror: ' + e.statusMessage));"
            "const gl = c.getContext('webgl');"
            "window.hasWebGL = !!gl;"
            "if (gl) { gl.clearColor(0, 1, 0, 1); gl.clear(gl.COLOR_BUFFER_BIT); }"
            "</script>"_s);
        for (int i = 0; i < 20; ++i) {
            ZeroG::runPendingWork();
            webGLView->render(frameTime);
            frameTime += 16_ms;
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        bool hasWebGL = webGLView->evaluateScript("String(window.hasWebGL)"_s) == "true"_s;
        bool built = webGLView->evaluateScript("typeof WebGLRenderingContext"_s) == "function"_s;
        if (!built)
            ok &= check(!hasWebGL, "this build has no WebGL: no context");
        else {
            ok &= check(hasWebGL, "a WebGL context");
            Vector<uint8_t> pixels;
            bool read = acquireAndRead(*webGLView, pixels);
            auto* center = read ? pixels.span().data() + (static_cast<size_t>(50) * viewWidth + 50) * 4 : nullptr;
            ok &= check(center && center[0] < 30 && center[1] > 220 && center[2] < 30, "the WebGL canvas is composited: %d,%d,%d",
                center ? center[0] : -1, center ? center[1] : -1, center ? center[2] : -1);
        }
    }

    std::printf(ok ? "PASS\n" : "FAILED\n");
    return ok ? 0 : 1;
}
