// Texture mode with dma-bufs on Linux (ZeroG.md, "Backend GPU per piattaforma"),
// the frames a Vulkan application imports: the engine renders the page on its
// own GPU display into dma-bufs; the application's render thread imports each
// acquired frame's planes (here into a GL context of its own, with
// EGL_EXT_image_dma_buf_import, as Vulkan would with
// VK_EXT_external_memory_dma_buf), waits on its sync_file, reads it and releases
// it, while the engine keeps rendering. Every frame read must be whole, the
// last one must match a CPU render of the page, and a buffer keeps its
// descriptors from frame to frame. Where Vulkan has VK_EXT_external_memory_dma_buf
// (Mesa's lavapipe in Docker) one frame is also imported into Vulkan memory and
// its bytes compared with GL's; elsewhere that part is skipped with a note.
//
// Usage: texture_dmabuf   (in Docker: docker run --device /dev/dri, for vgem's
//        dumb buffers when the driver, llvmpipe, exports no dma-buf itself)

#include "config.h"
#include "Embedding.h"
#include "View.h"
#include "graphics/Surface.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <WebCore/ColorSpace.h>
#include <WebCore/ImageBuffer.h>
#include <WebCore/Page.h>
#include <WebCore/PixelBuffer.h>
#include <WebCore/PixelBufferFormat.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <optional>
#include <poll.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include <wtf/text/WTFString.h>

using namespace WebCore;

namespace {

constexpr int width = 400;
constexpr int height = 200;
constexpr int frames = 60;

constexpr auto page = R"HTML(<!DOCTYPE html>
<html><head><style>
  html, body { margin: 0; background: rgb(240, 240, 255); }
  #box { position: absolute; top: 20px; left: 0; width: 50px; height: 50px; background: rgb(255, 0, 0); }
  p { position: absolute; top: 120px; left: 10px; margin: 0; font: 24px sans-serif; }
</style></head><body>
  <div id="box"></div>
  <p>dma-buf frames</p>
  <script>
    let start;
    function step(t) {
      if (start === undefined)
        start = t;
      document.getElementById("box").style.left = (Math.round((t - start) / 16) * 5) % 350 + "px";
      requestAnimationFrame(step);
    }
    requestAnimationFrame(step);
  </script>
</body></html>)HTML";

#define check(condition, ...) [&] {                    \
        bool result = (condition);                     \
        std::printf("%s ", result ? "ok  " : "FAIL");  \
        std::printf(__VA_ARGS__);                      \
        std::printf("\n");                             \
        return result;                                 \
    }()

struct Readback {
    std::vector<uint8_t> pixels; // RGBA8, rows top to bottom
    unsigned width { 0 };
    unsigned height { 0 };
};

// The application's side: a context of its own, importing dma-bufs.
struct Importer {
    EGLDisplay display { EGL_NO_DISPLAY };
    EGLContext context { EGL_NO_CONTEXT };
    EGLSurface surface { EGL_NO_SURFACE };
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTargetTexture { nullptr };
    struct Import {
        EGLImage image { EGL_NO_IMAGE };
        GLuint texture { 0 };
        ZeroG::DMABufFrame planes;
    };
    std::map<uint64_t, Import> imports; // by buffer ID
    GLuint framebuffer { 0 };
    bool descriptorsChanged { false };

