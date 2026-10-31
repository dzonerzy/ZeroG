// Window mode (ZeroG.md, "Modalità" and "Ciclo di un frame"): the engine owns a
// native window, presents with the GPU at the display's refresh rate, and
// requestAnimationFrame follows the presentations. The presented image must match a
// CPU render of the page, and resizing the window must resize the page.
//
// Usage: window   (needs a display: a desktop, or Xvfb on Linux)

#include "config.h"
#include "Embedding.h"
#include "window/Window.h"

#include <WebCore/ColorSpace.h>
#include <WebCore/ImageBuffer.h>
#include <WebCore/Page.h>
#include <WebCore/PixelBuffer.h>
#include <WebCore/PixelBufferFormat.h>
#include <SDL3/SDL.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <wtf/text/StringToIntegerConversion.h>
#include <wtf/text/WTFString.h>

#if OS(WINDOWS)
#include <windows.h>
#include <powrprof.h>
#else
extern "C" int zerog_test_x11_input_shape_contains(void* display, unsigned long window, int x, int y); // window_x11.c
#endif

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

constexpr auto shapedPage = R"HTML(<!DOCTYPE html>
<html><head><style>
  html, body { margin: 0; background: transparent; }
  header { position: absolute; top: 0; left: 0; width: 300px; height: 40px; background: rgb(30, 30, 60); --zg-app-region: drag; }
  button { position: absolute; top: 5px; right: 5px; width: 30px; height: 30px; --zg-app-region: no-drag; }
  #card { position: absolute; top: 60px; left: 20px; width: 100px; height: 100px; background: rgba(255, 0, 0, 0.5); }
</style></head><body><header><button>x</button></header><div id="card"></div></body></html>)HTML";

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

// The test's window as SDL sees it. The desktop can minimize or cover a test
// window (another application, "show desktop"); then the system gives it no
// vsync and does not resize it until it is restored, which is not the engine's doing.
SDL_Window* sdlWindow()
{
    int count = 0;
    auto* windows = SDL_GetWindows(&count);
    SDL_Window* window = windows && count ? windows[0] : nullptr;
    SDL_free(windows);
    return window;
}

// Whether the display is off (the system's power saving, or a locked session):
// windows then get no vsync either. Windows says so through the console display
// state, whose current value comes as soon as one subscribes to it.
bool displayOff()
{
#if OS(WINDOWS)
    std::atomic<DWORD> state { 1 };
    DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS parameters { };
    parameters.Callback = [](PVOID context, ULONG type, PVOID setting) -> ULONG {
        auto* broadcast = static_cast<POWERBROADCAST_SETTING*>(setting);
        if (type == PBT_POWERSETTINGCHANGE && broadcast && broadcast->DataLength >= sizeof(DWORD))
            static_cast<std::atomic<DWORD>*>(context)->store(*reinterpret_cast<const DWORD*>(broadcast->Data));
        return 0;
    };
    parameters.Context = &state;
    HPOWERNOTIFY registration = nullptr;
    if (PowerSettingRegisterNotification(&GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_CALLBACK, &parameters, &registration) != ERROR_SUCCESS)
        return false;
    Sleep(50);
    PowerSettingUnregisterNotification(registration);
    return !state; // 0: off, 1: on, 2: dimmed
#else
    return false; // Xvfb and Weston's headless backend have no power saving
#endif
}

