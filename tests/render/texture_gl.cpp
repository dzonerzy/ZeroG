// Texture mode on Linux (docs/design/graphics-presentation.md, "Separate render thread"): the application owns
// an EGL display, a GL context and a render thread. The engine renders the page
// with Skia into textures on the same display; the render thread binds each
// acquired frame's EGLImage in its own context, waits on the frame's fence, reads
// it and releases it, while the engine keeps rendering. Every frame read must be
// whole (no tearing), and the last one must match a CPU render of the page.
//
// Usage: texture_gl

#include "config.h"
#include "Embedding.h"
#include "View.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
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
#include <map>
#include <thread>
#include <vector>
#include <wtf/text/WTFString.h>

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

// The application's side: its display and a context on its render thread.
struct Host {
    EGLDisplay display { EGL_NO_DISPLAY };
    int platform { 0 }; // what the display was opened from (0: eglGetDisplay)
    EGLContext context { EGL_NO_CONTEXT };
    EGLSurface surface { EGL_NO_SURFACE };
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTargetTexture { nullptr };
    std::map<void*, GLuint> textures; // per EGLImage
    GLuint framebuffer { 0 };

    bool open()
    {
        auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        if (getPlatformDisplay)
            display = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        if (display != EGL_NO_DISPLAY)
            platform = EGL_PLATFORM_SURFACELESS_MESA;
        else
            display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        return display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr);
    }

    // Render thread.
    bool makeCurrent()
    {
        const EGLint configAttributes[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
        EGLConfig config;
        EGLint count = 0;
        if (!eglBindAPI(EGL_OPENGL_ES_API) || !eglChooseConfig(display, configAttributes, &config, 1, &count) || !count)
            return false;
        const EGLint contextAttributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
        const EGLint surfaceAttributes[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
        surface = eglCreatePbufferSurface(display, config, surfaceAttributes);
        if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, context))
            return false;
        imageTargetTexture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        glGenFramebuffers(1, &framebuffer);
        return imageTargetTexture;
    }

    bool readFrame(const ZeroG::Frame& frame, Readback& out)
    {
        if (frame.fence)
            eglWaitSync(display, static_cast<EGLSync>(frame.fence), 0);
        auto& texture = textures[frame.texture];
        if (!texture) {
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            imageTargetTexture(GL_TEXTURE_2D, static_cast<GLeglImageOES>(frame.texture));
        }
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            return false;
        out.width = frame.width;
        out.height = frame.height;
        out.pixels.resize(out.width * out.height * 4);
        glReadPixels(0, 0, out.width, out.height, GL_RGBA, GL_UNSIGNED_BYTE, out.pixels.data());
        return glGetError() == GL_NO_ERROR;
    }
};

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

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool ok = true;

    Host host;
    if (!check(host.open(), "application's EGL display"))
        return 1;

    ZeroG::initializeProcess();
    // With how the display was opened (zg_surface_config.egl_platform), so that
    // WebGL's ANGLE opens it too.
    ok &= check(ZeroG::initializeGPU(host.display, host.platform, EGL_DEFAULT_DISPLAY), "GPU on the application's display: %s", ZeroG::gpuRenderer().utf8().data());
    auto surface = ZeroG::createGLSurface(host.display, 3);
    if (!check(!!surface, "GL surface"))
        return 1;
    auto view = ZeroG::View::create({ { width, height }, 1, false, true, WTF::move(surface) });
    view->loadHTML(String::fromUTF8(page));

    // The application's render thread, with its own context.
    std::atomic<bool> stop { false }, ready { false }, contextFailed { false };
    std::atomic<int> acquired { 0 }, torn { 0 }, failed { 0 };
    Readback last;
    bool hasLast = false;
    std::thread renderThread([&] {
        if (!host.makeCurrent()) {
            contextFailed = true;
            ready = true;
            return;
        }
        ready = true;
        Readback readback;
        auto readOne = [&] {
            ZeroG::Frame frame;
            if (!view->surface().acquire(frame))
                return false;
            if (host.readFrame(frame, readback)) {
                ++acquired;
                if (!boxIsWhole(readback))
                    ++torn;
                // The newest frame read, the engine's final one at the end.
                last = readback;
                hasLast = true;
            } else
                ++failed;
            view->surface().release(frame);
            return true;
        };
        while (!stop) {
            if (!readOne())
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        // The engine's final frame, unless the loop already read it.
        readOne();
    });
    while (!ready)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!check(!contextFailed, "application's GL context")) {
        renderThread.join();
        return 1;
    }

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

    // Stop the animation and publish a last, still frame for the render thread.
    view->evaluateScript("step = () => { }; 0"_s);
    // A GPU view's frames are composited on the compositor thread and published
    // a little after render(): render until the page is still, then give the
    // render thread time to read the last one.
    for (int i = 0; i < 60 && (i < 3 || view->hasPendingRenderingUpdate()); ++i) {
        frameTime += 16_ms;
        while (MonotonicTime::now() < frameTime)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        view->render(frameTime);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop = true;
    renderThread.join();

    // render() asks for a frame once the previous one is composited; a frame
    // waiting for its tiles (the first, after the page loads) skips an update.
    ok &= check(published >= frames * 9 / 10, "engine asked for %d frames in %d updates", published, frames);
    ok &= check(acquired > frames / 4, "render thread read %d frames", acquired.load());
    ok &= check(!failed, "no failed readbacks (%d)", failed.load());
    ok &= check(!torn, "no torn frames (%d)", torn.load());
    ok &= check(hasLast, "read the last frame");

    RefPtr reference = ZeroG::paint(view->page(), ZeroG::Rendering::CPU);
    RefPtr expected = reference ? reference->getPixelBuffer({ AlphaPremultiplication::Unpremultiplied, PixelFormat::RGBA8, ColorSpace::SRGB() }, { { }, view->size() }) : nullptr;
    if (hasLast && expected) {
        // Frames are textures with their top row at t = 0, which glReadPixels on an
        // FBO returns first.
        unsigned different = 0;
        auto bytes = expected->bytes();
        for (unsigned y = 0; y < last.height; ++y) {
            auto* actualRow = last.pixels.data() + y * last.width * 4;
            auto* expectedRow = bytes.data() + y * last.width * 4;
            for (unsigned x = 0; x < last.width; ++x) {
                for (int c = 0; c < 4; ++c) {
                    if (std::abs(actualRow[x * 4 + c] - expectedRow[x * 4 + c]) > 48) {
                        ++different;
                        break;
                    }
                }
            }
        }
        ok &= check(different <= width * height / 100, "last frame matches a CPU render (%u pixels differ)", different);
    } else
        ok &= check(false, "CPU reference");

    // WebGL on the application's display (standard, dev and debug builds).
    if (view->evaluateScript("typeof WebGLRenderingContext"_s) == "function"_s) {
        auto pixel = view->evaluateScript("(() => { const gl = document.createElement('canvas').getContext('webgl');"
            "if (!gl) return 'no context'; gl.clearColor(0, 1, 0, 1); gl.clear(gl.COLOR_BUFFER_BIT);"
            "const p = new Uint8Array(4); gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, p); return p.join(); })()"_s);
        ok &= check(pixel == "0,255,0,255"_s, "WebGL draws on the application's display: %s", pixel.utf8().data());
    }

    std::printf(ok ? "PASS\n" : "FAILED\n");
    return ok ? 0 : 1;
}
