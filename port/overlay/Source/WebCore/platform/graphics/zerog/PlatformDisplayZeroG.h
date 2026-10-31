#pragma once

#if PLATFORM(ZEROG)

#include "PlatformDisplay.h"

namespace WebCore {

// The EGL display Skia's GPU backend renders through
// (docs/design/graphics-presentation.md, "GPU backend per platform"): ANGLE over
// D3D11 on Windows; on Linux the system driver, loaded at runtime, without a
// window system (surfaceless or device platform). Contexts are always
// offscreen: the application decides where frames are presented.
class PlatformDisplayZeroG final : public PlatformDisplay {
public:
    // How a display was opened: the EGL platform (0 for eglGetDisplay) and its
    // native display. On Linux WebGL's ANGLE opens the same one through its GL
    // backend, so its contexts share textures with the engine's.
    struct NativePlatform {
        int platform { 0 };
        void* display { nullptr };
    };

    // Null when no GPU display is available; rendering then stays on the CPU.
    // `hostDisplay`: an EGLDisplay of the application to render on, so textures
    // can be handed to it as EGLImages (Linux texture mode). `hostPlatform` is
    // how it was opened, when known; without it WebGL is not available on Linux.
    WEBCORE_EXPORT static std::unique_ptr<PlatformDisplayZeroG> create(void* hostDisplay = nullptr, std::optional<NativePlatform> hostPlatform = std::nullopt);

#if OS(WINDOWS)
    // The adapter ANGLE's D3D11 device is created on (its LUID, HighPart << 32 |
    // LowPart), for displays created from now on; 0 is the system's default.
    WEBCORE_EXPORT static void setAdapterLUID(uint64_t);
#endif

    ~PlatformDisplayZeroG();

private:
    PlatformDisplayZeroG(Ref<GLDisplay>&&, Type, std::optional<NativePlatform>);

    // Surfaceless for the engine's own headless display; Default on a window
    // system's display (X11, Wayland), whose configs are for windows, not pbuffers.
    Type type() const final { return m_type; }

    Type m_type;
};

} // namespace WebCore

#endif // PLATFORM(ZEROG)
