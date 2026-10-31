// Texture mode for Direct3D 9 games on Windows. The engine renders in BGRA (what
// D3DFMT_A8R8G8B8 is in memory) through ANGLE into D3D11 textures, and the
// game's device gets each frame one of two ways:
// - IDirect3DDevice9Ex on the engine's adapter: the texture itself, shared by a (legacy) shared handle
//   that 9Ex devices open with CreateTexture. D3D9 has no fences shared with
//   D3D11, so the engine waits on its own thread for a frame's drawing to finish
//   before publishing it, and a released frame stays the game's until an event
//   query issued at its release says the game's GPU is done with it.
// - a plain IDirect3DDevice9, which opens no shared resources (or a 9Ex one on
//   another adapter): the frame is
//   read back into memory on the engine's side, and acquire copies the pixels
//   that changed since the previous acquired frame into a system-memory texture
//   and from there (UpdateTexture) into a texture of the game's. D3D9 orders
//   these copies with the game's own drawing.
// The game's device is single-threaded: acquire, release and
// zg_surface_device_lost run on its thread, and only there are its objects made
// and released.

#include "config.h"

#if OS(WINDOWS)

#include "SurfaceD3D.h"

#include <d3d9.h>
#include <wtf/Scope.h>

using Microsoft::WRL::ComPtr;

namespace ZeroG {

using namespace WebCore;

namespace {

// IID_IDirect3DDevice9Ex and IID_IDirect3D9Ex (d3d9.h declares them for
// dxguid.lib, which the engine does not otherwise need).
constexpr GUID iidDevice9Ex = { 0xb18b10ce, 0x2649, 0x405a, { 0x87, 0x0f, 0x95, 0xf7, 0x77, 0xd4, 0x31, 0x3a } };
constexpr GUID iidDirect3D9Ex = { 0x02177241, 0x69fc, 0x400c, { 0x8f, 0xf1, 0x93, 0xa4, 0x4d, 0xf6, 0x86, 0x1d } };

// The adapter a 9Ex device was created on (its LUID), or 0.
uint64_t deviceAdapterLUID(IDirect3DDevice9* device)
{
    ComPtr<IDirect3D9> direct3D;
    ComPtr<IDirect3D9Ex> direct3DEx;
    D3DDEVICE_CREATION_PARAMETERS parameters;
    LUID luid;
    if (FAILED(device->GetDirect3D(&direct3D)) || FAILED(direct3D->QueryInterface(iidDirect3D9Ex, reinterpret_cast<void**>(direct3DEx.GetAddressOf())))
        || FAILED(device->GetCreationParameters(&parameters)) || FAILED(direct3DEx->GetAdapterLUID(parameters.AdapterOrdinal, &luid)))
        return 0;
    return static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32 | luid.LowPart;
}

class D3D9Surface final : public D3DSurface {
public:
    static std::unique_ptr<D3D9Surface> create(IDirect3DDevice9* host, unsigned bufferCount, bool copy)
    {
        ComPtr<ID3D11Device> engine;
        EGLDisplay display;
        if (!angleDevice(engine, display))
            return nullptr;
        ComPtr<ID3D11Device5> engine5;
        if (FAILED(engine.As(&engine5))) {
            setLastError("ANGLE's D3D11 device has no fences (Windows 10 1703 or later)");
            return nullptr;
        }
        // A 9Ex device on another adapter cannot open the engine's textures: it
        // gets copies, as a plain device does.
        ComPtr<IDirect3DDevice9Ex> hostEx;
        host->QueryInterface(iidDevice9Ex, reinterpret_cast<void**>(hostEx.GetAddressOf()));
        if (hostEx && (copy || deviceAdapterLUID(host) != adapterLUID(engine.Get())))
            hostEx = nullptr;

        std::unique_ptr<D3D9Surface> surface(new D3D9Surface(WTF::move(engine), display, bufferCount));
        surface->m_host = host;
        surface->m_hostEx = WTF::move(hostEx);
        surface->m_engineContext = sharedEngineContext(surface->m_engine.Get());
        surface->m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!surface->m_event || FAILED(engine5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&surface->m_fence)))
            || FAILED(surface->m_engineContext.As(&surface->m_engineContext4))) {
            setLastError("cannot create a D3D11 fence");
            return nullptr;
        }
        return surface;
    }

    ~D3D9Surface()
    {
        destroyAllBuffers();
        releaseHostObjects();
        if (m_event)
            CloseHandle(m_event);
    }

    void* device() const final { return m_host.Get(); }

    void deviceLost() final
    {
        // Before the game's Reset: none of its default-pool objects may remain.
        Locker locker { m_texturesLock };
        for (auto* texture : m_textures) {
            texture->hostTexture = nullptr;
            texture->query = nullptr;
        }
        m_hostDefault = nullptr;
        m_hostSystem = nullptr;
        releaseHostObjects();
    }

