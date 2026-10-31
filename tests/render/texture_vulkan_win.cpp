// Texture mode for Vulkan games on Windows: frames are NT handles of a D3D11
// texture and two D3D12 fences (zg_surface_win32), which the application imports
// (VK_KHR_external_memory_win32, VK_KHR_external_semaphore_win32 into timeline
// semaphores) and orders itself: wait for the ready value before reading, signal
// the release value when done. The reader imports each texture once (by its
// buffer ID) and copies every frame into a host-visible buffer. No frame may be
// torn and the last one must match a CPU render. Without a Vulkan driver, or one
// that cannot import these handles, the test is skipped.

#include "texture_common.h"

#define VK_USE_PLATFORM_WIN32_KHR 1
#include <dxgi1_2.h>
#include <map>
#include <vulkan/vulkan.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using namespace TextureTest;

namespace {

constexpr auto memoryHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
constexpr auto semaphoreHandleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;

struct VulkanReader {
    VkInstance instance { VK_NULL_HANDLE };
    VkPhysicalDevice physical { VK_NULL_HANDLE };
    VkDevice device { VK_NULL_HANDLE };
    uint32_t family { 0 };
    VkQueue queue { VK_NULL_HANDLE };
    VkCommandPool pool { VK_NULL_HANDLE };
    VkCommandBuffer commands { VK_NULL_HANDLE };
    VkFence fence { VK_NULL_HANDLE };
    VkSemaphore ready { VK_NULL_HANDLE };
    VkSemaphore release { VK_NULL_HANDLE };
    VkBuffer buffer { VK_NULL_HANDLE };
    VkDeviceMemory bufferMemory { VK_NULL_HANDLE };
    void* mapped { nullptr };
    PFN_vkGetMemoryWin32HandlePropertiesKHR getMemoryWin32HandleProperties { nullptr };
    PFN_vkImportSemaphoreWin32HandleKHR importSemaphoreWin32Handle { nullptr };
    struct Image { VkImage image { VK_NULL_HANDLE }; VkDeviceMemory memory { VK_NULL_HANDLE }; };
    std::map<uint64_t, Image> images; // by buffer ID
    bool semaphoresImported { false };
    std::string skipReason;

    ~VulkanReader()
    {
        if (device) {
            vkDeviceWaitIdle(device);
            for (auto& [id, image] : images) {
                vkDestroyImage(device, image.image, nullptr);
                vkFreeMemory(device, image.memory, nullptr);
            }
            if (mapped)
                vkUnmapMemory(device, bufferMemory);
            vkDestroyBuffer(device, buffer, nullptr);
            vkFreeMemory(device, bufferMemory, nullptr);
            vkDestroySemaphore(device, ready, nullptr);
            vkDestroySemaphore(device, release, nullptr);
            vkDestroyFence(device, fence, nullptr);
            vkDestroyCommandPool(device, pool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance)
            vkDestroyInstance(instance, nullptr);
    }

    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags)
    {
        VkPhysicalDeviceMemoryProperties properties;
        vkGetPhysicalDeviceMemoryProperties(physical, &properties);
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        }
        return UINT32_MAX;
    }

