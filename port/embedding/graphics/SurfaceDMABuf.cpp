// Texture mode for Vulkan, or any API that imports dma-bufs, on Linux (docs/design/
// graphics-presentation.md, "GPU backend per platform"): the engine keeps rendering with Skia over GL,
// on its own EGL display, into GL textures that are dma-bufs, and hands each
// frame over as the dma-buf's planes plus a sync_file to wait on.
//
// A buffer becomes a dma-buf in the first way the system allows, tried in order
// when the surface is created:
// - EGL export (EGL_MESA_image_dma_buf_export): a GL texture's EGLImage gives its
//   dma-buf. GPU drivers (Mesa's, NVIDIA's).
// - GBM (libgbm, loaded at run time) on a render node: a buffer object allocated
//   for rendering, imported into EGL (EGL_EXT_image_dma_buf_import).
// - A DRM device's dumb buffer (linear, CPU-mapped), exported with PRIME and
//   imported into EGL: for software rendering (llvmpipe imports dma-bufs but
//   exports none), on a device such as vgem.
// Fences: with EGL_ANDROID_native_fence_sync the frame's fence is a sync_file and
// the application's release fence is waited on by the GPU; without, the frame is
// complete when published (glFinish) and the release fence is waited on here.

#include "config.h"

#if !OS(WINDOWS)

#include "Surface.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <WebCore/FontRenderOptions.h>
#include <WebCore/GLContext.h>
#include <WebCore/GLDisplay.h>
#include <WebCore/PlatformDisplay.h>
#include <atomic>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/MakeString.h>

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

// DRM_FORMAT_ABGR8888: R, G, B, A bytes in memory, GL_RGBA's order.
constexpr uint32_t formatABGR8888 = 'A' | ('B' << 8) | ('2' << 16) | (static_cast<uint32_t>('4') << 24);
constexpr uint64_t modifierLinear = 0;
constexpr uint64_t modifierInvalid = 0x00ffffffffffffffull;

// DRM's uapi (drm.h, drm_mode.h), which needs no libdrm.
struct DRMCreateDumb {
    uint32_t height;
    uint32_t width;
    uint32_t bpp;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
};
struct DRMPrimeHandle {
    uint32_t handle;
    uint32_t flags;
    int32_t fd;
};
struct DRMGemClose {
    uint32_t handle;
    uint32_t pad;
};
constexpr unsigned long drmCreateDumb = _IOWR('d', 0xB2, DRMCreateDumb);
constexpr unsigned long drmPrimeHandleToFD = _IOWR('d', 0x2D, DRMPrimeHandle);
constexpr unsigned long drmGemClose = _IOW('d', 0x09, DRMGemClose);

// GBM (gbm.h), loaded at run time.
struct GBM {
    void* (*create_device)(int fd);
    void (*device_destroy)(void*);
    void* (*bo_create)(void* device, uint32_t width, uint32_t height, uint32_t format, uint32_t flags);
    void (*bo_destroy)(void*);
    int (*bo_get_plane_count)(void*);
    int (*bo_get_fd_for_plane)(void*, int plane);
    uint32_t (*bo_get_stride_for_plane)(void*, int plane);
    uint32_t (*bo_get_offset)(void*, int plane);
    uint64_t (*bo_get_modifier)(void*);

    static const GBM* get()
    {
        static NeverDestroyed<std::optional<GBM>> gbm = []() -> std::optional<GBM> {
            void* handle = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
            if (!handle)
                return std::nullopt;
            GBM result;
            bool complete = true;
            auto load = [&](auto& function, const char* name) {
                function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(dlsym(handle, name));
                complete &= !!function;
            };
            load(result.create_device, "gbm_create_device");
            load(result.device_destroy, "gbm_device_destroy");
            load(result.bo_create, "gbm_bo_create");
            load(result.bo_destroy, "gbm_bo_destroy");
            load(result.bo_get_plane_count, "gbm_bo_get_plane_count");
            load(result.bo_get_fd_for_plane, "gbm_bo_get_fd_for_plane");
            load(result.bo_get_stride_for_plane, "gbm_bo_get_stride_for_plane");
            load(result.bo_get_offset, "gbm_bo_get_offset");
            load(result.bo_get_modifier, "gbm_bo_get_modifier");
            if (!complete)
                return std::nullopt;
            return result;
        }();
        return gbm->has_value() ? &gbm->value() : nullptr;
    }
};
constexpr uint32_t gbmUseRendering = 1 << 2;

