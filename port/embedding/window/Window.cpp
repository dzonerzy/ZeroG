#include "config.h"
#include "Window.h"

#include "ClipboardSDL.h"
#include "Embedding.h"
#include "accessibility/Accessibility.h"
#include "graphics/Compositor.h"
#include "graphics/Surface.h"
#include "KeyboardSDL.h"
#include <WebCore/FileChooser.h>
#include <WebCore/MIMETypeRegistry.h>
#include <wtf/CrossThreadCopier.h>
#include <wtf/text/CString.h>
#include <wtf/text/StringBuilder.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <SDL3/SDL.h>
#include <WebCore/GLContext.h>
#include <WebCore/GLDisplay.h>
#include <WebCore/PlatformDisplay.h>
#include <atomic>
#include <cstring>
#include <wtf/HashMap.h>
#include <wtf/NeverDestroyed.h>
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
        addWakeUpObserver([] {
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

// The window system's EGL display (Linux), the platform and native display it was
// opened from, and the native window EGL draws into.
struct NativeWindow {
    void* eglDisplay { nullptr }; // null: the engine's default display (ANGLE)
    void* window { nullptr };
    int platform { 0 };
    void* display { nullptr };
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
        return NativeWindow { getPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, display, nullptr), wayland.create(surface, width, height), EGL_PLATFORM_WAYLAND_KHR, display };
    }
    void* display = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
    auto xWindow = SDL_GetNumberProperty(properties, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    if (!display || !xWindow)
        return std::nullopt;
    return NativeWindow { getPlatformDisplay(EGL_PLATFORM_X11_KHR, display, nullptr), reinterpret_cast<void*>(static_cast<uintptr_t>(xWindow)), EGL_PLATFORM_X11_KHR, display };
#endif
}

#if OS(WINDOWS)
#ifndef EGL_DIRECT_COMPOSITION_ANGLE
#define EGL_DIRECT_COMPOSITION_ANGLE 0x33A5 // EGL/eglext_angle.h
#endif

// A transparent window presents through DirectComposition: a swap chain with
// premultiplied alpha (ANGLE's EGL_ANGLE_direct_composition), where a window's
// own swap chain would ignore the alpha.
std::unique_ptr<GLContext> createDirectCompositionContext(GLDisplay& display, GLContext* sharing, void* hwnd)
{
    EGLDisplay eglDisplay = display.eglDisplay();
    const char* extensions = eglQueryString(eglDisplay, EGL_EXTENSIONS);
    if (!extensions || !std::strstr(extensions, "EGL_ANGLE_direct_composition"))
        return nullptr;
    const EGLint configAttributes[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_NONE
    };
    EGLConfig config;
    EGLint count = 0;
    if (!eglChooseConfig(eglDisplay, configAttributes, &config, 1, &count) || !count)
        return nullptr;
    const EGLint surfaceAttributes[] = { EGL_DIRECT_COMPOSITION_ANGLE, EGL_TRUE, EGL_NONE };
    EGLSurface surface = eglCreateWindowSurface(eglDisplay, config, static_cast<EGLNativeWindowType>(hwnd), surfaceAttributes);
    if (surface == EGL_NO_SURFACE)
        return nullptr;
    const EGLint contextAttributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext context = eglCreateContext(eglDisplay, config, sharing ? sharing->platformContext() : EGL_NO_CONTEXT, contextAttributes);
    if (context == EGL_NO_CONTEXT) {
        eglDestroySurface(eglDisplay, surface);
        return nullptr;
    }
    return makeUnique<GLContext>(display, context, surface, config);
}

#endif

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
    // With SDL's video running, the clipboard is the system's (unless the
    // application gave its own).
    useSDLClipboard();

    SDL_WindowFlags flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (configuration.resizable)
        flags |= SDL_WINDOW_RESIZABLE;
    if (configuration.frameless)
        flags |= SDL_WINDOW_BORDERLESS;
    if (configuration.transparent)
        flags |= SDL_WINDOW_TRANSPARENT;
    auto title = configuration.title.utf8();
    auto* window = SDL_CreateWindow(byteCast<char>(title.data()), configuration.size.width(), configuration.size.height(), flags);
    if (!window)
        return fail("cannot create the window"_s, SDL_GetError());
    auto destroyWindow = makeScopeExit([&] { SDL_DestroyWindow(window); });

    auto native = nativeWindow(window);
    if (!native)
        return fail("no native window for EGL"_s, SDL_GetCurrentVideoDriver());
    if (!initializeGPU(native->eglDisplay, native->platform, native->display))
        return fail("no GPU on the window system's display"_s, GLContext::lastErrorString());

    auto& display = PlatformDisplay::sharedDisplay();
    std::unique_ptr<GLContext> presentContext;
    bool clickThrough = configuration.transparent && configuration.clickThrough;
#if OS(WINDOWS)
    // Click-through: a layered window, its frames read back and given to it
    // with UpdateLayeredWindow (Windows lets clicks through its alpha 0); the
    // GL context only reads frames, with no window surface.
    if (clickThrough) {
        auto hwnd = static_cast<HWND>(native->window);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
        presentContext = GLContext::createOffscreen(display);
        if (!presentContext)
            return fail("no GL context to read a click-through window's frames"_s, GLContext::lastErrorString());
    } else if (configuration.transparent) {
        presentContext = createDirectCompositionContext(display.glDisplay(), display.sharingGLContext(), native->window);
        if (!presentContext)
            return fail("no DirectComposition for a transparent window"_s, GLContext::lastErrorString());
    }
#endif
    // Linux: the EGL config has alpha (RGBA8888), and SDL gave a transparent
    // window an ARGB visual (X11) or a surface with alpha (Wayland).
    if (!presentContext) {
        presentContext = GLContext::create(display.glDisplay(), GLContext::Target::Default, display.sharingGLContext(),
            reinterpret_cast<GLNativeWindowType>(native->window));
    }
    if (!presentContext)
        return fail("cannot create the window's GL context"_s, GLContext::lastErrorString());

    float scale = SDL_GetWindowDisplayScale(window);
    auto surface = createGPUSurface(2);
    if (!surface)
        return fail("no GPU surface"_s);
    auto view = View::create({ configuration.size, scale > 0 ? scale : 1, configuration.transparent, true, WTF::move(surface), WTF::move(configuration.session) });

    destroyWindow.release();
    std::unique_ptr<Window> result(new Window(window, WTF::move(view), WTF::move(presentContext), native->window));
    result->m_showWhenPresented = configuration.visible;
    result->m_transparent = configuration.transparent;
    result->m_clickThrough = clickThrough;
#if OS(WINDOWS)
    result->m_layered = clickThrough;
#endif
    result->updateSize();
    // Text input events (typed characters, input method commits).
    SDL_StartTextInput(window);
    return std::unique_ptr<Window> { result.release() };
}

