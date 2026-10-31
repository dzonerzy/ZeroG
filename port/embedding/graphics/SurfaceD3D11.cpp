// Texture mode on Windows (docs/design/graphics-presentation.md, "Separate render thread"): Skia renders through
// ANGLE into D3D11 textures on ANGLE's device (D3DSurface, shared by every Direct3D
// kind). The D3D11 kind shares each texture with the application's device by an
// NT handle; a keyed mutex orders the GPU work of the two devices both ways: the
// engine releases key 1 when a frame is ready, the application takes key 1 in
// acquire and gives key 0 back in release.

#include "config.h"

#if OS(WINDOWS)

#define EGL_EGLEXT_PROTOTYPES 1
#define GL_GLEXT_PROTOTYPES 1
#include "SurfaceD3D.h"

#include <EGL/eglext_angle.h>
#include <GLES2/gl2ext.h>
#include <WebCore/FontRenderOptions.h>
#include <WebCore/GLContext.h>
#include <WebCore/PlatformDisplay.h>
#include <dxgi1_2.h>
#include <wtf/Scope.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkColorSpace.h>
#include <skia/gpu/ganesh/GrBackendSurface.h>
#include <skia/gpu/ganesh/GrDirectContext.h>
#include <skia/gpu/ganesh/SkSurfaceGanesh.h>
#include <skia/gpu/ganesh/gl/GrGLBackendSurface.h>
#include <skia/gpu/ganesh/gl/GrGLTypes.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

#ifndef GL_RGBA8
#define GL_RGBA8 0x8058 // GLES 3; GL_RGBA8_OES in GLES 2
#endif
#ifndef GL_BGRA8_EXT
#define GL_BGRA8_EXT 0x93A1
#endif

using Microsoft::WRL::ComPtr;

namespace ZeroG {

using namespace WebCore;

static thread_local const char* t_lastSurfaceError = "";

const char* lastSurfaceError()
{
    return t_lastSurfaceError;
}

void D3DSurface::setLastError(const char* error)
{
    t_lastSurfaceError = error;
}

bool D3DSurface::angleDevice(ComPtr<ID3D11Device>& device, EGLDisplay& eglDisplay)
{
    auto* display = PlatformDisplay::sharedDisplayIfExists();
    if (!display || !display->skiaGLContext()) {
        setLastError("no GPU (ANGLE over D3D11)");
        return false;
    }
    // ANGLE's own device, through EGL_EXT_device_query. It is never handed to the
    // application: its single immediate context belongs to the engine's threads.
    EGLAttrib eglDevice = 0;
    EGLAttrib d3dDevice = 0;
    if (!eglQueryDisplayAttribEXT(display->eglDisplay(), EGL_DEVICE_EXT, &eglDevice)
        || !eglQueryDeviceAttribEXT(reinterpret_cast<EGLDeviceEXT>(eglDevice), EGL_D3D11_DEVICE_ANGLE, &d3dDevice)) {
        setLastError("ANGLE has no D3D11 device");
        return false;
    }
    device = reinterpret_cast<ID3D11Device*>(d3dDevice);
    eglDisplay = display->eglDisplay();
    return true;
}

uint64_t D3DSurface::adapterLUID(ID3D11Device* device)
{
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&description)))
        return 0;
    return static_cast<uint64_t>(static_cast<uint32_t>(description.AdapterLuid.HighPart)) << 32 | description.AdapterLuid.LowPart;
}

ComPtr<ID3D11DeviceContext> D3DSurface::sharedEngineContext(ID3D11Device* device)
{
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(context.As(&multithread)))
        multithread->SetMultithreadProtected(TRUE);
    return context;
}

D3DSurface::D3DSurface(SurfaceKind kind, unsigned bufferCount, ComPtr<ID3D11Device>&& engine, EGLDisplay display)
    : Surface(kind, bufferCount)
    , m_engine(WTF::move(engine))
    , m_display(display)
{
}

bool D3DSurface::makeSkiaContextCurrent()
{
    auto* context = PlatformDisplay::sharedDisplay().skiaGLContext();
    return context && context->makeContextCurrent();
}