    // Render thread.
    bool makeCurrent()
    {
        auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        if (getPlatformDisplay)
            display = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        if (display == EGL_NO_DISPLAY)
            display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr))
            return false;
        const EGLint configAttributes[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
        EGLConfig config;
        EGLint count = 0;
        if (!eglBindAPI(EGL_OPENGL_ES_API) || !eglChooseConfig(display, configAttributes, &config, 1, &count) || !count)
            return false;
        const EGLint contextAttributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
        const EGLint surfaceAttributes[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
        surface = eglCreatePbufferSurface(display, config, surfaceAttributes);
        if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, context))
            return false;
        imageTargetTexture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        glGenFramebuffers(1, &framebuffer);
        return imageTargetTexture;
    }

    // The buffer's texture, imported the first time it comes.
    GLuint texture(const ZeroG::DMABufFrame& planes, unsigned frameWidth, unsigned frameHeight)
    {
        auto& entry = imports[planes.bufferID];
        if (entry.texture) {
            // The same buffer comes back with the same descriptors.
            descriptorsChanged |= entry.planes.fds[0] != planes.fds[0] || entry.planes.strides[0] != planes.strides[0];
            return entry.texture;
        }
        entry.planes = planes;
        static constexpr EGLAttrib fdAttribute[] = { EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT };
        static constexpr EGLAttrib offsetAttribute[] = { EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT };
        static constexpr EGLAttrib pitchAttribute[] = { EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT };
        std::vector<EGLAttrib> attributes { EGL_WIDTH, static_cast<EGLAttrib>(frameWidth), EGL_HEIGHT, static_cast<EGLAttrib>(frameHeight), EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLAttrib>(planes.fourcc) };
        for (unsigned i = 0; i < planes.planeCount; ++i) {
            attributes.insert(attributes.end(), { fdAttribute[i], planes.fds[i], offsetAttribute[i], static_cast<EGLAttrib>(planes.offsets[i]), pitchAttribute[i], static_cast<EGLAttrib>(planes.strides[i]) });
        }
        attributes.push_back(EGL_NONE);
        entry.image = eglCreateImage(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes.data());
        if (entry.image == EGL_NO_IMAGE)
            return 0;
        glGenTextures(1, &entry.texture);
        glBindTexture(GL_TEXTURE_2D, entry.texture);
        imageTargetTexture(GL_TEXTURE_2D, static_cast<GLeglImageOES>(entry.image));
        return entry.texture;
    }

    bool readFrame(const ZeroG::Frame& frame, const ZeroG::DMABufFrame& planes, Readback& out)
    {
        // The frame's sync_file, as a Vulkan application would import it.
        if (planes.readyFence >= 0) {
            pollfd descriptor { planes.readyFence, POLLIN, 0 };
            if (poll(&descriptor, 1, 2000) != 1)
                return false;
        }
        auto glTexture = texture(planes, frame.width, frame.height);
        if (!glTexture)
            return false;
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glTexture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            return false;
        out.width = frame.width;
        out.height = frame.height;
        out.pixels.resize(out.width * out.height * 4);
        glReadPixels(0, 0, out.width, out.height, GL_RGBA, GL_UNSIGNED_BYTE, out.pixels.data());
        // Done reading before the engine draws into the buffer again.
        glFinish();
        return glGetError() == GL_NO_ERROR;
    }
};

