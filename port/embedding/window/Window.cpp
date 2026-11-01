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
#include <WebCore/Page.h>
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
#include <wtf/text/StringToIntegerConversion.h>

#if OS(WINDOWS)
#include <dwmapi.h>
#include <shellapi.h>
#else
#include <dlfcn.h>
#endif

namespace ZeroG {

using namespace WebCore;

namespace {

String s_lastError;

// Engine work posted from other threads wakes the window's event wait.
Uint32 s_wakeUpEvent;
std::atomic<bool> s_wakeUpPending;
// A native menu's item was chosen (code: its id).
Uint32 s_menuEvent;

bool initializeVideo()
{
    static bool initialized = [] {
        // Closing the last window is a close request like any other, which
        // the application may refuse; SDL would also make it a quit.
        SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
        if (!SDL_Init(SDL_INIT_VIDEO))
            return false;
        // Pages see the desktop's dark or light setting (and follow it:
        // SDL_EVENT_SYSTEM_THEME_CHANGED).
        View::setSystemDarkAppearance(SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK);
        s_wakeUpEvent = SDL_RegisterEvents(2);
        s_menuEvent = s_wakeUpEvent + 1;
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
    // Read by SDL as each window is created.
    if (!configuration.appID.isEmpty())
        SDL_SetHint(SDL_HINT_APP_ID, byteCast<char>(configuration.appID.utf8().data()));
    auto title = configuration.title.utf8();
    SDL_Window* window = nullptr;
    if (auto* parent = configuration.parent) {
        // A popup menu: placed relative to the parent's client area, in its
        // window units (Wayland places it with xdg_popup).
        float units = parent->unitsPerLogicalPixel();
        auto scaled = [&](int logical) { return static_cast<int>(std::lround(logical * units)); };
        window = SDL_CreatePopupWindow(parent->m_window, scaled(configuration.position.x()), scaled(configuration.position.y()),
            std::max(1, scaled(configuration.size.width())), std::max(1, scaled(configuration.size.height())), SDL_WINDOW_POPUP_MENU | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    } else
        window = SDL_CreateWindow(byteCast<char>(title.data()), configuration.size.width(), configuration.size.height(), flags);
    if (!window)
        return fail("cannot create the window"_s, SDL_GetError());
    auto destroyWindow = makeScopeExit([&] { SDL_DestroyWindow(window); });
    // The size asked for is logical: SDL's units are pixels on Windows and X11.
    if (float scale = SDL_GetWindowDisplayScale(window), density = SDL_GetWindowPixelDensity(window); !configuration.parent && scale > 0 && density > 0 && scale != density) {
        float units = scale / density;
        SDL_SetWindowSize(window, std::lround(configuration.size.width() * units), std::lround(configuration.size.height() * units));
        SDL_SyncWindow(window);
    }

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
    result->m_state = result->state();
    result->m_isPopup = configuration.parent;
    if (result->m_isPopup)
        return std::unique_ptr<Window> { result.release() };
    result->colorSchemeChanged();
#if OS(WINDOWS)
    // The executable's own icon (its first icon resource), as Explorer shows
    // it, rather than SDL's default.
    wchar_t executable[MAX_PATH];
    HICON bigIcon = nullptr, smallIcon = nullptr;
    if (GetModuleFileNameW(nullptr, executable, MAX_PATH) && ExtractIconExW(executable, 0, &bigIcon, &smallIcon, 1) > 0) {
        auto hwnd = static_cast<HWND>(native->window);
        if (bigIcon)
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(bigIcon));
        if (smallIcon)
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
        result->m_executableIcons = { bigIcon, smallIcon };
    }
#endif
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

// A <select> dropdown being shown (showSelectPopup).
struct Window::SelectPopup {
    std::unique_ptr<Window> window;
    std::unique_ptr<ViewClient> client;
    CompletionHandler<void(std::optional<unsigned>)> handler;
    // Set: closed with this answer at the parent's next turn of the loop.
    std::optional<std::optional<unsigned>> answer;
};

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
    // An open dropdown goes first, dismissed.
    if (auto popup = std::exchange(m_selectPopup, nullptr)) {
        popup->window = nullptr;
        popup->handler(std::nullopt);
    }
#if OS(WINDOWS)
    // Its window procedure was installed after the accessibility bridge's.
    m_menus = nullptr;
#endif
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
#if OS(WINDOWS)
    for (auto* icon : m_executableIcons) {
        if (icon)
            DestroyIcon(static_cast<HICON>(icon));
    }
#endif
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

uint32_t Window::id() const
{
    return SDL_GetWindowID(m_window);
}

bool Window::setIcon(std::span<const uint8_t> rgba, const IntSize& size)
{
    if (size.isEmpty() || rgba.size() < static_cast<size_t>(size.width()) * size.height() * 4)
        return false;
    auto* surface = SDL_CreateSurfaceFrom(size.width(), size.height(), SDL_PIXELFORMAT_RGBA32, const_cast<uint8_t*>(rgba.data()), size.width() * 4);
    if (!surface)
        return false;
    bool set = SDL_SetWindowIcon(m_window, surface);
    SDL_DestroySurface(surface);
    return set;
}

void Window::colorSchemeChanged()
{
#if OS(WINDOWS)
    // The title bar and frame, dark with a dark page (Windows 10 2004 and later;
    // earlier versions ignore the attribute).
    constexpr DWORD useImmersiveDarkMode = 20; // DWMWA_USE_IMMERSIVE_DARK_MODE
    BOOL dark = m_view->page().useDarkAppearance();
    DwmSetWindowAttribute(static_cast<HWND>(nativeHandle()), useImmersiveDarkMode, &dark, sizeof dark);
#endif
}

float Window::unitsPerLogicalPixel() const
{
    float scale = SDL_GetWindowDisplayScale(m_window);
    float density = SDL_GetWindowPixelDensity(m_window);
    return scale > 0 && density > 0 ? scale / density : 1;
}

static int toUnits(int logical, float unitsPerLogicalPixel)
{
    return logical ? std::max(1, static_cast<int>(std::lround(logical * unitsPerLogicalPixel))) : 0;
}

void Window::setSize(const IntSize& size)
{
    float units = unitsPerLogicalPixel();
    SDL_SetWindowSize(m_window, toUnits(size.width(), units), toUnits(size.height(), units));
    SDL_SyncWindow(m_window);
}

IntSize Window::size() const
{
    int width, height;
    SDL_GetWindowSize(m_window, &width, &height);
    float units = unitsPerLogicalPixel();
    return { static_cast<int>(std::lround(width / units)), static_cast<int>(std::lround(height / units)) };
}

void Window::setMinimumSize(const IntSize& size)
{
    float units = unitsPerLogicalPixel();
    SDL_SetWindowMinimumSize(m_window, toUnits(size.width(), units), toUnits(size.height(), units));
}

void Window::setMaximumSize(const IntSize& size)
{
    float units = unitsPerLogicalPixel();
    SDL_SetWindowMaximumSize(m_window, toUnits(size.width(), units), toUnits(size.height(), units));
}

void Window::setResizable(bool resizable)
{
    SDL_SetWindowResizable(m_window, resizable);
}

// Wayland gives applications no say in where their windows are.
static bool canPlaceWindows()
{
#if OS(WINDOWS)
    return true;
#else
    return !isWayland();
#endif
}

bool Window::setPosition(const IntPoint& position)
{
    if (!canPlaceWindows())
        return false;
    SDL_SetWindowPosition(m_window, position.x(), position.y());
    SDL_SyncWindow(m_window);
    return true;
}

std::optional<IntPoint> Window::position() const
{
    int x, y;
    if (!canPlaceWindows() || !SDL_GetWindowPosition(m_window, &x, &y))
        return std::nullopt;
    return IntPoint { x, y };
}

bool Window::center()
{
    if (!canPlaceWindows())
        return false;
    auto display = SDL_GetDisplayForWindow(m_window);
    SDL_SetWindowPosition(m_window, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_SyncWindow(m_window);
    return true;
}

void Window::minimize()
{
    SDL_MinimizeWindow(m_window);
}

void Window::maximize()
{
    SDL_MaximizeWindow(m_window);
}

void Window::restore()
{
    SDL_RestoreWindow(m_window);
}

void Window::show()
{
    // Shown now if it has a frame to show, else with its first one.
    m_showWhenPresented = true;
    if (m_presentedFrames && !m_shown) {
        SDL_ShowWindow(m_window);
        m_shown = true;
    }
}

void Window::hide()
{
    m_showWhenPresented = false;
    m_shown = false;
    SDL_HideWindow(m_window);
}

void Window::setAlwaysOnTop(bool onTop)
{
    SDL_SetWindowAlwaysOnTop(m_window, onTop);
}

Window::State Window::state() const
{
    auto flags = SDL_GetWindowFlags(m_window);
    // A window waiting for its first frame to appear is not hidden.
    if ((flags & SDL_WINDOW_HIDDEN) && !(m_showWhenPresented && !m_shown))
        return State::Hidden;
    if (flags & SDL_WINDOW_MINIMIZED)
        return State::Minimized;
    if (flags & SDL_WINDOW_FULLSCREEN)
        return State::Fullscreen;
    if (flags & SDL_WINDOW_MAXIMIZED)
        return State::Maximized;
    return State::Normal;
}

void Window::stateMayHaveChanged()
{
    auto state = this->state();
    if (state == m_state)
        return;
    m_state = state;
    if (m_client)
        m_client->stateChanged(state);
}

Vector<Window::Screen> Window::screens()
{
    Vector<Screen> result;
    if (!initializeVideo())
        return result;
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    if (!displays)
        return result;
    auto primary = SDL_GetPrimaryDisplay();
    for (int i = 0; i < count; ++i) {
        Screen screen;
        SDL_Rect rect;
        if (SDL_GetDisplayBounds(displays[i], &rect))
            screen.bounds = { rect.x, rect.y, rect.w, rect.h };
        screen.workArea = SDL_GetDisplayUsableBounds(displays[i], &rect) ? IntRect { rect.x, rect.y, rect.w, rect.h } : screen.bounds;
        if (float scale = SDL_GetDisplayContentScale(displays[i]); scale > 0)
            screen.scale = scale;
        if (auto* mode = SDL_GetCurrentDisplayMode(displays[i]))
            screen.refreshRate = mode->refresh_rate;
        screen.primary = displays[i] == primary;
        screen.name = String::fromUTF8(SDL_GetDisplayName(displays[i]));
        result.append(WTF::move(screen));
    }
    SDL_free(displays);
    return result;
}

std::optional<size_t> Window::screen() const
{
    auto display = SDL_GetDisplayForWindow(m_window);
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    std::optional<size_t> result;
    for (int i = 0; displays && i < count; ++i) {
        if (displays[i] == display)
            result = i;
    }
    SDL_free(displays);
    return result;
}

namespace {

// A file dialog until SDL answers: its filters and properties stay valid.
struct FileDialogRequest {
    Window::FileDialogHandler handler;
    Vector<UTF8CString> strings;
    Vector<SDL_DialogFileFilter> filters;
    SDL_PropertiesID properties { 0 };

    static void SDLCALL done(void* user, const char* const* files, int)
    {
        // SDL may answer on another thread. The strings are new: they move.
        std::expected<Vector<String>, String> result;
        if (!files)
            result = makeUnexpected(String::fromUTF8(SDL_GetError()));
        else {
            Vector<String> paths;
            for (auto* file = files; *file; ++file)
                paths.append(String::fromUTF8(*file));
            result = WTF::move(paths);
        }
        callOnMainThread([request = std::unique_ptr<FileDialogRequest>(static_cast<FileDialogRequest*>(user)), result = WTF::move(result)] mutable {
            SDL_DestroyProperties(request->properties);
            request->handler(WTF::move(result));
        });
    }
};

} // namespace

void Window::showFileDialog(FileDialog&& dialog, FileDialogHandler&& handler)
{
    auto request = std::make_unique<FileDialogRequest>();
    request->handler = WTF::move(handler);
    // The strings first: the filters point into them.
    request->strings.reserveInitialCapacity(dialog.filters.size() * 2);
    for (auto& [name, pattern] : dialog.filters) {
        request->strings.append(name.utf8());
        request->strings.append(pattern.utf8());
    }
    for (size_t i = 0; i < dialog.filters.size(); ++i)
        request->filters.append({ byteCast<char>(request->strings[i * 2].data()), byteCast<char>(request->strings[i * 2 + 1].data()) });

    auto properties = SDL_CreateProperties();
    request->properties = properties;
    SDL_SetPointerProperty(properties, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, m_window);
    if (!request->filters.isEmpty() && dialog.kind != FileDialog::Kind::OpenFolder) {
        SDL_SetPointerProperty(properties, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, request->filters.mutableSpan().data());
        SDL_SetNumberProperty(properties, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, request->filters.size());
    }
    auto setString = [&](const char* name, const String& value) {
        if (!value.isEmpty())
            SDL_SetStringProperty(properties, name, byteCast<char>(value.utf8().data()));
    };
    setString(SDL_PROP_FILE_DIALOG_TITLE_STRING, dialog.title);
    setString(SDL_PROP_FILE_DIALOG_ACCEPT_STRING, dialog.acceptLabel);
    setString(SDL_PROP_FILE_DIALOG_LOCATION_STRING, dialog.location);
    SDL_SetBooleanProperty(properties, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, dialog.multiple && dialog.kind != FileDialog::Kind::Save);

    SDL_FileDialogType type = SDL_FILEDIALOG_OPENFILE;
    if (dialog.kind == FileDialog::Kind::OpenFolder)
        type = SDL_FILEDIALOG_OPENFOLDER;
    else if (dialog.kind == FileDialog::Kind::Save)
        type = SDL_FILEDIALOG_SAVEFILE;
    SDL_ShowFileDialogWithProperties(type, FileDialogRequest::done, request.release(), properties);
}

void Window::showMessageDialog(MessageDialog&& dialog, MessageDialogHandler&& handler)
{
    // SDL's message box returns once answered: not during this call. The
    // window's page waits meanwhile.
    callOnMainThread([sdlWindow = m_window, windowID = id(), dialog = WTF::move(dialog), handler = WTF::move(handler)] mutable {
        if (SDL_GetWindowFromID(windowID) != sdlWindow || !windows().contains(sdlWindow)) {
            handler(makeUnexpected("the window was destroyed"_s));
            return;
        }
        if (dialog.buttons.isEmpty())
            dialog.buttons.append({ 0, "OK"_s, true, true });
        Vector<UTF8CString> labels;
        for (auto& button : dialog.buttons)
            labels.append(button.label.utf8());
        Vector<SDL_MessageBoxButtonData> buttons;
        std::optional<int> cancel;
        for (size_t i = 0; i < dialog.buttons.size(); ++i) {
            auto& button = dialog.buttons[i];
            SDL_MessageBoxButtonFlags flags = 0;
            if (button.isDefault)
                flags |= SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
            if (button.isCancel) {
                flags |= SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
                cancel = button.id;
            }
            buttons.append({ flags, button.id, byteCast<char>(labels[i].data()) });
        }
        SDL_MessageBoxFlags flags = SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT;
        switch (dialog.kind) {
        case MessageDialog::Kind::Info: flags |= SDL_MESSAGEBOX_INFORMATION; break;
        case MessageDialog::Kind::Warning: flags |= SDL_MESSAGEBOX_WARNING; break;
        case MessageDialog::Kind::Error: flags |= SDL_MESSAGEBOX_ERROR; break;
        }
        auto title = dialog.title.utf8();
        auto message = dialog.message.utf8();
        SDL_MessageBoxData data { flags, sdlWindow, byteCast<char>(title.data()), byteCast<char>(message.data()),static_cast<int>(buttons.size()), buttons.span().data(), nullptr };
        int chosen = -1;
        if (!SDL_ShowMessageBox(&data, &chosen)) {
            handler(makeUnexpected(String::fromUTF8(SDL_GetError())));
            return;
        }
        // Closed without a button: the cancel one's.
        if (chosen == -1 && cancel)
            chosen = *cancel;
        handler(chosen);
    });
}

bool Window::hasNativeMenus()
{
#if OS(WINDOWS)
    return true;
#else
    return false;
#endif
}

void Window::collectAccelerators(const Vector<Menu::Item>& items)
{
    for (auto& item : items) {
        if (item.kind == Menu::Item::Kind::Submenu)
            collectAccelerators(item.submenu);
        else if (item.kind == Menu::Item::Kind::Action) {
            if (auto accelerator = parseAccelerator(item.accelerator))
                m_accelerators.append({ *accelerator, item.id, item.enabled });
        }
    }
}

bool Window::setMenu(const Menu* menu)
{
#if OS(WINDOWS)
    m_accelerators.clear();
    if (!m_menus) {
        if (!menu)
            return true;
        m_menus = std::make_unique<NativeMenus>(nativeHandle(), id(), s_menuEvent);
    }
    // Alt and the items' access keys open the menu bar (SDL keeps them from
    // Windows by default).
    if (menu)
        SDL_SetHint(SDL_HINT_WINDOWS_ENABLE_MENU_MNEMONICS, "1");
    m_menus->setMenuBar(menu);
    if (menu)
        collectAccelerators(menu->items);
    return true;
#else
    UNUSED_PARAM(menu);
    return false;
#endif
}

void Window::setMenuItemChecked(unsigned id, bool checked)
{
#if OS(WINDOWS)
    if (m_menus)
        m_menus->setChecked(id, checked);
#else
    UNUSED_PARAM(id);
    UNUSED_PARAM(checked);
#endif
}

void Window::setMenuItemEnabled(unsigned id, bool enabled)
{
    for (auto& accelerator : m_accelerators) {
        if (accelerator.id == id)
            accelerator.enabled = enabled;
    }
#if OS(WINDOWS)
    if (m_menus)
        m_menus->setEnabled(id, enabled);
#endif
}

bool Window::popupMenu(const Menu& menu, const FloatPoint& point)
{
#if OS(WINDOWS)
    if (!m_menus)
        m_menus = std::make_unique<NativeMenus>(nativeHandle(), id(), s_menuEvent);
    // The view's logical pixels to the client area's.
    float scale = m_view->scale();
    m_menus->popup(menu, std::lround(point.x() * scale), std::lround(point.y() * scale));
    return true;
#else
    UNUSED_PARAM(menu);
    UNUSED_PARAM(point);
    return false;
#endif
}

namespace {

// The popup's page tells its choice by its title: "zg-select:<index>" or
// "zg-select:cancel".
class SelectPopupClient final : public ViewClient {
public:
    explicit SelectPopupClient(std::optional<std::optional<unsigned>>& answer)
        : m_answer(answer)
    {
    }

private:
    void titleChanged(const String& title) final
    {
        if (!title.startsWith("zg-select:"_s) || m_answer)
            return;
        auto value = title.substring(10);
        if (value == "cancel"_s)
            m_answer = std::optional<unsigned> { };
        else if (auto index = parseInteger<unsigned>(value))
            m_answer = std::optional<unsigned> { *index };
    }

    std::optional<std::optional<unsigned>>& m_answer;
};

String escapedHTML(const String& text)
{
    StringBuilder builder;
    for (auto character : StringView(text).codeUnits()) {
        switch (character) {
        case '&': builder.append("&amp;"_s); break;
        case '<': builder.append("&lt;"_s); break;
        case '>': builder.append("&gt;"_s); break;
        case '"': builder.append("&quot;"_s); break;
        default: builder.append(character);
        }
    }
    return builder.toString();
}

// A list like the system's: the selected option active, keyboard (arrows,
// Home, End, Page Up/Down, Enter, Escape, typing the start of an option) and
// mouse; system colors, so it follows the color scheme.
String selectPopupHTML(const SelectMenu& menu)
{
    StringBuilder html;
    html.append("<!DOCTYPE html><html dir="_s, menu.rightToLeft ? "rtl"_s : "ltr"_s, "><head><style>"
        ":root{color-scheme:light dark;font:"_s, menu.fontSize, "px system-ui,'Segoe UI',sans-serif}"
        "html,body{margin:0;height:100%;overflow:hidden}"
        "body{background:Canvas;color:CanvasText;border:1px solid color-mix(in srgb,CanvasText 22%,transparent);box-sizing:border-box}"
        "#list{height:100%;overflow-y:auto;padding:3px 0;box-sizing:border-box}"
        ".item{padding:3px 10px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;cursor:default;user-select:none}"
        ".option.active{background:Highlight;color:HighlightText}"
        ".group{font-weight:600}.ingroup{padding-inline-start:22px}.disabled{color:GrayText}"
        ".separator{height:1px;margin:3px 0;background:color-mix(in srgb,CanvasText 18%,transparent)}"
        "</style></head><body><div id=list>"_s);
    bool inGroup = false;
    for (size_t i = 0; i < menu.items.size(); ++i) {
        auto& item = menu.items[i];
        if (item.kind == SelectMenu::ItemKind::Separator) {
            html.append("<div class=separator></div>"_s);
            inGroup = false;
            continue;
        }
        if (item.kind == SelectMenu::ItemKind::Group) {
            html.append("<div class='item group'>"_s, escapedHTML(item.label), "</div>"_s);
            inGroup = true;
            continue;
        }
        html.append("<div class='item option"_s, inGroup ? " ingroup"_s : ""_s, item.enabled ? ""_s : " disabled"_s, item.selected ? " selected"_s : ""_s,
            "' data-i="_s, i, " title=\""_s, escapedHTML(item.label), "\">"_s, item.label.isEmpty() ? "&nbsp;"_s : escapedHTML(item.label), "</div>"_s);
    }
    html.append("</div><script>"
        "const items=[...document.querySelectorAll('.option:not(.disabled)')];"
        "let active=null;"
        "function setActive(e){if(active)active.classList.remove('active');active=e;if(e){e.classList.add('active');e.scrollIntoView({block:'nearest'});}}"
        "setActive(items.find(e=>e.classList.contains('selected'))||items[0]||null);"
        "function answer(a){document.title='zg-select:'+a;}"
        "document.addEventListener('mousemove',ev=>{const e=ev.target.closest('.option:not(.disabled)');if(e&&e!==active)setActive(e);});"
        "document.addEventListener('mouseup',ev=>{const e=ev.target.closest('.option:not(.disabled)');if(e)answer(e.dataset.i);});"
        "let typed='',typedAt=0;"
        "document.addEventListener('keydown',ev=>{const i=items.indexOf(active),last=items.length-1;"
        "switch(ev.key){"
        "case 'ArrowDown':setActive(items[Math.min(last,i+1)]);break;"
        "case 'ArrowUp':setActive(items[Math.max(0,i-1)]);break;"
        "case 'Home':setActive(items[0]);break;"
        "case 'End':setActive(items[last]);break;"
        "case 'PageDown':setActive(items[Math.min(last,i+10)]);break;"
        "case 'PageUp':setActive(items[Math.max(0,i-10)]);break;"
        "case 'Enter':answer(active?active.dataset.i:'cancel');break;"
        "case 'Escape':case 'Tab':answer('cancel');break;"
        "default:if(ev.key.length!==1)return;"
        "{const now=Date.now();typed=(now-typedAt<800?typed:'')+ev.key.toLowerCase();typedAt=now;"
        "const m=items.find(e=>e.textContent.trim().toLowerCase().startsWith(typed));if(m)setActive(m);}}"
        "ev.preventDefault();});"
        "</script></body></html>"_s);
    return html.toString();
}

} // namespace

void Window::showSelectPopup(SelectMenu&& menu, CompletionHandler<void(std::optional<unsigned>)>&& handler)
{
    // One at a time: another page's request replaces it.
    if (m_selectPopup) {
        if (!m_selectPopup->answer)
            m_selectPopup->answer = std::optional<unsigned> { };
        finishSelectPopup();
    }

    // Its size: a row per item (at the menu's font size), as wide as the
    // element or its longest label, at most 20 rows tall (then it scrolls).
    float row = std::ceil(menu.fontSize * 1.25f) + 6;
    float height = 8;
    size_t longest = 0;
    for (auto& item : menu.items) {
        height += item.kind == SelectMenu::ItemKind::Separator ? 7 : row;
        longest = std::max<size_t>(longest, item.label.length());
    }
    height = std::min(height, row * 20 + 8);
    float width = std::clamp(longest * menu.fontSize * 0.55f + 36, menu.bounds.width(), std::max(menu.bounds.width(), 600.f));
    auto bounds = enclosingIntRect(menu.bounds);
    IntPoint position { bounds.x(), bounds.maxY() };
    // Above the element when the screen has more room there.
    if (auto origin = this->position(); origin && screen()) {
        auto screens = Window::screens();
        auto work = screens[*screen()].workArea;
        float units = unitsPerLogicalPixel();
        float below = (work.maxY() - (origin->y() + bounds.maxY() * units)) / units;
        float above = (origin->y() + bounds.y() * units - work.y()) / units;
        if (below < height && above > below)
            position.setY(bounds.y() - static_cast<int>(height));
    }

    auto popup = std::make_unique<SelectPopup>();
    popup->handler = WTF::move(handler);
    Configuration configuration;
    configuration.size = { static_cast<int>(std::ceil(width)), static_cast<int>(std::ceil(height)) };
    configuration.resizable = false;
    configuration.frameless = true;
    configuration.parent = this;
    configuration.position = position;
    popup->window = Window::create(WTF::move(configuration));
    if (!popup->window) {
        popup->handler(std::nullopt);
        return;
    }
    popup->client = std::make_unique<SelectPopupClient>(popup->answer);
    auto& view = popup->window->view();
    view.setClient(popup->client.get());
    view.setColorScheme(m_view->colorScheme());
    view.loadHTML(selectPopupHTML(menu));
    view.setFocused(true);
    m_selectPopup = WTF::move(popup);
}

void Window::hideSelectPopup()
{
    if (m_selectPopup && !m_selectPopup->answer)
        m_selectPopup->answer = std::optional<unsigned> { };
}

void Window::finishSelectPopup()
{
    auto popup = std::exchange(m_selectPopup, nullptr);
    if (!popup)
        return;
    auto answer = popup->answer.value_or(std::nullopt);
    popup->window = nullptr;
    popup->handler(answer);
}

bool Window::handleSelectPopupEvent(const SDL_Event& event)
{
    if (!m_selectPopup || m_selectPopup->answer)
        return false;
    auto dismiss = [&] {
        m_selectPopup->answer = std::optional<unsigned> { };
    };
    switch (event.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_WHEEL:
        // A click or a scroll elsewhere closes it, and does nothing else.
        dismiss();
        return true;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return true;
    case SDL_EVENT_KEY_DOWN:
        // Keys go to the list, whichever window has the keyboard.
        m_selectPopup->window->view().keyDown(keyEventFromSDL(event.key.scancode, event.key.mod, event.key.repeat));
        return true;
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
        return true;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        // To another application, not to the popup.
        if (SDL_GetKeyboardFocus() != m_selectPopup->window->m_window)
            dismiss();
        return false;
    case SDL_EVENT_WINDOW_MOVED:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WINDOW_HIDDEN:
        dismiss();
        return false;
    default:
        return false;
    }
}

bool Window::handleAccelerator(const SDL_Event& event)
{
    if (m_accelerators.isEmpty())
        return false;
    auto pressed = acceleratorForKey(event.key.scancode, event.key.mod);
    for (auto& accelerator : m_accelerators) {
        if (accelerator.accelerator != pressed)
            continue;
        if (accelerator.enabled && !event.key.repeat && m_client)
            m_client->menuItemChosen(accelerator.id);
        return true;
    }
    return false;
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
        case SDL_EVENT_SYSTEM_THEME_CHANGED:
            View::setSystemDarkAppearance(SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK);
            for (auto* window : windows().values())
                window->colorSchemeChanged();
            break;
        default:
            if (event.type == s_wakeUpEvent)
                s_wakeUpPending = false;
            else if (auto* window = windows().get(SDL_GetWindowFromEvent(&event)))
                window->handleEvent(event);
            // Events without a window (pushed with no windowID, keys while no
            // window has the keyboard) go to the only one, if there is one;
            // dropdowns do not count, their parent passes keys on to them.
            else if (!SDL_GetWindowFromEvent(&event)) {
                Window* only = nullptr;
                unsigned count = 0;
                for (auto* window : windows().values()) {
                    if (!window->m_isPopup) {
                        only = window;
                        ++count;
                    }
                }
                if (count == 1)
                    only->handleEvent(event);
            }
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
    if (handleSelectPopupEvent(event))
        return;
    {
        switch (event.type) {
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (m_client && !m_client->closeRequested())
                break;
            // Gone from the screen at once; the application destroys it.
            m_closed = true;
            SDL_HideWindow(m_window);
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            updateSize();
            break;
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            updateSize();
            if (m_client)
                m_client->screenChanged();
            break;
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            // Animations follow the new screen's refresh rate.
            if (auto* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(m_window)); mode && mode->refresh_rate > 0) {
                m_refreshInterval = 1_s / mode->refresh_rate;
                if (auto* compositor = m_view->compositor())
                    compositor->setFrameInterval(m_refreshInterval);
            }
            if (m_client)
                m_client->screenChanged();
            break;
        case SDL_EVENT_WINDOW_RESIZED:
            if (m_client)
                m_client->resized(size());
            break;
        case SDL_EVENT_WINDOW_MOVED:
            if (m_client)
                m_client->moved({ event.window.data1, event.window.data2 });
            break;
        case SDL_EVENT_WINDOW_SHOWN:
            // What was presented while the window was hidden (its first frame,
            // before it appears) may be gone: a new frame, also for a page
            // that would otherwise not paint again.
            m_view->repaint();
            stateMayHaveChanged();
            break;
        case SDL_EVENT_WINDOW_HIDDEN:
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
            stateMayHaveChanged();
            break;
        case SDL_EVENT_WINDOW_EXPOSED:
            // A whole new frame: invalidate() alone gives a composited view
            // none (its layers keep their own damage).
            m_view->repaint();
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            m_view->setFocused(event.type == SDL_EVENT_WINDOW_FOCUS_GAINED);
            if (m_client)
                m_client->focusChanged(event.type == SDL_EVENT_WINDOW_FOCUS_GAINED);
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
            // The menu bar's accelerators first, as in any application.
            if (handleAccelerator(event))
                break;
            // Ahead of the page, as in a browser; with no inspector the page gets the key.
            if (isInspectorShortcut(event.key) && !event.key.repeat && m_view->showInspector())
                break;
            if (!m_view->keyDown(keyEventFromSDL(event.key.scancode, event.key.mod, event.key.repeat))) {
                // SDL gives no text for Enter and Tab, which Windows sends as
                // WM_CHAR '\r' and '\t': the keypress that submits a form or
                // inserts a newline or a tab.
                auto scancode = event.key.scancode;
                if (scancode == SDL_SCANCODE_RETURN || scancode == SDL_SCANCODE_KP_ENTER || scancode == SDL_SCANCODE_TAB)
                    m_view->insertText(scancode == SDL_SCANCODE_TAB ? "\t"_s : "\r"_s, modifiersFromSDL(event.key.mod));
            }
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
            // Files are the application's first: it may keep them from the page.
            if (!m_drop.files.isEmpty() && m_client && m_client->filesDropped(m_drop.files, m_dropPoint)) {
                m_drop = { };
                break;
            }
            // SDL tells what was dropped only at the drop: the page sees the drag
            // arrive there, then the drop.
            m_view->drag(View::DragEvent::Enter, m_dropPoint, m_drop);
            m_view->drag(View::DragEvent::Drop, m_dropPoint, m_drop);
            m_drop = { };
            break;
        default:
            if (event.type == s_menuEvent && m_client)
                m_client->menuItemChosen(static_cast<unsigned>(event.user.code));
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
    // An open dropdown: its frames, then its answer once it has one.
    if (m_selectPopup && !m_selectPopup->answer)
        m_selectPopup->window->runOnce(0_s);
    if (m_selectPopup && m_selectPopup->answer)
        finishSelectPopup();
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
        wakeUp = std::min(wakeUp, *engineWork);
    // The dropdown's frames are its own window's: drawn from this loop.
    if (m_selectPopup)
        wakeUp = std::min(wakeUp, now + 8_ms);
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
    // The first frame at a new size went to the old buffers: the swap chain
    // takes the window's new size at the swap (ANGLE on Windows), with buffers
    // that start black. Another frame, into them, for a page that would not
    // draw again.
    bool resized = std::exchange(m_lastPresentedSize, IntSize { width, height }) != IntSize { width, height };
    if (resized && m_presentedFrames > 1) {
        callOnMainThread([sdlWindow = m_window] {
            if (auto* window = windows().get(sdlWindow))
                window->m_view->repaint();
        });
    }
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
