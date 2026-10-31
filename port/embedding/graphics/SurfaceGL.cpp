// Texture mode on Linux (docs/design/graphics-presentation.md, "Separate render thread"): Skia renders into GL
// textures in the engine's context, on the application's EGLDisplay, and each
// texture is handed over as an EGLImage the application binds in its own context
// (glEGLImageTargetTexture2DOES). EGL fences order the GPU work both ways: the
// frame's fence, which the application waits on (eglWaitSync) before sampling, and
// one created when the application releases the frame, which the engine waits on
// before drawing into that texture again.
//
// The same surface without the EGLImages serves window mode on every OS: frames
// are textures in the engine's GL share group, which the window's context blits.

#include "config.h"
#include "Surface.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <WebCore/FontRenderOptions.h>
#include <WebCore/GLContext.h>
#include <WebCore/PlatformDisplay.h>
#include <atomic>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkColorSpace.h>
#include <skia/gpu/ganesh/GrBackendSurface.h>
#include <skia/gpu/ganesh/GrDirectContext.h>
#include <skia/gpu/ganesh/SkSurfaceGanesh.h>
#include <skia/gpu/ganesh/gl/GrGLBackendSurface.h>
#include <skia/gpu/ganesh/gl/GrGLTypes.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif

namespace ZeroG {

using namespace WebCore;

namespace {

class GLSurface final : public Surface {
public:
    // `hostDisplay`: export EGLImages on that display (it must be the engine's).
    // Null: textures for the engine's own share group.
    static std::unique_ptr<GLSurface> create(EGLDisplay hostDisplay, unsigned bufferCount)
    {
        auto* display = PlatformDisplay::sharedDisplayIfExists();
        if (!display || (hostDisplay && display->eglDisplay() != hostDisplay) || !display->skiaGLContext())
            return nullptr;
        return std::unique_ptr<GLSurface>(new GLSurface(display->eglDisplay(), !!hostDisplay, bufferCount));
    }

    ~GLSurface()
    {
        destroyAllBuffers();
    }

private:
    GLSurface(EGLDisplay display, bool exportImages, unsigned bufferCount)
        : Surface(exportImages ? SurfaceKind::GL : SurfaceKind::GPU, bufferCount)
        , m_display(display)
        , m_exportImages(exportImages)
    {
    }

    struct Texture {
        GLuint glTexture { 0 };
        EGLImage image { EGL_NO_IMAGE };
        EGLSync readyFence { EGL_NO_SYNC }; // signaled when the frame is drawn
        std::atomic<EGLSync> releaseFence { EGL_NO_SYNC }; // from the application
    };

    static GLContext* skiaContext()
    {
        auto* context = PlatformDisplay::sharedDisplay().skiaGLContext();
        return context && context->makeContextCurrent() ? context : nullptr;
    }

    bool createBuffer(Buffer& buffer, const IntSize& size) final
    {
        auto* context = skiaContext();
        if (!context)
            return false;

        auto texture = std::make_unique<Texture>();
        glGenTextures(1, &texture->glTexture);
        glBindTexture(GL_TEXTURE_2D, texture->glTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size.width(), size.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);

        if (m_exportImages) {
            texture->image = PlatformDisplay::sharedDisplay().createEGLImage(static_cast<EGLContext>(context->platformContext()), EGL_GL_TEXTURE_2D,
                reinterpret_cast<EGLClientBuffer>(static_cast<uintptr_t>(texture->glTexture)), { });
            if (texture->image == EGL_NO_IMAGE) {
                glDeleteTextures(1, &texture->glTexture);
                return false;
            }
        }

        auto* grContext = PlatformDisplay::sharedDisplay().skiaGrContext();
        // Skia caches GL state; it has to know about the binding changed above.
        grContext->resetContext(kTextureBinding_GrGLBackendState);
        GrGLTextureInfo info { GL_TEXTURE_2D, texture->glTexture, GL_RGBA8 };
        auto backendTexture = GrBackendTextures::MakeGL(size.width(), size.height(), skgpu::Mipmapped::kNo, info);
        auto properties = FontRenderOptions::singleton().createSurfaceProps();
        buffer.skSurface = SkSurfaces::WrapBackendTexture(grContext, backendTexture, kTopLeft_GrSurfaceOrigin, 0, kRGBA_8888_SkColorType, SkColorSpace::MakeSRGB(), &properties);
        buffer.backend = texture.release();
        if (!buffer.skSurface) {
            destroyBuffer(buffer);
            return false;
        }
        return true;
    }