enum class Allocator : uint8_t { Export, GBM, Dumb };

// The process's DRM devices for the allocators that need one, opened once.
struct Devices {
    int renderNode { -1 }; // GBM's
    void* gbmDevice { nullptr };
    int primaryNode { -1 }; // dumb buffers'

    static Devices& singleton()
    {
        static NeverDestroyed<Devices> devices;
        return devices;
    }

    void* gbm()
    {
        auto* library = GBM::get();
        if (gbmDevice || !library)
            return gbmDevice;
        for (int i = 128; i < 136 && !gbmDevice; ++i) {
            int fd = open(byteCast<char>(makeString("/dev/dri/renderD"_s, i).utf8().data()), O_RDWR | O_CLOEXEC);
            if (fd < 0)
                continue;
            if (auto* device = library->create_device(fd)) {
                renderNode = fd;
                gbmDevice = device;
            } else
                close(fd);
        }
        return gbmDevice;
    }

    // The first DRM device that makes dumb buffers.
    int dumb()
    {
        if (primaryNode >= 0)
            return primaryNode;
        for (int i = 0; i < 16 && primaryNode < 0; ++i) {
            int fd = open(byteCast<char>(makeString("/dev/dri/card"_s, i).utf8().data()), O_RDWR | O_CLOEXEC);
            if (fd < 0)
                continue;
            DRMCreateDumb request { 16, 16, 32, 0, 0, 0, 0 };
            if (!ioctl(fd, drmCreateDumb, &request)) {
                DRMGemClose closing { request.handle, 0 };
                ioctl(fd, drmGemClose, &closing);
                primaryNode = fd;
            } else
                close(fd);
        }
        return primaryNode;
    }
};

static std::atomic<uint64_t> s_lastBufferID;

class DMABufSurface final : public Surface {
public:
    static std::unique_ptr<DMABufSurface> create(unsigned bufferCount)
    {
        auto* display = PlatformDisplay::sharedDisplayIfExists();
        if (!display || !display->skiaGLContext() || !display->skiaGLContext()->makeContextCurrent())
            return nullptr;
        auto& extensions = display->eglExtensions();
        auto imageTargetTexture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        std::unique_ptr<DMABufSurface> surface(new DMABufSurface(display->eglDisplay(), bufferCount));
        surface->m_imageTargetTexture = imageTargetTexture;
        if (extensions.ANDROID_native_fence_sync)
            surface->m_dupNativeFence = reinterpret_cast<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(eglGetProcAddress("eglDupNativeFenceFDANDROID"));
        surface->m_importModifiers = extensions.EXT_image_dma_buf_import_modifiers;

        // The first allocator that gives a buffer the driver renders into.
        Vector<Allocator> allocators;
        if (extensions.MESA_image_dma_buf_export) {
            surface->m_exportQuery = reinterpret_cast<PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC>(eglGetProcAddress("eglExportDMABUFImageQueryMESA"));
            surface->m_export = reinterpret_cast<PFNEGLEXPORTDMABUFIMAGEMESAPROC>(eglGetProcAddress("eglExportDMABUFImageMESA"));
            if (surface->m_exportQuery && surface->m_export)
                allocators.append(Allocator::Export);
        }
        if (extensions.EXT_image_dma_buf_import && imageTargetTexture) {
            allocators.append(Allocator::GBM);
            allocators.append(Allocator::Dumb);
        }
        for (auto allocator : allocators) {
            surface->m_allocator = allocator;
            Buffer probe;
            if (surface->createBuffer(probe, { 16, 16 })) {
                surface->destroyBuffer(probe);
                return surface;
            }
        }
        return nullptr;
    }

