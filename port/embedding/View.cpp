#include "config.h"
#include "View.h"

#include "clients/ChromeClientZeroG.h"
#include "clients/DragClientZeroG.h"
#include "clients/EditorClientZeroG.h"
#include "clients/FrameLoaderClientZeroG.h"
#include "graphics/Compositor.h"
#include "inspector/InspectorZeroG.h"
#include "loader/Download.h"
#include "loader/Loader.h"
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
#include <WebCore/CacheStorageProvider.h>
#include <WebCore/Chrome.h>
#include <WebCore/CSSComputedStyleDeclaration.h>
#include <WebCore/ContextMenu.h>
#include <WebCore/Element.h>
#include <WebCore/HitTestRequest.h>
#include <WebCore/NodeInlines.h>
#include <WebCore/ContextMenuClient.h>
#include <WebCore/ContextMenuController.h>
#include <WebCore/ContextMenuItem.h>
#include <WebCore/CookieJar.h>
#include <WebCore/HitTestResult.h>
#include <WebCore/ResourceRequest.h>
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

// The context menu's items that need the embedder: downloading a link or an
// image goes the way other downloads go (Download.h).
class ContextMenuClientZeroG final : public ContextMenuClient {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ContextMenuClientZeroG);
public:
    explicit ContextMenuClientZeroG(View& view)
        : m_view(view)
    {
    }

private:
    void downloadURL(const URL& url) final { startDownload(m_view, ResourceRequest { URL { url } }); }
    void searchWithGoogle(const LocalFrame*) final { }
    void lookUpInDictionary(LocalFrame*) final { }
    bool isSpeaking() const final { return false; }
    void speak(const String&) final { }
    void stopSpeaking() final { }

    View& m_view;
};

