// Texture mode on Windows (ZeroG.md, "Render thread separato"): Skia renders through
// ANGLE into D3D11 textures on ANGLE's device; each texture is shared with the
// application's device by an NT handle. A keyed mutex orders the GPU work of the
// two devices both ways: the engine releases key 1 when a frame is ready, the
// application takes key 1 in acquire and gives key 0 back in release.

#include "config.h"

#if OS(WINDOWS)

#define EGL_EGLEXT_PROTOTYPES 1
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "Surface.h"
#include <WebCore/FontRenderOptions.h>
#include <WebCore/GLContext.h>
#include <WebCore/PlatformDisplay.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
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

using Microsoft::WRL::ComPtr;

namespace ZeroG {

using namespace WebCore;

namespace {

class D3D11Surface final : public Surface {
public:
    static std::unique_ptr<D3D11Surface> create(ID3D11Device* hostDevice, unsigned bufferCount)
    {
        auto* display = PlatformDisplay::sharedDisplayIfExists();
        if (!display || !display->skiaGLContext())
            return nullptr;

        // ANGLE's own device, through EGL_EXT_device_query.
        EGLAttrib device = 0;
        EGLAttrib d3dDevice = 0;
        if (!eglQueryDisplayAttribEXT(display->eglDisplay(), EGL_DEVICE_EXT, &device)
            || !eglQueryDeviceAttribEXT(reinterpret_cast<EGLDeviceEXT>(device), EGL_D3D11_DEVICE_ANGLE, &d3dDevice))
            return nullptr;
        ComPtr<ID3D11Device> engine = reinterpret_cast<ID3D11Device*>(d3dDevice);

        // Without a device from the application the engine creates one for it on
        // ANGLE's adapter. ANGLE's device itself is never handed out: its single
        // immediate context belongs to the engine thread.
        ComPtr<ID3D11Device> created;
        if (!hostDevice) {
            ComPtr<IDXGIDevice> dxgiDevice;
            ComPtr<IDXGIAdapter> adapter;
            if (FAILED(engine.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter))
                || FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &created, nullptr, nullptr)))
                return nullptr;
            hostDevice = created.Get();
        }

        ComPtr<ID3D11Device1> host;
        if (FAILED(hostDevice->QueryInterface(IID_PPV_ARGS(&host))))
            return nullptr;

        return std::unique_ptr<D3D11Surface>(new D3D11Surface(WTF::move(host), WTF::move(engine), display->eglDisplay(), bufferCount));
    }

    ~D3D11Surface()
    {
        destroyAllBuffers();
    }

    void* device() const final { return m_host.Get(); }

