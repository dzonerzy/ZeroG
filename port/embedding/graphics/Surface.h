#pragma once

// Rendering targets of a view (docs/design/graphics-presentation.md, "Modes" and "Separate render thread").
// A surface owns 2 or 3 buffers. The rendering thread (the engine thread, or the
// compositor thread for a composited view) renders into a free buffer and
// publishes it as the newest frame; the application, on any thread, acquires the
// newest frame and releases it when done. An unacquired frame is replaced by a
// newer one (latest wins), so the engine never waits for the application.
//
// A frame repaints only the view's damage:
// - CPU surfaces paint into the buffer itself; each buffer remembers what changed
//   since it was last drawn and catches up on it.
// - GPU surfaces paint into a private texture that keeps its contents, then copy
//   it whole into the buffer. Shared textures are never updated partially: after
//   another device has held one, ANGLE does not keep drawing reliably on top of
//   its contents.

#include <WebCore/IntRect.h>
#include <WebCore/IntSize.h>
#include <WebCore/Region.h>
#include <memory>
#include <optional>
#include <wtf/Lock.h>
#include <wtf/Vector.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkRefCnt.h>
#include <skia/core/SkSurface.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

class SkCanvas;

namespace ZeroG {

enum class SurfaceKind : uint8_t {
    CPU, // BGRA8 premultiplied pixels in memory
    D3D11, // Windows: a texture shared with the host's ID3D11Device
    GL, // Linux: an EGLImage on the host's EGLDisplay
    GPU, // a GL texture in the engine's share group (window mode)
    DMABuf, // Linux: a dma-buf, for any API that imports them (Vulkan)
    D3D12, // Windows: a texture and fences opened on the host's ID3D12CommandQueue
    Win32, // Windows: NT handles of a texture and fences, for any API that opens them (Vulkan)
    D3D9, // Windows: a texture on the host's IDirect3DDevice9 (shared with 9Ex, copied otherwise)
};

// A frame's Win32 handles (D3D12 and Win32 surfaces): the texture and the
// timeline fences ordering it, as NT handles. They stay the surface's.
struct Win32Frame {
    void* texture { nullptr }; // NT handle of the ID3D11Texture2D
    unsigned dxgiFormat { 0 };
    void* readyFence { nullptr }; // NT handle of a D3D fence: wait for readyValue before reading
    uint64_t readyValue { 0 };
    void* releaseFence { nullptr }; // signal releaseValue when done reading (null: the engine does)
    uint64_t releaseValue { 0 };
    uint64_t bufferID { 0 }; // the same texture and handle until a resize
    uint64_t adapterLUID { 0 }; // the adapter the texture is on (LUID, HighPart << 32 | LowPart)
};

// A DMABuf surface's frame: the buffer's planes, as DRM describes them, and
// the sync_file to wait on before reading. The descriptors stay the surface's;
// importing (which takes ownership in Vulkan) needs a dup.
struct DMABufFrame {
    uint32_t fourcc { 0 }; // DRM_FORMAT_*
    uint64_t modifier { 0 }; // DRM_FORMAT_MOD_*
    unsigned planeCount { 0 };
    int fds[4] { -1, -1, -1, -1 };
    uint32_t offsets[4] { };
    uint32_t strides[4] { };
    int readyFence { -1 }; // -1: the frame is complete already
    uint64_t bufferID { 0 }; // the same buffer, with the same descriptors, until a resize
};

// One acquired frame. Rows go from top to bottom.
struct Frame {
    unsigned width { 0 };
    unsigned height { 0 };
    // CPU
    const void* pixels { nullptr };
    size_t stride { 0 };
    // GPU: the texture for the host's device (ID3D11Texture2D*, EGLImage), the
    // fence to wait on before reading it (EGLSync; D3D11 frames are ordered by
    // their keyed mutex, already acquired), and the NT handle the texture was
    // shared with (D3D11).
    void* texture { nullptr };
    void* fence { nullptr };
    uint64_t fenceValue { 0 };
    void* sharedHandle { nullptr };
    // Changed since the previous acquired frame, physical pixels.
    WebCore::IntRect dirty;
    // Internal: the buffer to give back.
    void* handle { nullptr };
};

class Surface {
    WTF_MAKE_NONCOPYABLE(Surface);
public:
    virtual ~Surface();

