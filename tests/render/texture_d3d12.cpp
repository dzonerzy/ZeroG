// Texture mode for D3D12 games: the application owns a D3D12 device and a command
// queue; the engine renders into textures its queue's device opens, and acquire
// and release queue the waits and signals on that queue. The reader copies each
// frame into a readback buffer on the same queue. No frame may be torn, and the
// last one must match a CPU render. A device on another adapter (WARP) is
// refused with a clear error.

#include "texture_common.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using namespace TextureTest;

namespace {

struct D3D12Reader {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    uint64_t fenceValue { 0 };
    HANDLE event { nullptr };

    bool create(IDXGIAdapter* adapter)
    {
        D3D12_COMMAND_QUEUE_DESC queueDescription { };
        queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))
            && SUCCEEDED(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue)))
            && SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))
            && SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)))
            && SUCCEEDED(list->Close())
            && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))
            && event;
    }

    // The frame's texture copied into a readback buffer on the queue the engine
    // queued the frame's wait on, then mapped once the copy is done.
    bool read(const ZeroG::Frame& frame, Readback& out)
    {
        auto* texture = static_cast<ID3D12Resource*>(frame.texture);
        if (!texture)
            return false;
        auto description = texture->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
        UINT64 total = 0;
        device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &total);

        D3D12_HEAP_PROPERTIES heap { };
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bufferDescription { };
        bufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDescription.Width = total;
        bufferDescription.Height = 1;
        bufferDescription.DepthOrArraySize = 1;
        bufferDescription.MipLevels = 1;
        bufferDescription.SampleDesc.Count = 1;
        bufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDescription, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
            return false;

        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
        // The frame comes in COMMON and goes back to it.
        D3D12_RESOURCE_BARRIER barrier { };
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION destination { readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        destination.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION source { texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        source.SubresourceIndex = 0;
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
        list->Close();
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), ++fenceValue);
        fence->SetEventOnCompletion(fenceValue, event);
        WaitForSingleObject(event, INFINITE);

        void* data = nullptr;
        D3D12_RANGE range { 0, static_cast<SIZE_T>(total) };
        if (FAILED(readback->Map(0, &range, &data)))
            return false;
        out.assign(static_cast<uint8_t*>(data) + footprint.Offset, footprint.Footprint.RowPitch, footprint.Footprint.Width, footprint.Footprint.Height, false);
        D3D12_RANGE written { 0, 0 };
        readback->Unmap(0, &written);
        return true;
    }
};

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool ok = true;

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        std::printf("FAIL no DXGI factory\n");
        return 1;
    }

    ZeroG::initializeProcess();
    ok &= check(ZeroG::initializeGPU(), "GPU: %s", ZeroG::gpuRenderer().utf8().data());

    // The application's device on the engine's adapter (the default one).
    ComPtr<IDXGIAdapter1> adapter;
    factory->EnumAdapters1(0, &adapter);
    D3D12Reader reader;
    if (!check(reader.create(adapter.Get()), "D3D12 device and queue"))
        return 1;
    auto surface = ZeroG::createD3D12Surface(reader.queue.Get(), 3);
    if (!check(!!surface, "D3D12 surface on the application's queue %s", ZeroG::lastSurfaceError()))
        return 1;

    // Frames carry their handles too.
    auto view = ZeroG::View::create({ { width, height }, 1, false, true, WTF::move(surface) });
    ok &= run(*view, [&](const ZeroG::Frame& frame, Readback& out) {
        ZeroG::Win32Frame handles;
        if (!view->surface().win32(frame, handles) || !handles.texture || !handles.readyFence || handles.releaseFence || !handles.bufferID)
            return false;
        return reader.read(frame, out);
    });

    // A device on WARP, another adapter: refused, saying why.
    ComPtr<IDXGIAdapter> warp;
    D3D12Reader warpReader;
    if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) && warpReader.create(warp.Get())) {
        auto refused = ZeroG::createD3D12Surface(warpReader.queue.Get(), 2);
        ok &= check(!refused && std::strstr(ZeroG::lastSurfaceError(), "another adapter"), "a queue on another adapter is refused: %s", ZeroG::lastSurfaceError());
    } else
        std::printf("     skipped the other adapter: no WARP device\n");

    view = nullptr;
    std::printf(ok ? "PASS\n" : "FAILED\n");
    return ok ? 0 : 1;
}
