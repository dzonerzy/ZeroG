#include "config.h"
#include "PlatformDisplayZeroG.h"

#if PLATFORM(ZEROG)

#include "GLContext.h"
#include "GLDisplay.h"
#include "Logging.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>

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

// No window system is involved: Mesa's surfaceless platform, then the first EGL
// device (NVIDIA's headless path), then the default display.
static RefPtr<GLDisplay> createGLDisplay()
{
    const char* extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (!extensions)
        return nullptr; // No EGL driver on this system.

    auto eglGetPlatformDisplayEXT = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    auto eglQueryDevicesEXT = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(eglGetProcAddress("eglQueryDevicesEXT"));
    if (!eglGetPlatformDisplayEXT)
        return GLDisplay::create(eglGetDisplay(EGL_DEFAULT_DISPLAY));

    if (GLContext::isExtensionSupported(extensions, "EGL_MESA_platform_surfaceless")) {
        if (auto display = GLDisplay::create(eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)))
            return display;
    }

    if (eglQueryDevicesEXT && GLContext::isExtensionSupported(extensions, "EGL_EXT_platform_device")) {
        EGLDeviceEXT device = nullptr;
        EGLint count = 0;
        if (eglQueryDevicesEXT(1, &device, &count) && count > 0) {
            if (auto display = GLDisplay::create(eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, device, nullptr)))
                return display;
        }
    }

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
    return std::unique_ptr<PlatformDisplayZeroG>(new PlatformDisplayZeroG(glDisplay.releaseNonNull()));
}

PlatformDisplayZeroG::PlatformDisplayZeroG(Ref<GLDisplay>&& glDisplay)
    : PlatformDisplay(WTF::move(glDisplay))
{
}

PlatformDisplayZeroG::~PlatformDisplayZeroG() = default;

} // namespace WebCore

#endif // PLATFORM(ZEROG)
