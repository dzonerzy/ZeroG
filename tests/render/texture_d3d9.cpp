// Texture mode for Direct3D 9 games, both ways the engine serves them:
// - an IDirect3DDevice9Ex opens the engine's textures (shared);
// - a plain IDirect3DDevice9 (or a 9Ex one on another adapter) gets the changed
//   pixels copied into a texture of its own at acquire. A 9Ex device tests this
//   path too, since a plain one cannot be made without a desktop (remote
//   sessions), where it is skipped.
// The reader draws each frame as a game would (a textured quad onto a render
// target) and reads the render target back. No frame may be torn and the last
// one must match a CPU render; then the device goes through
// zg_surface_device_lost and Reset and keeps getting frames.

#include "texture_common.h"

#include <d3d9.h>
#include <wrl/client.h>
#include <wtf/Scope.h>

using Microsoft::WRL::ComPtr;
using namespace TextureTest;

namespace {

struct D3D9Reader {
    ComPtr<IDirect3D9> direct3D;
    ComPtr<IDirect3D9Ex> direct3DEx;
    ComPtr<IDirect3DDevice9> device;
    ComPtr<IDirect3DDevice9Ex> deviceEx;
    D3DPRESENT_PARAMETERS parameters { };
    ComPtr<IDirect3DSurface9> target;
    ComPtr<IDirect3DSurface9> system;
    HRESULT lastResult { S_OK };

