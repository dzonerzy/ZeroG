#pragma once

// A view: one page, its size and scale, and the surface it renders into
// (ZeroG.md, "Ciclo di un frame"). Engine thread only, except the surface's
// acquire/release.

#include "graphics/Surface.h"
#include "loader/Loader.h"
#include <WebCore/AuthenticationChallenge.h>
#include <WebCore/Credential.h>
#include <WebCore/Cursor.h>
#include <wtf/CompletionHandler.h>
#include <WebCore/FloatPoint.h>
#include <WebCore/FloatSize.h>
#include <WebCore/IntRect.h>
#include <WebCore/PlatformEvent.h>
#include <WebCore/Region.h>
#include <memory>
#include <wtf/MonotonicTime.h>
#include <optional>
#include <wtf/Function.h>
#include <wtf/Ref.h>
#include <wtf/URL.h>
#include <wtf/text/WTFString.h>

namespace JSC {
enum class MessageLevel : uint8_t;
enum class MessageSource : uint8_t;
}

namespace WebCore {
class LocalFrame;
class Page;
class PasteboardItemsZeroG;
enum class DragOperation : uint8_t;
struct FileChooserSettings;
}

namespace ZeroG {

class Compositor;
class Download;

// What a view reports to its embedder (the C API maps it onto zg_view_callbacks).
// Every call happens on the engine thread.
class ViewClient {
public:
    virtual ~ViewClient() = default;
    // `stack`: one "function@url:line:column" per line, innermost first, when the
    // message has one (errors); empty otherwise.
    virtual void consoleMessage(JSC::MessageSource, JSC::MessageLevel, const String& /* message */, unsigned /* line */, unsigned /* column */, const String& /* sourceURL */, const String& /* stack */) { }
    virtual void cursorChanged(WebCore::PlatformCursorType) { }
    virtual void tooltipChanged(const String&) { }
    virtual void titleChanged(const String&) { }
    // The main frame's load ended; a null error means it succeeded.
    virtual void loadFinished(const URL&, const WebCore::ResourceError&) { }
    // A navigation of the main frame or an iframe; false blocks it.
    virtual bool allowNavigation(const URL&, bool /* isMainFrame */) { return true; }
    // Links and window.open targeting a new window.
    virtual void openExternal(const URL&) { }
    // A frame's window object was (re)created: bindings are installed here.
    virtual void windowObjectCleared(WebCore::LocalFrame&) { }
    // A server asks for HTTP credentials (401). Answer with a credential to retry
    // with it, or nullopt to show the server's response; the answer may come
    // later. The default answers nullopt.
    virtual void authenticationChallenge(const WebCore::AuthenticationChallenge&, CompletionHandler<void(std::optional<WebCore::Credential>&&)>&& completionHandler) { completionHandler(std::nullopt); }
    // Downloads (Download.h): accept one by giving it a destination during
    // downloadStarted. downloadFinished has a null error on success.
    virtual void downloadStarted(Download&) { }
    virtual void downloadProgress(Download&) { }
    virtual void downloadFinished(Download&, const WebCore::ResourceError&) { }
    virtual void alert(const String&) { }
    virtual bool confirm(const String&) { return false; }
    virtual std::optional<String> prompt(const String& /* message */, const String& /* defaultValue */) { return std::nullopt; }
    // <input type=file>: answer with the chosen paths, or nullopt to cancel; the
    // answer may come later. The default cancels.
    virtual void runOpenPanel(const WebCore::FileChooserSettings&, CompletionHandler<void(std::optional<Vector<String>>&&)>&& completionHandler) { completionHandler(std::nullopt); }
    // The Web Inspector (dev and debug profiles, inspector/InspectorZeroG.h).
    // Opening it: create a view of `session` for its frontend, owned and shown by
    // the embedder, and return it; null opens nothing. The frontend closing
    // itself: destroy that view. The embedder may also destroy it at any time.
    virtual View* createInspectorView(Session&) { return nullptr; }
    virtual void closeInspectorView(View&) { }
    virtual void bringInspectorViewToFront(View&) { }
};

class ViewInspector;

class View {
    WTF_MAKE_NONCOPYABLE(View);
public:
    struct Configuration {
        WebCore::IntSize size { 800, 600 }; // logical pixels
        float scale { 1 };
        bool transparent { false };
        bool scriptEnabled { true };
        std::unique_ptr<Surface> surface; // null: a CPU surface
        RefPtr<Session> session; // null: the default session
    };
    static std::unique_ptr<View> create(Configuration&&);
    ~View();

    // The view showing a page, or null (pages of SVG images, for example).
    static View* fromPage(const WebCore::Page&);