    sk_sp<SkSurface> createPrivateTarget(const IntSize& size) final
    {
        if (!skiaContext())
            return nullptr;
        auto info = SkImageInfo::Make(size.width(), size.height(), kRGBA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
        auto properties = FontRenderOptions::singleton().createSurfaceProps();
        return SkSurfaces::RenderTarget(PlatformDisplay::sharedDisplay().skiaGrContext(), skgpu::Budgeted::kNo, info, 0, kTopLeft_GrSurfaceOrigin, &properties);
    }

    void destroyBuffer(Buffer& buffer) final
    {
        buffer.skSurface = nullptr;
        auto* texture = static_cast<Texture*>(std::exchange(buffer.backend, nullptr));
        if (!texture)
            return;
        if (skiaContext()) {
            if (texture->glTexture)
                glDeleteTextures(1, &texture->glTexture);
        }
        if (texture->image != EGL_NO_IMAGE)
            PlatformDisplay::sharedDisplay().destroyEGLImage(texture->image);
        if (texture->readyFence != EGL_NO_SYNC)
            eglDestroySync(m_display, texture->readyFence);
        if (auto fence = texture->releaseFence.exchange(EGL_NO_SYNC); fence != EGL_NO_SYNC)
            eglDestroySync(m_display, fence);
        delete texture;
    }

    bool willRender(Buffer& buffer) final
    {
        if (!skiaContext())
            return false;
        auto& texture = *static_cast<Texture*>(buffer.backend);
        // The application's last reads of this texture come first, on the GPU.
        if (auto fence = texture.releaseFence.exchange(EGL_NO_SYNC); fence != EGL_NO_SYNC) {
            eglWaitSync(m_display, fence, 0);
            eglDestroySync(m_display, fence);
        }
        if (texture.readyFence != EGL_NO_SYNC) {
            eglDestroySync(m_display, texture.readyFence);
            texture.readyFence = EGL_NO_SYNC;
        }
        return true;
    }

    void didRender(Buffer& buffer) final
    {
        auto& texture = *static_cast<Texture*>(buffer.backend);
        PlatformDisplay::sharedDisplay().skiaGrContext()->flushAndSubmit(buffer.skSurface.get());
        texture.readyFence = eglCreateSync(m_display, EGL_SYNC_FENCE, nullptr);
        glFlush();
    }

    void fillFrame(Buffer& buffer, Frame& frame) final
    {
        auto& texture = *static_cast<Texture*>(buffer.backend);
        frame.texture = m_exportImages ? texture.image : reinterpret_cast<void*>(static_cast<uintptr_t>(texture.glTexture));
        frame.fence = texture.readyFence;
    }

    // Application thread, with the context that used the texture current.
    bool didRelease(Buffer& buffer) final
    {
        if (eglGetCurrentContext() == EGL_NO_CONTEXT)
            return false;
        auto fence = eglCreateSync(m_display, EGL_SYNC_FENCE, nullptr);
        glFlush();
        if (auto previous = static_cast<Texture*>(buffer.backend)->releaseFence.exchange(fence); previous != EGL_NO_SYNC)
            eglDestroySync(m_display, previous);
        return false;
    }

    EGLDisplay m_display;
    bool m_exportImages;
};

} // namespace

#if !OS(WINDOWS)
std::unique_ptr<Surface> createGLSurface(void* hostDisplay, unsigned bufferCount)
{
    if (!hostDisplay)
        return nullptr;
    return GLSurface::create(static_cast<EGLDisplay>(hostDisplay), bufferCount);
}
#endif

std::unique_ptr<Surface> createGPUSurface(unsigned bufferCount)
{
    return GLSurface::create(EGL_NO_DISPLAY, bufferCount);
}

} // namespace ZeroG