// The open windows, for their events.
static HashMap<SDL_Window*, Window*>& windows()
{
    static NeverDestroyed<HashMap<SDL_Window*, Window*>> windows;
    return windows;
}

Window::Window(SDL_Window* window, std::unique_ptr<View>&& view, std::unique_ptr<GLContext>&& presentContext, void* nativeWindow)
    : m_window(window)
    , m_view(WTF::move(view))
    , m_presentContext(WTF::move(presentContext))
    , m_nativeWindow(nativeWindow)
{
    windows().set(m_window, this);
    if (auto* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(m_window)); mode && mode->refresh_rate > 0)
        m_refreshInterval = 1_s / mode->refresh_rate;
    // Animations are composited at the display's rate, by the compositor itself.
    if (auto* compositor = m_view->compositor()) {
        compositor->setPacing(Compositor::Pacing::Display);
        compositor->setFrameInterval(m_refreshInterval);
    }
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
    {
        int width, height;
        SDL_GetWindowSizeInPixels(m_window, &width, &height);
        Locker locker { m_presentLock };
        m_pixelSize = { width, height };
    }
    // A composited view's frames are presented on the compositor thread as it
    // publishes them: the window's context becomes that thread's (one thread at
    // a time may have it current).
    if (auto* compositor = m_view->compositor()) {
        m_presentContext->unmakeContextCurrent();
        compositor->setPresenter([this] {
            present();
        });
    }
    m_view->setCursorChangedCallback([this](PlatformCursorType type) {
        updateCursor(type);
    });
    m_accessibility = Accessibility::createBridge(*m_view, nativeHandle(), String::fromUTF8(SDL_GetWindowTitle(m_window)));
    // A frameless window moves and resizes by what the page says (hitTest);
    // the window system asks on its own thread of events, which is this one.
    if (SDL_GetWindowFlags(m_window) & SDL_WINDOW_BORDERLESS) {
        SDL_SetWindowHitTest(m_window, [](SDL_Window*, const SDL_Point* area, void* data) -> SDL_HitTestResult {
            switch (static_cast<Window*>(data)->hitTest(area->x, area->y)) {
            case HitTest::Page: return SDL_HITTEST_NORMAL;
            case HitTest::Drag: return SDL_HITTEST_DRAGGABLE;
            case HitTest::ResizeTopLeft: return SDL_HITTEST_RESIZE_TOPLEFT;
            case HitTest::ResizeTop: return SDL_HITTEST_RESIZE_TOP;
            case HitTest::ResizeTopRight: return SDL_HITTEST_RESIZE_TOPRIGHT;
            case HitTest::ResizeRight: return SDL_HITTEST_RESIZE_RIGHT;
            case HitTest::ResizeBottomRight: return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
            case HitTest::ResizeBottom: return SDL_HITTEST_RESIZE_BOTTOM;
            case HitTest::ResizeBottomLeft: return SDL_HITTEST_RESIZE_BOTTOMLEFT;
            case HitTest::ResizeLeft: return SDL_HITTEST_RESIZE_LEFT;
            }
            return SDL_HITTEST_NORMAL;
        }, this);
    }
}

