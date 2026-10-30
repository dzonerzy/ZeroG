#pragma once

// Window mode (ZeroG.md, "Modalità"): the engine creates a native window (SDL3, which
// loads X11 or Wayland at run time on Linux), renders its view with the GPU and
// presents it itself, paced by the display's vsync. requestAnimationFrame follows
// the presentations (engine-driven, unlike texture mode).

#include "View.h"
#include <array>
#include <memory>
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

    void setTitle(const String&);
    void setSize(const WebCore::IntSize&); // logical pixels
    void setFullscreen(bool);
    // Above the other windows, with the keyboard focus.
    void raise();
    Seconds refreshInterval() const { return m_refreshInterval; }
    void* nativeHandle() const; // HWND, X11 Window, wl_surface*

    // The system's file dialog for <input type=file> (SDL3), its answer on the
    // engine thread: the chosen paths, or nullopt.
    void showOpenDialog(const WebCore::FileChooserSettings&, CompletionHandler<void(std::optional<Vector<String>>&&)>&&);

    // Tests: frames presented so far, and the contents of the next presented frame
    // (RGBA8, rows top to bottom) read back before the swap.
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
    bool present();

    SDL_Window* m_window { nullptr };
    std::unique_ptr<View> m_view;
    std::unique_ptr<WebCore::GLContext> m_presentContext;
    void* m_nativeWindow { nullptr }; // the EGL native window (wl_egl_window* on Wayland)
    unsigned m_readFramebuffer { 0 };
    MonotonicTime m_lastPresent;
    MonotonicTime m_nextFrameTime;
    Seconds m_refreshInterval { 1_s / 60 };
    unsigned m_presentedFrames { 0 };
    bool m_closed { false };
    bool m_shown { false };
    bool m_showWhenPresented { true };
    bool m_captureRequested { false };
    Vector<uint8_t> m_capture;
    std::array<SDL_Cursor*, 32> m_cursors { }; // by SDL_SystemCursor
    WebCore::IntSize m_captureSize;
    // Files and text dropped on the window: SDL sends them one by one, then
    // DROP_COMPLETE; the page gets the drop then.
    View::DragItems m_drop;
    WebCore::FloatPoint m_dropPoint;
};

} // namespace ZeroG