    SurfaceKind kind() const { return m_kind; }
    // The device frames are given for: the application's, or the one the engine
    // created for it (ID3D11Device*). Null for other kinds.
    virtual void* device() const { return nullptr; }
    WebCore::IntSize size() const { return m_size; }

    // Engine thread. Buffers are recreated at the new size; buffers the
    // application still holds stay valid until it releases them.
    void resize(const WebCore::IntSize& physicalSize);

    struct Target {
        SkCanvas& canvas;
        // What to repaint in this buffer: the frame's damage plus the damage
        // this buffer missed. Physical pixels.
        WebCore::Region paintRegion;
    };
    // Engine thread: picks a buffer for a frame whose damage is `damage`. Null if
    // no buffer is available or it cannot be created.
    std::optional<Target> beginFrame(const WebCore::IntRect& damage);
    // Engine thread: publishes the frame begun by beginFrame().
    void endFrame();

    // Any thread: the newest published frame, or false when there is none newer
    // than the last one acquired. The frame stays valid until release().
    bool acquire(Frame&);
    // Any thread: whether acquire() would give a frame.
    bool hasNewFrame();
    // `releaseFence` (Linux, DMABuf surfaces): a sync_file signaled when the
    // application's reads of the frame are done, which the engine waits on
    // before drawing into that buffer again; the surface owns it from here.
    void release(const Frame&, int releaseFence = -1);
    // Any thread, an acquired frame of a DMABuf surface: its dma-buf.
    virtual bool dmabuf(const Frame&, DMABufFrame&) const { return false; }
    // Any thread, an acquired frame of a D3D12 or Win32 surface: its handles.
    virtual bool win32(const Frame&, Win32Frame&) const { return false; }
    // Application thread (D3D9): the host's device is about to be reset; its
    // objects for this surface are released and made again on the next acquire.
    virtual void deviceLost() { }

    // Engine thread: frees the GPU resources of released buffers from older
    // generations. Called by beginFrame() and resize().
    void collectGarbage();

protected:
    Surface(SurfaceKind, unsigned bufferCount);

    // For subclass destructors, which own the buffers' storage. Frames the
    // application still holds become invalid: release them before destroying a
    // surface.
    void destroyAllBuffers();

    // Releasing: given back, but the host's GPU may still read it (D3D9, where
    // the engine cannot wait for the host's device); free once releaseFinished().
    enum class BufferState : uint8_t { Free, Rendering, Ready, Acquired, Releasing };
    struct Buffer {
        BufferState state { BufferState::Free };
        uint64_t generation { 0 };
        bool published { false }; // drawn at least once in this generation
        bool acquiredSinceRender { false };
        WebCore::Region missedDamage;
        WebCore::IntRect frameDamage; // what the frame being rendered changed in the page
        sk_sp<SkSurface> skSurface;
        void* backend { nullptr }; // owned by the subclass
    };