// Vulkan, as much of it as importing a dma-buf into memory and mapping it takes:
// the types and values of Vulkan's ABI (vulkan_core.h), loaded at run time.
namespace vk {

using Result = int32_t;
using Flags = uint32_t;
struct InstanceT;
struct PhysicalDeviceT;
struct DeviceT;
struct DeviceMemoryT;
using Instance = InstanceT*;
using PhysicalDevice = PhysicalDeviceT*;
using Device = DeviceT*;
using DeviceMemory = DeviceMemoryT*;
using VoidFunction = void (*)();

constexpr int32_t structureApplicationInfo = 0;
constexpr int32_t structureInstanceCreateInfo = 1;
constexpr int32_t structureDeviceQueueCreateInfo = 2;
constexpr int32_t structureDeviceCreateInfo = 3;
constexpr int32_t structureMemoryAllocateInfo = 5;
constexpr int32_t structureImportMemoryFdInfo = 1000074000;
constexpr int32_t structureMemoryFdProperties = 1000074001;
constexpr uint32_t handleTypeDMABuf = 0x00000200;
constexpr Flags memoryHostVisible = 0x2;
constexpr uint32_t apiVersion11 = (1u << 22) | (1u << 12);
constexpr uint64_t wholeSize = ~0ull;

struct ApplicationInfo {
    int32_t sType;
    const void* pNext;
    const char* pApplicationName;
    uint32_t applicationVersion;
    const char* pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
};
struct InstanceCreateInfo {
    int32_t sType;
    const void* pNext;
    Flags flags;
    const ApplicationInfo* pApplicationInfo;
    uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
};
struct DeviceQueueCreateInfo {
    int32_t sType;
    const void* pNext;
    Flags flags;
    uint32_t queueFamilyIndex;
    uint32_t queueCount;
    const float* pQueuePriorities;
};
struct DeviceCreateInfo {
    int32_t sType;
    const void* pNext;
    Flags flags;
    uint32_t queueCreateInfoCount;
    const DeviceQueueCreateInfo* pQueueCreateInfos;
    uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
    const void* pEnabledFeatures;
};
struct MemoryAllocateInfo {
    int32_t sType;
    const void* pNext;
    uint64_t allocationSize;
    uint32_t memoryTypeIndex;
};
struct ImportMemoryFdInfo {
    int32_t sType;
    const void* pNext;
    uint32_t handleType;
    int fd;
};
struct MemoryFdProperties {
    int32_t sType;
    void* pNext;
    uint32_t memoryTypeBits;
};
struct MemoryType {
    Flags propertyFlags;
    uint32_t heapIndex;
};
struct MemoryHeap {
    uint64_t size;
    Flags flags;
};
struct PhysicalDeviceMemoryProperties {
    uint32_t memoryTypeCount;
    MemoryType memoryTypes[32];
    uint32_t memoryHeapCount;
    MemoryHeap memoryHeaps[16];
};

// The frame's first plane, imported into Vulkan memory, mapped, and read as
// tight RGBA rows; nullopt with `note` saying why not.
std::optional<std::vector<uint8_t>> read(const ZeroG::DMABufFrame& planes, unsigned width, unsigned height, std::string& note)
{
    // Mapped bytes are rows only in a linear buffer (a GPU's tiled one needs a
    // VkImage with its modifier, VK_EXT_image_drm_format_modifier).
    if (planes.modifier || planes.planeCount != 1) {
        note = "the buffer is not linear";
        return std::nullopt;
    }
    void* library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        note = "no libvulkan.so.1";
        return std::nullopt;
    }
    auto getInstanceProcAddress = reinterpret_cast<VoidFunction (*)(Instance, const char*)>(dlsym(library, "vkGetInstanceProcAddr"));
    auto instanceFunction = [&](Instance instance, const char* name) {
        return getInstanceProcAddress ? getInstanceProcAddress(instance, name) : nullptr;
    };
    auto createInstance = reinterpret_cast<Result (*)(const InstanceCreateInfo*, const void*, Instance*)>(instanceFunction(nullptr, "vkCreateInstance"));
    ApplicationInfo application { structureApplicationInfo, nullptr, "ZeroG texture_dmabuf", 1, "ZeroG", 1, apiVersion11 };
    InstanceCreateInfo instanceInfo { structureInstanceCreateInfo, nullptr, 0, &application, 0, nullptr, 0, nullptr };
    Instance instance = nullptr;
    if (!createInstance || createInstance(&instanceInfo, nullptr, &instance) || !instance) {
        note = "no Vulkan instance";
        return std::nullopt;
    }
    auto destroyInstance = reinterpret_cast<void (*)(Instance, const void*)>(instanceFunction(instance, "vkDestroyInstance"));
    auto enumerateDevices = reinterpret_cast<Result (*)(Instance, uint32_t*, PhysicalDevice*)>(instanceFunction(instance, "vkEnumeratePhysicalDevices"));
    auto memoryProperties = reinterpret_cast<void (*)(PhysicalDevice, PhysicalDeviceMemoryProperties*)>(instanceFunction(instance, "vkGetPhysicalDeviceMemoryProperties"));
    auto createDevice = reinterpret_cast<Result (*)(PhysicalDevice, const DeviceCreateInfo*, const void*, Device*)>(instanceFunction(instance, "vkCreateDevice"));
    auto getDeviceProcAddress = reinterpret_cast<VoidFunction (*)(Device, const char*)>(instanceFunction(instance, "vkGetDeviceProcAddr"));

    std::optional<std::vector<uint8_t>> result;
    uint32_t count = 1;
    PhysicalDevice physicalDevice = nullptr;
    enumerateDevices(instance, &count, &physicalDevice);
    const char* extensions[] = { "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf" };
    float priority = 1;
    DeviceQueueCreateInfo queue { structureDeviceQueueCreateInfo, nullptr, 0, 0, 1, &priority };
    DeviceCreateInfo deviceInfo { structureDeviceCreateInfo, nullptr, 0, 1, &queue, 0, nullptr, 2, extensions, nullptr };
    Device device = nullptr;
    if (!physicalDevice || createDevice(physicalDevice, &deviceInfo, nullptr, &device) || !device) {
        note = "no Vulkan device with VK_EXT_external_memory_dma_buf";
        destroyInstance(instance, nullptr);
        return std::nullopt;
    }
    auto destroyDevice = reinterpret_cast<void (*)(Device, const void*)>(getDeviceProcAddress(device, "vkDestroyDevice"));
    auto fdProperties = reinterpret_cast<Result (*)(Device, uint32_t, int, MemoryFdProperties*)>(getDeviceProcAddress(device, "vkGetMemoryFdPropertiesKHR"));
    auto allocateMemory = reinterpret_cast<Result (*)(Device, const MemoryAllocateInfo*, const void*, DeviceMemory*)>(getDeviceProcAddress(device, "vkAllocateMemory"));
    auto freeMemory = reinterpret_cast<void (*)(Device, DeviceMemory, const void*)>(getDeviceProcAddress(device, "vkFreeMemory"));
    auto mapMemory = reinterpret_cast<Result (*)(Device, DeviceMemory, uint64_t, uint64_t, Flags, void**)>(getDeviceProcAddress(device, "vkMapMemory"));
    auto unmapMemory = reinterpret_cast<void (*)(Device, DeviceMemory)>(getDeviceProcAddress(device, "vkUnmapMemory"));