static Ref<Page> createPage(View& view)
{
    // A page identifier, as WebKit2 gives each page: WebCore keys per-page
    // services by it (the media session manager that admits playback, drags).
    auto configuration = pageConfigurationWithEmptyClients(PageIdentifier::generate(), view.session().sessionID());
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
#if defined(ZEROG_FULL_WEB)
    configuration.cacheStorageProvider = Ref { view.session().cacheStorageProvider() };
    // The session's service workers exist before its first document asks
    // WebCore for their connection.
    view.session().serviceWorkers();
#endif
    configuration.chromeClient = makeUniqueRef<ChromeClientZeroG>(view);
    configuration.editorClient = makeUniqueRef<EditorClientZeroG>();
    configuration.dragClient = makeUnique<DragClientZeroG>();
    configuration.contextMenuClient = makeUniqueRef<ContextMenuClientZeroG>(view);
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

// The desktop's dark setting, as window mode knows it.
static bool s_systemDarkAppearance;

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
    // <img> and CSS images (off in WebCore's defaults: without it no image
    // element ever starts loading, from any scheme).
    settings.setLoadsImagesAutomatically(true);
    // window.visualViewport (on in WebKit's defaults, off in WebCore's).
    settings.setVisualViewportAPIEnabled(true);
#if defined(ZEROG_FULL_WEB)
    // The Cache API (standard, dev and debug profiles; the session's storage).
    settings.setCacheAPIEnabled(true);
    // Service workers (serviceworkers/ServiceWorkersZeroG.h).
    settings.setServiceWorkersEnabled(true);
#endif
    // navigator.clipboard (the clipboard is the application's, PasteboardZeroG.cpp);
    // reading it asks the editor client, which grants it during a user gesture.
    settings.setAsyncClipboardAPIEnabled(true);
    settings.setDOMPasteAccessRequestsEnabled(true);
    if (s_systemDarkAppearance)
        page->setUseColorAppearance(true, false);
    if (processOptions().synchronousImageDecoding) {
        settings.setLargeImageAsyncDecodingEnabled(false);
        settings.setAnimatedImageAsyncDecodingEnabled(false);
    }
    // Compositing (docs/design/graphics-presentation.md, "Compositing"): GPU views composite layers on the
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
    // 2D canvases draw on the GPU (Skia's Ganesh on the shared GL context) and
    // are layers of their own, in GPU views; CPU views keep them in memory.
    settings.setCanvasUsesAcceleratedDrawing(!!m_compositor);
    // Web Audio (off in WebCore's defaults, on in WebKit2's): in the profiles
    // with the media engine (media/WebAudioZeroG.cpp).
#if ENABLE(WEB_AUDIO)
    settings.setWebAudioEnabled(true);
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
    // Texture mode: this is the application's frame, which running animations
    // get a composited frame for (window mode's display paces those itself).
    m_compositor->hostFrame(frameTime);
    // One rendering update per composited frame.
    if (m_compositor->isWaitingForFrame())
        return false;
    Ref page = m_page;
    if (std::exchange(m_forceRepaint, false)) {
        page->forceRepaintAllFrames();
        m_compositor->requestFrame();
    }
    if (std::exchange(m_needsRenderingUpdate, false)) {
        ++m_renderingUpdates;
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
    bool handled = frame->eventHandler().handleMousePressEvent(event).wasHandled();
#if !OS(WINDOWS)
    // The context menu comes with the press, as on Linux and macOS.
    if (button == MouseButton::Right)
        handled |= contextMenu(position, modifiers);
#endif
    return handled;
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
    bool handled = frame->eventHandler().handleMouseReleaseEvent(event).wasHandled();
#if OS(WINDOWS)
    // The context menu comes with the release, as WM_CONTEXTMENU does.
    if (button == MouseButton::Right)
        handled |= contextMenu(position, modifiers);
#endif
    return handled;
}

// ---- Window regions ----

String View::appRegionAt(const FloatPoint& point)
{
    RefPtr frame = mainFrame(m_page);
    RefPtr frameView = frame ? frame->view() : nullptr;
    if (!frameView || !frame->document())
        return { };
    constexpr OptionSet<HitTestRequest::Type> type { HitTestRequest::Type::ReadOnly, HitTestRequest::Type::Active, HitTestRequest::Type::DisallowUserAgentShadowContent };
    auto result = frame->eventHandler().hitTestResultAtPoint(frameView->windowToContents(roundedIntPoint(point)), type);
    RefPtr node = result.innerNonSharedNode();
    RefPtr element = dynamicDowncast<Element>(node.get());
    if (!element && node)
        element = node->parentElement();
    if (!element)
        return { };
    // A custom property: inherited, so the element under the pointer has it.
    Ref style = CSSComputedStyleDeclaration::create(*element, CSSComputedStyleDeclaration::AllowVisited::No);
    return static_cast<CSSStyleDeclaration&>(style.get()).getPropertyValue("--zg-app-region"_s).trim(isASCIIWhitespace);
}

// ---- Context menus ----

// New windows and system services (speech, dictionary, web search, spelling:
// ZeroG has no spell checker) an application engine does not have: their
// items are left out.
static bool isUnavailableContextMenuAction(ContextMenuAction action)
{
    switch (action) {
    case ContextMenuItemTagSpellingGuess:
    case ContextMenuItemTagNoGuessesFound:
    case ContextMenuItemTagIgnoreSpelling:
    case ContextMenuItemTagLearnSpelling:
    case ContextMenuItemTagIgnoreGrammar:
    case ContextMenuItemTagSpellingMenu:
    case ContextMenuItemTagShowSpellingPanel:
    case ContextMenuItemTagCheckSpelling:
    case ContextMenuItemTagCheckSpellingWhileTyping:
    case ContextMenuItemTagCheckGrammarWithSpelling:
    case ContextMenuItemTagOpenLinkInNewWindow:
    case ContextMenuItemTagOpenImageInNewWindow:
    case ContextMenuItemTagOpenFrameInNewWindow:
    case ContextMenuItemTagOpenMediaInNewWindow:
    case ContextMenuItemTagSearchWeb:
    case ContextMenuItemTagLookUpInDictionary:
    case ContextMenuItemTagSpeechMenu:
    case ContextMenuItemTagStartSpeaking:
    case ContextMenuItemTagStopSpeaking:
        return true;
    default:
        return false;
    }
}

using ContextMenuActions = Vector<std::pair<ContextMenuAction, String>>;

static void appendContextMenuItems(ContextMenuController& controller, const Vector<ContextMenuItem>& source, unsigned parent, ZeroG::ContextMenu& menu, ContextMenuActions& actions)
{
    bool lastWasSeparator = true; // no separator first
    for (auto item : source) {
        if (isUnavailableContextMenuAction(item.action()))
            continue;
        // Items with nothing to show (platform services left without a title).
        if (item.type() != ContextMenuItemType::Separator && item.title().isEmpty())
            continue;
        if (item.type() == ContextMenuItemType::Separator) {
            if (lastWasSeparator)
                continue;
        }
        // Copy without a selection, Paste with an empty clipboard: disabled.
        controller.checkOrEnableIfNeeded(item);
        ZeroG::ContextMenu::Item out;
        out.id = actions.size() + 1;
        out.parent = parent;
        out.label = item.title();
        out.enabled = item.enabled();
        out.checked = item.checked();
        switch (item.type()) {
        case ContextMenuItemType::Action:
            out.kind = ZeroG::ContextMenu::ItemKind::Action;
            break;
        case ContextMenuItemType::CheckableAction:
            out.kind = ZeroG::ContextMenu::ItemKind::Checkbox;
            break;
        case ContextMenuItemType::Separator:
            out.kind = ZeroG::ContextMenu::ItemKind::Separator;
            break;
        case ContextMenuItemType::Submenu:
            out.kind = ZeroG::ContextMenu::ItemKind::Submenu;
            break;
        }
        actions.append({ item.action(), item.title() });
        menu.items.append(out);
        lastWasSeparator = out.kind == ZeroG::ContextMenu::ItemKind::Separator;
        if (out.kind == ZeroG::ContextMenu::ItemKind::Submenu)
            appendContextMenuItems(controller, item.subMenuItems(), out.id, menu, actions);
    }
    // No separator last.
    if (lastWasSeparator && !menu.items.isEmpty() && menu.items.last().parent == parent && menu.items.last().kind == ZeroG::ContextMenu::ItemKind::Separator)
        menu.items.removeLast();
}

bool View::contextMenu(const FloatPoint& position, Modifiers modifiers)
{
    RefPtr frame = mainFrame(m_page);
    if (!frame)
        return false;
    m_page->contextMenuController().clearContextMenu();
    PlatformMouseEvent event(position, position, WebCore::MouseButton::Right, PlatformEvent::Type::MousePressed, 1, modifiers, MonotonicTime::now(), 0,
        SyntheticClickType::NoTap, MouseEventInputSource::UserDriven);
    bool handled = frame->eventHandler().sendContextMenuEvent(event);
    return showContextMenu(position) || handled;
}

bool View::contextMenuForKeyboard()
{
    RefPtr frame = m_page->focusController().focusedOrMainFrame();
    if (!frame)
        return false;
    m_page->contextMenuController().clearContextMenu();
    bool handled = frame->eventHandler().sendContextMenuEventForKey();
    return showContextMenu(m_page->contextMenuController().hitTestResult().roundedPointInMainFrame()) || handled;
}

// The menu WebCore built (none: the page cancelled the contextmenu event, or
// there is nothing to offer) goes to the client, which answers later.
bool View::showContextMenu(const FloatPoint& position)
{
    auto& controller = m_page->contextMenuController();
    auto* menu = controller.contextMenu();
    if (!menu)
        return false;
    ZeroG::ContextMenu request;
    request.position = position;
    auto& hit = controller.hitTestResult();
    request.linkURL = hit.absoluteLinkURL();
    if (!request.linkURL.isEmpty())
        request.linkText = hit.textContent();
    request.imageURL = hit.absoluteImageURL();
    request.mediaURL = hit.absoluteMediaURL();
    request.selectedText = hit.selectedText();
    request.isEditable = hit.isContentEditable();
    if (RefPtr frame = hit.innerNodeFrame())
        request.frameURL = frame->document() ? frame->document()->url() : URL { };
    ContextMenuActions actions;
    appendContextMenuItems(controller, menu->items(), 0, request, actions);
    if (request.items.isEmpty()) {
        controller.didDismissContextMenu();
        return true;
    }
    client().showContextMenu(WTF::move(request), [page = m_page, actions = WTF::move(actions)](std::optional<unsigned> id) {
        auto& controller = page->contextMenuController();
        // A later menu replaced this one.
        if (!controller.contextMenu())
            return;
        if (!id || !*id || *id > actions.size()) {
            controller.didDismissContextMenu();
            controller.clearContextMenu();
            return;
        }
        auto& [action, title] = actions[*id - 1];
        controller.contextMenuItemSelected(action, title);
        controller.clearContextMenu();
    });
    return true;
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
    if (frame->eventHandler().keyEvent(keyboardEvent(PlatformEvent::Type::RawKeyDown, key, { })))
        return true;
    // The menu key, or Shift+F10, the page did not take: the focused element's
    // context menu.
    constexpr int menuKey = 0x5D; // VK_APPS
    constexpr int f10 = 0x79; // VK_F10
    if (key.windowsKeyCode == menuKey || (key.windowsKeyCode == f10 && key.modifiers.contains(PlatformEvent::Modifier::ShiftKey)))
        return contextMenuForKeyboard();
    return false;
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

void View::setColorScheme(ColorScheme scheme)
{
    m_colorScheme = scheme;
    bool dark = scheme == ColorScheme::Dark || (scheme == ColorScheme::System && s_systemDarkAppearance);
    // Styles are resolved again, and prefers-color-scheme listeners told.
    if (m_page->useDarkAppearance() != dark)
        m_page->setUseColorAppearance(dark, false);
}

void View::setSystemDarkAppearance(bool dark)
{
    if (s_systemDarkAppearance == dark)
        return;
    s_systemDarkAppearance = dark;
    for (auto* view : viewsByPage().values()) {
        if (view->m_colorScheme == ColorScheme::System)
            view->setColorScheme(ColorScheme::System);
    }
}

bool View::systemDarkAppearance()
{
    return s_systemDarkAppearance;
}

} // namespace ZeroG
