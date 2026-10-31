// Texture mode for D3D12 and Vulkan games on Windows: frames are D3D11 textures on
// ANGLE's device shared by NT handles, ordered by two shared timeline fences
// instead of a keyed mutex (neither D3D12 nor Vulkan has one):
// - the ready fence: the engine signals a frame's value from ANGLE's context once
//   its drawing is submitted; the reader waits for it on its GPU;
// - the release fence: the reader signals it when done with a frame; the engine
//   has ANGLE's context wait for it on the GPU before drawing into that texture
//   again, so neither side ever blocks a CPU thread.
// A D3D12 surface opens the texture and the fences on the application's queue's
// device and queues the waits and signals itself; a Win32 surface hands out the
// handles (Surface::win32), and the application imports them (Vulkan:
// VK_KHR_external_memory_win32, VK_KHR_external_semaphore_win32 with
// D3D12_FENCE handles) and signals the release value itself.

#include "config.h"

#if OS(WINDOWS)

#include "SurfaceD3D.h"

#include <atomic>
#include <d3d12.h>
#include <wtf/Scope.h>

using Microsoft::WRL::ComPtr;

namespace ZeroG {

using namespace WebCore;

namespace {

class FenceSurface final : public D3DSurface {
public:
    static std::unique_ptr<FenceSurface> create(ID3D12CommandQueue* queue, unsigned bufferCount)
    {
        ComPtr<ID3D11Device> engine;
        EGLDisplay display;
        if (!angleDevice(engine, display))
            return nullptr;
        ComPtr<ID3D11Device5> engine5;
        auto context = sharedEngineContext(engine.Get());
        ComPtr<ID3D11DeviceContext4> context4;
        if (FAILED(engine.As(&engine5)) || FAILED(context.As(&context4))) {
            setLastError("ANGLE's D3D11 device has no fences (Windows 10 1703 or later)");
            return nullptr;
        }

        ComPtr<ID3D12Device> hostDevice;
        if (queue) {
            if (FAILED(queue->GetDevice(IID_PPV_ARGS(&hostDevice)))) {
                setLastError("cannot get the D3D12 queue's device");
                return nullptr;
            }
            LUID luid = hostDevice->GetAdapterLuid();
            if ((static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32 | luid.LowPart) != adapterLUID(engine.Get())) {
                setLastError("the D3D12 device is on another adapter than the engine's (zg_runtime_config.adapter_luid)");
                return nullptr;
            }
        }

        std::unique_ptr<FenceSurface> surface(new FenceSurface(queue ? SurfaceKind::D3D12 : SurfaceKind::Win32, WTF::move(engine), display, bufferCount));
        surface->m_engineContext = WTF::move(context);
        surface->m_engineContext4 = WTF::move(context4);
        surface->m_queue = queue;
        surface->m_hostDevice = WTF::move(hostDevice);
        if (!surface->createFence(engine5.Get(), surface->m_readyFence, surface->m_readyHandle)
            || !surface->createFence(engine5.Get(), surface->m_releaseFence, surface->m_releaseHandle)) {
            setLastError("cannot create shared D3D11 fences");
            return nullptr;
        }
        if (queue && (FAILED(surface->m_hostDevice->OpenSharedHandle(surface->m_readyHandle, IID_PPV_ARGS(&surface->m_hostReadyFence)))
            || FAILED(surface->m_hostDevice->OpenSharedHandle(surface->m_releaseHandle, IID_PPV_ARGS(&surface->m_hostReleaseFence))))) {
            setLastError("the D3D12 device cannot open the engine's fences");
            return nullptr;
        }
        return surface;
    }

    ~FenceSurface()
    {
        // The application's queue may still have a release signal or a ready
        // wait on these objects queued: let it run before they go.
        waitForReleases(m_releaseCounter.load());
        destroyAllBuffers();
        if (m_readyHandle)
            CloseHandle(m_readyHandle);
        if (m_releaseHandle)
            CloseHandle(m_releaseHandle);
    }

    bool win32(const Frame& frame, Win32Frame& out) const final
    {
        auto* buffer = static_cast<Buffer*>(frame.handle);
        auto* texture = buffer ? static_cast<Texture*>(buffer->backend) : nullptr;
        if (!texture)
            return false;
        out.texture = texture->sharedHandle;
        out.dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        out.readyFence = m_readyHandle;
        out.readyValue = texture->readyValue;
        // A D3D12 surface signals the release on the application's queue itself.
        out.releaseFence = kind() == SurfaceKind::Win32 ? m_releaseHandle : nullptr;
        out.releaseValue = kind() == SurfaceKind::Win32 ? texture->releaseValue : 0;
        out.bufferID = texture->bufferID;
        out.adapterLUID = adapterLUID(m_engine.Get());
        return true;
    }

private:
    FenceSurface(SurfaceKind kind, ComPtr<ID3D11Device>&& engine, EGLDisplay display, unsigned bufferCount)
        : D3DSurface(kind, bufferCount, WTF::move(engine), display)
    {
    }

