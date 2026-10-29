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
    WEBCORE_EXPORT static std::unique_ptr<PlatformDisplayZeroG> create();

    ~PlatformDisplayZeroG();

private:
    explicit PlatformDisplayZeroG(Ref<GLDisplay>&&);

    Type type() const final { return PlatformDisplay::Type::Surfaceless; }
};

} // namespace WebCore

#endif // PLATFORM(ZEROG)
