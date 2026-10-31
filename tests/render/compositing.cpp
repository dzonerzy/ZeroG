// GPU compositing (ZeroG.md, "Compositing", version 2): a GPU view's layers are
// composited on the compositor thread. CSS animations of transform and opacity
// produce frames there with no rendering update at all, one per application
// frame (render()), none without, and at most as many as a cap allows;
// will-change makes a layer; a still frame matches the CPU render of the same
// page, its layers flattened.
//
// Usage: compositing   (a GPU without a window: ANGLE on Windows, EGL on Linux)

#include "config.h"
#include "Embedding.h"
#include "View.h"
#include "graphics/Compositor.h"
#include "graphics/Surface.h"
#include "loader/Loader.h"
#include "loader/MemoryFS.h"
#if defined(ZEROG_MEDIA)
#include "media/MediaFile.h"
#endif

#include <GLES2/gl2.h>
#include <WebCore/ColorSpace.h>
#include <WebCore/Document.h>
#include <WebCore/Element.h>
#include <WebCore/GLContext.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/Page.h>
#include <WebCore/RenderLayer.h>
#include <WebCore/RenderLayerModelObject.h>
#include <WebCore/ImageBuffer.h>
#include <WebCore/PixelBuffer.h>
#include <WebCore/PixelBufferFormat.h>
#include <WebCore/PlatformDisplay.h>
#include <chrono>
#include <cstdio>
#include <thread>
#include <wtf/FileSystem.h>
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
  /* Clear of the animations' paths: an element they may overlap gets a layer too. */
  #still { position: absolute; left: 100px; top: 180px; width: 80px; height: 15px; background: rgb(120, 60, 160); }
  .still #slider, .still #fader { animation: none; }
</style></head><body><div id="slider"></div><div id="fader"></div><div id="layer"></div><div id="still"></div></body></html>)HTML";

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

