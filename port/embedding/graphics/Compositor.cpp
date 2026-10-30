/*
 * Copyright (C) 2014, 2025 Igalia S.L.
 * Copyright (C) 2011-2025 Apple Inc. All rights reserved.
 * Copyright (C) 2011 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2012 Company 100, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

// The scheduling follows WebKit2's ThreadedCompositor: a composition is
// Scheduled, InProgress, or ScheduledWhileInProgress when asked for again while
// one runs; a rendering update's frame waits for its tiles. In window mode the
// frame timer stands in for the UI process's "frame done" message: a frame
// completes one frame interval after it started, which paces animations. In
// texture mode the application's frames pace them (hostFrame), and a frame
// completes as soon as it is published.

#include "config.h"
#include "graphics/Compositor.h"

#include "View.h"
#include "graphics/Surface.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <WebCore/CoordinatedTileManager.h>
#include <WebCore/Damage.h>
#include <WebCore/GLContext.h>
#include <WebCore/GraphicsLayerCoordinated.h>
#include <WebCore/NativeImage.h>
#include <WebCore/PlatformDisplay.h>
#include <WebCore/SkiaCompositingLayer.h>
#include <WebCore/TransformationMatrix.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMallocInlines.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkCanvas.h>
#include <skia/gpu/ganesh/GrDirectContext.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace ZeroG {

using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(Compositor);

// One compositor thread for every view of the runtime, with one Skia GL context:
// GL is then used from as few threads as possible (ANGLE's D3D11 backend shares
// one device context between all GL contexts, which threads drawing at once
// corrupt even under its share-context lock). A window's swap waits for vsync
// there, which other views' frames wait behind at most once per refresh.
static WorkQueue& compositorQueue()
{
    static NeverDestroyed<Ref<WorkQueue>> queue = WorkQueue::create("ZeroG compositor"_s);
    return queue.get();
}

// Engine thread: the compositors alive, so that the last one gives the compositor
// thread's GL state back.
static unsigned s_compositorCount;

// Compositor thread: the compositors its tasks may still run for.
static HashSet<Compositor*>& liveCompositors()
{
    static NeverDestroyed<HashSet<Compositor*>> compositors;
    return compositors;
}

// Frame generations, unique across compositors (one may reuse another's address).
static std::atomic<uint64_t> s_frameGenerations;

std::unique_ptr<Compositor> Compositor::create(View& view, std::unique_ptr<Surface>& surface)
{
    std::unique_ptr<Compositor> compositor(new Compositor(view, WTF::move(surface)));
    if (compositor->initialize())
        return compositor;
    surface = std::exchange(compositor->m_surface, nullptr);
    return nullptr;
}

Compositor::Compositor(View& view, std::unique_ptr<Surface>&& surface)
    : m_view(view)
    , m_surface(WTF::move(surface))
    , m_sceneState(WebKit::CoordinatedSceneState::create())
    , m_workQueue(compositorQueue())
{
    ++s_compositorCount;
    auto& rootLayer = m_sceneState->rootLayer();
    Locker locker { rootLayer.lock() };
    rootLayer.setAnchorPoint(FloatPoint3D(0, 0, 0));
}

bool Compositor::initialize()
{
    bool initialized = false;
    m_workQueue->dispatchSync([&] {
        liveCompositors().add(this);
        // The compositor thread's own context, in the engine's share group
        // (created by the first compositor, shared by the others).
        auto* context = PlatformDisplay::sharedDisplay().skiaGLContext();
        if (!context || !context->makeContextCurrent())
            return;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &m_maxTextureSize);
        m_threadSafeGrContext = PlatformDisplay::sharedDisplay().skiaGrContext()->threadSafeProxy();
        initialized = true;
    });
    if (!initialized) {
        Locker locker { m_stateLock };
        m_state = State::Invalidated;
        return false;
    }
    m_tileManager = CoordinatedTileManager::create(sk_sp { m_threadSafeGrContext });
    return true;
}

Compositor::~Compositor()
{
    // As LayerTreeHost: tiles first, then the compositor thread's state, then
    // the layers (which may outlive the view with their page).
    m_tileManager = nullptr;
    {
        Locker locker { m_stateLock };
        m_state = State::Invalidated;
    }
    bool last = !--s_compositorCount;
    m_workQueue->dispatchSync([this, last] {
        // Its tasks still queued do nothing from now on.
        liveCompositors().remove(this);
        if (m_maxTextureSize) {
            m_sceneState->flushCompositingState(CompositionReason::RenderingUpdate);
            m_sceneState->invalidateCommittedLayers();
        }
        // The surface's GPU resources are this thread's context's.
        m_surface = nullptr;
        if (!last)
            return;
        // The last compositor: the thread's context, and EGL's state for the
        // thread (ANGLE keeps its device there), go too.
        PlatformDisplay::sharedDisplay().setSkiaGLContextForCurrentThread(nullptr);
        eglReleaseThread();
    });
    m_sceneState->invalidate();
}

// ---- Engine thread ----

void Compositor::setRootLayer(GraphicsLayer* layer)
{
    if (m_rootLayer == layer)
        return;
    m_rootLayer = layer;
    updateRootLayer();
}

void Compositor::setOverlayLayer(GraphicsLayer* layer)
{
    if (m_overlayLayer == layer)
        return;
    m_overlayLayer = layer;
    updateRootLayer();
}

void Compositor::updateRootLayer()
{
    Vector<Ref<CoordinatedPlatformLayer>> children;
    if (m_rootLayer) {
        children.append(downcast<GraphicsLayerCoordinated>(*m_rootLayer).coordinatedPlatformLayer());
        if (m_overlayLayer)
            children.append(downcast<GraphicsLayerCoordinated>(*m_overlayLayer).coordinatedPlatformLayer());
    }
    m_sceneState->setRootLayerChildren(WTF::move(children));
    m_view.setNeedsRenderingUpdate();
}

void Compositor::flushOverlay(const FloatRect& visibleContents)
{
    if (m_overlayLayer)
        m_overlayLayer->flushCompositingState(visibleContents);
}

bool Compositor::commit()
{
    if (m_pendingResize) {
        auto& rootLayer = m_sceneState->rootLayer();
        Locker locker { rootLayer.lock() };
        rootLayer.setSize(m_logicalSize);
    }
    bool layersChanged = m_sceneState->flush();
    bool requested = m_compositionRequired || m_pendingResize || layersChanged;
    if (requested)
        requestCompositionForRenderingUpdate();
    m_compositionRequired = false;
    m_pendingResize = false;

    // Image backing stores no layer uses any more.
    m_imageBackingStores.removeIf([](auto& entry) {
        return entry.value->hasOneRef();
    });
    return requested;
}

void Compositor::setSize(const IntSize& size, float scale)
{
    m_logicalSize = size;
    m_pendingResize = true;
    Locker locker { m_attributesLock };
    m_scale = scale;
    m_physicalSize = expandedIntSize(FloatSize(size).scaled(scale));
}

void Compositor::setFrameInterval(Seconds interval)
{
    m_frameIntervalSeconds = interval.seconds();
}

void Compositor::hostFrame(MonotonicTime frameTime)
{
    if (m_pacing == Pacing::Display)
        return;
    m_hostFrameTime = frameTime.secondsSinceEpoch().seconds();
    if (!m_animationsRunning)
        return;
    // At most one frame per frame interval, when the application set one (a
    // little early is still the same frame: application clocks jitter).
    auto interval = Seconds(m_frameIntervalSeconds.load());
    if (interval > 0_s && frameTime - m_lastHostAnimationFrame < interval * 0.9)
        return;
    m_lastHostAnimationFrame = frameTime;
    requestComposition(CompositionReason::Animation);
}

void Compositor::setTransparent(bool transparent)
{
    m_transparent = transparent;
}

void Compositor::setPresenter(Function<void()>&& presenter)
{
    m_workQueue->dispatchSync([&] {
        m_presenter = WTF::move(presenter);
    });
}

void Compositor::requestCompositionForRenderingUpdate()
{
    m_waitingForFrame = true;
    Locker locker { m_stateLock };
    m_reasons.add(CompositionReason::RenderingUpdate);
    m_notifyRenderingUpdate = true;
    if (m_sceneState->pendingTiles())
        m_waitingForTiles = true;
    scheduleUpdateLocked();
    // The frame interval paces the compositor's own animation frames; a
    // rendering update has its clock already (the application's, or vsync):
    // its frame does not wait for the previous one's interval to end.
    if (m_state == State::ScheduledWhileInProgress) {
        runOnCompositorThread([this] {
            if (std::exchange(m_framePending, false))
                frameComplete();
        });
    }
}

Ref<CoordinatedImageBackingStore> Compositor::imageBackingStore(Ref<NativeImage>&& nativeImage)
{
    auto identifier = nativeImage->uniqueID();
    return m_imageBackingStores.ensure(identifier, [&] {
        return CoordinatedImageBackingStore::create(WTF::move(nativeImage), sk_sp { m_threadSafeGrContext });
    }).iterator->value;
}

void Compositor::attachLayer(CoordinatedPlatformLayer& layer)
{
    m_sceneState->addLayer(layer);
}

void Compositor::detachLayer(CoordinatedPlatformLayer& layer)
{
    m_sceneState->removeLayer(layer);
}

Ref<GraphicsLayer> Compositor::createGraphicsLayer(GraphicsLayer::Type type, GraphicsLayerClient& client)
{
    return adoptRef(*new GraphicsLayerCoordinated(type, client, CoordinatedPlatformLayer::create(*this)));
}

// ---- Any thread ----

bool Compositor::isCompositionRequiredOrOngoing() const
{
    if (m_compositionRequired)
        return true;
    Locker locker { m_stateLock };
    return m_state != State::Idle && m_state != State::Invalidated;
}

void Compositor::requestComposition(CompositionReason reason)
{
    Locker locker { m_stateLock };
    m_reasons.add(reason);
    scheduleUpdateLocked();
}

void Compositor::didPaintTile()
{
    m_sceneState->didPaintTile();
    Locker locker { m_stateLock };
    if (!m_waitingForTiles || m_sceneState->pendingTiles())
        return;
    m_waitingForTiles = false;
    scheduleUpdateLocked();
}

void Compositor::scheduleUpdateLocked()
{
    switch (m_state) {
    case State::Idle:
        m_state = State::Scheduled;
        if (!m_waitingForTiles)
            scheduleRenderLocked();
        break;
    case State::Scheduled:
        if (!m_waitingForTiles)
            scheduleRenderLocked();
        break;
    case State::InProgress:
        m_state = State::ScheduledWhileInProgress;
        break;
    case State::ScheduledWhileInProgress:
    case State::Invalidated:
        break;
    }
}

void Compositor::scheduleRenderLocked()
{
    if (std::exchange(m_renderScheduled, true))
        return;
    runOnCompositorThread([this] {
        {
            Locker locker { m_stateLock };
            m_renderScheduled = false;
        }
        renderLayerTree();
    });
}

void Compositor::runOnCompositorThread(Function<void()>&& task, Seconds delay)
{
    auto run = [this, task = WTF::move(task)] mutable {
        if (liveCompositors().contains(this))
            task();
    };
    if (delay > 0_s)
        m_workQueue->dispatchAfter(delay, WTF::move(run));
    else
        m_workQueue->dispatch(WTF::move(run));
}

// ---- Compositor thread ----

void Compositor::renderLayerTree()
{
    OptionSet<CompositionReason> reasons;
    bool notifyRenderingUpdate = false;
    {
        Locker locker { m_stateLock };
        if (m_state == State::Invalidated)
            return;
        reasons = std::exchange(m_reasons, { });
        if (reasons.contains(CompositionReason::RenderingUpdate)) {
            if (m_waitingForTiles) {
                reasons.remove(CompositionReason::RenderingUpdate);
                m_reasons.add(CompositionReason::RenderingUpdate);
            } else
                notifyRenderingUpdate = std::exchange(m_notifyRenderingUpdate, false);
        }
        m_state = State::InProgress;
    }

    IntSize size;
    float scale;
    {
        Locker locker { m_attributesLock };
        size = m_physicalSize;
        scale = m_scale;
    }

    m_lastFrameStart = MonotonicTime::now();
    bool hostPaced = m_pacing == Pacing::Host;
    // Texture mode: animations at the application's frame time, as the page's
    // own (View::render); window mode: now, the display's.
    std::optional<MonotonicTime> animationTime;
    if (double hostFrameTime = m_hostFrameTime; hostPaced && hostFrameTime)
        animationTime = MonotonicTime::fromRawSeconds(hostFrameTime);
    if (reasons && !size.isEmpty()) {
        if (m_surface->size() != size)
            m_surface->resize(size);
        // Animations alone only move what is committed already.
        if (!(reasons.hasExactlyOneBitSet() && reasons.contains(CompositionReason::Animation)))
            m_sceneState->flushCompositingState(reasons);

        bool hasRunningAnimations = false;
        if (auto target = m_surface->beginFrame({ { }, size })) {
            auto& rootLayer = m_sceneState->rootLayer().ensureTarget();
            TransformationMatrix viewportTransform;
            viewportTransform.scale(scale);
            rootLayer.setTransform(viewportTransform);
            std::optional<Damage> frameDamage;
            target->canvas.save();
            hasRunningAnimations = rootLayer.paint(target->canvas, frameDamage, std::nullopt, m_transparent ? SK_ColorTRANSPARENT : SK_ColorWHITE, animationTime);
            target->canvas.restore();
            m_surface->endFrame();
            ++m_compositedFrames;
            if (m_presenter) {
                m_presenter();
                // The presenter used its window's context: Skia's again.
                if (auto* context = PlatformDisplay::sharedDisplay().skiaGLContext())
                    context->makeContextCurrent();
            }
        }
        // The next animation frame: now in window mode, at the application's
        // next frame in texture mode.
        if (hostPaced)
            m_animationsRunning = hasRunningAnimations;
        else if (hasRunningAnimations)
            requestComposition(CompositionReason::Animation);
    }

    // The rendering update's frame is out: the next update may go. The engine
    // thread wakes up either way: a new frame is there for window mode to
    // present and for the application to acquire.
    if (notifyRenderingUpdate)
        m_waitingForFrame = false;
    RunLoop::mainSingleton().dispatch([] { });

    // The frame completes one frame interval after it started in window mode,
    // at once in texture mode (the application's next frame is the clock).
    auto interval = hostPaced ? 0_s : Seconds(m_frameIntervalSeconds.load());
    auto remaining = interval - (MonotonicTime::now() - m_lastFrameStart);
    m_framePending = true;
    auto generation = m_frameGeneration = ++s_frameGenerations;
    runOnCompositorThread([this, generation] {
        if (m_framePending && m_frameGeneration == generation) {
            m_framePending = false;
            frameComplete();
        }
    }, std::max(0_s, remaining));
}

void Compositor::frameComplete()
{
    Locker locker { m_stateLock };
    switch (m_state) {
    case State::Idle:
    case State::Scheduled:
    case State::Invalidated:
        break;
    case State::InProgress:
        m_state = m_reasons.contains(CompositionReason::RenderingUpdate) && m_waitingForTiles ? State::Scheduled : State::Idle;
        break;
    case State::ScheduledWhileInProgress:
        m_state = State::Scheduled;
        if (!(m_reasons.containsOnly({ CompositionReason::RenderingUpdate }) && m_waitingForTiles))
            scheduleRenderLocked();
        break;
    }
}

} // namespace ZeroG