    ~DMABufSurface()
    {
        destroyAllBuffers();
    }

    bool dmabuf(const Frame& frame, DMABufFrame& out) const final
    {
        auto* buffer = static_cast<Buffer*>(frame.handle);
        if (!buffer || !buffer->backend)
            return false;
        auto& texture = *static_cast<Texture*>(buffer->backend);
        out = texture.planes;
        out.readyFence = texture.readyFence;
        return true;
    }

private:
    DMABufSurface(EGLDisplay display, unsigned bufferCount)
        : Surface(SurfaceKind::DMABuf, bufferCount)
        , m_display(display)
    {
    }

    struct Texture {
        GLuint glTexture { 0 };
        EGLImage image { EGL_NO_IMAGE };
        void* gbmBuffer { nullptr };
        DMABufFrame planes;
        int readyFence { -1 }; // the last frame's, until the buffer is drawn again
        std::atomic<int> releaseFence { -1 }; // from the application
    };

    static GLContext* skiaContext()
    {
        auto* context = PlatformDisplay::sharedDisplay().skiaGLContext();
        return context && context->makeContextCurrent() ? context : nullptr;
    }

    // A GL texture for the allocator's dma-buf; its planes in `texture`.
    bool allocate(Texture& texture, const IntSize& size, GLContext& context)
    {
        auto& planes = texture.planes;
        switch (m_allocator) {
        case Allocator::Export: {
            glGenTextures(1, &texture.glTexture);
            glBindTexture(GL_TEXTURE_2D, texture.glTexture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size.width(), size.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            texture.image = PlatformDisplay::sharedDisplay().createEGLImage(static_cast<EGLContext>(context.platformContext()), EGL_GL_TEXTURE_2D,
                reinterpret_cast<EGLClientBuffer>(static_cast<uintptr_t>(texture.glTexture)), { });
            if (texture.image == EGL_NO_IMAGE)
                return false;
            int fourcc = 0, planeCount = 0;
            EGLuint64KHR modifier = modifierInvalid;
            if (!m_exportQuery(m_display, texture.image, &fourcc, &planeCount, &modifier) || planeCount < 1 || planeCount > 4)
                return false;
            EGLint strides[4] { }, offsets[4] { };
            if (!m_export(m_display, texture.image, planes.fds, strides, offsets))
                return false;
            planes.fourcc = fourcc;
            planes.modifier = modifier;
            planes.planeCount = planeCount;
            for (int i = 0; i < planeCount; ++i) {
                planes.strides[i] = strides[i];
                planes.offsets[i] = offsets[i];
            }
            return true;
        }
        case Allocator::GBM: {
            auto* library = GBM::get();
            auto* device = Devices::singleton().gbm();
            if (!library || !device)
                return false;
            texture.gbmBuffer = library->bo_create(device, size.width(), size.height(), formatABGR8888, gbmUseRendering);
            if (!texture.gbmBuffer)
                return false;
            int planeCount = library->bo_get_plane_count(texture.gbmBuffer);
            if (planeCount < 1 || planeCount > 4)
                return false;
            planes.fourcc = formatABGR8888;
            planes.modifier = library->bo_get_modifier(texture.gbmBuffer);
            planes.planeCount = planeCount;
            for (int i = 0; i < planeCount; ++i) {
                planes.fds[i] = library->bo_get_fd_for_plane(texture.gbmBuffer, i);
                planes.strides[i] = library->bo_get_stride_for_plane(texture.gbmBuffer, i);
                planes.offsets[i] = library->bo_get_offset(texture.gbmBuffer, i);
                if (planes.fds[i] < 0)
                    return false;
            }
            return import(texture, size);
        }
        case Allocator::Dumb: {
            int device = Devices::singleton().dumb();
            if (device < 0)
                return false;
            DRMCreateDumb request { static_cast<uint32_t>(size.height()), static_cast<uint32_t>(size.width()), 32, 0, 0, 0, 0 };
            if (ioctl(device, drmCreateDumb, &request))
                return false;
            DRMPrimeHandle prime { request.handle, O_CLOEXEC | O_RDWR, -1 };
            bool exported = !ioctl(device, drmPrimeHandleToFD, &prime);
            // The dma-buf keeps the buffer alive.
            DRMGemClose closing { request.handle, 0 };
            ioctl(device, drmGemClose, &closing);
            if (!exported)
                return false;
            planes.fourcc = formatABGR8888;
            planes.modifier = modifierLinear;
            planes.planeCount = 1;
            planes.fds[0] = prime.fd;
            planes.strides[0] = request.pitch;
            planes.offsets[0] = 0;
            return import(texture, size);
        }
        }
        return false;
    }

    // The dma-buf into EGL, then a GL texture of it.
    bool import(Texture& texture, const IntSize& size)
    {
        auto& planes = texture.planes;
        static constexpr EGLAttrib fdAttribute[] = { EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT };
        static constexpr EGLAttrib offsetAttribute[] = { EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT };
        static constexpr EGLAttrib pitchAttribute[] = { EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT };
        static constexpr EGLAttrib modifierLowAttribute[] = { EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT };
        static constexpr EGLAttrib modifierHighAttribute[] = { EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT };
        Vector<EGLAttrib> attributes { EGL_WIDTH, size.width(), EGL_HEIGHT, size.height(), EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLAttrib>(planes.fourcc) };
        bool withModifier = m_importModifiers && planes.modifier != modifierInvalid;
        for (unsigned i = 0; i < planes.planeCount; ++i) {
            attributes.appendList({ fdAttribute[i], static_cast<EGLAttrib>(planes.fds[i]), offsetAttribute[i], static_cast<EGLAttrib>(planes.offsets[i]), pitchAttribute[i], static_cast<EGLAttrib>(planes.strides[i]) });
            if (withModifier)
                attributes.appendList({ modifierLowAttribute[i], static_cast<EGLAttrib>(planes.modifier & 0xffffffff), modifierHighAttribute[i], static_cast<EGLAttrib>(planes.modifier >> 32) });
        }
        attributes.append(EGL_NONE);
        texture.image = PlatformDisplay::sharedDisplay().createEGLImage(EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes);
        if (texture.image == EGL_NO_IMAGE)
            return false;
        glGenTextures(1, &texture.glTexture);
        glBindTexture(GL_TEXTURE_2D, texture.glTexture);
        m_imageTargetTexture(GL_TEXTURE_2D, static_cast<GLeglImageOES>(texture.image));
        return glGetError() == GL_NO_ERROR;
    }

    bool createBuffer(Buffer& buffer, const IntSize& size) final
    {
        auto* context = skiaContext();
        if (!context)
            return false;
        auto texture = std::make_unique<Texture>();
        texture->planes.bufferID = ++s_lastBufferID;
        bool allocated = allocate(*texture, size, *context);
        if (texture->glTexture) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        buffer.backend = texture.release();
        if (!allocated) {
            destroyBuffer(buffer);
            return false;
        }

        auto& glTexture = *static_cast<Texture*>(buffer.backend);
        auto* grContext = PlatformDisplay::sharedDisplay().skiaGrContext();
        // Skia caches GL state; it has to know about the binding changed above.
        grContext->resetContext(kTextureBinding_GrGLBackendState);
        GrGLTextureInfo info { GL_TEXTURE_2D, glTexture.glTexture, GL_RGBA8 };
        auto backendTexture = GrBackendTextures::MakeGL(size.width(), size.height(), skgpu::Mipmapped::kNo, info);
        auto properties = FontRenderOptions::singleton().createSurfaceProps();
        buffer.skSurface = SkSurfaces::WrapBackendTexture(grContext, backendTexture, kTopLeft_GrSurfaceOrigin, 0, kRGBA_8888_SkColorType, SkColorSpace::MakeSRGB(), &properties);
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
        if (texture->glTexture && skiaContext())
            glDeleteTextures(1, &texture->glTexture);
        if (texture->image != EGL_NO_IMAGE)
            PlatformDisplay::sharedDisplay().destroyEGLImage(texture->image);
        if (texture->gbmBuffer)
            GBM::get()->bo_destroy(texture->gbmBuffer);
        for (int fd : texture->planes.fds) {
            if (fd >= 0)
                close(fd);
        }
        if (texture->readyFence >= 0)
            close(texture->readyFence);
        if (int fence = texture->releaseFence.exchange(-1); fence >= 0)
            close(fence);
        delete texture;
    }

    bool willRender(Buffer& buffer) final
    {
        if (!skiaContext())
            return false;
        auto& texture = *static_cast<Texture*>(buffer.backend);
        // The application's reads of this buffer come first: on the GPU with
        // native fences, here without.
        if (int fence = texture.releaseFence.exchange(-1); fence >= 0) {
            EGLSync sync = EGL_NO_SYNC;
            if (m_dupNativeFence) {
                EGLAttrib attributes[] = { EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fence, EGL_NONE };
                sync = eglCreateSync(m_display, EGL_SYNC_NATIVE_FENCE_ANDROID, attributes);
            }
            if (sync != EGL_NO_SYNC) {
                // EGL owns the descriptor now.
                eglWaitSync(m_display, sync, 0);
                eglDestroySync(m_display, sync);
            } else {
                pollfd descriptor { fence, POLLIN, 0 };
                poll(&descriptor, 1, 1000);
                close(fence);
            }
        }
        if (texture.readyFence >= 0) {
            close(texture.readyFence);
            texture.readyFence = -1;
        }
        return true;
    }

    void didRender(Buffer& buffer) final
    {
        auto& texture = *static_cast<Texture*>(buffer.backend);
        PlatformDisplay::sharedDisplay().skiaGrContext()->flushAndSubmit(buffer.skSurface.get());
        if (m_dupNativeFence) {
            EGLAttrib attributes[] = { EGL_SYNC_NATIVE_FENCE_FD_ANDROID, EGL_NO_NATIVE_FENCE_FD_ANDROID, EGL_NONE };
            if (auto sync = eglCreateSync(m_display, EGL_SYNC_NATIVE_FENCE_ANDROID, attributes); sync != EGL_NO_SYNC) {
                glFlush();
                texture.readyFence = m_dupNativeFence(m_display, sync);
                eglDestroySync(m_display, sync);
                if (texture.readyFence >= 0)
                    return;
            }
        }
        // No fence to hand over: the frame is complete when published.
        glFinish();
    }

    void fillFrame(Buffer&, Frame&) final
    {
        // Frames are read through dmabuf().
    }

    void takeReleaseFence(Buffer& buffer, int fence) final
    {
        if (int previous = static_cast<Texture*>(buffer.backend)->releaseFence.exchange(fence); previous >= 0)
            close(previous);
    }

    EGLDisplay m_display;
    Allocator m_allocator { Allocator::Export };
    bool m_importModifiers { false };
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC m_imageTargetTexture { nullptr };
    PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC m_exportQuery { nullptr };
    PFNEGLEXPORTDMABUFIMAGEMESAPROC m_export { nullptr };
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC m_dupNativeFence { nullptr };
};

} // namespace

std::unique_ptr<Surface> createDMABufSurface(unsigned bufferCount)
{
    return DMABufSurface::create(bufferCount);
}

} // namespace ZeroG

#endif // !OS(WINDOWS)
