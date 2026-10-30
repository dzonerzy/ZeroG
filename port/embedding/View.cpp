#include "config.h"
#include "View.h"

#include "clients/ChromeClientZeroG.h"
#include "clients/DragClientZeroG.h"
#include "clients/EditorClientZeroG.h"
#include "clients/FrameLoaderClientZeroG.h"
#include "graphics/Compositor.h"
#include "inspector/InspectorZeroG.h"
#include "loader/Download.h"
#include "loader/WebSocketChannelZeroG.h"
#include <WebCore/FrameLoadRequest.h>
#include <WebCore/FrameTree.h>
#include <WebCore/FrameLoader.h>
#include <WebCore/LocalFrameLoaderClient.h>
#include <WebCore/EventHandler.h>
#include <WebCore/FocusController.h>
#include <WebCore/HandleUserInputEventResult.h>
#include <WebCore/PlatformKeyboardEvent.h>
#include <WebCore/PlatformMouseEvent.h>
#include <WebCore/PlatformWheelEvent.h>
#include <WebCore/ScrollingCoordinatorTypes.h>
#include "Embedding.h"
#include <WebCore/ActivityState.h>
#include <WebCore/Chrome.h>
#include <WebCore/CookieJar.h>
#include <WebCore/DatabaseProvider.h>
#include <WebCore/DragController.h>
#include <WebCore/DragData.h>
#include <WebCore/PasteboardItemsZeroG.h>
#include <WebCore/StorageNamespaceProvider.h>
#include <WebCore/UserContentProvider.h>
#include <WebCore/DocumentView.h>
#include <WebCore/EmptyClients.h>
#include <WebCore/FloatRect.h>
#include <WebCore/FrameDestructionObserverInlines.h>
#include <WebCore/GraphicsContextSkia.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/LocalFrameView.h>
#include <WebCore/Page.h>
#include <WebCore/PageConfiguration.h>
#include <WebCore/PageInspectorController.h>
#include <WebCore/Settings.h>
#include <pal/SessionID.h>
#include <wtf/HashMap.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/MakeString.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkCanvas.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace ZeroG {

using namespace WebCore;

std::unique_ptr<View> View::create(Configuration&& configuration)
{
    initializeProcess();
    if (!configuration.surface)
        configuration.surface = createCPUSurface(2);
    return std::unique_ptr<View>(new View(WTF::move(configuration)));
}

static Ref<Page> createPage(View& view)
{
    auto configuration = pageConfigurationWithEmptyClients(std::nullopt, view.session().sessionID());
    // The empty clients are made for SVG images, whose frames are fully sandboxed;
    // a ZeroG page is the application's own UI.
    auto& mainFrameParameters = std::get<PageConfiguration::LocalMainFrameCreationParameters>(configuration.mainFrameCreationParameters);
    mainFrameParameters.effectiveSandboxFlags = { };
    mainFrameParameters.clientCreator = [&view](auto&, auto& frameLoader) {
        return createFrameLoaderClient(frameLoader, view);
    };
    configuration.cookieJar = createCookieJar(view.session());
    configuration.socketProvider = createSocketProvider();
    configuration.userContentProvider = view.session().userContentProvider();
    configuration.storageNamespaceProvider = view.session().storageNamespaceProvider();
    configuration.databaseProvider = view.session().databaseProvider();
    configuration.chromeClient = makeUniqueRef<ChromeClientZeroG>(view);
    configuration.editorClient = makeUniqueRef<EditorClientZeroG>();
    configuration.dragClient = makeUnique<DragClientZeroG>();
    if (auto inspectorClient = ViewInspector::createBackendClient(view))
        configuration.inspectorBackendClient = WTF::move(inspectorClient);
    return Page::create(WTF::move(configuration));
}

// Pages shown by views, for the loader: a load knows its frame, hence its page.
static HashMap<const Page*, View*>& viewsByPage()
{
    static NeverDestroyed<HashMap<const Page*, View*>> views;
    return views;
}

