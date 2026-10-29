#include "config.h"
#include "PlatformDisplayZeroG.h"

#if PLATFORM(ZEROG)

#include "GLContext.h"
#include "GLDisplay.h"
#include "Logging.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace WebCore {

#if OS(WINDOWS)

static RefPtr<GLDisplay> createGLDisplay()
{
    const EGLint attributes[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
        // Debug layers fill uninitialized buffers with a clear color.
        EGL_PLATFORM_ANGLE_DEBUG_LAYERS_ENABLED_ANGLE, EGL_FALSE,
        EGL_NONE,
    };
    return GLDisplay::create(eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, attributes));
}

#else

// No window system is involved: Mesa's surfaceless platform, then an EGL device
// (NVIDIA's headless path; hardware devices before software ones), then the
// default display. ZEROG_EGL_PLATFORM=surfaceless|device|default forces one, for
// tests and for systems where the first choice misbehaves.
static RefPtr<GLDisplay> createGLDisplay()
{
    const char* extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (!extensions)
        return nullptr; // No EGL driver on this system.

    const char* forced = getenv("ZEROG_EGL_PLATFORM");
    auto allowed = [&](const char* platform) {
        return !forced || !*forced || !strcmp(forced, platform);
    };

    auto eglGetPlatformDisplayEXT = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    auto eglQueryDevicesEXT = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(eglGetProcAddress("eglQueryDevicesEXT"));
    auto eglQueryDeviceStringEXT = reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(eglGetProcAddress("eglQueryDeviceStringEXT"));

    if (eglGetPlatformDisplayEXT && allowed("surfaceless") && GLContext::isExtensionSupported(extensions, "EGL_MESA_platform_surfaceless")) {
        if (auto display = GLDisplay::create(eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)))
            return display;
    }

    if (eglGetPlatformDisplayEXT && eglQueryDevicesEXT && allowed("device") && GLContext::isExtensionSupported(extensions, "EGL_EXT_platform_device")) {
        EGLDeviceEXT devices[16];
        EGLint count = 0;
        if (eglQueryDevicesEXT(std::size(devices), devices, &count)) {
            auto isSoftware = [&](EGLDeviceEXT device) {
                const char* deviceExtensions = eglQueryDeviceStringEXT ? eglQueryDeviceStringEXT(device, EGL_EXTENSIONS) : nullptr;
                return deviceExtensions && GLContext::isExtensionSupported(deviceExtensions, "EGL_MESA_device_software");
            };
            for (bool software : { false, true }) {
                for (EGLint i = 0; i < count; ++i) {
                    if (isSoftware(devices[i]) != software)
                        continue;
                    if (auto display = GLDisplay::create(eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, devices[i], nullptr)))
                        return display;
                }
            }
        }
    }

    if (!allowed("default"))
        return nullptr;
    return GLDisplay::create(eglGetDisplay(EGL_DEFAULT_DISPLAY));
}

#endif

std::unique_ptr<PlatformDisplayZeroG> PlatformDisplayZeroG::create(void* hostDisplay)
{
    // eglInitialize on a display the application already initialized only
    // returns its version; it is never terminated by the engine.
    auto glDisplay = hostDisplay ? GLDisplay::create(static_cast<EGLDisplay>(hostDisplay)) : createGLDisplay();
    if (!glDisplay) {
        RELEASE_LOG_INFO(Compositing, "No GPU display available, rendering on the CPU: %s", GLContext::lastErrorString());
        return nullptr;
    }
    // WebCore picks offscreen configs by the display's type: pbuffer configs for
    // Surfaceless, window configs for Default. Window systems such as Wayland
    // have no pbuffer configs.
    const EGLint pbufferAttributes[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    EGLint pbufferConfigs = 0;
    eglChooseConfig(glDisplay->eglDisplay(), pbufferAttributes, nullptr, 0, &pbufferConfigs);
    auto type = pbufferConfigs > 0 ? Type::Surfaceless : Type::Default;
    return std::unique_ptr<PlatformDisplayZeroG>(new PlatformDisplayZeroG(glDisplay.releaseNonNull(), type));
}

PlatformDisplayZeroG::PlatformDisplayZeroG(Ref<GLDisplay>&& glDisplay, Type type)
    : PlatformDisplay(WTF::move(glDisplay))
    , m_type(type)
{
}

PlatformDisplayZeroG::~PlatformDisplayZeroG() = default;

} // namespace WebCore

#endif // PLATFORM(ZEROG)
