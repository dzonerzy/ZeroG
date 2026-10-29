#include "config.h"
#include "Window.h"

#include "Embedding.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <SDL3/SDL.h>
#include <WebCore/GLContext.h>
#include <WebCore/GLDisplay.h>
#include <WebCore/PlatformDisplay.h>
#include <atomic>
#include <cstring>
#include <wtf/RunLoop.h>
#include <wtf/Scope.h>
#include <wtf/text/MakeString.h>

#if !OS(WINDOWS)
#include <dlfcn.h>
#endif

namespace ZeroG {

using namespace WebCore;

namespace {

String s_lastError;

// Engine work posted from other threads wakes the window's event wait.
Uint32 s_wakeUpEvent;
std::atomic<bool> s_wakeUpPending;

bool initializeVideo()
{
    static bool initialized = [] {
        if (!SDL_Init(SDL_INIT_VIDEO))
            return false;
        s_wakeUpEvent = SDL_RegisterEvents(1);
        RunLoop::setWakeUpCallback([] {
            if (s_wakeUpPending.exchange(true))
                return;
            SDL_Event event { };
            event.type = s_wakeUpEvent;
            SDL_PushEvent(&event);
        });
        return true;
    }();
    return initialized;
}

#if !OS(WINDOWS)
// Wayland EGL windows come from libwayland-egl, loaded with the driver.
struct WaylandEGL {
    void* (*create)(void* surface, int width, int height) { nullptr };
    void (*resize)(void* window, int width, int height, int dx, int dy) { nullptr };
    void (*destroy)(void* window) { nullptr };

    static WaylandEGL& singleton()
    {
        static WaylandEGL library = [] {
            WaylandEGL library;
            if (void* handle = dlopen("libwayland-egl.so.1", RTLD_LAZY | RTLD_LOCAL)) {
                library.create = reinterpret_cast<decltype(library.create)>(dlsym(handle, "wl_egl_window_create"));
                library.resize = reinterpret_cast<decltype(library.resize)>(dlsym(handle, "wl_egl_window_resize"));
                library.destroy = reinterpret_cast<decltype(library.destroy)>(dlsym(handle, "wl_egl_window_destroy"));
            }
            return library;
        }();
        return library;
    }
};

bool isWayland()
{
    const char* driver = SDL_GetCurrentVideoDriver();
    return driver && !std::strcmp(driver, "wayland");
}
#endif

// The window system's EGL display (Linux) and the native window EGL draws into.
struct NativeWindow {
    void* eglDisplay { nullptr }; // null: the engine's default display (ANGLE)
    void* window { nullptr };
};

std::optional<NativeWindow> nativeWindow(SDL_Window* window)
{
    auto properties = SDL_GetWindowProperties(window);
#if OS(WINDOWS)
    if (void* hwnd = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr))
        return NativeWindow { nullptr, hwnd };
    return std::nullopt;
#else
    auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (!getPlatformDisplay)
        return std::nullopt;
    if (isWayland()) {
        void* display = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        void* surface = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
        auto& wayland = WaylandEGL::singleton();
        if (!display || !surface || !wayland.create)
            return std::nullopt;
        int width, height;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        return NativeWindow { getPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, display, nullptr), wayland.create(surface, width, height) };
    }
    void* display = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
    auto xWindow = SDL_GetNumberProperty(properties, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    if (!display || !xWindow)
        return std::nullopt;
    return NativeWindow { getPlatformDisplay(EGL_PLATFORM_X11_KHR, display, nullptr), reinterpret_cast<void*>(static_cast<uintptr_t>(xWindow)) };
#endif
}

} // namespace