View* View::fromPage(const Page& page)
{
    return viewsByPage().get(&page);
}

View::View(Configuration&& configuration)
    : m_session(configuration.session ? configuration.session.releaseNonNull() : Ref { Session::defaultSession() })
    , m_page(createPage(*this))
    , m_surface(WTF::move(configuration.surface))
    , m_size(configuration.size)
    , m_scale(configuration.scale > 0 ? configuration.scale : 1)
    , m_transparent(configuration.transparent)
{
    Ref page = m_page;
    viewsByPage().set(page.ptr(), this);
    // The views of a session form one page group, as a WebKit browser's windows
    // do: local storage events reach the session's other views.
    page->setGroupName(makeString("ZeroG-"_s, m_session->sessionID().toUInt64()));
    auto& settings = page->settings();
    settings.setScriptEnabled(configuration.scriptEnabled);
    // <a download> (WebKit2 turns it on; it is an embedder setting).
    settings.setDownloadAttributeEnabled(true);
    // Off in WebCore's defaults, on in every WebKit port: the session provides them.
    settings.setLocalStorageEnabled(true);
    settings.setSessionStorageEnabled(true);
    // navigator.clipboard (the clipboard is the application's, PasteboardZeroG.cpp);
    // reading it asks the editor client, which grants it during a user gesture.
    settings.setAsyncClipboardAPIEnabled(true);
    settings.setDOMPasteAccessRequestsEnabled(true);
    if (processOptions().synchronousImageDecoding) {
        settings.setLargeImageAsyncDecodingEnabled(false);
        settings.setAnimatedImageAsyncDecodingEnabled(false);
    }
    // Compositing (ZeroG.md, "Compositing"): GPU views composite layers on the
    // compositor thread (graphics/Compositor.h), which takes their surface over;
    // CPU views paint the page into one surface, damage only.
    if (m_surface->kind() != SurfaceKind::CPU) {
        m_compositor = Compositor::create(*this, m_surface);
        if (m_compositor) {
            m_compositor->setTransparent(m_transparent);
            m_compositor->setSize(m_size, m_scale);
        }
    }
    settings.setAcceleratedCompositingEnabled(!!m_compositor);
    // The whole page in layers, as GTK and WPE run coordinated graphics: a
    // composited view paints nothing outside them.
    settings.setForceCompositingMode(!!m_compositor);
    // WebGL draws into a layer of its own, which only the compositor shows
    // (off in WebCore's defaults, on in WebKit2's).
#if ENABLE(WEBGL)
    settings.setWebGLEnabled(!!m_compositor);
#endif
    page->setDeviceScaleFactor(m_scale);

    RefPtr frame = page->localMainFrame();
    RELEASE_ASSERT(frame);
    Ref frameView = LocalFrameView::create(*frame, m_size);
    if (m_transparent) {
        frameView->setTransparent(true);
        frameView->setBaseBackgroundColor(Color::transparentBlack);
    }
    frame->setView(frameView.copyRef());
    frame->init();

    // The Web Inspector (its target is the page).
    m_inspector = ViewInspector::create(*this);
    if (m_inspector)
        settings.setDeveloperExtrasEnabled(true);

    // A view is visible and active until the application says otherwise; hidden
    // pages have requestAnimationFrame and timers suspended. Needs the frame's
    // initial document.
    page->setActivityState({ ActivityState::WindowIsActive, ActivityState::IsFocused, ActivityState::IsVisible,
        ActivityState::IsVisibleOrOccluded, ActivityState::IsInWindow, ActivityState::IsConnectedToHardwareConsole });

    if (!m_compositor)
        m_surface->resize(physicalSize());
    invalidateAll();
}

ViewClient View::s_defaultClient;

