#include "config.h"
#include "View.h"

#include "ChromeClientZeroG.h"
#include "Embedding.h"
#include <WebCore/ActivityState.h>
#include <WebCore/Chrome.h>
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
    auto configuration = pageConfigurationWithEmptyClients(std::nullopt, PAL::SessionID::defaultSessionID());
    // The empty clients are made for SVG images, whose frames are fully sandboxed;
    // a ZeroG page is the application's own UI.
    std::get<PageConfiguration::LocalMainFrameCreationParameters>(configuration.mainFrameCreationParameters).effectiveSandboxFlags = { };
    configuration.chromeClient = makeUniqueRef<ChromeClientZeroG>(view);
    return Page::create(WTF::move(configuration));
}

View::View(Configuration&& configuration)
    : m_page(createPage(*this))
    , m_surface(WTF::move(configuration.surface))
    , m_size(configuration.size)
    , m_scale(configuration.scale > 0 ? configuration.scale : 1)
    , m_transparent(configuration.transparent)
{
    Ref page = m_page;
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

View::~View()
{
    static_cast<ChromeClientZeroG&>(m_page->chrome().client()).detachFromView();
}

IntSize View::physicalSize() const
{
    return { static_cast<int>(std::ceil(m_size.width() * m_scale)), static_cast<int>(std::ceil(m_size.height() * m_scale)) };
}

void View::loadHTML(const String& html)
{
    ZeroG::loadHTML(m_page, html);
    invalidateAll();
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

} // namespace ZeroG
