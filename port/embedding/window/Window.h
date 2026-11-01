#pragma once

// Window mode (docs/design/graphics-presentation.md, "Modes"): the engine creates a native window (SDL3, which
// loads X11 or Wayland at run time on Linux), renders its view with the GPU and
// presents it itself, paced by the display's vsync. requestAnimationFrame follows
// the presentations (engine-driven, unlike texture mode).

#include "Menu.h"
#include "View.h"
#include <array>
#include <atomic>
#include <memory>
#include <expected>
#include <wtf/Lock.h>
#include <wtf/StdLibExtras.h>
#include <optional>
#include <wtf/Seconds.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

struct SDL_Window;
struct SDL_Cursor;
union SDL_Event;

namespace WebCore {
class GLContext;
}

namespace ZeroG {

namespace Accessibility {
class Bridge;
}

class Window {
    WTF_MAKE_NONCOPYABLE(Window);
public:
    struct Configuration {
        String title;
        WebCore::IntSize size { 1024, 768 }; // logical pixels
        bool resizable { true };
        bool frameless { false };
        bool visible { true };
        RefPtr<Session> session; // null: the default session
        // The page's transparent pixels show what is behind the window (alpha
        // composited by the window system).
        bool transparent { false };
        // With transparent: clicks where the page is fully transparent go to
        // what is behind the window. Each frame is read back for it: on
        // Windows the window becomes a layered window updated from the CPU
        // copy (UpdateLayeredWindow, which lets clicks through alpha 0); on
        // X11 the frame's alpha is the window's input shape. Wayland: no
        // effect (no input region through SDL).
        bool clickThrough { false };
        // The application's reverse-DNS name: on Linux the window's Wayland
        // app_id and X11 class, which desktops match to its .desktop file.
        String appID;
        // A popup menu of `parent`'s (select dropdowns): at `position`, the
        // parent view's logical pixels; no frame, no taskbar entry.
        Window* parent { nullptr };
        WebCore::IntPoint position;
    };
    // Null if no window or no GPU is available. The first window decides the GPU
    // display on Linux (X11 or Wayland); it has to be created before any GPU view.
    static std::unique_ptr<Window> create(Configuration&&);
    // Why the last create() failed.
    static const String& lastError();
    ~Window();

    View& view() { return *m_view; }

    // Runs until the window is closed or quit() is called.
    void run();
    // One iteration of the loop: engine work, window events, a frame if one is
    // due. Waits at most `maxWait` for something to do. False once closed.
    bool runOnce(std::optional<Seconds> maxWait = std::nullopt);
    void quit() { m_closed = true; }
    bool isClosed() const { return m_closed; }

    enum class State : uint8_t { Normal, Minimized, Maximized, Fullscreen, Hidden };

    // What happens to the window, on the engine thread. Calls come last in the
    // handling of an event: the client may close the window (the API defers
    // destroying it until the loop is out of its calls).
    class Client {
    public:
        virtual ~Client() = default;
        // False keeps the window open.
        virtual bool closeRequested() { return true; }
        virtual void resized(const WebCore::IntSize&) { } // logical pixels
        virtual void moved(const WebCore::IntPoint&) { } // desktop coordinates
        virtual void focusChanged(bool) { }
        virtual void stateChanged(State) { }
        virtual void menuItemChosen(unsigned) { }
        virtual void screenChanged() { }
        // True keeps the drop from the page.
        virtual bool filesDropped(const Vector<String>&, const WebCore::FloatPoint&) { return false; }
    };
    void setClient(Client* client) { m_client = client; }