Window::HitTest Window::hitTest(float x, float y)
{
    auto flags = SDL_GetWindowFlags(m_window);
    // The edges of a resizable window, as a frame's would be.
    if ((flags & SDL_WINDOW_RESIZABLE) && !(flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED))) {
        constexpr float edge = 6;
        int width, height;
        SDL_GetWindowSize(m_window, &width, &height);
        bool left = x < edge, right = x >= width - edge, top = y < edge, bottom = y >= height - edge;
        if (top && left)
            return HitTest::ResizeTopLeft;
        if (top && right)
            return HitTest::ResizeTopRight;
        if (bottom && left)
            return HitTest::ResizeBottomLeft;
        if (bottom && right)
            return HitTest::ResizeBottomRight;
        if (top)
            return HitTest::ResizeTop;
        if (bottom)
            return HitTest::ResizeBottom;
        if (left)
            return HitTest::ResizeLeft;
        if (right)
            return HitTest::ResizeRight;
    }
    return m_view->appRegionAt(viewPoint(x, y)) == "drag"_s ? HitTest::Drag : HitTest::Page;
}

Window::~Window()
{
    // Assistive technologies let go of the page before it goes.
    m_accessibility = nullptr;
    windows().remove(m_window);
    // The window's context comes back to this thread.
    if (auto* compositor = m_view->compositor())
        compositor->setPresenter(nullptr);
    if (m_presentContext->makeContextCurrent() && m_readFramebuffer)
        glDeleteFramebuffers(1, &m_readFramebuffer);
    m_presentContext = nullptr;
    m_view = nullptr;
    for (auto* cursor : m_cursors) {
        if (cursor)
            SDL_DestroyCursor(cursor);
    }
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

namespace {

// What SDL's dialog needs until it answers: the filters, and the answer to give.
struct OpenDialog {
    CompletionHandler<void(std::optional<Vector<String>>&&)> completionHandler;
    std::string name;
    std::string pattern;
    SDL_DialogFileFilter filter { };

    static void SDLCALL done(void* user, const char* const* files, int)
    {
        // SDL may answer on another thread.
        std::optional<Vector<String>> paths;
        if (files && *files) {
            paths = Vector<String> { };
            for (auto* file = files; *file; ++file)
                paths->append(String::fromUTF8(*file));
        }
        callOnMainThread([dialog = std::unique_ptr<OpenDialog>(static_cast<OpenDialog*>(user)), paths = crossThreadCopy(WTF::move(paths))] mutable {
            dialog->completionHandler(WTF::move(paths));
        });
    }
};

} // namespace

void Window::showOpenDialog(const FileChooserSettings& settings, CompletionHandler<void(std::optional<Vector<String>>&&)>&& completionHandler)
{
    auto dialog = std::make_unique<OpenDialog>();
    dialog->completionHandler = WTF::move(completionHandler);
    // accept=".txt,image/png": the extensions, those of each MIME type included.
    Vector<String> extensions;
    for (auto& extension : settings.acceptFileExtensions)
        extensions.append(extension.startsWith('.') ? extension.substring(1) : extension);
    for (auto& type : settings.acceptMIMETypes) {
        for (auto& extension : MIMETypeRegistry::extensionsForMIMEType(type))
            extensions.append(extension);
    }
    int filters = 0;
    if (!extensions.isEmpty()) {
        auto pattern = makeStringByJoining(extensions, ";"_s).utf8();
        dialog->name = "Accepted files";
        dialog->pattern.assign(byteCast<char>(pattern.data()), pattern.length());
        dialog->filter = { dialog->name.c_str(), dialog->pattern.c_str() };
        filters = 1;
    }
    auto* raw = dialog.release();
    if (settings.allowsDirectories)
        SDL_ShowOpenFolderDialog(OpenDialog::done, raw, m_window, nullptr, settings.allowsMultipleFiles);
    else
        SDL_ShowOpenFileDialog(OpenDialog::done, raw, m_window, filters ? &raw->filter : nullptr, filters, nullptr, settings.allowsMultipleFiles);
}

void Window::setTitle(const String& title)
{
    auto utf8 = title.utf8();
    SDL_SetWindowTitle(m_window, byteCast<char>(utf8.data()));
    if (m_accessibility)
        m_accessibility->titleChanged(title);
}

void Window::setFullscreen(bool fullscreen)
{
    SDL_SetWindowFullscreen(m_window, fullscreen);
}

void Window::raise()
{
    SDL_RaiseWindow(m_window);
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
    {
        // The native window is resized where frames are presented, before a
        // swap at the new size.
        Locker locker { m_presentLock };
        m_pixelSize = { width, height };
        m_nativeWindowSize = { width, height };
    }
    if (!m_view->compositor()) {
        // Presented here: the Wayland window takes its size now.
#if !OS(WINDOWS)
        if (isWayland() && m_nativeWindow)
            WaylandEGL::singleton().resize(m_nativeWindow, width, height, 0, 0);
#endif
        Locker locker { m_presentLock };
        m_nativeWindowSize = { };
    }
    m_view->setScale(scale);
    m_view->resize({ static_cast<int>(width / scale), static_cast<int>(height / scale) });
}

static SDL_SystemCursor systemCursor(PlatformCursorType type)
{
    switch (type) {
    case PlatformCursorType::Hand:
        return SDL_SYSTEM_CURSOR_POINTER;
    case PlatformCursorType::IBeam:
    case PlatformCursorType::VerticalText:
        return SDL_SYSTEM_CURSOR_TEXT;
    case PlatformCursorType::Wait:
        return SDL_SYSTEM_CURSOR_WAIT;
    case PlatformCursorType::Progress:
        return SDL_SYSTEM_CURSOR_PROGRESS;
    case PlatformCursorType::Cross:
    case PlatformCursorType::Cell:
        return SDL_SYSTEM_CURSOR_CROSSHAIR;
    case PlatformCursorType::Move:
    case PlatformCursorType::MiddlePanning:
    case PlatformCursorType::Grab:
    case PlatformCursorType::Grabbing:
        return SDL_SYSTEM_CURSOR_MOVE;
    case PlatformCursorType::NotAllowed:
    case PlatformCursorType::NoDrop:
        return SDL_SYSTEM_CURSOR_NOT_ALLOWED;
    case PlatformCursorType::EastResize:
    case PlatformCursorType::WestResize:
    case PlatformCursorType::EastWestResize:
    case PlatformCursorType::ColumnResize:
        return SDL_SYSTEM_CURSOR_EW_RESIZE;
    case PlatformCursorType::NorthResize:
    case PlatformCursorType::SouthResize:
    case PlatformCursorType::NorthSouthResize:
    case PlatformCursorType::RowResize:
        return SDL_SYSTEM_CURSOR_NS_RESIZE;
    case PlatformCursorType::NorthEastResize:
    case PlatformCursorType::SouthWestResize:
    case PlatformCursorType::NorthEastSouthWestResize:
        return SDL_SYSTEM_CURSOR_NESW_RESIZE;
    case PlatformCursorType::NorthWestResize:
    case PlatformCursorType::SouthEastResize:
    case PlatformCursorType::NorthWestSouthEastResize:
        return SDL_SYSTEM_CURSOR_NWSE_RESIZE;
    default:
        return SDL_SYSTEM_CURSOR_DEFAULT;
    }
}

void Window::updateCursor(PlatformCursorType type)
{
    if (type == PlatformCursorType::None) {
        SDL_HideCursor();
        return;
    }
    SDL_ShowCursor();
    auto cursor = systemCursor(type);
    if (!m_cursors[cursor])
        m_cursors[cursor] = SDL_CreateSystemCursor(cursor);
    SDL_SetCursor(m_cursors[cursor]);
}

// SDL window coordinates to the view's logical pixels.
FloatPoint Window::viewPoint(float x, float y) const
{
    int width, height, pixelWidth, pixelHeight;
    SDL_GetWindowSize(m_window, &width, &height);
    SDL_GetWindowSizeInPixels(m_window, &pixelWidth, &pixelHeight);
    float toPixelsX = width ? static_cast<float>(pixelWidth) / width : 1;
    float toPixelsY = height ? static_cast<float>(pixelHeight) / height : 1;
    return { x * toPixelsX / m_view->scale(), y * toPixelsY / m_view->scale() };
}

View::MouseButton Window::mouseButton(uint8_t button)
{
    switch (button) {
    case SDL_BUTTON_LEFT:
        return View::MouseButton::Left;
    case SDL_BUTTON_MIDDLE:
        return View::MouseButton::Middle;
    case SDL_BUTTON_RIGHT:
        return View::MouseButton::Right;
    case SDL_BUTTON_X1:
        return View::MouseButton::Back;
    case SDL_BUTTON_X2:
        return View::MouseButton::Forward;
    default:
        return View::MouseButton::None;
    }
}

// The button held during a move, for drags and selections.
View::MouseButton Window::pressedButton(uint32_t buttons)
{
    if (buttons & SDL_BUTTON_LMASK)
        return View::MouseButton::Left;
    if (buttons & SDL_BUTTON_MMASK)
        return View::MouseButton::Middle;
    if (buttons & SDL_BUTTON_RMASK)
        return View::MouseButton::Right;
    return View::MouseButton::None;
}

void Window::handleEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
            for (auto* window : windows().values())
                window->m_closed = true;
            break;
        case SDL_EVENT_CLIPBOARD_UPDATE:
            sdlClipboardChanged();
            break;
        default:
            if (event.type == s_wakeUpEvent)
                s_wakeUpPending = false;
            else if (auto* window = windows().get(SDL_GetWindowFromEvent(&event)))
                window->handleEvent(event);
            // Events pushed without a window (SDL_PushEvent with no windowID) go
            // to the only one, if there is one.
            else if (windows().size() == 1 && !SDL_GetWindowFromEvent(&event))
                windows().begin()->value->handleEvent(event);
            break;
        }
    }
}