std::unique_ptr<Window> Window::create(Configuration&& configuration)
{
    initializeProcess();
    auto fail = [](ASCIILiteral what, const char* detail = nullptr) -> std::unique_ptr<Window> {
        s_lastError = detail && *detail ? makeString(what, ": "_s, String::fromUTF8(detail)) : String(what);
        return nullptr;
    };
    if (!initializeVideo())
        return fail("no video subsystem"_s, SDL_GetError());

    SDL_WindowFlags flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (configuration.resizable)
        flags |= SDL_WINDOW_RESIZABLE;
    if (configuration.frameless)
        flags |= SDL_WINDOW_BORDERLESS;
    auto title = configuration.title.utf8();
    auto* window = SDL_CreateWindow(byteCast<char>(title.data()), configuration.size.width(), configuration.size.height(), flags);
    if (!window)
        return fail("cannot create the window"_s, SDL_GetError());
    auto destroyWindow = makeScopeExit([&] { SDL_DestroyWindow(window); });

    auto native = nativeWindow(window);
    if (!native)
        return fail("no native window for EGL"_s, SDL_GetCurrentVideoDriver());
    if (!initializeGPU(native->eglDisplay))
        return fail("no GPU on the window system's display"_s, GLContext::lastErrorString());

    auto& display = PlatformDisplay::sharedDisplay();
    auto presentContext = GLContext::create(display.glDisplay(), GLContext::Target::Default, display.sharingGLContext(),
        reinterpret_cast<GLNativeWindowType>(native->window));
    if (!presentContext)
        return fail("cannot create the window's GL context"_s, GLContext::lastErrorString());

    float scale = SDL_GetWindowDisplayScale(window);
    auto surface = createGPUSurface(2);
    if (!surface)
        return fail("no GPU surface"_s);
    auto view = View::create({ configuration.size, scale > 0 ? scale : 1, false, true, WTF::move(surface) });

    destroyWindow.release();
    std::unique_ptr<Window> result(new Window(window, WTF::move(view), WTF::move(presentContext), native->window));
    result->m_showWhenPresented = configuration.visible;
    result->updateSize();
    return std::unique_ptr<Window> { result.release() };
}

Window::Window(SDL_Window* window, std::unique_ptr<View>&& view, std::unique_ptr<GLContext>&& presentContext, void* nativeWindow)
    : m_window(window)
    , m_view(WTF::move(view))
    , m_presentContext(WTF::move(presentContext))
    , m_nativeWindow(nativeWindow)
{
    if (auto* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(m_window)); mode && mode->refresh_rate > 0)
        m_refreshInterval = 1_s / mode->refresh_rate;
    if (m_presentContext->makeContextCurrent()) {
        // Present at the display's refresh rate. On Wayland a blocking swap waits
        // for the compositor's frame callback, which hidden or minimized windows
        // never get: there the frame clock alone paces presentation.
        int interval = 1;
#if !OS(WINDOWS)
        if (isWayland())
            interval = 0;
#endif
        eglSwapInterval(eglGetCurrentDisplay(), interval);
        glGenFramebuffers(1, &m_readFramebuffer);
    }
}

Window::~Window()
{
    if (m_presentContext->makeContextCurrent() && m_readFramebuffer)
        glDeleteFramebuffers(1, &m_readFramebuffer);
    m_presentContext = nullptr;
    m_view = nullptr;
#if !OS(WINDOWS)
    if (isWayland() && m_nativeWindow)
        WaylandEGL::singleton().destroy(m_nativeWindow);
#endif
    SDL_DestroyWindow(m_window);
}

const String& Window::lastError()
{
    return s_lastError;
}

void* Window::nativeHandle() const
{
    auto properties = SDL_GetWindowProperties(m_window);
#if OS(WINDOWS)
    return SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#else
    if (isWayland())
        return SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    return reinterpret_cast<void*>(static_cast<uintptr_t>(SDL_GetNumberProperty(properties, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0)));
#endif
}

void Window::setTitle(const String& title)
{
    auto utf8 = title.utf8();
    SDL_SetWindowTitle(m_window, byteCast<char>(utf8.data()));
}

void Window::setSize(const IntSize& size)
{
    SDL_SetWindowSize(m_window, size.width(), size.height());
    SDL_SyncWindow(m_window);
}

void Window::updateSize()
{
    int width, height;
    SDL_GetWindowSizeInPixels(m_window, &width, &height);
    float scale = SDL_GetWindowDisplayScale(m_window);
    if (scale <= 0)
        scale = 1;
#if !OS(WINDOWS)
    if (isWayland() && m_nativeWindow)
        WaylandEGL::singleton().resize(m_nativeWindow, width, height, 0, 0);
#endif
    m_view->setScale(scale);
    m_view->resize({ static_cast<int>(width / scale), static_cast<int>(height / scale) });
}