private:
    D3D9Surface(ComPtr<ID3D11Device>&& engine, EGLDisplay display, unsigned bufferCount)
        : D3DSurface(SurfaceKind::D3D9, bufferCount, WTF::move(engine), display)
    {
    }

    bool shared() const { return !!m_hostEx; }

    struct Texture {
        EngineTexture engine;
        IntSize size;
        // 9Ex: the legacy shared handle (not closed: it is not an NT handle), the
        // game's side of the texture, and the query issued at its last release.
        HANDLE sharedHandle { nullptr };
        ComPtr<IDirect3DTexture9> hostTexture;
        ComPtr<IDirect3DQuery9> query;
        // Plain D3D9: the frame in memory, and what of it is older than the page.
        ComPtr<ID3D11Texture2D> staging;
        Vector<uint8_t> pixels;
        IntRect stale;
    };

    bool createBuffer(Buffer& buffer, const IntSize& size) final
    {
        auto texture = std::make_unique<Texture>();
        texture->size = size;
        if (!createEngineTexture(texture->engine, buffer, size, DXGI_FORMAT_B8G8R8A8_UNORM, shared() ? D3D11_RESOURCE_MISC_SHARED : 0))
            return false;
        auto destroy = makeScopeExit([&] { destroyTexture(*texture); buffer.skSurface = nullptr; });
        if (shared()) {
            ComPtr<IDXGIResource> resource;
            if (FAILED(texture->engine.texture.As(&resource)) || FAILED(resource->GetSharedHandle(&texture->sharedHandle)))
                return false;
        } else {
            D3D11_TEXTURE2D_DESC description;
            texture->engine.texture->GetDesc(&description);
            description.Usage = D3D11_USAGE_STAGING;
            description.BindFlags = 0;
            description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            description.MiscFlags = 0;
            if (FAILED(m_engine->CreateTexture2D(&description, nullptr, &texture->staging)))
                return false;
            texture->pixels.grow(static_cast<size_t>(size.width()) * size.height() * 4);
            texture->stale = { { }, size };
        }
        destroy.release();
        Locker locker { m_texturesLock };
        m_textures.append(texture.get());
        buffer.backend = texture.release();
        return true;
    }

    void destroyTexture(Texture& texture)
    {
        destroyEngineTexture(texture.engine);
        Locker locker { m_texturesLock };
        m_textures.removeFirst(&texture);
        releaseOnHostThread(WTF::move(texture.hostTexture));
        releaseOnHostThread(WTF::move(texture.query));
        texture.staging = nullptr;
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

    bool willRender(Buffer&) final
    {
        return makeSkiaContextCurrent();
    }

    void didRender(Buffer& buffer) final
    {
        flush(buffer);
        auto& texture = *static_cast<Texture*>(buffer.backend);
        if (shared()) {
            // The game's device cannot wait for ANGLE's: the frame is published
            // once its drawing is done.
            m_engineContext4->Signal(m_fence.Get(), ++m_fenceValue);
            m_engineContext->Flush();
            if (SUCCEEDED(m_fence->SetEventOnCompletion(m_fenceValue, m_event)))
                WaitForSingleObject(m_event, INFINITE);
            return;
        }

        // Read back what this buffer's pixels miss: the frame's damage, and the
        // damage of frames drawn into the other buffers since.
        auto rect = unionRect(texture.stale, buffer.frameDamage);
        rect.intersect({ { }, texture.size });
        {
            Locker locker { m_texturesLock };
            for (auto* other : m_textures) {
                if (other != &texture)
                    other->stale.unite(buffer.frameDamage);
            }
        }
        texture.stale = { };
        if (rect.isEmpty())
            return;
        D3D11_BOX box { static_cast<UINT>(rect.x()), static_cast<UINT>(rect.y()), 0, static_cast<UINT>(rect.maxX()), static_cast<UINT>(rect.maxY()), 1 };
        m_engineContext->CopySubresourceRegion(texture.staging.Get(), 0, box.left, box.top, 0, texture.engine.texture.Get(), 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(m_engineContext->Map(texture.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            texture.stale = { { }, texture.size };
            return;
        }
        size_t stride = static_cast<size_t>(texture.size.width()) * 4;
        for (int y = rect.y(); y < rect.maxY(); ++y)
            memcpy(texture.pixels.mutableSpan().data() + y * stride + rect.x() * 4, static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch + rect.x() * 4, rect.width() * 4);
        m_engineContext->Unmap(texture.staging.Get(), 0);
    }

    void willAcquire(Buffer& buffer) final
    {
        releaseHostObjects();
        auto& texture = *static_cast<Texture*>(buffer.backend);
        if (!shared())
            return;
        Locker locker { m_texturesLock };
        if (!texture.hostTexture) {
            HANDLE handle = texture.sharedHandle;
            m_hostEx->CreateTexture(texture.size.width(), texture.size.height(), 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture.hostTexture, &handle);
        }
    }

    // The plain-D3D9 copy: the pixels changed since the previous acquired frame
    // (all of them for new textures) into the game's texture.
    IDirect3DTexture9* uploadToHost(const Texture& texture, const IntRect& dirty)
    {
        bool fresh = false;
        if (!m_hostDefault || m_hostSize != texture.size) {
            m_hostDefault = nullptr;
            m_hostSystem = nullptr;
            m_hostSize = texture.size;
            if (FAILED(m_host->CreateTexture(m_hostSize.width(), m_hostSize.height(), 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &m_hostSystem, nullptr))
                || FAILED(m_host->CreateTexture(m_hostSize.width(), m_hostSize.height(), 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_hostDefault, nullptr))) {
                m_hostDefault = nullptr;
                m_hostSystem = nullptr;
                return nullptr;
            }
            fresh = true;
        }
        auto rect = fresh ? IntRect { { }, texture.size } : intersection(dirty, { { }, texture.size });
        if (!rect.isEmpty()) {
            RECT lockRect { rect.x(), rect.y(), rect.maxX(), rect.maxY() };
            D3DLOCKED_RECT locked;
            // Locking a system-memory texture marks the rectangle dirty for UpdateTexture.
            if (FAILED(m_hostSystem->LockRect(0, &locked, &lockRect, 0)))
                return nullptr;
            size_t stride = static_cast<size_t>(texture.size.width()) * 4;
            for (int y = 0; y < rect.height(); ++y)
                memcpy(static_cast<uint8_t*>(locked.pBits) + y * locked.Pitch, texture.pixels.span().data() + (rect.y() + y) * stride + rect.x() * 4, rect.width() * 4);
            m_hostSystem->UnlockRect(0);
            m_host->UpdateTexture(m_hostSystem.Get(), m_hostDefault.Get());
        }
        return m_hostDefault.Get();
    }

    void fillFrame(Buffer& buffer, Frame& frame) final
    {
        auto& texture = *static_cast<Texture*>(buffer.backend);
        if (shared()) {
            frame.texture = texture.hostTexture.Get();
            frame.sharedHandle = texture.sharedHandle;
        } else
            frame.texture = uploadToHost(texture, frame.dirty);
    }

    bool didRelease(Buffer& buffer) final
    {
        releaseHostObjects();
        if (!shared())
            return false; // the copy is ordered by D3D9 itself
        auto& texture = *static_cast<Texture*>(buffer.backend);
        Locker locker { m_texturesLock };
        if (!texture.query && FAILED(m_host->CreateQuery(D3DQUERYTYPE_EVENT, &texture.query)))
            return false;
        texture.query->Issue(D3DISSUE_END);
        return true;
    }

    bool releaseFinished(Buffer& buffer) final
    {
        auto* texture = static_cast<Texture*>(buffer.backend);
        if (!texture || !texture->query)
            return true;
        // No flush: the game submits its frames itself. A lost device is done too.
        return texture->query->GetData(nullptr, 0, 0) != S_FALSE;
    }

    ComPtr<IDirect3DDevice9> m_host;
    ComPtr<IDirect3DDevice9Ex> m_hostEx;
    ComPtr<ID3D11DeviceContext> m_engineContext;
    ComPtr<ID3D11DeviceContext4> m_engineContext4;
    ComPtr<ID3D11Fence> m_fence;
    uint64_t m_fenceValue { 0 };
    HANDLE m_event { nullptr };

    Lock m_texturesLock;
    Vector<Texture*> m_textures WTF_GUARDED_BY_LOCK(m_texturesLock);

    // Plain D3D9, the game's thread: the textures frames are copied into.
    ComPtr<IDirect3DTexture9> m_hostSystem;
    ComPtr<IDirect3DTexture9> m_hostDefault;
    IntSize m_hostSize;
};

} // namespace

std::unique_ptr<Surface> createD3D9Surface(void* hostDevice, unsigned bufferCount, bool copy)
{
    if (!hostDevice)
        return nullptr;
    return D3D9Surface::create(static_cast<IDirect3DDevice9*>(hostDevice), bufferCount, copy);
}

} // namespace ZeroG

#endif // OS(WINDOWS)
