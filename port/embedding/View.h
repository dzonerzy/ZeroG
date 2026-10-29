#pragma once

// A view: one page, its size and scale, and the surface it renders into
// (ZeroG.md, "Ciclo di un frame"). Engine thread only, except the surface's
// acquire/release.

#include "Surface.h"
#include <WebCore/Cursor.h>
#include <WebCore/FloatPoint.h>
#include <WebCore/FloatSize.h>
#include <WebCore/IntRect.h>
#include <WebCore/PlatformEvent.h>
#include <WebCore/Region.h>
#include <memory>
#include <wtf/MonotonicTime.h>
#include <wtf/Ref.h>
#include <wtf/text/WTFString.h>

namespace WebCore {
class Page;
}

namespace ZeroG {

class View {
    WTF_MAKE_NONCOPYABLE(View);
public:
    struct Configuration {
        WebCore::IntSize size { 800, 600 }; // logical pixels
        float scale { 1 };
        bool transparent { false };
        bool scriptEnabled { true };
        std::unique_ptr<Surface> surface; // null: a CPU surface
    };
    static std::unique_ptr<View> create(Configuration&&);
    ~View();

    WebCore::Page& page() { return m_page.get(); }
    Surface& surface() { return *m_surface; }
    WebCore::IntSize size() const { return m_size; }
    float scale() const { return m_scale; }

    void loadHTML(const String&);
    String evaluateScript(const String&);

    void resize(const WebCore::IntSize&);
    void setScale(float);

    // One frame of the application's loop: runs the rendering update (animations,
    // requestAnimationFrame at `frameTime`) if one is pending, lays out, and
    // repaints the damaged region into a surface buffer. Returns whether a frame
    // was published.
    bool render(MonotonicTime frameTime);

    // Input, in logical pixels (texture mode feeds these directly; window mode
    // translates its window events into them). Each returns whether the page
    // handled the event (for keys: prevented its default or used it for editing).
    enum class MouseButton : uint8_t { None, Left, Middle, Right, Back, Forward };
    using Modifiers = OptionSet<WebCore::PlatformEvent::Modifier>;
    bool mouseMove(const WebCore::FloatPoint&, MouseButton pressed, Modifiers);
    bool mouseDown(const WebCore::FloatPoint&, MouseButton, int clickCount, Modifiers);
    bool mouseUp(const WebCore::FloatPoint&, MouseButton, int clickCount, Modifiers);
    void mouseLeave();
    // Pixels to scroll; positive y scrolls up (towards the start), as a wheel
    // turned away from the user.
    bool wheel(const WebCore::FloatPoint&, const WebCore::FloatSize& delta, Modifiers);

    struct KeyEvent {
        String key; // DOM KeyboardEvent.key: "a", "Enter", "ArrowLeft"
        String code; // DOM KeyboardEvent.code: "KeyA", "Enter"
        int windowsKeyCode { 0 }; // VK_*: keyCode, and the editing key bindings
        bool isAutoRepeat { false };
        bool isKeypad { false };
        Modifiers modifiers;
    };
    bool keyDown(const KeyEvent&);
    bool keyUp(const KeyEvent&);
    // Text typed or committed by an input method, after its keyDown.
    bool insertText(const String&, Modifiers = { });

    void setFocused(bool);

    // The cursor the page wants over its current mouse position.
    WebCore::PlatformCursorType cursor() const { return m_cursor; }
    void setCursorChangedCallback(Function<void(WebCore::PlatformCursorType)>&& callback) { m_cursorChanged = WTF::move(callback); }
    void setCursor(WebCore::PlatformCursorType); // from ChromeClientZeroG

    // Number of pixels painted by the last published frame, for tests.
    uint64_t lastPaintedArea() const { return m_lastPaintedArea; }

    // From ChromeClientZeroG.
    void invalidate(const WebCore::IntRect& rootViewRect);
    void setNeedsRenderingUpdate() { m_needsRenderingUpdate = true; }
    // An animation or requestAnimationFrame callback waits for the next frame.
    bool hasPendingRenderingUpdate() const { return m_needsRenderingUpdate; }
    bool hasDamage() const { return !m_damage.isEmpty(); }

private:
    explicit View(Configuration&&);
    void invalidateAll() { invalidate({ { }, m_size }); }
    WebCore::IntSize physicalSize() const;

    const Ref<WebCore::Page> m_page;
    std::unique_ptr<Surface> m_surface;
    WebCore::IntSize m_size;
    float m_scale { 1 };
    bool m_transparent { false };
    bool m_needsRenderingUpdate { true };
    WebCore::Region m_damage; // logical pixels
    uint64_t m_lastPaintedArea { 0 };
    WebCore::PlatformCursorType m_cursor { WebCore::PlatformCursorType::Pointer };
    Function<void(WebCore::PlatformCursorType)> m_cursorChanged;
};

} // namespace ZeroG