View::~View()
{
    if (auto willDestroy = std::exchange(m_willDestroy, nullptr))
        willDestroy();
    // Closes the frontend, if open, while the page is whole.
    m_inspector = nullptr;
    // The compositor thread stops; the page's layers forget it (the page may
    // outlive the view a little), and the surface goes with its thread's context.
    m_compositor = nullptr;
    if (auto* inspectorClient = m_page->inspectorController().inspectorBackendClient(); inspectorClient && ViewInspector::isAvailable())
        ViewInspector::detachBackendClient(*inspectorClient);
    for (auto& download : std::exchange(m_downloads, { }))
        download->detachFromView();
    viewsByPage().remove(m_page.ptr());
    if (RefPtr frame = m_page->localMainFrame())
        frame->loader().stopAllLoaders();
    static_cast<ChromeClientZeroG&>(m_page->chrome().client()).detachFromView();
    // Every frame's client (iframes included) points at this view.
    for (RefPtr<WebCore::Frame> frame = m_page->localMainFrame(); frame; frame = frame->tree().traverseNext()) {
        if (RefPtr localFrame = dynamicDowncast<LocalFrame>(*frame))
            detachFrameLoaderClient(localFrame->loader().client());
    }
}

void View::setClient(ViewClient* client)
{
    m_client = client;
}

bool View::showInspector()
{
    return m_inspector && m_inspector->show();
}

void View::addDownload(Download& download)
{
    m_downloads.append(download);
}

void View::removeDownload(Download& download)
{
    m_downloads.removeFirstMatching([&](auto& item) { return item.ptr() == &download; });
}

IntSize View::physicalSize() const
{
    return { static_cast<int>(std::ceil(m_size.width() * m_scale)), static_cast<int>(std::ceil(m_size.height() * m_scale)) };
}

void View::loadHTML(const String& html, const URL& baseURL)
{
    ZeroG::loadHTML(m_page, html, baseURL);
    invalidateAll();
}

void View::loadURL(const URL& url)
{
    RefPtr frame = m_page->localMainFrame();
    if (!frame)
        return;
    frame->loader().load(FrameLoadRequest { *frame, ResourceRequest { URL { url } } });
}

String View::evaluateScript(const String& script)
{
    return ZeroG::evaluateScript(m_page, script);
}

void View::resize(const IntSize& size)
{
    if (size == m_size)
        return;
    m_size = size;
    if (RefPtr frame = m_page->localMainFrame()) {
        if (RefPtr frameView = frame->view())
            frameView->resize(size);
    }
    resizeSurface();
    invalidateAll();
    m_needsRenderingUpdate = true;
}

void View::setScale(float scale)
{
    if (scale <= 0 || scale == m_scale)
        return;
    m_scale = scale;
    m_page->setDeviceScaleFactor(scale);
    resizeSurface();
    invalidateAll();
    m_needsRenderingUpdate = true;
}

void View::resizeSurface()
{
    // A composited view's surface is resized on the compositor thread.
    if (m_compositor)
        m_compositor->setSize(m_size, m_scale);
    else
        m_surface->resize(physicalSize());
}

Surface& View::surface()
{
    return m_compositor ? m_compositor->surface() : *m_surface;
}

void View::repaint()
{
    if (!m_compositor) {
        invalidateAll();
        return;
    }
    m_forceRepaint = true;
    m_needsRenderingUpdate = true;
}

void View::invalidate(const IntRect& rect)
{
    m_damage.unite(intersection(rect, { { }, m_size }));
}

// A GPU view: the rendering update here, the painting on the compositor thread.
// Returns whether a frame was asked for; it is published once composited.
bool View::renderComposited(MonotonicTime frameTime)
{
    // Repaints go to the layers, which track their own damage.
    m_damage = { };
    m_lastPaintedArea = 0;
    // One rendering update per composited frame.
    if (m_compositor->isWaitingForFrame())
        return false;
    Ref page = m_page;
    if (std::exchange(m_forceRepaint, false)) {
        page->forceRepaintAllFrames();
        m_compositor->requestFrame();
    }
    if (std::exchange(m_needsRenderingUpdate, false)) {
        page->updateRendering(frameTime);
        if (RefPtr frame = page->localMainFrame(); frame && frame->view())
            m_compositor->flushOverlay({ { }, frame->view()->sizeForVisibleContent(ScrollableArea::VisibleContentRectIncludesScrollbars::Yes) });
        page->finalizeRenderingUpdate({ FinalizeRenderingUpdateFlags::ApplyScrollingTreeLayerPositions });
    }
    return m_compositor->commit();
}