    bool create(HWND window, bool ex)
    {
        parameters.Windowed = TRUE;
        parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
        parameters.BackBufferFormat = D3DFMT_UNKNOWN;
        parameters.BackBufferWidth = width;
        parameters.BackBufferHeight = height;
        parameters.hDeviceWindow = window;
        // FPU_PRESERVE: D3D9 otherwise lowers the x87 precision of the thread.
        DWORD flags = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE;
        HRESULT result;
        if (ex) {
            result = Direct3DCreate9Ex(D3D_SDK_VERSION, &direct3DEx);
            if (SUCCEEDED(result))
                result = direct3DEx->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, flags, &parameters, nullptr, &deviceEx);
            device = deviceEx;
        } else {
            direct3D.Attach(Direct3DCreate9(D3D_SDK_VERSION));
            result = direct3D ? direct3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, flags, &parameters, &device) : E_FAIL;
        }
        lastResult = result;
        if (FAILED(result)) {
            std::printf("     device creation failed: 0x%08lx\n", static_cast<unsigned long>(result));
            return false;
        }
        if (!createTargets()) {
            std::printf("     render target creation failed\n");
            return false;
        }
        return true;
    }

    bool createTargets()
    {
        return SUCCEEDED(device->CreateRenderTarget(width, height, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr))
            && SUCCEEDED(device->CreateOffscreenPlainSurface(width, height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &system, nullptr));
    }

    // The game's side of a lost device: everything in the default pool goes,
    // the engine's objects too (zg_surface_device_lost), then Reset.
    bool reset(ZeroG::Surface& surface)
    {
        surface.deviceLost();
        target = nullptr;
        system = nullptr;
        HRESULT result = deviceEx ? deviceEx->ResetEx(&parameters, nullptr) : device->Reset(&parameters);
        return SUCCEEDED(result) && createTargets();
    }

    // The frame drawn with no blending and point sampling onto the render
    // target, which is then read back.
    bool read(const ZeroG::Frame& frame, Readback& out)
    {
        auto* texture = static_cast<IDirect3DTexture9*>(frame.texture);
        if (!texture)
            return false;
        struct Vertex { float x, y, z, rhw, u, v; };
        const float w = static_cast<float>(frame.width), h = static_cast<float>(frame.height);
        // Pixel centers: D3D9 rasterizes at half-pixel offsets.
        const Vertex quad[] = {
            { -0.5f, -0.5f, 0, 1, 0, 0 }, { w - 0.5f, -0.5f, 0, 1, 1, 0 },
            { -0.5f, h - 0.5f, 0, 1, 0, 1 }, { w - 0.5f, h - 0.5f, 0, 1, 1, 1 },
        };
        device->SetRenderTarget(0, target.Get());
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTexture(0, texture);
        device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        if (FAILED(device->BeginScene()))
            return false;
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
        device->EndScene();
        device->SetTexture(0, nullptr);
        if (FAILED(device->GetRenderTargetData(target.Get(), system.Get())))
            return false;
        D3DLOCKED_RECT locked;
        if (FAILED(system->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
            return false;
        out.assign(locked.pBits, locked.Pitch, frame.width, frame.height, true);
        system->UnlockRect();
        return true;
    }
};

// After a device reset: a repaint reaches the device whole and matches a CPU render.
bool frameAfterReset(ZeroG::View& view, D3D9Reader& reader)
{
    view.repaint();
    auto frameTime = MonotonicTime::now();
    for (int i = 0; i < 100 && !view.surface().hasNewFrame(); ++i) {
        view.render(frameTime);
        frameTime += 16_ms;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    ZeroG::Frame frame;
    if (!view.surface().acquire(frame))
        return false;
    Readback readback;
    bool read = reader.read(frame, readback);
    view.surface().release(frame);
    if (!read || !boxIsWhole(readback))
        return false;
    RefPtr reference = ZeroG::paint(view.page(), ZeroG::Rendering::CPU);
    RefPtr expected = reference ? reference->getPixelBuffer({ WebCore::AlphaPremultiplication::Unpremultiplied, WebCore::PixelFormat::RGBA8, WebCore::ColorSpace::SRGB() }, { { }, view.size() }) : nullptr;
    if (!expected)
        return false;
    unsigned different = 0;
    auto bytes = expected->bytes();
    for (size_t i = 0; i < readback.pixels.size(); i += 4) {
        for (int c = 0; c < 4; ++c) {
            if (std::abs(readback.pixels[i + c] - bytes[i + c]) > 48) {
                ++different;
                break;
            }
        }
    }
    return different <= width * height / 100;
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool ok = true;

    WNDCLASSW windowClass { };
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"ZeroGTextureD3D9";
    RegisterClassW(&windowClass);

    ZeroG::initializeProcess();
    ok &= check(ZeroG::initializeGPU(), "GPU: %s", ZeroG::gpuRenderer().utf8().data());

    // A 9Ex device shared, the same device with the copy (what a 9Ex device on
    // another adapter gets), and a plain device (copied).
    struct Case { const char* name; bool ex; bool copy; };
    for (auto [name, ex, copy] : { Case { "IDirect3DDevice9Ex, shared", true, false }, Case { "IDirect3DDevice9Ex, copied", true, true }, Case { "IDirect3DDevice9, copied", false, false } }) {
        std::printf("     %s\n", name);
        // Each device on a window of its own.
        HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"ZeroG D3D9", WS_OVERLAPPEDWINDOW, 0, 0, width, height, nullptr, nullptr, windowClass.hInstance, nullptr);
        auto destroyWindow = makeScopeExit([&] { DestroyWindow(window); });
        D3D9Reader reader;
        if (!reader.create(window, ex)) {
            // A plain device cannot be made while the desktop is unavailable
            // (D3DERR_DEVICELOST: a remote session, a locked or sleeping display).
            if (!ex && reader.lastResult == D3DERR_DEVICELOST) {
                std::printf("     skipped: no plain D3D9 device while the desktop is unavailable\n");
                continue;
            }
            ok &= check(false, "%s", name);
            continue;
        }
        auto surface = ZeroG::createD3D9Surface(reader.device.Get(), 3, copy);
        if (!check(!!surface, "D3D9 surface on the device %s", ZeroG::lastSurfaceError())) {
            ok = false;
            continue;
        }
        auto view = ZeroG::View::create({ { width, height }, 1, false, true, WTF::move(surface) });
        bool shared = ex && !copy;
        ok &= run(*view, [&](const ZeroG::Frame& frame, Readback& out) {
            // Shared frames carry the engine's texture's handle; copies do not.
            if (shared != !!frame.sharedHandle)
                return false;
            return reader.read(frame, out);
        });
        ok &= check(reader.reset(view->surface()), "the device is reset after zg_surface_device_lost");
        ok &= check(frameAfterReset(*view, reader), "and then gets whole frames that match a CPU render");
        view = nullptr;
    }

    std::printf(ok ? "PASS\n" : "FAILED\n");
    return ok ? 0 : 1;
}