private:
    D3D11Surface(ComPtr<ID3D11Device1>&& host, ComPtr<ID3D11Device>&& engine, EGLDisplay display, unsigned bufferCount)
        : Surface(SurfaceKind::D3D11, bufferCount)
        , m_host(WTF::move(host))
        , m_engine(WTF::move(engine))
        , m_display(display)
    {
    }

    struct Texture {
        ComPtr<ID3D11Texture2D> engineTexture;
        ComPtr<IDXGIKeyedMutex> engineMutex;
        HANDLE sharedHandle { nullptr };
        ComPtr<ID3D11Texture2D> hostTexture;
        ComPtr<IDXGIKeyedMutex> hostMutex;
        EGLImageKHR image { EGL_NO_IMAGE_KHR };
        GLuint glTexture { 0 };
    };

    static bool makeSkiaContextCurrent()
    {
        auto* context = PlatformDisplay::sharedDisplay().skiaGLContext();
        return context && context->makeContextCurrent();
    }

    bool createBuffer(Buffer& buffer, const IntSize& size) final
    {
        if (!makeSkiaContextCurrent())
            return false;

        auto texture = std::make_unique<Texture>();
        D3D11_TEXTURE2D_DESC description { };
        description.Width = size.width();
        description.Height = size.height();
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        description.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        if (FAILED(m_engine->CreateTexture2D(&description, nullptr, &texture->engineTexture))
            || FAILED(texture->engineTexture.As(&texture->engineMutex)))
            return false;

        ComPtr<IDXGIResource1> resource;
        if (FAILED(texture->engineTexture.As(&resource))
            || FAILED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &texture->sharedHandle)))
            return false;
        auto destroy = makeScopeExit([&] { destroyTexture(*texture); });

        // Opening on the host device needs the same adapter.
        if (FAILED(m_host->OpenSharedResource1(texture->sharedHandle, IID_PPV_ARGS(&texture->hostTexture)))
            || FAILED(texture->hostTexture.As(&texture->hostMutex)))
            return false;

        const EGLint attributes[] = { EGL_NONE };
        texture->image = eglCreateImageKHR(m_display, EGL_NO_CONTEXT, EGL_D3D11_TEXTURE_ANGLE, reinterpret_cast<EGLClientBuffer>(texture->engineTexture.Get()), attributes);
        if (texture->image == EGL_NO_IMAGE_KHR)
            return false;
        glGenTextures(1, &texture->glTexture);
        glBindTexture(GL_TEXTURE_2D, texture->glTexture);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, texture->image);
        glBindTexture(GL_TEXTURE_2D, 0);

        auto* grContext = PlatformDisplay::sharedDisplay().skiaGrContext();
        // Skia caches GL state; it has to know about the binding changed above.
        grContext->resetContext(kTextureBinding_GrGLBackendState);
        GrGLTextureInfo info { GL_TEXTURE_2D, texture->glTexture, GL_RGBA8 };
        auto backendTexture = GrBackendTextures::MakeGL(size.width(), size.height(), skgpu::Mipmapped::kNo, info);
        auto properties = FontRenderOptions::singleton().createSurfaceProps();
        buffer.skSurface = SkSurfaces::WrapBackendTexture(grContext, backendTexture, kTopLeft_GrSurfaceOrigin, 0, kRGBA_8888_SkColorType, SkColorSpace::MakeSRGB(), &properties);
        if (!buffer.skSurface)
            return false;

        destroy.release();
        buffer.backend = texture.release();
        return true;
    }

    sk_sp<SkSurface> createPrivateTarget(const IntSize& size) final
    {
        if (!makeSkiaContextCurrent())
            return nullptr;
        auto info = SkImageInfo::Make(size.width(), size.height(), kRGBA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
        auto properties = FontRenderOptions::singleton().createSurfaceProps();
        return SkSurfaces::RenderTarget(PlatformDisplay::sharedDisplay().skiaGrContext(), skgpu::Budgeted::kNo, info, 0, kTopLeft_GrSurfaceOrigin, &properties);
    }

    void destroyTexture(Texture& texture)
    {
        if (texture.glTexture || texture.image != EGL_NO_IMAGE_KHR) {
            if (makeSkiaContextCurrent()) {
                if (texture.glTexture)
                    glDeleteTextures(1, &texture.glTexture);
                if (texture.image != EGL_NO_IMAGE_KHR)
                    eglDestroyImageKHR(m_display, texture.image);
            }
        }
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
        // A frame nobody acquired still holds key 1 from didRender().
        UINT64 key = buffer.published && !buffer.acquiredSinceRender ? 1 : 0;
        return static_cast<Texture*>(buffer.backend)->engineMutex->AcquireSync(key, INFINITE) == S_OK;
    }

    void didRender(Buffer& buffer) final
    {
        auto* grContext = PlatformDisplay::sharedDisplay().skiaGrContext();
        grContext->flushAndSubmit(buffer.skSurface.get());
        glFlush();
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
        static_cast<Texture*>(buffer.backend)->hostMutex->AcquireSync(1, INFINITE);
    }

    void didRelease(Buffer& buffer) final
    {
        static_cast<Texture*>(buffer.backend)->hostMutex->ReleaseSync(0);
    }

    ComPtr<ID3D11Device1> m_host;
    ComPtr<ID3D11Device> m_engine;
    EGLDisplay m_display;
};

} // namespace

std::unique_ptr<Surface> createD3D11Surface(void* hostDevice, unsigned bufferCount)
{
    return D3D11Surface::create(static_cast<ID3D11Device*>(hostDevice), bufferCount);
}

} // namespace ZeroG

#endif // OS(WINDOWS)