bool View::render(MonotonicTime frameTime)
{
    if (m_compositor)
        return renderComposited(frameTime);
    Ref page = m_page;
    if (std::exchange(m_needsRenderingUpdate, false)) {
        page->updateRendering(frameTime);
        page->finalizeRenderingUpdate({ });
    }

    RefPtr frame = page->localMainFrame();
    RefPtr frameView = frame ? frame->view() : nullptr;
    if (!frameView)
        return false;
    frameView->updateLayoutAndStyleIfNeededRecursive();

    m_lastPaintedArea = 0;
    if (m_damage.isEmpty())
        return false;
    auto damage = std::exchange(m_damage, { }).bounds();

    FloatRect physicalDamage { damage };
    physicalDamage.scale(m_scale);
    auto target = m_surface->beginFrame(enclosingIntRect(physicalDamage));
    if (!target) {
        m_damage.unite(damage);
        return false;
    }

    auto mode = m_surface->kind() == SurfaceKind::CPU ? RenderingMode::Unaccelerated : RenderingMode::Accelerated;
    auto& canvas = target->canvas;
    for (auto& rect : target->paintRegion.rects()) {
        // Buffers keep older frames: clear what is repainted, so transparent pages
        // do not blend with them.
        canvas.save();
        canvas.clipRect(SkRect::MakeXYWH(rect.x(), rect.y(), rect.width(), rect.height()));
        canvas.clear(SK_ColorTRANSPARENT);
        {
            GraphicsContextSkia context(canvas, mode, RenderingPurpose::Unspecified);
            context.scale(m_scale);
            FloatRect logicalRect { rect };
            logicalRect.scale(1 / m_scale);
            frameView->paint(context, enclosingIntRect(logicalRect));
            if (m_inspector)
                m_inspector->paintHighlight(context);
        }
        canvas.restore();
        m_lastPaintedArea += static_cast<uint64_t>(rect.width()) * rect.height();
    }
    m_surface->endFrame();
    return true;
}

// Input.

static WebCore::MouseButton platformButton(View::MouseButton button)
{
    switch (button) {
    case View::MouseButton::None:
        return WebCore::MouseButton::None;
    case View::MouseButton::Left:
        return WebCore::MouseButton::Left;
    case View::MouseButton::Middle:
        return WebCore::MouseButton::Middle;
    case View::MouseButton::Right:
        return WebCore::MouseButton::Right;
    case View::MouseButton::Back:
        return WebCore::MouseButton::Back;
    case View::MouseButton::Forward:
        return WebCore::MouseButton::Forward;
    }
    return WebCore::MouseButton::None;
}

static RefPtr<LocalFrame> mainFrame(Page& page)
{
    return page.localMainFrame();
}

bool View::mouseMove(const FloatPoint& position, MouseButton pressed, Modifiers modifiers)
{
    RefPtr frame = mainFrame(m_page);
    if (!frame)
        return false;
    // A drag from the page follows the mouse until the button goes up.
    if (RefPtr items = m_internalDrag) {
        m_internalDragOperation = dragUpdated(*items, flooredIntPoint(position));
        return true;
    }
    PlatformMouseEvent event(position, position, platformButton(pressed), PlatformEvent::Type::MouseMoved, 0, modifiers, MonotonicTime::now(), 0,
        SyntheticClickType::NoTap, MouseEventInputSource::UserDriven);
    return frame->eventHandler().mouseMoved(event).wasHandled();
}

bool View::mouseDown(const FloatPoint& position, MouseButton button, int clickCount, Modifiers modifiers)
{
    RefPtr frame = mainFrame(m_page);
    if (!frame)
        return false;
    PlatformMouseEvent event(position, position, platformButton(button), PlatformEvent::Type::MousePressed, clickCount, modifiers, MonotonicTime::now(), 0,
        SyntheticClickType::NoTap, MouseEventInputSource::UserDriven);
    return frame->eventHandler().handleMousePressEvent(event).wasHandled();
}