// The Web Inspector's shortcuts: F12, Ctrl+Shift+I.
static bool isInspectorShortcut(const SDL_KeyboardEvent& key)
{
    if (key.scancode == SDL_SCANCODE_F12 && !(key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_SHIFT | SDL_KMOD_GUI)))
        return true;
    return key.scancode == SDL_SCANCODE_I && (key.mod & SDL_KMOD_CTRL) && (key.mod & SDL_KMOD_SHIFT) && !(key.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI));
}

void Window::handleEvent(const SDL_Event& event)
{
    {
        switch (event.type) {
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
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            m_view->setFocused(event.type == SDL_EVENT_WINDOW_FOCUS_GAINED);
            break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            m_view->mouseLeave();
            break;
        case SDL_EVENT_MOUSE_MOTION:
            m_view->mouseMove(viewPoint(event.motion.x, event.motion.y), pressedButton(event.motion.state), modifiersFromSDL(SDL_GetModState()));
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            auto point = viewPoint(event.button.x, event.button.y);
            auto button = mouseButton(event.button.button);
            auto modifiers = modifiersFromSDL(SDL_GetModState());
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                m_view->mouseDown(point, button, event.button.clicks, modifiers);
            else
                m_view->mouseUp(point, button, event.button.clicks, modifiers);
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            // One notch scrolls 100 pixels; SDL's y is positive away from the user.
            float x = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.x : event.wheel.x;
            float y = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
            m_view->wheel(viewPoint(event.wheel.mouse_x, event.wheel.mouse_y), { -x * 100, y * 100 }, modifiersFromSDL(SDL_GetModState()));
            break;
        }
        case SDL_EVENT_KEY_DOWN:
            // Ahead of the page, as in a browser; with no inspector the page gets the key.
            if (isInspectorShortcut(event.key) && !event.key.repeat && m_view->showInspector())
                break;
            m_view->keyDown(keyEventFromSDL(event.key.scancode, event.key.mod, event.key.repeat));
            break;
        case SDL_EVENT_KEY_UP:
            m_view->keyUp(keyEventFromSDL(event.key.scancode, event.key.mod, false));
            break;
        case SDL_EVENT_TEXT_INPUT:
            m_view->insertText(String::fromUTF8(event.text.text), modifiersFromSDL(SDL_GetModState()));
            break;
        case SDL_EVENT_DROP_BEGIN:
            m_drop = { };
            break;
        case SDL_EVENT_DROP_POSITION:
            m_dropPoint = viewPoint(event.drop.x, event.drop.y);
            break;
        case SDL_EVENT_DROP_FILE:
            m_dropPoint = viewPoint(event.drop.x, event.drop.y);
            if (event.drop.data)
                m_drop.files.append(String::fromUTF8(event.drop.data));
            break;
        case SDL_EVENT_DROP_TEXT:
            m_dropPoint = viewPoint(event.drop.x, event.drop.y);
            if (event.drop.data)
                m_drop.text = String::fromUTF8(event.drop.data);
            break;
        case SDL_EVENT_DROP_COMPLETE:
            // SDL tells what was dropped only at the drop: the page sees the drag
            // arrive there, then the drop.
            m_view->drag(View::DragEvent::Enter, m_dropPoint, m_drop);
            m_view->drag(View::DragEvent::Drop, m_dropPoint, m_drop);
            m_drop = { };
            break;
        default:
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
    // A composited view is presented on the compositor thread: here only
    // rendering updates, at most one per refresh interval (a rendering update
    // that changes nothing asks for no frame) and each once the previous frame
    // is out (its swap waited for vsync there). The compositor's frames wake
    // this loop.
    auto now = MonotonicTime::now();
    bool hasWork = m_view->hasPendingRenderingUpdate() || m_view->hasDamage();
    if (auto* compositor = m_view->compositor()) {
        if (hasWork && now >= m_nextFrameTime && !compositor->isWaitingForFrame()) {
            m_view->render(now);
            // One interval after the previous one, not after now: the rate stays the display's.
            m_nextFrameTime = std::max(now, m_nextFrameTime + m_refreshInterval);
        }
        hasWork = m_view->hasPendingRenderingUpdate() && !compositor->isWaitingForFrame();
    } else if (hasWork && now >= m_nextFrameTime) {
        bool presented = m_view->render(now) && present();
        m_nextFrameTime = now + (presented ? m_refreshInterval * 0.9 : m_refreshInterval);
        if (presented) {
            didPresent();
            return !m_closed;
        }
    }

    // Sleep until an event, engine work, or the next frame if one is wanted.
    now = MonotonicTime::now();
    auto wakeUp = maxWait ? now + *maxWait : MonotonicTime::infinity();
    if (hasWork && (m_view->hasPendingRenderingUpdate() || m_view->hasDamage()))
        wakeUp = std::min(wakeUp, m_nextFrameTime);
    if (auto engineWork = RunLoop::mainSingleton().nextWakeUpTime())
        wakeUp = std::min(wakeUp, *engineWork);    Sint32 timeout = wakeUp.isInfinity() ? -1 : static_cast<Sint32>(std::max<double>(0, std::ceil((wakeUp - now).milliseconds())));
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
    IntSize nativeWindowSize;
    {
        Locker locker { m_presentLock };
        width = m_pixelSize.width();
        height = m_pixelSize.height();
        nativeWindowSize = std::exchange(m_nativeWindowSize, { });
    }
#if !OS(WINDOWS)
    if (!nativeWindowSize.isEmpty() && isWayland() && m_nativeWindow)
        WaylandEGL::singleton().resize(m_nativeWindow, nativeWindowSize.width(), nativeWindowSize.height(), 0, 0);
#else
    UNUSED_VARIABLE(nativeWindowSize);
#endif

    // Click-through needs the frame on the CPU: the window's size of it, rows
    // top to bottom, premultiplied RGBA (transparent where the frame does not
    // reach, after a resize).
    Vector<uint8_t> readback;
#if OS(WINDOWS)
    bool readBack = m_layered;
#else
    bool readBack = m_clickThrough && !isWayland();
#endif
    if (readBack) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_readFramebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, static_cast<GLuint>(reinterpret_cast<uintptr_t>(frame.texture)), 0);
        int readWidth = std::min<int>(frame.width, width);
        int readHeight = std::min<int>(frame.height, height);
        Vector<uint8_t> rows(static_cast<size_t>(readWidth) * readHeight * 4);
        glReadPixels(0, 0, readWidth, readHeight, GL_RGBA, GL_UNSIGNED_BYTE, rows.mutableSpan().data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        readback.grow(static_cast<size_t>(width) * height * 4);
        std::memset(readback.mutableSpan().data(), 0, readback.size());
        for (int y = 0; y < readHeight; ++y)
            std::memcpy(readback.mutableSpan().data() + static_cast<size_t>(y) * width * 4, rows.span().data() + static_cast<size_t>(y) * readWidth * 4, readWidth * 4);
    }

#if OS(WINDOWS)
    if (m_layered) {
        if (m_captureRequested.exchange(false)) {
            Locker locker { m_presentLock };
            m_capture = readback;
            m_captureSize = { width, height };
        }
        // To the window as a 32-bit premultiplied BGRA bitmap.
        BITMAPINFO info { };
        info.bmiHeader.biSize = sizeof info.bmiHeader;
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height; // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC memory = CreateCompatibleDC(nullptr);
        HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap && bits) {
            auto* out = static_cast<uint8_t*>(bits);
            auto in = readback.span();
            for (size_t i = 0; i + 3 < in.size(); i += 4) {
                out[i] = in[i + 2];
                out[i + 1] = in[i + 1];
                out[i + 2] = in[i];
                out[i + 3] = in[i + 3];
            }
            auto* previous = SelectObject(memory, bitmap);
            SIZE size { width, height };
            POINT origin { 0, 0 };
            BLENDFUNCTION blend { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            UpdateLayeredWindow(static_cast<HWND>(m_nativeWindow), nullptr, nullptr, &size, memory, &origin, 0, &blend, ULW_ALPHA);
            SelectObject(memory, previous);
        }
        if (bitmap)
            DeleteObject(bitmap);
        DeleteDC(memory);
        ++m_presentedFrames;
        if (m_view->compositor() && m_presentedFrames == 1) {
            callOnMainThread([sdlWindow = m_window] {
                if (auto* window = windows().get(sdlWindow))
                    window->didPresent();
            });
        }
        return true;
    }
#else
    // X11: the frame's alpha is the window's input shape (SDL's window shape
    // there; it reads the alpha byte of ARGB32 pixels).
    if (readBack) {
        Vector<uint8_t> shape(readback.size());
        auto in = readback.span();
        for (size_t i = 0; i + 3 < in.size(); i += 4)
            shape[i] = in[i + 3];
        callOnMainThread([sdlWindow = m_window, shape = WTF::move(shape), width, height]() mutable {
            if (!windows().contains(sdlWindow))
                return;
            if (auto* surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_ARGB32, shape.mutableSpan().data(), width * 4)) {
                SDL_SetWindowShape(sdlWindow, surface);
                SDL_DestroySurface(surface);
            }
        });
    }