    void setTitle(const String&);
    // Logical pixels.
    void setSize(const WebCore::IntSize&);
    WebCore::IntSize size() const;
    // 0 for no limit.
    void setMinimumSize(const WebCore::IntSize&);
    void setMaximumSize(const WebCore::IntSize&);
    void setResizable(bool);
    // Desktop coordinates; false where windows cannot be placed (Wayland).
    bool setPosition(const WebCore::IntPoint&);
    std::optional<WebCore::IntPoint> position() const;
    bool center();
    void setFullscreen(bool);
    void minimize();
    void maximize();
    void restore();
    void show();
    void hide();
    void setAlwaysOnTop(bool);
    State state() const;
    // Above the other windows, with the keyboard focus.
    void raise();
    Seconds refreshInterval() const { return m_refreshInterval; }
    void* nativeHandle() const; // HWND, X11 Window, wl_surface*
    // SDL's window ID: never reused in the process.
    uint32_t id() const;
    // RGBA8, rows top to bottom. Windows: the executable's icon by default.
    bool setIcon(std::span<const uint8_t> rgba, const WebCore::IntSize&);
    // The view's color scheme or the desktop's changed: the frame follows
    // (a dark title bar on Windows).
    void colorSchemeChanged();

    // The connected screens; empty without a window system.
    struct Screen {
        WebCore::IntRect bounds; // desktop coordinates
        WebCore::IntRect workArea;
        float scale { 1 };
        float refreshRate { 0 };
        bool primary { false };
        String name;
    };
    static Vector<Screen> screens();
    // The index in screens() of the window's.
    std::optional<size_t> screen() const;

    // The system's file dialog for <input type=file> (SDL3), its answer on the
    // engine thread: the chosen paths, or nullopt.
    void showOpenDialog(const WebCore::FileChooserSettings&, CompletionHandler<void(std::optional<Vector<String>>&&)>&&);

    // The system's dialogs for the application. Answers come on the engine
    // thread after the call, always; the window may be gone by then (the
    // handler must not assume it exists).
    struct FileDialog {
        enum class Kind : uint8_t { Open, OpenFolder, Save };
        Kind kind { Kind::Open };
        String title;
        String acceptLabel;
        String location;
        Vector<std::pair<String, String>> filters; // name, "png;jpg"
        bool multiple { false };
    };
    // The chosen paths (none: cancelled), or why there is no dialog.
    using FileDialogHandler = CompletionHandler<void(std::expected<Vector<String>, String>&&)>;
    void showFileDialog(FileDialog&&, FileDialogHandler&&);

    struct MessageDialog {
        enum class Kind : uint8_t { Info, Warning, Error };
        struct Button {
            int id { 0 };
            String label;
            bool isDefault { false };
            bool isCancel { false };
        };
        Kind kind { Kind::Info };
        String title;
        String message;
        Vector<Button> buttons;
    };
    // The chosen button's id, or why there is no box.
    using MessageDialogHandler = CompletionHandler<void(std::expected<int, String>&&)>;
    void showMessageDialog(MessageDialog&&, MessageDialogHandler&&);

    // A <select> dropdown under its element: a popup window of the engine's,
    // drawn as HTML like the page (light or dark). Closed by a choice, Escape,
    // a click elsewhere or the window losing the focus.
    void showSelectPopup(SelectMenu&&, CompletionHandler<void(std::optional<unsigned>)>&&);
    void hideSelectPopup();

    // Native menus (Windows; false elsewhere). The menu bar's accelerators
    // choose their items ahead of the page.
    static bool hasNativeMenus();
    bool setMenu(const Menu*);
    void setMenuItemChecked(unsigned id, bool);
    void setMenuItemEnabled(unsigned id, bool);
    // At a point of the view (logical pixels); returns once the menu closed,
    // the chosen item (0: none) comes to the client afterwards.
    bool popupMenu(const Menu&, const WebCore::FloatPoint&);

    // What a point of a frameless window is to the window system (window
    // points): an edge resizes it, a region the page marks
    // --zg-app-region: drag moves it, the rest is the page's.
    enum class HitTest : uint8_t { Page, Drag, ResizeTopLeft, ResizeTop, ResizeTopRight, ResizeRight, ResizeBottomRight, ResizeBottom, ResizeBottomLeft, ResizeLeft };
    HitTest hitTest(float x, float y);

