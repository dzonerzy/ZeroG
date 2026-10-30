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
// one runs; a rendering update's frame waits for its tiles. The frame timer stands
// in for the UI process's "frame done" message: a frame completes one frame
// interval after it started, which paces animations.

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
#include <wtf/TZoneMallocInlines.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkCanvas.h>
#include <skia/gpu/ganesh/GrDirectContext.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace ZeroG {

using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(Compositor);

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
    , m_workQueue(WorkQueue::create("ZeroG compositor"_s))
    , m_renderTimer(m_workQueue->runLoop(), "ZeroG::Compositor::renderTimer"_s, this, &Compositor::renderLayerTree)
    , m_frameTimer(m_workQueue->runLoop(), "ZeroG::Compositor::frameTimer"_s, this, &Compositor::frameComplete)
{
    auto& rootLayer = m_sceneState->rootLayer();
    Locker locker { rootLayer.lock() };
    rootLayer.setAnchorPoint(FloatPoint3D(0, 0, 0));
}

bool Compositor::initialize()
{
    bool initialized = false;
    m_workQueue->dispatchSync([&] {
        // The compositor thread's own context, in the engine's share group.
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
    m_workQueue->dispatchSync([this] {
        m_renderTimer.stop();
        m_frameTimer.stop();
        if (m_maxTextureSize) {
            m_sceneState->flushCompositingState(CompositionReason::RenderingUpdate);
            m_sceneState->invalidateCommittedLayers();
        }
        // The surface's GPU resources are this thread's context's.
        m_surface = nullptr;
        PlatformDisplay::sharedDisplay().setSkiaGLContextForCurrentThread(nullptr);
        // EGL's state for this thread (ANGLE keeps its device there) goes too.
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

void Compositor::setTransparent(bool transparent)
{
    m_transparent = transparent;
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
        m_workQueue->dispatch([this] {
            if (m_frameTimer.isActive()) {
                m_frameTimer.stop();
                frameComplete();
            }
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
            startRenderTimerLocked();
        break;
    case State::Scheduled:
        if (!m_waitingForTiles)
            startRenderTimerLocked();
        break;
    case State::InProgress:
        m_state = State::ScheduledWhileInProgress;
        break;
    case State::ScheduledWhileInProgress:
    case State::Invalidated:
        break;
    }
}

void Compositor::startRenderTimerLocked()
{
    // The timer belongs to the compositor thread's run loop.
    if (m_workQueue->runLoop().isCurrent()) {
        if (!m_renderTimer.isActive())
            m_renderTimer.startOneShot(0_s);
        return;
    }
    m_workQueue->dispatch([this] {
        Locker locker { m_stateLock };
        if (m_state != State::Invalidated && !m_renderTimer.isActive())
            m_renderTimer.startOneShot(0_s);
    });
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
            hasRunningAnimations = rootLayer.paint(target->canvas, frameDamage, std::nullopt, m_transparent ? SK_ColorTRANSPARENT : SK_ColorWHITE);
            target->canvas.restore();
            m_surface->endFrame();
            ++m_compositedFrames;
        }
        if (hasRunningAnimations)
            requestComposition(CompositionReason::Animation);
    }

    // The rendering update's frame is out: the next update may go. The engine
    // thread wakes up either way: a new frame is there for window mode to
    // present and for the application to acquire.
    if (notifyRenderingUpdate)
        m_waitingForFrame = false;
    RunLoop::mainSingleton().dispatch([] { });

    auto remaining = Seconds(m_frameIntervalSeconds.load()) - (MonotonicTime::now() - m_lastFrameStart);
    m_frameTimer.startOneShot(std::max(0_s, remaining));
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
            startRenderTimerLocked();
        break;
    }
}

} // namespace ZeroG