    bool createFence(ID3D11Device5* device, ComPtr<ID3D11Fence>& fence, HANDLE& handle)
    {
        return SUCCEEDED(device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)))
            && SUCCEEDED(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle));
    }

    struct Texture {
        EngineTexture engine;
        HANDLE sharedHandle { nullptr };
        uint64_t bufferID { 0 };
        uint64_t readyValue { 0 }; // signaled when the frame's drawing is done
        uint64_t releaseValue { 0 }; // the reader's signal when done with it; 0: none yet
        ComPtr<ID3D12Resource> hostResource; // D3D12
    };

    bool createBuffer(Buffer& buffer, const IntSize& size) final
    {
        auto texture = std::make_unique<Texture>();
        if (!createEngineTexture(texture->engine, buffer, size, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE))
            return false;
        auto destroy = makeScopeExit([&] { destroyTexture(*texture); buffer.skSurface = nullptr; });
        ComPtr<IDXGIResource1> resource;
        if (FAILED(texture->engine.texture.As(&resource))
            || FAILED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &texture->sharedHandle)))
            return false;
        // D3D12 devices are free-threaded: the texture is opened here, once.
        if (m_hostDevice && FAILED(m_hostDevice->OpenSharedHandle(texture->sharedHandle, IID_PPV_ARGS(&texture->hostResource))))
            return false;
        texture->bufferID = nextBufferID();
        destroy.release();
        buffer.backend = texture.release();
        return true;
    }

    // Until the reader's GPU is done with everything released so far (bounded:
    // a reader that never signals must not hang the engine).
    void waitForReleases(uint64_t value)
    {
        if (!value || m_releaseFence->GetCompletedValue() >= value)
            return;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event && SUCCEEDED(m_releaseFence->SetEventOnCompletion(value, event)))
            WaitForSingleObject(event, 1000);
        if (event)
            CloseHandle(event);
    }

    void destroyTexture(Texture& texture)
    {
        waitForReleases(texture.releaseValue);
        destroyEngineTexture(texture.engine);
        if (texture.sharedHandle)
            CloseHandle(texture.sharedHandle);
        texture = { };
    }

    void destroyBuffer(Buffer& buffer) final
    {
        buffer.skSurface = nullptr;
        auto* texture = static_cast<Texture*>(std::exchange(buffer.backend, nullptr));
        if (!texture)
            return;
        destroyTexture(*texture);
        delete texture;
    }

    bool willRender(Buffer& buffer) final
    {
        if (!makeSkiaContextCurrent())
            return false;
        // The reader's last use of this texture comes first, on the GPU.
        if (auto value = static_cast<Texture*>(buffer.backend)->releaseValue)
            m_engineContext4->Wait(m_releaseFence.Get(), value);
        return true;
    }

    void didRender(Buffer& buffer) final
    {
        flush(buffer);
        auto& texture = *static_cast<Texture*>(buffer.backend);
        texture.readyValue = ++m_readyCounter;
        m_engineContext4->Signal(m_readyFence.Get(), texture.readyValue);
        // Submitted now: the reader's GPU waits for it.
        m_engineContext->Flush();
    }

    void willAcquire(Buffer& buffer) final
    {
        auto& texture = *static_cast<Texture*>(buffer.backend);
        if (m_queue)
            m_queue->Wait(m_hostReadyFence.Get(), texture.readyValue);
        else
            texture.releaseValue = ++m_releaseCounter; // what the reader signals when done
    }

    void fillFrame(Buffer& buffer, Frame& frame) final
    {
        auto& texture = *static_cast<Texture*>(buffer.backend);
        frame.texture = texture.hostResource.Get();
        frame.fence = m_hostReadyFence.Get();
        frame.fenceValue = texture.readyValue;
        frame.sharedHandle = texture.sharedHandle;
    }

    bool didRelease(Buffer& buffer) final
    {
        if (m_queue) {
            auto& texture = *static_cast<Texture*>(buffer.backend);
            texture.releaseValue = ++m_releaseCounter;
            m_queue->Signal(m_hostReleaseFence.Get(), texture.releaseValue);
        }
        return false;
    }

    ComPtr<ID3D11DeviceContext> m_engineContext;
    ComPtr<ID3D11DeviceContext4> m_engineContext4;
    ComPtr<ID3D11Fence> m_readyFence;
    ComPtr<ID3D11Fence> m_releaseFence;
    HANDLE m_readyHandle { nullptr };
    HANDLE m_releaseHandle { nullptr };
    uint64_t m_readyCounter { 0 }; // rendering thread
    std::atomic<uint64_t> m_releaseCounter { 0 }; // application threads

    // D3D12
    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<ID3D12Device> m_hostDevice;
    ComPtr<ID3D12Fence> m_hostReadyFence;
    ComPtr<ID3D12Fence> m_hostReleaseFence;
};

} // namespace

std::unique_ptr<Surface> createD3D12Surface(void* hostQueue, unsigned bufferCount)
{
    if (!hostQueue)
        return nullptr;
    return FenceSurface::create(static_cast<ID3D12CommandQueue*>(hostQueue), bufferCount);
}

std::unique_ptr<Surface> createWin32Surface(unsigned bufferCount)
{
    return FenceSurface::create(nullptr, bufferCount);
}

} // namespace ZeroG

#endif // OS(WINDOWS)
