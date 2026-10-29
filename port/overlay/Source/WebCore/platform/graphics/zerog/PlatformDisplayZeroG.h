#pragma once

#if PLATFORM(ZEROG)

#include "PlatformDisplay.h"

namespace WebCore {

// The EGL display Skia's GPU backend renders through (ZeroG.md, "Backend GPU per
// piattaforma"): ANGLE over D3D11 on Windows; on Linux the system driver, loaded
// at runtime, without a window system (surfaceless or device platform). Contexts
// are always offscreen: the application decides where frames are presented.
class PlatformDisplayZeroG final : public PlatformDisplay {
public:
    // Null when no GPU display is available; rendering then stays on the CPU.
    // `hostDisplay`: an EGLDisplay of the application to render on, so textures
    // can be handed to it as EGLImages (Linux texture mode).
    WEBCORE_EXPORT static std::unique_ptr<PlatformDisplayZeroG> create(void* hostDisplay = nullptr);

    ~PlatformDisplayZeroG();

private:
    PlatformDisplayZeroG(Ref<GLDisplay>&&, Type);

    // Surfaceless for the engine's own headless display; Default on a window
    // system's display (X11, Wayland), whose configs are for windows, not pbuffers.
    Type type() const final { return m_type; }

    Type m_type;
};

} // namespace WebCore

#endif // PLATFORM(ZEROG)