bool View::mouseUp(const FloatPoint& position, MouseButton button, int clickCount, Modifiers modifiers)
{
    RefPtr frame = mainFrame(m_page);
    if (!frame)
        return false;
    if (m_internalDrag) {
        finishInternalDrag(flooredIntPoint(position), true);
        return true;
    }
    PlatformMouseEvent event(position, position, platformButton(button), PlatformEvent::Type::MouseReleased, clickCount, modifiers, MonotonicTime::now(), 0,
        SyntheticClickType::NoTap, MouseEventInputSource::UserDriven);
    return frame->eventHandler().handleMouseReleaseEvent(event).wasHandled();
}

// ---- Drag and drop ----

std::optional<DragOperation> View::dragUpdated(PasteboardItemsZeroG& items, const IntPoint& point)
{
    RefPtr frame = mainFrame(m_page);
    if (!frame)
        return std::nullopt;
    DragData data(&items, point, point, anyDragOperation(), { }, anyDragDestinationAction(), m_page->identifier());
    auto result = m_page->dragController().dragEnteredOrUpdated(*frame, WTF::move(data));
    if (auto* operation = std::get_if<std::optional<DragOperation>>(&result))
        return *operation;
    return std::nullopt;
}

bool View::drag(DragEvent event, const FloatPoint& position, const DragItems& dragItems)
{
    RefPtr frame = mainFrame(m_page);
    if (!frame)
        return false;
    auto point = flooredIntPoint(position);
    if (event == DragEvent::Enter || !m_externalDrag) {
        auto items = MemoryPasteboardItemsZeroG::create();
        Vector<PasteboardItemsZeroG::Item> data;
        auto add = [&](ASCIILiteral type, const String& value) {
            if (!value.isEmpty()) {
                auto utf8 = value.utf8();
                data.append({ type, SharedBuffer::create(byteCast<uint8_t>(utf8.span())) });
            }
        };
        add("text/plain"_s, dragItems.text);
        add("text/uri-list"_s, dragItems.url);
        add("text/html"_s, dragItems.html);
        items->write(WTF::move(data));
        items->setFilePaths(Vector<String> { dragItems.files });
        m_externalDrag = WTF::move(items);
    }
    Ref items = *m_externalDrag;
    switch (event) {
    case DragEvent::Enter:
    case DragEvent::Over:
        return dragUpdated(items, point).has_value();
    case DragEvent::Leave:
        m_page->dragController().dragExited(*frame, DragData(items.ptr(), point, point, anyDragOperation(), { }, anyDragDestinationAction(), m_page->identifier()));
        m_externalDrag = nullptr;
        return false;
    case DragEvent::Drop: {
        // Dropped without moving over first: the page sees it arrive.
        bool accepted = dragUpdated(items, point).has_value();
        if (accepted)
            m_page->dragController().performDragOperation(DragData(items.ptr(), point, point, anyDragOperation(), { }, anyDragDestinationAction(), m_page->identifier()), *frame);
        else
            m_page->dragController().dragExited(*frame, DragData(items.ptr(), point, point, anyDragOperation(), { }, anyDragDestinationAction(), m_page->identifier()));
        m_externalDrag = nullptr;
        return accepted;
    }
    }
    return false;
}

void View::startInternalDrag(Ref<PasteboardItemsZeroG>&& items)
{
    m_internalDrag = WTF::move(items);
    m_internalDragOperation = std::nullopt;
}