#endif

    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_readFramebuffer);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, static_cast<GLuint>(reinterpret_cast<uintptr_t>(frame.texture)), 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    // A frame older than a resize does not cover the window: clear the rest.
    if (static_cast<int>(frame.width) < width || static_cast<int>(frame.height) < height) {
        if (m_transparent)
            glClearColor(0, 0, 0, 0);
        else
            glClearColor(1, 1, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    // Frames are stored top row first, GL windows bottom row first.
    int copyWidth = std::min<int>(frame.width, width);
    int copyHeight = std::min<int>(frame.height, height);
    glBlitFramebuffer(0, 0, copyWidth, copyHeight, 0, height, copyWidth, height - copyHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    if (m_captureRequested.exchange(false)) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        Vector<uint8_t> bottomUp(static_cast<size_t>(width) * height * 4);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.mutableSpan().data());
        Vector<uint8_t> capture(bottomUp.size());
        for (int y = 0; y < height; ++y)
            std::memcpy(capture.mutableSpan().data() + static_cast<size_t>(y) * width * 4, bottomUp.span().data() + static_cast<size_t>(height - 1 - y) * width * 4, width * 4);
        Locker locker { m_presentLock };
        m_capture = WTF::move(capture);
        m_captureSize = { width, height };
    }

    m_presentContext->swapBuffers();
    ++m_presentedFrames;
    // The window is shown with its first frame, by the engine thread (SDL's
    // windows belong to it).
    if (m_view->compositor()) {
        if (m_presentedFrames == 1) {
            callOnMainThread([sdlWindow = m_window] {
                if (auto* window = windows().get(sdlWindow))
                    window->didPresent();
            });
        }
    }
    return true;
}

void Window::didPresent()
{
    if (!m_shown && m_showWhenPresented) {
        SDL_ShowWindow(m_window);
        m_shown = true;
    }
}

bool Window::takeCapture(Vector<uint8_t>& pixels, IntSize& size)
{
    Locker locker { m_presentLock };
    if (m_capture.isEmpty())
        return false;
    pixels = std::exchange(m_capture, { });
    size = m_captureSize;
    return true;
}

} // namespace ZeroG