    // A device on the adapter whose LUID is `luid`, able to import D3D11
    // textures and D3D12 fences; false (with skipReason) otherwise.
    bool create(uint64_t luid)
    {
        VkApplicationInfo application { VK_STRUCTURE_TYPE_APPLICATION_INFO };
        application.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo instanceInfo { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        instanceInfo.pApplicationInfo = &application;
        if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) {
            skipReason = "no Vulkan 1.2 driver";
            return false;
        }
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance, &count, devices.data());
        for (auto candidate : devices) {
            VkPhysicalDeviceIDProperties id { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
            VkPhysicalDeviceProperties2 properties { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id };
            vkGetPhysicalDeviceProperties2(candidate, &properties);
            // A Windows LUID: LowPart, then HighPart, which reads as HighPart << 32 | LowPart.
            uint64_t deviceLUID = 0;
            std::memcpy(&deviceLUID, id.deviceLUID, sizeof(deviceLUID));
            if (id.deviceLUIDValid && deviceLUID == luid) {
                physical = candidate;
                std::printf("     Vulkan device: %s\n", properties.properties.deviceName);
                break;
            }
        }
        if (!physical) {
            skipReason = "no Vulkan device on the engine's adapter";
            return false;
        }

        // Can these handles be imported?
        VkPhysicalDeviceExternalImageFormatInfo externalImage { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO };
        externalImage.handleType = memoryHandleType;
        VkPhysicalDeviceImageFormatInfo2 imageFormat { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &externalImage };
        imageFormat.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageFormat.type = VK_IMAGE_TYPE_2D;
        imageFormat.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageFormat.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkExternalImageFormatProperties externalImageProperties { VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
        VkImageFormatProperties2 imageProperties { VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &externalImageProperties };
        if (vkGetPhysicalDeviceImageFormatProperties2(physical, &imageFormat, &imageProperties) != VK_SUCCESS
            || !(externalImageProperties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
            skipReason = "the Vulkan driver cannot import D3D11 textures";
            return false;
        }
        VkSemaphoreTypeCreateInfo timelineType { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
        timelineType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkPhysicalDeviceExternalSemaphoreInfo semaphoreInfo { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, &timelineType };
        semaphoreInfo.handleType = semaphoreHandleType;
        VkExternalSemaphoreProperties semaphoreProperties { VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES };
        vkGetPhysicalDeviceExternalSemaphoreProperties(physical, &semaphoreInfo, &semaphoreProperties);
        if (!(semaphoreProperties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT)) {
            skipReason = "the Vulkan driver cannot import D3D12 fences";
            return false;
        }

        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
        for (family = 0; family < familyCount && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT); ++family) { }
        float priority = 1;
        VkDeviceQueueCreateInfo queueInfo { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        const char* extensions[] = { VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME };
        VkPhysicalDeviceVulkan12Features features12 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        features12.timelineSemaphore = VK_TRUE;
        VkDeviceCreateInfo deviceInfo { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &features12 };
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = 2;
        deviceInfo.ppEnabledExtensionNames = extensions;
        if (vkCreateDevice(physical, &deviceInfo, nullptr, &device) != VK_SUCCESS) {
            skipReason = "the Vulkan device lacks the Win32 external memory and semaphore extensions";
            return false;
        }
        vkGetDeviceQueue(device, family, 0, &queue);
        getMemoryWin32HandleProperties = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandlePropertiesKHR"));
        importSemaphoreWin32Handle = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(vkGetDeviceProcAddr(device, "vkImportSemaphoreWin32HandleKHR"));

        VkCommandPoolCreateInfo poolInfo { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = family;
        VkCommandBufferAllocateInfo allocateInfo { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        VkFenceCreateInfo fenceInfo { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkSemaphoreCreateInfo semaphoreCreate { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &timelineType };
        if (vkCreateCommandPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS)
            return false;
        allocateInfo.commandPool = pool;
        return vkAllocateCommandBuffers(device, &allocateInfo, &commands) == VK_SUCCESS
            && vkCreateFence(device, &fenceInfo, nullptr, &fence) == VK_SUCCESS
            && vkCreateSemaphore(device, &semaphoreCreate, nullptr, &ready) == VK_SUCCESS
            && vkCreateSemaphore(device, &semaphoreCreate, nullptr, &release) == VK_SUCCESS
            && createBuffer();
    }

    bool createBuffer()
    {
        VkBufferCreateInfo info { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = static_cast<VkDeviceSize>(width) * height * 4;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(device, &info, nullptr, &buffer) != VK_SUCCESS)
            return false;
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, buffer, &requirements);
        VkMemoryAllocateInfo allocate { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        return allocate.memoryTypeIndex != UINT32_MAX
            && vkAllocateMemory(device, &allocate, nullptr, &bufferMemory) == VK_SUCCESS
            && vkBindBufferMemory(device, buffer, bufferMemory, 0) == VK_SUCCESS
            && vkMapMemory(device, bufferMemory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS;
    }

    bool importSemaphore(VkSemaphore semaphore, HANDLE handle)
    {
        VkImportSemaphoreWin32HandleInfoKHR info { VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
        info.semaphore = semaphore;
        info.handleType = semaphoreHandleType;
        info.handle = handle;
        return importSemaphoreWin32Handle(device, &info) == VK_SUCCESS;
    }

    // The frame's texture, imported once per buffer ID.
    VkImage image(const ZeroG::Win32Frame& handles, unsigned w, unsigned h)
    {
        if (auto found = images.find(handles.bufferID); found != images.end())
            return found->second.image;
        VkExternalMemoryImageCreateInfo external { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
        external.handleTypes = memoryHandleType;
        VkImageCreateInfo info { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &external };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = { w, h, 1 };
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        Image imported;
        if (vkCreateImage(device, &info, nullptr, &imported.image) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(device, imported.image, &requirements);
        VkMemoryWin32HandlePropertiesKHR handleProperties { VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
        getMemoryWin32HandleProperties(device, memoryHandleType, handles.texture, &handleProperties);
        VkMemoryDedicatedAllocateInfo dedicated { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        dedicated.image = imported.image;
        VkImportMemoryWin32HandleInfoKHR import { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &dedicated };
        import.handleType = memoryHandleType;
        import.handle = handles.texture;
        VkMemoryAllocateInfo allocate { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import };
        allocate.allocationSize = requirements.size;
        uint32_t bits = requirements.memoryTypeBits & (handleProperties.memoryTypeBits ? handleProperties.memoryTypeBits : ~0u);
        allocate.memoryTypeIndex = memoryType(bits, 0);
        if (allocate.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device, &allocate, nullptr, &imported.memory) != VK_SUCCESS
            || vkBindImageMemory(device, imported.image, imported.memory, 0) != VK_SUCCESS) {
            vkDestroyImage(device, imported.image, nullptr);
            return VK_NULL_HANDLE;
        }
        images[handles.bufferID] = imported;
        return imported.image;
    }

    // Waits for the frame, copies it into the buffer, signals its release
    // (always: the engine's GPU waits for it), and reads the buffer.
    bool read(ZeroG::Surface& surface, const ZeroG::Frame& frame, Readback& out)
    {
        ZeroG::Win32Frame handles;
        if (!surface.win32(frame, handles) || !handles.releaseFence || handles.dxgiFormat != 28)
            return false;
        if (!semaphoresImported) {
            if (!importSemaphore(ready, handles.readyFence) || !importSemaphore(release, handles.releaseFence))
                return false;
            semaphoresImported = true;
        }
        VkImage source = image(handles, frame.width, frame.height);

        vkResetCommandBuffer(commands, 0);
        VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commands, &begin);
        if (source) {
            // Taken from the external owner (the engine's D3D11 device) with its
            // contents, and given back after the copy.
            VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
            barrier.dstQueueFamilyIndex = family;
            barrier.image = source;
            barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            VkBufferImageCopy region { };
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { frame.width, frame.height, 1 };
            vkCmdCopyImageToBuffer(commands, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.dstAccessMask = 0;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcQueueFamilyIndex = family;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }
        vkEndCommandBuffer(commands);

        uint64_t waitValue = handles.readyValue;
        uint64_t signalValue = handles.releaseValue;
        VkTimelineSemaphoreSubmitInfo timeline { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
        timeline.waitSemaphoreValueCount = 1;
        timeline.pWaitSemaphoreValues = &waitValue;
        timeline.signalSemaphoreValueCount = 1;
        timeline.pSignalSemaphoreValues = &signalValue;
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO, &timeline };
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &ready;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &release;
        vkResetFences(device, 1, &fence);
        if (vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS || vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS || !source)
            return false;
        out.assign(mapped, static_cast<size_t>(frame.width) * 4, frame.width, frame.height, false);
        return true;
    }
};

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool ok = true;

    ZeroG::initializeProcess();
    ok &= check(ZeroG::initializeGPU(), "GPU: %s", ZeroG::gpuRenderer().utf8().data());

    auto surface = ZeroG::createWin32Surface(3);
    if (!check(!!surface, "Win32 surface %s", ZeroG::lastSurfaceError()))
        return 1;

    // The engine renders on the default adapter: the Vulkan device must be there.
    ComPtr<IDXGIFactory1> factory;
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 description { };
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapters1(0, &adapter)) || FAILED(adapter->GetDesc1(&description))) {
        std::printf("FAIL no DXGI adapter\n");
        return 1;
    }
    uint64_t luid = static_cast<uint64_t>(static_cast<uint32_t>(description.AdapterLuid.HighPart)) << 32 | description.AdapterLuid.LowPart;
    VulkanReader reader;
    if (!reader.create(luid)) {
        std::printf("     skipped: %s\n", reader.skipReason.c_str());
        std::printf(ok ? "PASS\n" : "FAILED\n");
        return ok ? 0 : 1;
    }

    auto view = ZeroG::View::create({ { width, height }, 1, false, true, WTF::move(surface) });
    std::atomic<bool> sameAdapter { true };
    ok &= run(*view, [&](const ZeroG::Frame& frame, Readback& out) {
        ZeroG::Win32Frame handles;
        if (view->surface().win32(frame, handles) && handles.adapterLUID != luid)
            sameAdapter = false;
        return reader.read(view->surface(), frame, out);
    });
    ok &= check(sameAdapter, "frames are on the engine's adapter");
    ok &= check(reader.images.size() <= 3, "each texture imported once (%zu imports)", reader.images.size());

    view = nullptr;
    std::printf(ok ? "PASS\n" : "FAILED\n");
    return ok ? 0 : 1;
}