    // Tests: frames presented so far, and the contents of the next presented frame
    // (RGBA8, rows top to bottom) read back before the swap. Any thread.
    unsigned presentedFrames() const { return m_presentedFrames; }
    void captureNextFrame() { m_captureRequested = true; }
    bool takeCapture(Vector<uint8_t>& pixels, WebCore::IntSize& size);

private:
    Window(SDL_Window*, std::unique_ptr<View>&&, std::unique_ptr<WebCore::GLContext>&&, void* nativeWindow);
    // SDL's queue is the process's: every window's events go to their window.
    static void handleEvents();
    void handleEvent(const SDL_Event&);
    WebCore::FloatPoint viewPoint(float x, float y) const;
    void updateCursor(WebCore::PlatformCursorType);
    static View::MouseButton mouseButton(uint8_t);
    static View::MouseButton pressedButton(uint32_t);
    void updateSize();
    // SDL's window units per logical pixel (pixels on Windows and X11, so the
    // display's scale; 1 on Wayland).
    float unitsPerLogicalPixel() const;
    void stateMayHaveChanged();
    // True if the key chose a menu bar item.
    bool handleAccelerator(const SDL_Event&);
    // The select popup's events from the parent's side; true if used.
    bool handleSelectPopupEvent(const SDL_Event&);
    // Its answer, given and the popup destroyed here, in the loop: never
    // during the popup's own events.
    void finishSelectPopup();
    void collectAccelerators(const Vector<Menu::Item>&);
    // Presents the surface's newest frame with the window's context: on the
    // compositor thread for a composited view (the swap's vsync wait is there),
    // else on the engine thread.
    bool present();
    void didPresent();

    SDL_Window* m_window { nullptr };
    std::unique_ptr<View> m_view;
    std::unique_ptr<WebCore::GLContext> m_presentContext;
    void* m_nativeWindow { nullptr }; // the EGL native window (wl_egl_window* on Wayland)
    unsigned m_readFramebuffer { 0 };
    MonotonicTime m_nextFrameTime;
    Seconds m_refreshInterval { 1_s / 60 };
    std::atomic<unsigned> m_presentedFrames { 0 };
    bool m_closed { false };
    bool m_shown { false };
    bool m_showWhenPresented { true };
    bool m_transparent { false };
    bool m_clickThrough { false };
    // Windows click-through: frames go to the window with UpdateLayeredWindow,
    // not to a swap chain.
    bool m_layered { false };
    // Set by the engine thread, read where frames are presented.
    Lock m_presentLock;
    WebCore::IntSize m_pixelSize WTF_GUARDED_BY_LOCK(m_presentLock);
    WebCore::IntSize m_nativeWindowSize WTF_GUARDED_BY_LOCK(m_presentLock); // Wayland: applied before a swap
    std::atomic<bool> m_captureRequested { false };
    // Where frames are presented (the compositor thread, or the engine's).
    WebCore::IntSize m_lastPresentedSize;
    Vector<uint8_t> m_capture WTF_GUARDED_BY_LOCK(m_presentLock);
    WebCore::IntSize m_captureSize WTF_GUARDED_BY_LOCK(m_presentLock);
    std::array<SDL_Cursor*, 32> m_cursors { }; // by SDL_SystemCursor
    // Files and text dropped on the window: SDL sends them one by one, then
    // DROP_COMPLETE; the page gets the drop then.
    View::DragItems m_drop;
    WebCore::FloatPoint m_dropPoint;
    // The page for screen readers: UI Automation, AT-SPI (accessibility/).
    std::unique_ptr<Accessibility::Bridge> m_accessibility;
    Client* m_client { nullptr };
    State m_state { State::Hidden };
    struct SelectPopup;
    std::unique_ptr<SelectPopup> m_selectPopup;
    bool m_isPopup { false };
    // The menu bar's items with accelerators, and which are disabled.
    struct MenuAccelerator {
        Accelerator accelerator;
        unsigned id { 0 };
        bool enabled { true };
    };
    Vector<MenuAccelerator> m_accelerators;
#if OS(WINDOWS)
    std::unique_ptr<NativeMenus> m_menus;
    // The executable's icons (HICON), destroyed with the window.
    std::array<void*, 2> m_executableIcons { };
#endif
};

} // namespace ZeroG