    // The memory type: one the dma-buf may be imported as, and that maps.
    MemoryFdProperties properties { structureMemoryFdProperties, nullptr, 0 };
    PhysicalDeviceMemoryProperties types { };
    memoryProperties(physicalDevice, &types);
    std::optional<uint32_t> typeIndex;
    if (fdProperties && !fdProperties(device, handleTypeDMABuf, planes.fds[0], &properties)) {
        for (uint32_t i = 0; i < types.memoryTypeCount && !typeIndex; ++i) {
            if ((properties.memoryTypeBits & (1u << i)) && (types.memoryTypes[i].propertyFlags & memoryHostVisible))
                typeIndex = i;
        }
    }
    off_t size = lseek(planes.fds[0], 0, SEEK_END);
    // Vulkan takes ownership of the descriptor it imports: a dup of the engine's.
    int fd = typeIndex && size > 0 ? dup(planes.fds[0]) : -1;
    ImportMemoryFdInfo import { structureImportMemoryFdInfo, nullptr, handleTypeDMABuf, fd };
    MemoryAllocateInfo allocation { structureMemoryAllocateInfo, &import, static_cast<uint64_t>(size), typeIndex.value_or(0) };
    DeviceMemory memory = nullptr;
    void* mapped = nullptr;
    if (fd < 0)
        note = "the dma-buf cannot be imported as host-visible Vulkan memory";
    else if (allocateMemory(device, &allocation, nullptr, &memory) || !memory) {
        close(fd);
        note = "vkAllocateMemory refused the dma-buf";
    } else if (mapMemory(device, memory, 0, wholeSize, 0, &mapped) || !mapped)
        note = "vkMapMemory failed";
    else {
        std::vector<uint8_t> pixels(width * height * 4);
        auto* bytes = static_cast<const uint8_t*>(mapped) + planes.offsets[0];
        for (unsigned y = 0; y < height; ++y)
            memcpy(pixels.data() + y * width * 4, bytes + y * planes.strides[0], width * 4);
        result = std::move(pixels);
        unmapMemory(device, memory);
    }
    if (memory)
        freeMemory(device, memory, nullptr);
    destroyDevice(device, nullptr);
    destroyInstance(instance, nullptr);
    return result;
}

} // namespace vk