bool D3DSurface::createEngineTexture(EngineTexture& texture, Buffer& buffer, const IntSize& size, DXGI_FORMAT format, UINT miscFlags)
{
    if (!makeSkiaContextCurrent())
        return false;

    D3D11_TEXTURE2D_DESC description { };
    description.Width = size.width();
    description.Height = size.height();
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    description.MiscFlags = miscFlags;
    if (FAILED(m_engine->CreateTexture2D(&description, nullptr, &texture.texture)))
        return false;
    auto destroy = makeScopeExit([&] { destroyEngineTexture(texture); });

    const EGLint attributes[] = { EGL_NONE };
    texture.image = eglCreateImageKHR(m_display, EGL_NO_CONTEXT, EGL_D3D11_TEXTURE_ANGLE, reinterpret_cast<EGLClientBuffer>(texture.texture.Get()), attributes);
    if (texture.image == EGL_NO_IMAGE_KHR)
        return false;
    glGenTextures(1, &texture.glTexture);
    glBindTexture(GL_TEXTURE_2D, texture.glTexture);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, texture.image);
    glBindTexture(GL_TEXTURE_2D, 0);

    auto* grContext = PlatformDisplay::sharedDisplay().skiaGrContext();
    // Skia caches GL state; it has to know about the binding changed above.
    grContext->resetContext(kTextureBinding_GrGLBackendState);
    bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM;
    GrGLTextureInfo info { GL_TEXTURE_2D, texture.glTexture, static_cast<GrGLenum>(bgra ? GL_BGRA8_EXT : GL_RGBA8) };
    auto backendTexture = GrBackendTextures::MakeGL(size.width(), size.height(), skgpu::Mipmapped::kNo, info);
    auto properties = FontRenderOptions::singleton().createSurfaceProps();
    buffer.skSurface = SkSurfaces::WrapBackendTexture(grContext, backendTexture, kTopLeft_GrSurfaceOrigin, 0, bgra ? kBGRA_8888_SkColorType : kRGBA_8888_SkColorType, SkColorSpace::MakeSRGB(), &properties);
    if (!buffer.skSurface)
        return false;
    destroy.release();
    return true;
}

void D3DSurface::destroyEngineTexture(EngineTexture& texture)
{
    if (texture.glTexture || texture.image != EGL_NO_IMAGE_KHR) {
        if (makeSkiaContextCurrent()) {
            if (texture.glTexture)
                glDeleteTextures(1, &texture.glTexture);
            if (texture.image != EGL_NO_IMAGE_KHR)
                eglDestroyImageKHR(m_display, texture.image);
        }
    }
    texture = { };
}

sk_sp<SkSurface> D3DSurface::createPrivateTarget(const IntSize& size)
{
    if (!makeSkiaContextCurrent())
        return nullptr;
    auto info = SkImageInfo::Make(size.width(), size.height(), kRGBA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    auto properties = FontRenderOptions::singleton().createSurfaceProps();
    return SkSurfaces::RenderTarget(PlatformDisplay::sharedDisplay().skiaGrContext(), skgpu::Budgeted::kNo, info, 0, kTopLeft_GrSurfaceOrigin, &properties);
}

void D3DSurface::flush(Buffer& buffer)
{
    PlatformDisplay::sharedDisplay().skiaGrContext()->flushAndSubmit(buffer.skSurface.get());
    glFlush();
}

void D3DSurface::releaseOnHostThread(ComPtr<IUnknown>&& object)
{
    if (!object)
        return;
    Locker locker { m_hostObjectsLock };
    m_releasedHostObjects.append(WTF::move(object));
}

void D3DSurface::releaseHostObjects()
{
    Vector<ComPtr<IUnknown>> objects;
    {
        Locker locker { m_hostObjectsLock };
        objects = std::exchange(m_releasedHostObjects, { });
    }
}

namespace {

class D3D11Surface final : public D3DSurface {
public:
    static std::unique_ptr<D3D11Surface> create(ID3D11Device* hostDevice, unsigned bufferCount)
    {
        ComPtr<ID3D11Device> engine;
        EGLDisplay display;
        if (!angleDevice(engine, display))
            return nullptr;

        // Without a device from the application the engine creates one for it on
        // ANGLE's adapter.
        ComPtr<ID3D11Device> created;
        if (!hostDevice) {
            ComPtr<IDXGIDevice> dxgiDevice;
            ComPtr<IDXGIAdapter> adapter;
            if (FAILED(engine.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter))
                || FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &created, nullptr, nullptr))) {
                setLastError("cannot create a D3D11 device on ANGLE's adapter");
                return nullptr;
            }
            hostDevice = created.Get();
        } else if (adapterLUID(hostDevice) != adapterLUID(engine.Get())) {
            setLastError("the D3D11 device is on another adapter than the engine's (zg_runtime_config.adapter_luid)");
            return nullptr;
        }

        ComPtr<ID3D11Device1> host;
        if (FAILED(hostDevice->QueryInterface(IID_PPV_ARGS(&host)))) {
            setLastError("the D3D11 device has no ID3D11Device1");
            return nullptr;
        }
        return std::unique_ptr<D3D11Surface>(new D3D11Surface(WTF::move(host), WTF::move(engine), display, bufferCount));
    }

    ~D3D11Surface()
    {
        destroyAllBuffers();
    }

    void* device() const final { return m_host.Get(); }