    WebCore::Page& page() { return m_page.get(); }
    // A composited view's is the compositor's (graphics/Compositor.h).
    Surface& surface();
    // GPU views composite their layers on the compositor thread; null for CPU
    // views (and GPU views whose compositor got no GL context).
    Compositor* compositor() const { return m_compositor.get(); }
    Session& session() { return m_session.get(); }
    WebCore::IntSize size() const { return m_size; }
    float scale() const { return m_scale; }

    void loadHTML(const String&, const URL& baseURL = { });
    // Navigates the main frame; the load goes through the loader (schemes, curl).
    void loadURL(const URL&);
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

    // A drag from outside the page (files from the desktop, text from another
    // application), in logical pixels: entering, moving over, leaving, or dropped
    // on the view. Returns whether the page would accept a drop there.
    struct DragItems {
        Vector<String> files; // paths
        String text;
        String url;
        String html;
    };
    enum class DragEvent : uint8_t { Enter, Over, Leave, Drop };
    bool drag(DragEvent, const WebCore::FloatPoint&, const DragItems&);
    // A drag that started in the page (DragClientZeroG): the mouse events that
    // follow move it until the button is released.
    void startInternalDrag(Ref<WebCore::PasteboardItemsZeroG>&&);

    // Not owned; null restores the default (silent) client.
    void setClient(ViewClient*);

    // Downloads in progress; they end with the view.
    void addDownload(Download&);
    void removeDownload(Download&);
    ViewClient& client() { return m_client ? *m_client : s_defaultClient; }

    // The cursor the page wants over its current mouse position.
    WebCore::PlatformCursorType cursor() const { return m_cursor; }
    void setCursorChangedCallback(Function<void(WebCore::PlatformCursorType)>&& callback) { m_cursorChanged = WTF::move(callback); }
    void setCursor(WebCore::PlatformCursorType); // from ChromeClientZeroG

    // Number of pixels painted by the last published frame, for tests.
    uint64_t lastPaintedArea() const { return m_lastPaintedArea; }

    // The Web Inspector of the page; null in profiles without it.
    ViewInspector* inspector() const { return m_inspector.get(); }
    // Opens the inspector's frontend (ViewClient::createInspectorView). False
    // without the inspector, or when the embedder shows none.
    bool showInspector();
    // Called first thing in the destructor (the inspector's frontend view tells
    // the inspector it is going).
    void setWillDestroyCallback(Function<void()>&& callback) { m_willDestroy = WTF::move(callback); }

    // The whole view is painted again at the next render(), a composited view's
    // layers too (WebKit2's forced repaint).
    void repaint();

    // From ChromeClientZeroG: damage, for views that are not composited.
    void invalidate(const WebCore::IntRect& rootViewRect);
    void setNeedsRenderingUpdate() { m_needsRenderingUpdate = true; }
    // An animation or requestAnimationFrame callback waits for the next frame.
    bool hasPendingRenderingUpdate() const { return m_needsRenderingUpdate; }
    bool hasDamage() const { return !m_damage.isEmpty(); }

private:
    explicit View(Configuration&&);
    void invalidateAll() { invalidate({ { }, m_size }); }
    WebCore::IntSize physicalSize() const;
    void resizeSurface();
    bool renderComposited(MonotonicTime frameTime);
    // The drag controller's answer for the items at a point; nullopt: no drop.
    std::optional<WebCore::DragOperation> dragUpdated(WebCore::PasteboardItemsZeroG&, const WebCore::IntPoint&);
    void finishInternalDrag(const WebCore::IntPoint&, bool drop);

    // A drag from outside, and one from inside the page, in progress.
    RefPtr<WebCore::PasteboardItemsZeroG> m_externalDrag;
    RefPtr<WebCore::PasteboardItemsZeroG> m_internalDrag;
    std::optional<WebCore::DragOperation> m_internalDragOperation;

    const Ref<Session> m_session;
    const Ref<WebCore::Page> m_page;
    std::unique_ptr<Surface> m_surface; // null once the compositor has it
    std::unique_ptr<Compositor> m_compositor;
    WebCore::IntSize m_size;
    float m_scale { 1 };
    bool m_transparent { false };
    bool m_needsRenderingUpdate { true };
    bool m_forceRepaint { false };
    WebCore::Region m_damage; // logical pixels
    uint64_t m_lastPaintedArea { 0 };
    ViewClient* m_client { nullptr };
    static ViewClient s_defaultClient;
    Vector<Ref<Download>> m_downloads;
    WebCore::PlatformCursorType m_cursor { WebCore::PlatformCursorType::Pointer };
    Function<void(WebCore::PlatformCursorType)> m_cursorChanged;
    Function<void()> m_willDestroy;
    std::unique_ptr<ViewInspector> m_inspector;
};

} // namespace ZeroG