bool hiddenByDesktop()
{
    auto* window = sdlWindow();
    return displayOff() || (window && (SDL_GetWindowFlags(window) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED)));
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
    if (hiddenByDesktop()) {
        // No vsync for a hidden window: the rate says nothing about the engine.
        std::printf("     skipped: the display is off, or the desktop minimized or covered the window (rAF %.0f per second, %u frames)\n", rate, window->presentedFrames());
    } else {
        ok &= check(rate <= refresh * 1.1 && rate >= refresh * 0.4, "rAF at the display's rate: %.0f per second at %.0f Hz", rate, refresh);
        ok &= check(window->presentedFrames() >= 30 && window->presentedFrames() <= callbacks, "presented %u frames for the animation's changes", window->presentedFrames());
    }

    // The presented image against a CPU render, with the animation stopped.
    window->view().evaluateScript("step = () => { }; 0"_s);
    runUntil(*window, 1_s, [&] { return false; });
    window->view().repaint();
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

    // WebGL on the window system's display (standard, dev and debug builds):
    // ANGLE opens it too, and draws.
    if (window->view().evaluateScript("typeof WebGLRenderingContext"_s) == "function"_s) {
        auto pixel = window->view().evaluateScript("(() => { const gl = document.createElement('canvas').getContext('webgl');"
            "if (!gl) return 'no context'; gl.clearColor(0, 1, 0, 1); gl.clear(gl.COLOR_BUFFER_BIT);"
            "const p = new Uint8Array(4); gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, p); return p.join(); })()"_s);
        ok &= check(pixel == "0,255,0,255"_s, "WebGL draws in the window: %s", pixel.utf8().data());
    }

    // Input through the window's own events: SDL events in, page effects out.
    window->view().evaluateScript("const field = document.createElement('input'); field.id = 'field';"
        "field.style.cssText = 'position: absolute; left: 20px; top: 150px; width: 150px; height: 25px';"
        "document.body.appendChild(field); 0"_s);
    runUntil(*window, 200_ms, [&] { return false; });
    auto push = [](SDL_Event event) {
        SDL_PushEvent(&event);
    };
    SDL_Event motion { };
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.x = 60;
    motion.motion.y = 162;
    push(motion);
    ok &= check(runUntil(*window, 2_s, [&] { return window->view().cursor() == PlatformCursorType::IBeam; }), "text cursor over the text field");
    for (auto type : { SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP }) {
        SDL_Event button { };
        button.type = type;
        button.button.button = SDL_BUTTON_LEFT;
        button.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        button.button.clicks = 1;
        button.button.x = 60;
        button.button.y = 162;
        push(button);
    }
    for (auto [scancode, text] : { std::pair { SDL_SCANCODE_H, "h" }, std::pair { SDL_SCANCODE_I, "i" } }) {
        SDL_Event key { };
        key.type = SDL_EVENT_KEY_DOWN;
        key.key.scancode = scancode;
        key.key.down = true;
        push(key);
        SDL_Event input { };
        input.type = SDL_EVENT_TEXT_INPUT;
        input.text.text = text;
        push(input);
        key.type = SDL_EVENT_KEY_UP;
        key.key.down = false;
        push(key);
    }
    ok &= check(runUntil(*window, 2_s, [&] { return window->view().evaluateScript("field.value"_s) == "hi"_s; }),
        "click and typing reach the text field: '%s'", window->view().evaluateScript("field.value"_s).utf8().data());

    // Resizing the window resizes the page. A minimized window is resized only
    // once restored: restore it first if the desktop minimized it.
    if (auto* sdl = sdlWindow(); sdl && (SDL_GetWindowFlags(sdl) & SDL_WINDOW_MINIMIZED)) {
        std::printf("     the desktop minimized the window: restoring it\n");
        SDL_RestoreWindow(sdl);
        SDL_SyncWindow(sdl);
    }
    window->setSize({ 500, 300 });
    bool resized = runUntil(*window, 5_s, [&] { return window->view().size() == IntSize(500, 300); });
    ok &= check(resized, "page resized with the window: %dx%d", window->view().size().width(), window->view().size().height());
    if (!resized) {
        // What SDL reports, to tell a resize it refused from one the view missed.
        if (auto* sdl = sdlWindow()) {
            int width = 0, height = 0, pixelWidth = 0, pixelHeight = 0;
            SDL_GetWindowSize(sdl, &width, &height);
            SDL_GetWindowSizeInPixels(sdl, &pixelWidth, &pixelHeight);
            std::printf("     SDL: %dx%d (%dx%d pixels), flags 0x%llx\n", width, height, pixelWidth, pixelHeight,
                static_cast<unsigned long long>(SDL_GetWindowFlags(sdl)));
        }
    }
    auto innerWidth = window->view().evaluateScript("innerWidth + 'x' + innerHeight"_s);
    ok &= check(innerWidth == "500x300"_s, "window.innerWidth/innerHeight: %s", innerWidth.utf8().data());
    window = nullptr;

    // A frameless, transparent window: what the page leaves transparent is
    // transparent in what the window presents, a translucent part translucent;
    // its header moves it (--zg-app-region: drag), the header's button does
    // not (no-drag), the edges resize it.
    auto shaped = ZeroG::Window::create({ "ZeroG shaped window"_s, { 300, 200 }, true, true, true, nullptr, true });
    ok &= check(!!shaped, "a frameless transparent window %s", ZeroG::Window::lastError().utf8().data());
    if (shaped) {
        shaped->view().loadHTML(String::fromUTF8(shapedPage));
        runUntil(*shaped, 1_s, [&] { return false; });
        shaped->view().repaint();
        shaped->captureNextFrame();
        Vector<uint8_t> shapedPixels;
        IntSize shapedSize;
        ok &= check(runUntil(*shaped, 5_s, [&] { return shaped->takeCapture(shapedPixels, shapedSize); }), "captured its frame (%dx%d)", shapedSize.width(), shapedSize.height());
        float scale = shaped->view().scale();
        auto pixelAt = [&](int x, int y) -> std::array<int, 4> {
            int px = static_cast<int>(x * scale), py = static_cast<int>(y * scale);
            if (px >= shapedSize.width() || py >= shapedSize.height())
                return { -1, -1, -1, -1 };
            auto* p = shapedPixels.span().data() + (static_cast<size_t>(py) * shapedSize.width() + px) * 4;
            return { p[0], p[1], p[2], p[3] };
        };
        if (!shapedPixels.isEmpty()) {
            auto empty = pixelAt(200, 150);
            auto header = pixelAt(50, 20);
            auto card = pixelAt(60, 100);
            ok &= check(empty[3] == 0, "the page's empty part is transparent: alpha %d", empty[3]);
            ok &= check(header[3] == 255 && header[0] < 60 && header[2] > 40, "its header opaque: (%d, %d, %d, %d)", header[0], header[1], header[2], header[3]);
            ok &= check(card[3] > 100 && card[3] < 160 && card[0] > 100 && card[0] < 160, "a half transparent red, premultiplied: (%d, %d, %d, %d)", card[0], card[1], card[2], card[3]);
        }
        using HitTest = ZeroG::Window::HitTest;
        ok &= check(shaped->hitTest(50, 20) == HitTest::Drag, "the header moves the window");
        ok &= check(shaped->hitTest(280, 20) == HitTest::Page, "its button is the page's");
        ok &= check(shaped->hitTest(150, 150) == HitTest::Page, "and so is the rest");
        ok &= check(shaped->hitTest(1, 100) == HitTest::ResizeLeft && shaped->hitTest(298, 198) == HitTest::ResizeBottomRight && shaped->hitTest(150, 1) == HitTest::ResizeTop,
            "the edges resize it");
#if OS(WINDOWS)
        // What Windows itself gets for the header: the caption, moved (and
        // snapped, maximized by a double click) by the system.
        if (auto* hwnd = static_cast<HWND>(shaped->nativeHandle())) {
            auto ask = [&](float x, float y) {
                POINT point { static_cast<LONG>(x * scale), static_cast<LONG>(y * scale) };
                ClientToScreen(hwnd, &point);
                return SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y));
            };
            ok &= check(ask(50, 20) == HTCAPTION && ask(280, 20) == HTCLIENT && ask(1, 100) == HTLEFT, "Windows gets the caption, the client area and the edge: %d, %d, %d",
                static_cast<int>(ask(50, 20)), static_cast<int>(ask(280, 20)), static_cast<int>(ask(1, 100)));

            // On the screen, as the desktop composes it: the header is drawn, the
            // empty part shows what is behind the window (the same pixel with the
            // window hidden).
            auto* sdl = sdlWindow();
            if (sdl && !hiddenByDesktop()) {
                // Behind it, a window of one known colour (what is behind on a
                // real desktop changes on its own).
                int left, top;
                SDL_GetWindowPosition(sdl, &left, &top);
                auto* backdrop = SDL_CreateWindow("ZeroG backdrop", 400, 300, SDL_WINDOW_BORDERLESS);
                auto paintBackdrop = [&] {
                    if (auto* surface = backdrop ? SDL_GetWindowSurface(backdrop) : nullptr) {
                        SDL_FillSurfaceRect(surface, nullptr, SDL_MapSurfaceRGB(surface, 0, 160, 0));
                        SDL_UpdateWindowSurface(backdrop);
                    }
                };
                if (backdrop) {
                    SDL_SetWindowPosition(backdrop, left - 50, top - 50);
                    SDL_ShowWindow(backdrop);
                    paintBackdrop();
                }
                SDL_SetWindowAlwaysOnTop(sdl, true);
                SDL_RaiseWindow(sdl);
                // Shown and painted (the system may need a few turns).
                runUntil(*shaped, 500_ms, [&] { return false; });
                paintBackdrop();
                runUntil(*shaped, 500_ms, [&] { return false; });
                auto screen = [&](float x, float y) {
                    POINT point { static_cast<LONG>(x * scale), static_cast<LONG>(y * scale) };
                    ClientToScreen(hwnd, &point);
                    HDC dc = GetDC(nullptr);
                    COLORREF color = GetPixel(dc, point.x, point.y);
                    ReleaseDC(nullptr, dc);
                    return color;
                };
                auto headerOnScreen = screen(50, 20);
                auto emptyOnScreen = screen(200, 150);
                // The window's own edges, active: nothing of a frame (the system's
                // border, its shadow) may show there either.
                auto leftEdge = screen(0, 150);
                auto bottomEdge = screen(200, 199.5f);
                auto rightEdge = screen(299.5f, 150);
                SDL_HideWindow(sdl);
                runUntil(*shaped, 500_ms, [&] { return false; });
                auto behind = screen(200, 150);
                // Only when the desktop let the test see it: the backdrop showing
                // where the window was, the screen readable (another window, a
                // lock screen or a busy desktop can be in the way).
                if (behind == RGB(0, 160, 0) && headerOnScreen != CLR_INVALID) {
                    ok &= check(GetRValue(headerOnScreen) == 30 && GetBValue(headerOnScreen) == 60, "on screen the header is drawn: #%06lx", static_cast<unsigned long>(headerOnScreen));
                    ok &= check(emptyOnScreen == behind, "and the empty part shows the window behind: #%06lx, #%06lx without the window",
                        static_cast<unsigned long>(emptyOnScreen), static_cast<unsigned long>(behind));
                    ok &= check(leftEdge == behind && bottomEdge == behind && rightEdge == behind, "no frame at its edges: #%06lx, #%06lx, #%06lx",
                        static_cast<unsigned long>(leftEdge), static_cast<unsigned long>(bottomEdge), static_cast<unsigned long>(rightEdge));
                } else
                    std::printf("     skipped the screen check: the backdrop was not where the window was (#%06lx)\n", static_cast<unsigned long>(behind));
                if (backdrop)
                    SDL_DestroyWindow(backdrop);
            } else
                std::printf("     skipped the screen check: the window is hidden or the display off\n");
        }