// Whether an element of the page has a composited layer of its own.
bool isComposited(ZeroG::View& view, ASCIILiteral id)
{
    RefPtr frame = view.page().localMainFrame();
    RefPtr document = frame ? frame->document() : nullptr;
    RefPtr element = document ? document->getElementById(AtomString(id)) : nullptr;
    auto* renderer = element ? dynamicDowncast<RenderLayerModelObject>(element->renderer()) : nullptr;
    return renderer && renderer->hasLayer() && renderer->layer()->isComposited();
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

    // will-change makes a layer of its own (composited, not painted with the page).
    ok &= check(isComposited(*view, "layer"), "will-change: transform makes a composited layer");
    ok &= check(!isComposited(*view, "still"), "a plain element is painted with its container");

    // The animations' frames come from the application's: one composited frame
    // per render(), with no rendering update needed.
    auto composited = [&] {
        return view->compositor()->compositedFrames();
    };
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    unsigned animationBefore = composited();
    for (int i = 0; i < 30; ++i)
        frame();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    unsigned animationFrames = composited() - animationBefore;
    ok &= check(animationFrames >= 24 && animationFrames <= 31, "CSS animations: one frame per application frame (%u for 30)", animationFrames);

    // No application frames, no animation frames: a paused game's UI stops too.
    unsigned idleBefore = composited();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ok &= check(composited() - idleBefore <= 1, "no frames without the application's (%u in 300 ms)", composited() - idleBefore);

    // A cap below the application's rate (zg_view_set_frame_rate): 20 a second
    // for an application at 60.
    view->compositor()->setFrameInterval(1_s / 20);
    unsigned cappedBefore = composited();
    for (int i = 0; i < 30; ++i)
        frame();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    unsigned cappedFrames = composited() - cappedBefore;
    ok &= check(cappedFrames >= 7 && cappedFrames <= 12, "capped at 20 a second: %u frames for 30 application frames at 60", cappedFrames);
    view->compositor()->setFrameInterval(0_s);

    // And they move: two frames apart differ where the slider is.
    Vector<uint8_t> first, second;
    bool readFirst = acquireAndRead(*view, first);
    for (int i = 0; i < 9; ++i)
        frame();
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
    unsigned stillFrames = composited();
    for (int i = 0; i < 18; ++i)
        frame();
    ok &= check(composited() - stillFrames <= 1, "a still page composites no frames for the application's (%u for 18)", composited() - stillFrames);
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

    // A 2D canvas draws on the GPU in a GPU view: a layer of its own, composited,
    // and readable back.
    {
        auto canvasView = ZeroG::View::create({ { viewWidth, viewHeight }, 1, false, true, ZeroG::createGPUSurface(3) });
        canvasView->loadHTML("<!DOCTYPE html><body style='margin: 0; background: white'><canvas id=canvas width=300 height=180></canvas><script>"
            "const context = canvas.getContext('2d');"
            "context.fillStyle = 'rgb(0, 160, 0)'; context.fillRect(0, 0, 300, 180);"
            "context.fillStyle = 'rgb(0, 0, 200)'; context.fillRect(100, 60, 100, 60);"
            "window.readBack = Array.from(context.getImageData(150, 90, 1, 1).data).slice(0, 3).join(',');"
            "</script>"_s);
        for (int i = 0; i < 20; ++i) {
            ZeroG::runPendingWork();
            canvasView->render(frameTime);
            frameTime += 16_ms;
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        ok &= check(isComposited(*canvasView, "canvas"), "an accelerated 2D canvas has a composited layer");
        auto readBack = canvasView->evaluateScript("String(window.readBack)"_s);
        ok &= check(readBack == "0,0,200"_s, "getImageData reads its pixels back: %s", readBack.utf8().data());
        Vector<uint8_t> pixels;
        bool read = acquireAndRead(*canvasView, pixels);
        auto at = [&](int x, int y) -> std::array<int, 3> {
            auto* pixel = pixels.span().data() + (static_cast<size_t>(y) * viewWidth + x) * 4;
            return { pixel[0], pixel[1], pixel[2] };
        };
        auto green = read ? at(30, 30) : std::array<int, 3> { -1, -1, -1 };
        auto blue = read ? at(150, 90) : std::array<int, 3> { -1, -1, -1 };
        ok &= check(green[0] < 30 && green[1] > 130 && green[2] < 30 && blue[0] < 30 && blue[1] < 30 && blue[2] > 170, "its drawing is composited: %d,%d,%d and %d,%d,%d",
            green[0], green[1], green[2], blue[0], blue[1], blue[2]);
    }

#if defined(ZEROG_MEDIA)
    // Video (standard, dev and debug builds): a layer of its own whose frames
    // go to the compositor as images, without repainting the page.
    {
        auto contents = FileSystem::readEntireFile(FileSystem::pathByAppendingComponent(String::fromUTF8(ZEROG_TEST_MEDIA), "red-blue.webm"_s));
        auto fs = ZeroG::MemoryFS::create();
        constexpr auto videoPage = "<!DOCTYPE html><body style='margin: 0; background: white'>"
            "<video id=video src=red-blue.webm muted style='position: absolute; left: 0; top: 0; width: 64px; height: 48px'></video>"
            "<script>video.onloadeddata = () => window.loaded = true; video.onended = () => window.ended = true;</script>";
        fs->addFile("/index.html"_s, std::span { byteCast<uint8_t>(videoPage), strlen(videoPage) });
        fs->addFile("/red-blue.webm"_s, contents ? contents->span() : std::span<const uint8_t> { });
        auto session = ZeroG::Session::create();
        session->registerScheme("app"_s, fs.copyRef());
        ZeroG::View::Configuration configuration;
        configuration.size = { viewWidth, viewHeight };
        configuration.surface = ZeroG::createGPUSurface(3);
        configuration.session = session.copyRef();
        auto videoView = ZeroG::View::create(WTF::move(configuration));
        videoView->loadURL(URL { "app://localhost/index.html"_s });
        auto videoFrame = [&] {
            ZeroG::runPendingWork();
            videoView->render(frameTime);
            frameTime += 16_ms;
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        };
        for (int i = 0; i < 300 && videoView->evaluateScript("String(window.loaded)"_s) != "true"_s; ++i)
            videoFrame();
        for (int i = 0; i < 20; ++i)
            videoFrame();
        ok &= check(isComposited(*videoView, "video"), "a video has a composited layer");
        auto centerColor = [&](Vector<uint8_t>& pixels) -> std::array<int, 3> {
            if (!acquireAndRead(*videoView, pixels))
                return { -1, -1, -1 };
            auto* center = pixels.span().data() + (static_cast<size_t>(24) * viewWidth + 32) * 4;
            return { center[0], center[1], center[2] };
        };
        Vector<uint8_t> pixels;
        auto red = centerColor(pixels);
        ok &= check(red[0] > 200 && red[1] < 60 && red[2] < 60, "its first frame is composited: %d,%d,%d", red[0], red[1], red[2]);

        // Playing: frames change ten times a second; the page does not repaint
        // for them.
        videoView->evaluateScript("video.play(); 0"_s);
        for (int i = 0; i < 10; ++i)
            videoFrame();
        unsigned updatesBefore = videoView->renderingUpdates();
        for (int i = 0; i < 60; ++i)
            videoFrame();
        unsigned updates = videoView->renderingUpdates() - updatesBefore;
        ok &= check(updates <= 4, "a second of playback (10 video frames) takes %u rendering updates of the page", updates);
        // Its frames stay YUV: the compositor's GPU converts them.
        auto yuvFrames = ZeroG::Media::framesKeptAsYUV();
        ok &= check(yuvFrames >= 5, "they reach the GPU as YUV planes, converted there (%llu frames)", static_cast<unsigned long long>(yuvFrames));
        for (int i = 0; i < 300 && videoView->evaluateScript("String(window.ended)"_s) != "true"_s; ++i)
            videoFrame();
        for (int i = 0; i < 10; ++i)
            videoFrame();
        videoView->repaint();
        videoFrame();
        auto blue = centerColor(pixels);
        ok &= check(blue[0] < 60 && blue[1] < 60 && blue[2] > 200, "and its last frame is composited: %d,%d,%d", blue[0], blue[1], blue[2]);
    }
#endif

    std::printf(ok ? "PASS\n" : "FAILED\n");
    return ok ? 0 : 1;
}