void View::finishInternalDrag(const IntPoint& point, bool drop)
{
    Ref items = m_internalDrag.releaseNonNull();
    RefPtr frame = mainFrame(m_page);
    if (!frame)
        return;
    auto operation = std::exchange(m_internalDragOperation, std::nullopt);
    DragData data(items.ptr(), point, point, anyDragOperation(), { }, anyDragDestinationAction(), m_page->identifier());
    if (drop && operation)
        m_page->dragController().performDragOperation(WTF::move(data), *frame);
    else
        m_page->dragController().dragExited(*frame, WTF::move(data));
    // As WebKit2's WebPage::dragEnded: the source gets dragend.
    m_page->dragController().dragEnded();
    PlatformMouseEvent event(point, point, WebCore::MouseButton::Left, PlatformEvent::Type::MouseMoved, 0, { }, MonotonicTime::now(), 0,
        SyntheticClickType::NoTap, MouseEventInputSource::UserDriven);
    frame->eventHandler().dragSourceEndedAt(event, operation && drop ? OptionSet<DragOperation> { *operation } : OptionSet<DragOperation> { });
}

void View::mouseLeave()
{
    // A move far outside the view: hover and :hover end.
    mouseMove({ -1e6, -1e6 }, MouseButton::None, { });
}

bool View::wheel(const FloatPoint& position, const FloatSize& delta, Modifiers modifiers)
{
    RefPtr frame = mainFrame(m_page);
    if (!frame)
        return false;
    // Ticks as Windows reports them: one per 120 units, here 100 pixels.
    PlatformWheelEvent event(flooredIntPoint(position), flooredIntPoint(position), delta.width(), delta.height(), delta.width() / 100, delta.height() / 100,
        PlatformWheelEventGranularity::ScrollByPixelWheelEvent, modifiers.contains(PlatformEvent::Modifier::ShiftKey),
        modifiers.contains(PlatformEvent::Modifier::ControlKey), modifiers.contains(PlatformEvent::Modifier::AltKey), modifiers.contains(PlatformEvent::Modifier::MetaKey));
    auto [result, handling] = frame->eventHandler().handleWheelEvent(event, { WheelEventProcessingSteps::SynchronousScrolling, WheelEventProcessingSteps::BlockingDOMEventDispatch });
    return result.wasHandled();
}

static PlatformKeyboardEvent keyboardEvent(PlatformEvent::Type type, const View::KeyEvent& key, const String& text)
{
    bool isSystemKey = key.modifiers.contains(PlatformEvent::Modifier::AltKey) && !key.modifiers.contains(PlatformEvent::Modifier::ControlKey);
    return PlatformKeyboardEvent(type, text, text, key.key, key.code, key.key, key.windowsKeyCode, key.isAutoRepeat, key.isKeypad, isSystemKey, key.modifiers, MonotonicTime::now());
}

bool View::keyDown(const KeyEvent& key)
{
    RefPtr frame = m_page->focusController().focusedOrMainFrame();
    if (!frame)
        return false;
    // The text comes separately (insertText), as with WM_KEYDOWN and WM_CHAR.
    return frame->eventHandler().keyEvent(keyboardEvent(PlatformEvent::Type::RawKeyDown, key, { }));
}

bool View::keyUp(const KeyEvent& key)
{
    RefPtr frame = m_page->focusController().focusedOrMainFrame();
    if (!frame)
        return false;
    return frame->eventHandler().keyEvent(keyboardEvent(PlatformEvent::Type::KeyUp, key, { }));
}

bool View::insertText(const String& text, Modifiers modifiers)
{
    RefPtr frame = m_page->focusController().focusedOrMainFrame();
    if (!frame || text.isEmpty())
        return false;
    KeyEvent key { text, { }, 0, false, false, modifiers };
    // One keypress per text, as an input method commit is one insertion.
    return frame->eventHandler().keyEvent(keyboardEvent(PlatformEvent::Type::Char, key, text));
}

void View::setCursor(PlatformCursorType cursor)
{
    if (cursor == m_cursor)
        return;
    m_cursor = cursor;
    if (m_cursorChanged)
        m_cursorChanged(cursor);
    client().cursorChanged(cursor);
}

void View::setFocused(bool focused)
{
    auto& focusController = m_page->focusController();
    focusController.setActive(focused);
    focusController.setFocused(focused);
}

} // namespace ZeroG