private:
    D3D11Surface(ComPtr<ID3D11Device1>&& host, ComPtr<ID3D11Device>&& engine, EGLDisplay display, unsigned bufferCount)
        : D3DSurface(SurfaceKind::D3D11, bufferCount, WTF::move(engine), display)
        , m_host(WTF::move(host))
    {
    }

    struct Texture {
        EngineTexture engine;
        ComPtr<IDXGIKeyedMutex> engineMutex;
        HANDLE sharedHandle { nullptr };
        ComPtr<ID3D11Texture2D> hostTexture;
        ComPtr<IDXGIKeyedMutex> hostMutex;
    };

    bool createBuffer(Buffer& buffer, const IntSize& size) final
    {
        auto texture = std::make_unique<Texture>();
        if (!createEngineTexture(texture->engine, buffer, size, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX))
            return false;
        auto destroy = makeScopeExit([&] { destroyTexture(*texture); buffer.skSurface = nullptr; });
        ComPtr<IDXGIResource1> resource;
        if (FAILED(texture->engine.texture.As(&texture->engineMutex)) || FAILED(texture->engine.texture.As(&resource))
            || FAILED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &texture->sharedHandle)))
            return false;
        destroy.release();
        buffer.backend = texture.release();
        return true;
    }

    void destroyTexture(Texture& texture)
    {
        destroyEngineTexture(texture.engine);
        if (texture.sharedHandle)
            CloseHandle(texture.sharedHandle);
        releaseOnHostThread(WTF::move(texture.hostTexture));
        releaseOnHostThread(WTF::move(texture.hostMutex));
        texture = { };
    }

    // Application thread: the host device's side of a texture, opened on first
    // use (the device may be single-threaded: only the application's threads
    // touch it). Opening needs the same adapter as ANGLE's device.
    bool openOnHost(Texture& texture)
    {
        releaseHostObjects();
        if (texture.hostMutex)
            return true;
        return SUCCEEDED(m_host->OpenSharedResource1(texture.sharedHandle, IID_PPV_ARGS(&texture.hostTexture)))
            && SUCCEEDED(texture.hostTexture.As(&texture.hostMutex));
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
        // A frame nobody acquired still holds key 1 from didRender().
        UINT64 key = buffer.published && !buffer.acquiredSinceRender ? 1 : 0;
        return static_cast<Texture*>(buffer.backend)->engineMutex->AcquireSync(key, INFINITE) == S_OK;
    }

    void didRender(Buffer& buffer) final
    {
        flush(buffer);
        static_cast<Texture*>(buffer.backend)->engineMutex->ReleaseSync(1);
    }

    void fillFrame(Buffer& buffer, Frame& frame) final
    {
        auto& texture = *static_cast<Texture*>(buffer.backend);
        frame.texture = texture.hostTexture.Get();
        frame.sharedHandle = texture.sharedHandle;
    }

    void willAcquire(Buffer& buffer) final
    {
        auto& texture = *static_cast<Texture*>(buffer.backend);
        if (openOnHost(texture))
            texture.hostMutex->AcquireSync(1, INFINITE);
    }

    bool didRelease(Buffer& buffer) final
    {
        auto& texture = *static_cast<Texture*>(buffer.backend);
        if (texture.hostMutex)
            texture.hostMutex->ReleaseSync(0);
        releaseHostObjects();
        return false;
    }

    ComPtr<ID3D11Device1> m_host;
};

} // namespace

std::unique_ptr<Surface> createD3D11Surface(void* hostDevice, unsigned bufferCount)
{
    return D3D11Surface::create(static_cast<ID3D11Device*>(hostDevice), bufferCount);
}

} // namespace ZeroG

#endif // OS(WINDOWS)