// The box is 50px of pure red on row 45: exactly one run of that length.
bool boxIsWhole(const Readback& frame)
{
    unsigned run = 0, runs = 0, longest = 0;
    auto* row = frame.pixels.data() + 45 * frame.width * 4;
    for (unsigned x = 0; x <= frame.width; ++x) {
        bool red = x < frame.width && row[x * 4] == 255 && row[x * 4 + 1] == 0 && row[x * 4 + 2] == 0;
        if (red)
            ++run;
        else if (run) {
            ++runs;
            longest = std::max(longest, run);
            run = 0;
        }
    }
    return runs == 1 && longest == 50;
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool ok = true;

    ZeroG::initializeProcess();
    ok &= check(ZeroG::initializeGPU(), "the engine's GPU: %s", ZeroG::gpuRenderer().utf8().data());
    auto surface = ZeroG::createDMABufSurface(3);
    if (!check(!!surface, "a dma-buf surface (EGL export, GBM, or a DRM device's dumb buffers)"))
        return 1;
    auto view = ZeroG::View::create({ { width, height }, 1, false, true, WTF::move(surface) });
    view->loadHTML(String::fromUTF8(page));

    std::atomic<bool> stop { false }, ready { false }, contextFailed { false };
    std::atomic<int> acquired { 0 }, torn { 0 }, failed { 0 }, notDMABuf { 0 };
    Readback last;
    bool hasLast = false;
    ZeroG::DMABufFrame lastPlanes;
    Importer importer;
    // One frame, read through Vulkan too, against GL's reading of it.
    std::optional<bool> vulkanMatches;
    std::string vulkanNote;
    std::thread renderThread([&] {
        if (!importer.makeCurrent()) {
            contextFailed = true;
            ready = true;
            return;
        }
        ready = true;
        Readback readback;
        auto readOne = [&] {
            ZeroG::Frame frame;
            if (!view->surface().acquire(frame))
                return false;
            ZeroG::DMABufFrame planes;
            if (!view->surface().dmabuf(frame, planes))
                ++notDMABuf;
            else if (importer.readFrame(frame, planes, readback)) {
                ++acquired;
                if (!boxIsWhole(readback))
                    ++torn;
                if (acquired == 20 && vulkanNote.empty() && !vulkanMatches) {
                    if (auto pixels = vk::read(planes, frame.width, frame.height, vulkanNote))
                        vulkanMatches = *pixels == readback.pixels;
                }
                last = readback;
                lastPlanes = planes;
                hasLast = true;
            } else
                ++failed;
            view->surface().release(frame);
            return true;
        };
        while (!stop) {
            if (!readOne())
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        readOne();
    });
    while (!ready)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!check(!contextFailed, "the application's GL context")) {
        renderThread.join();
        return 1;
    }

    // The engine thread: one frame every 16ms of the application's clock.
    auto frameTime = MonotonicTime::now();
    for (int i = 0; i < frames; ++i) {
        ZeroG::runPendingWork();
        view->render(frameTime);
        frameTime += 16_ms;
        while (MonotonicTime::now() < frameTime)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    view->evaluateScript("step = () => { }; 0"_s);
    for (int i = 0; i < 60 && (i < 3 || view->hasPendingRenderingUpdate()); ++i) {
        frameTime += 16_ms;
        while (MonotonicTime::now() < frameTime)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ZeroG::runPendingWork();
        view->render(frameTime);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop = true;
    renderThread.join();

    ok &= check(!notDMABuf, "every frame is a dma-buf (%d are not)", notDMABuf.load());
    if (hasLast) {
        std::printf("     format %c%c%c%c, modifier 0x%llx, %u plane(s), stride %u, %s fence\n", lastPlanes.fourcc & 0xff, (lastPlanes.fourcc >> 8) & 0xff,
            (lastPlanes.fourcc >> 16) & 0xff, lastPlanes.fourcc >> 24, static_cast<unsigned long long>(lastPlanes.modifier), lastPlanes.planeCount,
            lastPlanes.strides[0], lastPlanes.readyFence >= 0 ? "a sync_file" : "no (complete when published)");
    }
    ok &= check(acquired > frames / 4, "the render thread imported and read %d frames", acquired.load());
    ok &= check(!failed, "no failed imports or readbacks (%d)", failed.load());
    ok &= check(!torn, "no torn frames (%d)", torn.load());
    ok &= check(!importer.descriptorsChanged, "a buffer keeps its descriptors (%zu buffers)", importer.imports.size());
    if (vulkanMatches)
        ok &= check(*vulkanMatches, "a frame imported into Vulkan memory (VK_EXT_external_memory_dma_buf) has the bytes GL reads");
    else
        std::printf("     skipped the Vulkan import: %s\n", vulkanNote.empty() ? "no frame for it" : vulkanNote.c_str());

    RefPtr reference = ZeroG::paint(view->page(), ZeroG::Rendering::CPU);
    RefPtr expected = reference ? reference->getPixelBuffer({ AlphaPremultiplication::Unpremultiplied, PixelFormat::RGBA8, ColorSpace::SRGB() }, { { }, view->size() }) : nullptr;
    if (hasLast && expected) {
        unsigned different = 0;
        auto bytes = expected->bytes();
        for (unsigned y = 0; y < last.height; ++y) {
            auto* actualRow = last.pixels.data() + y * last.width * 4;
            auto* expectedRow = bytes.data() + y * last.width * 4;
            for (unsigned x = 0; x < last.width; ++x) {
                for (int c = 0; c < 4; ++c) {
                    if (std::abs(actualRow[x * 4 + c] - expectedRow[x * 4 + c]) > 48) {
                        ++different;
                        break;
                    }
                }
            }
        }
        ok &= check(different <= width * height / 100, "the last frame matches a CPU render (%u pixels differ)", different);
    } else
        ok &= check(false, "a last frame and the CPU reference");

    std::printf(ok ? "PASS\n" : "FAILED\n");
    return ok ? 0 : 1;
}