    // Engine thread: create the buffer's storage and its Skia surface.
    virtual bool createBuffer(Buffer&, const WebCore::IntSize&) = 0;
    // Engine thread, GPU surfaces: the private target frames are painted into.
    virtual sk_sp<SkSurface> createPrivateTarget(const WebCore::IntSize&) { return nullptr; }
    // Engine thread: free the buffer's storage.
    virtual void destroyBuffer(Buffer&) = 0;
    // Engine thread, around drawing into the buffer.
    virtual bool willRender(Buffer&) { return true; }
    virtual void didRender(Buffer&) { }
    // Application thread, inside acquire() and release().
    virtual void fillFrame(Buffer&, Frame&) = 0;
    virtual void willAcquire(Buffer&) { }
    // Returns whether the buffer must wait for releaseFinished() before the
    // engine draws into it again.
    virtual bool didRelease(Buffer&) { return false; }
    // Application thread, at each acquire and release, for Releasing buffers.
    virtual bool releaseFinished(Buffer&) { return true; }
    // Application thread, inside release(): the application's release fence,
    // which the surface now owns. Kinds without dma-bufs close it.
    virtual void takeReleaseFence(Buffer&, int fence);

private:
    Buffer* pickBuffer() WTF_REQUIRES_LOCK(m_lock);
    void finishReleases() WTF_REQUIRES_LOCK(m_lock);
    static bool heldByHost(const Buffer& buffer) { return buffer.state == BufferState::Acquired || buffer.state == BufferState::Releasing; }
    bool usesPrivateTarget() const { return m_kind != SurfaceKind::CPU; }

    sk_sp<SkSurface> m_privateTarget; // engine thread

    const SurfaceKind m_kind;
    WebCore::IntSize m_size;
    uint64_t m_generation { 1 };

    Lock m_lock;
    Vector<std::unique_ptr<Buffer>> m_buffers WTF_GUARDED_BY_LOCK(m_lock);
    Vector<std::unique_ptr<Buffer>> m_retired WTF_GUARDED_BY_LOCK(m_lock);
    Buffer* m_rendering WTF_GUARDED_BY_LOCK(m_lock) { nullptr };
    Buffer* m_newest WTF_GUARDED_BY_LOCK(m_lock) { nullptr };
    WebCore::IntRect m_pendingDamage WTF_GUARDED_BY_LOCK(m_lock);
    WebCore::IntRect m_dirtySinceAcquire WTF_GUARDED_BY_LOCK(m_lock);
    bool m_hasUnacquiredFrame WTF_GUARDED_BY_LOCK(m_lock) { false };
};

std::unique_ptr<Surface> createCPUSurface(unsigned bufferCount);
// Frames are GL textures (names cast to void*) in the engine's share group, with
// an EGLSync to wait on. Null without a GPU (initializeGPU).
std::unique_ptr<Surface> createGPUSurface(unsigned bufferCount);
#if OS(WINDOWS)
// `hostDevice` is the application's ID3D11Device; null makes the engine create one
// on ANGLE's adapter, which device() returns. Null if ANGLE's D3D11 device or the
// shared textures are not available.
std::unique_ptr<Surface> createD3D11Surface(void* hostDevice, unsigned bufferCount);
// `hostQueue` is the application's ID3D12CommandQueue: frames are opened on its
// device, acquire queues a wait for the frame on it, release a signal.
std::unique_ptr<Surface> createD3D12Surface(void* hostQueue, unsigned bufferCount);
// Frames are NT handles (Surface::win32), for Vulkan or any API that opens them.
std::unique_ptr<Surface> createWin32Surface(unsigned bufferCount);
// `hostDevice` is the application's IDirect3DDevice9: frames are textures on it,
// shared with ANGLE's device for an IDirect3DDevice9Ex on its adapter, copied
// otherwise (always with `copy`, which tests use to reach that path with a 9Ex
// device).
std::unique_ptr<Surface> createD3D9Surface(void* hostDevice, unsigned bufferCount, bool copy = false);
// Why the last create*Surface above failed, for zg_last_error.
const char* lastSurfaceError();
#else
// `hostDisplay` is the application's EGLDisplay, which must be the display the
// engine's GPU was initialized with (initializeGPU).
std::unique_ptr<Surface> createGLSurface(void* hostDisplay, unsigned bufferCount);
// Frames are dma-bufs (Surface::dmabuf), on the engine's own GPU display. Null
// when no buffer can be made one: the driver exports none, and neither GBM nor
// a DRM device (dumb buffers) gives one the driver imports.
std::unique_ptr<Surface> createDMABufSurface(unsigned bufferCount);
#endif

} // namespace ZeroG
