#include "config.h"
#include "View.h"

#include "ChromeClientZeroG.h"
#include "EditorClientZeroG.h"
#include "FrameLoaderClientZeroG.h"
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
#include <WebCore/Settings.h>
#include <pal/SessionID.h>
#include <wtf/HashMap.h>
#include <wtf/NeverDestroyed.h>

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
    configuration.chromeClient = makeUniqueRef<ChromeClientZeroG>(view);
    configuration.editorClient = makeUniqueRef<EditorClientZeroG>();
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
    auto& settings = page->settings();
    settings.setScriptEnabled(configuration.scriptEnabled);
    // Compositing version 1 (ZeroG.md, "Compositing"): one surface, dirty regions.
    settings.setAcceleratedCompositingEnabled(false);
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

    // A view is visible and active until the application says otherwise; hidden
    // pages have requestAnimationFrame and timers suspended. Needs the frame's
    // initial document.
    page->setActivityState({ ActivityState::WindowIsActive, ActivityState::IsFocused, ActivityState::IsVisible,
        ActivityState::IsVisibleOrOccluded, ActivityState::IsInWindow, ActivityState::IsConnectedToHardwareConsole });

    m_surface->resize(physicalSize());
    invalidateAll();
}

ViewClient View::s_defaultClient;

View::~View()
{
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
    m_surface->resize(physicalSize());
    invalidateAll();
    m_needsRenderingUpdate = true;
}

void View::setScale(float scale)
{
    if (scale <= 0 || scale == m_scale)
        return;
    m_scale = scale;
    m_page->setDeviceScaleFactor(scale);
    m_surface->resize(physicalSize());
    invalidateAll();
    m_needsRenderingUpdate = true;
}

void View::invalidate(const IntRect& rect)
{
    m_damage.unite(intersection(rect, { { }, m_size }));
}

bool View::render(MonotonicTime frameTime)
{
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
    PlatformMouseEvent event(position, position, platformButton(button), PlatformEvent::Type::MouseReleased, clickCount, modifiers, MonotonicTime::now(), 0,
        SyntheticClickType::NoTap, MouseEventInputSource::UserDriven);
    return frame->eventHandler().handleMouseReleaseEvent(event).wasHandled();
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