void Window::handleEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            m_closed = true;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            updateSize();
            break;
        case SDL_EVENT_WINDOW_EXPOSED:
            m_view->invalidate({ { }, m_view->size() });
            break;
        default:
            if (event.type == s_wakeUpEvent)
                s_wakeUpPending = false;
            break;
        }
    }
}

bool Window::runOnce(std::optional<Seconds> maxWait)
{
    if (m_closed)
        return false;

    // Engine work: expired timers and tasks posted to the main run loop.
    RunLoop::cycle();
    handleEvents();
    if (m_closed)
        return false;

    // One frame per display refresh. The swap of a presented frame waits for vsync
    // and gives the exact timing, so the next frame may start a little early; a
    // frame that changed nothing on screen waits a whole refresh interval. The
    // interval also bounds the rate where swaps do not block (no vblank, as on
    // Xvfb).
    auto now = MonotonicTime::now();
    bool hasWork = m_view->hasPendingRenderingUpdate() || m_view->hasDamage();
    if (hasWork && now >= m_nextFrameTime) {
        bool presented = m_view->render(now) && present();
        m_nextFrameTime = now + (presented ? m_refreshInterval * 0.9 : m_refreshInterval);
        if (presented)
            return !m_closed;
    }

    // Sleep until an event, engine work, or the next frame if one is wanted.
    now = MonotonicTime::now();
    auto wakeUp = maxWait ? now + *maxWait : MonotonicTime::infinity();
    if (m_view->hasPendingRenderingUpdate() || m_view->hasDamage())
        wakeUp = std::min(wakeUp, m_nextFrameTime);
    if (auto engineWork = RunLoop::mainSingleton().nextWakeUpTime())
        wakeUp = std::min(wakeUp, *engineWork);
    Sint32 timeout = wakeUp.isInfinity() ? -1 : static_cast<Sint32>(std::max<double>(0, std::ceil((wakeUp - now).milliseconds())));
    // Without an event to fill in, the event stays queued for the next iteration.
    SDL_WaitEventTimeout(nullptr, timeout);
    return !m_closed;
}

void Window::run()
{
    while (runOnce()) { }
}

bool Window::present()
{
    auto& surface = m_view->surface();
    Frame frame;
    if (!surface.acquire(frame))
        return false;
    auto releaseFrame = makeScopeExit([&] { surface.release(frame); });
    if (!m_presentContext->makeContextCurrent())
        return false;

    if (frame.fence)
        eglWaitSync(eglGetCurrentDisplay(), static_cast<EGLSync>(frame.fence), 0);

    int width, height;
    SDL_GetWindowSizeInPixels(m_window, &width, &height);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_readFramebuffer);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, static_cast<GLuint>(reinterpret_cast<uintptr_t>(frame.texture)), 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    // A frame older than a resize does not cover the window: clear the rest.
    if (static_cast<int>(frame.width) < width || static_cast<int>(frame.height) < height) {
        glClearColor(1, 1, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    // Frames are stored top row first, GL windows bottom row first.
    int copyWidth = std::min<int>(frame.width, width);
    int copyHeight = std::min<int>(frame.height, height);
    glBlitFramebuffer(0, 0, copyWidth, copyHeight, 0, height, copyWidth, height - copyHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    if (std::exchange(m_captureRequested, false)) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        Vector<uint8_t> bottomUp(static_cast<size_t>(width) * height * 4);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.mutableSpan().data());
        m_capture.resize(bottomUp.size());
        for (int y = 0; y < height; ++y)
            std::memcpy(m_capture.mutableSpan().data() + static_cast<size_t>(y) * width * 4, bottomUp.span().data() + static_cast<size_t>(height - 1 - y) * width * 4, width * 4);
        m_captureSize = { width, height };
    }

    m_presentContext->swapBuffers();
    m_lastPresent = MonotonicTime::now();
    ++m_presentedFrames;
    if (!m_shown && m_showWhenPresented) {
        SDL_ShowWindow(m_window);
        m_shown = true;
    }
    return true;
}

bool Window::takeCapture(Vector<uint8_t>& pixels, IntSize& size)
{
    if (m_capture.isEmpty())
        return false;
    pixels = std::exchange(m_capture, { });
    size = m_captureSize;
    return true;
}

} // namespace ZeroG