#endif
    }
    shaped = nullptr;

    // Click-through: the same page, clicks on its transparent part going to
    // what is behind the window. On Windows a layered window fed from a CPU
    // copy of each frame, which the system hit tests by alpha; on X11 the
    // frame's alpha as the input shape; on Wayland nothing to check.
    auto through = ZeroG::Window::create({ "ZeroG click-through window"_s, { 300, 200 }, true, true, true, nullptr, true, true });
    ok &= check(!!through, "a click-through window %s", ZeroG::Window::lastError().utf8().data());
    if (through) {
        through->view().loadHTML(String::fromUTF8(shapedPage));
        runUntil(*through, 1_s, [&] { return false; });
        through->view().repaint();
        through->captureNextFrame();
        Vector<uint8_t> throughPixels;
        IntSize throughSize;
        ok &= check(runUntil(*through, 5_s, [&] { return through->takeCapture(throughPixels, throughSize); }), "its frame was presented (%dx%d)", throughSize.width(), throughSize.height());
        float scale = through->view().scale();
        auto alphaAt = [&](int x, int y) {
            int px = static_cast<int>(x * scale), py = static_cast<int>(y * scale);
            if (throughPixels.isEmpty() || px >= throughSize.width() || py >= throughSize.height())
                return -1;
            return static_cast<int>(throughPixels[(static_cast<size_t>(py) * throughSize.width() + px) * 4 + 3]);
        };
        ok &= check(alphaAt(200, 150) == 0 && alphaAt(50, 20) == 255, "transparent and opaque where the page is: %d, %d", alphaAt(200, 150), alphaAt(50, 20));
        ok &= check(through->hitTest(50, 20) == ZeroG::Window::HitTest::Drag, "its header still moves it");
#if OS(WINDOWS)
        auto* hwnd = static_cast<HWND>(through->nativeHandle());
        auto windowAt = [&](float x, float y) {
            POINT point { static_cast<LONG>(x * scale), static_cast<LONG>(y * scale) };
            ClientToScreen(hwnd, &point);
            return GetAncestor(WindowFromPoint(point), GA_ROOT);
        };
        if (!hiddenByDesktop()) {
            ok &= check(windowAt(200, 150) != hwnd, "a click on the transparent part goes behind the window");
            ok &= check(windowAt(50, 20) == hwnd && windowAt(60, 100) == hwnd, "on the header and the translucent card it is the window's");
        } else
            std::printf("     skipped the clicks: the window is hidden or the display off\n");
#else
        if (auto* sdl = sdlWindow(); sdl && !std::getenv("WAYLAND_DISPLAY")) {
            auto* shape = static_cast<SDL_Surface*>(SDL_GetPointerProperty(SDL_GetWindowProperties(sdl), SDL_PROP_WINDOW_SHAPE_POINTER, nullptr));
            Uint8 r, g, b, emptyAlpha = 255, headerAlpha = 0;
            if (shape) {
                SDL_ReadSurfacePixel(shape, static_cast<int>(200 * scale), static_cast<int>(150 * scale), &r, &g, &b, &emptyAlpha);
                SDL_ReadSurfacePixel(shape, static_cast<int>(50 * scale), static_cast<int>(20 * scale), &r, &g, &b, &headerAlpha);
            }
            ok &= check(shape && !emptyAlpha && headerAlpha == 255, "its input shape is the page's alpha: %d, %d", emptyAlpha, headerAlpha);
            // And the X server has it: clicks on the empty part go through.
            auto properties = SDL_GetWindowProperties(sdl);
            auto* display = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
            auto window = static_cast<unsigned long>(SDL_GetNumberProperty(properties, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
            if (display && window) {
                int empty = zerog_test_x11_input_shape_contains(display, window, static_cast<int>(200 * scale), static_cast<int>(150 * scale));
                int header = zerog_test_x11_input_shape_contains(display, window, static_cast<int>(50 * scale), static_cast<int>(20 * scale));
                ok &= check(!empty && header == 1, "the X server's input shape lets the empty part's clicks through: %d, %d", empty, header);
            }
        }
#endif
    }

    std::printf(ok ? "PASS\n" : "FAILED\n");
    return ok ? 0 : 1;
}
