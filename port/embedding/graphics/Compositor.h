/*
 * Copyright (C) 2014, 2025 Igalia S.L.
 * Copyright (C) 2011-2025 Apple Inc. All rights reserved.
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

#pragma once

// GPU compositing (ZeroG.md, "Compositing", version 2): the single-process
// counterpart of WebKit2's LayerTreeHost and ThreadedCompositor
// (Source/WebKit/WebProcess/WebPage/CoordinatedGraphics), on upstream's
// coordinated graphics with Skia.
//
// - The page's graphics layers are coordinated layers (this is their factory).
//   On the engine thread View::render updates the rendering, which flushes the
//   layers, and commit() hands their state to the compositor thread
//   (CoordinatedSceneState).
// - Layer contents are painted in tiles by WebCore's painting workers.
// - The compositor thread has a Skia GL context of its own in the engine's
//   share group. It applies the committed state, paints the layer tree
//   (SkiaCompositingLayer) into the view's surface and publishes the frame.
//   While layers animate (transform, opacity, filters) it goes on producing
//   frames by itself, one per frame interval, with no work on the engine thread.
//
// A composited view's surface belongs to the compositor thread: only it draws,
// resizes and destroys it (its GPU resources live in that thread's context).

#include "graphics/CoordinatedSceneState.h"
#include <WebCore/CoordinatedImageBackingStore.h>
#include <WebCore/CoordinatedPlatformLayer.h>
#include <WebCore/FloatRect.h>
#include <WebCore/GraphicsLayerFactory.h>
#include <WebCore/IntSize.h>
#include <atomic>
#include <memory>
#include <wtf/CheckedPtr.h>
#include <wtf/HashMap.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Lock.h>
#include <wtf/RunLoop.h>
#include <wtf/Seconds.h>
#include <wtf/WorkQueue.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/gpu/ganesh/GrContextThreadSafeProxy.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace WebCore {
class CoordinatedTileManager;
class GraphicsLayer;
}

namespace ZeroG {

class Surface;
class View;

class Compositor final : public WebCore::GraphicsLayerFactory, public WebCore::CoordinatedPlatformLayer::Client, public CanMakeCheckedPtr<Compositor> {
    WTF_MAKE_NONCOPYABLE(Compositor);
    WTF_MAKE_TZONE_ALLOCATED(Compositor);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(Compositor);
public:
    // Takes over the view's surface (a GPU one). Null, with the surface given
    // back, if the compositor thread gets no GL context.
    static std::unique_ptr<Compositor> create(View&, std::unique_ptr<Surface>&);
    ~Compositor();

    // The surface the compositor draws into: application threads acquire its
    // frames; the engine thread only reads its kind, size and device.
    Surface& surface() const { return *m_surface; }

    // ChromeClient: the page's root layer and the view overlay's.
    void setRootLayer(WebCore::GraphicsLayer*);
    void setOverlayLayer(WebCore::GraphicsLayer*);

    // Engine thread, View::render. Before the rendering update is finalized:
    // the overlay layers' own flush. After it: hands the layers' new state to the
    // compositor thread and asks for a frame if anything changed; returns
    // whether it did.
    void flushOverlay(const WebCore::FloatRect& visibleContents);
    bool commit();
    // The next commit() asks for a frame even if no layer changed.
    void requestFrame() { m_compositionRequired = true; }
    // A frame for the last rendering update is still being composited: the next
    // rendering update waits for it (as in WebKit2), so the page's updates go at
    // the compositor's pace.
    bool isWaitingForFrame() const { return m_waitingForFrame; }

    // Engine thread. Logical size and scale; the surface follows on the
    // compositor thread before the next frame.
    void setSize(const WebCore::IntSize&, float scale);
    // The shortest time between two frames (the display's refresh interval in
    // window mode). Animations are composited at this rate.
    void setFrameInterval(Seconds);
    void setTransparent(bool);

    // For tests: frames composited so far, any thread.
    unsigned compositedFrames() const { return m_compositedFrames; }

private:
    Compositor(View&, std::unique_ptr<Surface>&&);
    bool initialize();

    // CoordinatedPlatformLayer::Client
    WebCore::CoordinatedTileManager& tileManager() const final { return *m_tileManager; }
    Ref<WebCore::CoordinatedImageBackingStore> imageBackingStore(Ref<WebCore::NativeImage>&&) final;
    void attachLayer(WebCore::CoordinatedPlatformLayer&) final;
    void detachLayer(WebCore::CoordinatedPlatformLayer&) final;
    void notifyCompositionRequired() final { m_compositionRequired = true; }
    bool isCompositionRequiredOrOngoing() const final;
    void requestComposition(WebCore::CompositionReason) final;
    RunLoop* compositingRunLoop() const final { return &m_workQueue->runLoop(); }
    int maxTextureSize() const final { return m_maxTextureSize; }
    void willPaintTile() final { m_sceneState->willPaintTile(); }
    void didPaintTile() final;

    // GraphicsLayerFactory
    Ref<WebCore::GraphicsLayer> createGraphicsLayer(WebCore::GraphicsLayer::Type, WebCore::GraphicsLayerClient&) final;

    void updateRootLayer();
    void requestCompositionForRenderingUpdate();

    // Compositor thread.
    void renderLayerTree();
    void frameComplete();
    void scheduleUpdateLocked() WTF_REQUIRES_LOCK(m_stateLock);
    void startRenderTimerLocked() WTF_REQUIRES_LOCK(m_stateLock);

    View& m_view;
    std::unique_ptr<Surface> m_surface;
    const Ref<WebKit::CoordinatedSceneState> m_sceneState;
    const Ref<WorkQueue> m_workQueue;
    std::unique_ptr<WebCore::CoordinatedTileManager> m_tileManager;
    sk_sp<GrContextThreadSafeProxy> m_threadSafeGrContext;
    HashMap<uint64_t, Ref<WebCore::CoordinatedImageBackingStore>> m_imageBackingStores;
    int m_maxTextureSize { 0 };

    // Engine thread.
    WebCore::GraphicsLayer* m_rootLayer { nullptr };
    WebCore::GraphicsLayer* m_overlayLayer { nullptr };
    bool m_compositionRequired { false };
    bool m_pendingResize { true };
    WebCore::IntSize m_logicalSize;
    // Cleared by the compositor thread itself: the engine thread need not run
    // its tasks (hosts that only call render()) for the next update to go.
    std::atomic<bool> m_waitingForFrame { false };

    // Shared with the compositor thread.
    enum class State : uint8_t { Idle, Scheduled, InProgress, ScheduledWhileInProgress, Invalidated };
    mutable Lock m_stateLock;
    State m_state WTF_GUARDED_BY_LOCK(m_stateLock) { State::Idle };
    OptionSet<WebCore::CompositionReason> m_reasons WTF_GUARDED_BY_LOCK(m_stateLock);
    bool m_waitingForTiles WTF_GUARDED_BY_LOCK(m_stateLock) { false };
    bool m_notifyRenderingUpdate WTF_GUARDED_BY_LOCK(m_stateLock) { false };
    Lock m_attributesLock;
    WebCore::IntSize m_physicalSize WTF_GUARDED_BY_LOCK(m_attributesLock);
    float m_scale WTF_GUARDED_BY_LOCK(m_attributesLock) { 1 };
    std::atomic<bool> m_transparent { false };
    std::atomic<double> m_frameIntervalSeconds { 1.0 / 60 };
    std::atomic<unsigned> m_compositedFrames { 0 };

    // Compositor thread.
    RunLoop::Timer m_renderTimer;
    RunLoop::Timer m_frameTimer;
    MonotonicTime m_lastFrameStart;
};

} // namespace ZeroG
