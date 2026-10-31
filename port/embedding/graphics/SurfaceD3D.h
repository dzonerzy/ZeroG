#pragma once

// What the Direct3D surfaces share (SurfaceD3D11.cpp, SurfaceD3D12.cpp,
// SurfaceD3D9.cpp): Skia renders through ANGLE into D3D11 textures on ANGLE's own
// device, which the application's device opens or copies. The kinds differ in
// how a texture is shared and how the two devices are ordered.

#if OS(WINDOWS)

#include "Surface.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <wtf/Lock.h>
#include <wtf/Vector.h>

namespace ZeroG {

class D3DSurface : public Surface {
protected:
    // ANGLE's device and display, or false (with lastSurfaceError() set) when
    // the engine has no ANGLE D3D11 device.
    static bool angleDevice(Microsoft::WRL::ComPtr<ID3D11Device>&, EGLDisplay&);
    static uint64_t adapterLUID(ID3D11Device*);
    // ANGLE's immediate context, for fences and copies made outside ANGLE from
    // the rendering thread while other threads may be in ANGLE: D3D serializes
    // its calls from then on (a fence or a copy between two of ANGLE's calls
    // changes no state ANGLE relies on).
    static Microsoft::WRL::ComPtr<ID3D11DeviceContext> sharedEngineContext(ID3D11Device*);
    static void setLastError(const char*);

    D3DSurface(SurfaceKind, unsigned bufferCount, Microsoft::WRL::ComPtr<ID3D11Device>&& engine, EGLDisplay);

    // A texture on ANGLE's device that Skia renders into.
    struct EngineTexture {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        EGLImageKHR image { EGL_NO_IMAGE_KHR };
        GLuint glTexture { 0 };
    };
    // Engine thread: `format` is DXGI_FORMAT_R8G8B8A8_UNORM or B8G8R8A8_UNORM;
    // `miscFlags` how it is shared (D3D11_RESOURCE_MISC_*).
    bool createEngineTexture(EngineTexture&, Buffer&, const WebCore::IntSize&, DXGI_FORMAT, UINT miscFlags);
    void destroyEngineTexture(EngineTexture&);
    sk_sp<SkSurface> createPrivateTarget(const WebCore::IntSize&) final;

    static bool makeSkiaContextCurrent();
    // Engine thread: submits the buffer's drawing to ANGLE's device.
    void flush(Buffer&);

    // The application's objects are released on its own thread (its device may
    // be single-threaded): queued from the engine thread, dropped at the next
    // acquire or release.
    void releaseOnHostThread(Microsoft::WRL::ComPtr<IUnknown>&&);
    void releaseHostObjects();

    uint64_t nextBufferID() { return ++m_lastBufferID; }

    Microsoft::WRL::ComPtr<ID3D11Device> m_engine;
    EGLDisplay m_display;

private:
    Lock m_hostObjectsLock;
    Vector<Microsoft::WRL::ComPtr<IUnknown>> m_releasedHostObjects WTF_GUARDED_BY_LOCK(m_hostObjectsLock);
    uint64_t m_lastBufferID { 0 };
};

} // namespace ZeroG

#endif // OS(WINDOWS)
