// Runtime loading of the system's EGL and GLES driver on Linux (docs/design/
// graphics-presentation.md, "GPU backend per platform": drivers are loaded with dlopen, as games do). Only the C
// library and the GPU driver stay dynamic.

#include "GLLoader.h"

#include <EGL/egl.h>
#include <dlfcn.h>
#include <initializer_list>

namespace ZeroG::GL {

static void* openFirst(std::initializer_list<const char*> names)
{
    for (auto* name : names) {
        if (void* handle = dlopen(name, RTLD_LAZY | RTLD_LOCAL))
            return handle;
    }
    return nullptr;
}

// The real eglGetProcAddress, never the generated entry point of the same name.
static PFNEGLGETPROCADDRESSPROC getProcAddress(void* libEGL)
{
    return libEGL ? reinterpret_cast<PFNEGLGETPROCADDRESSPROC>(dlsym(libEGL, "eglGetProcAddress")) : nullptr;
}

void* resolve(Library library, const char* name)
{
    static void* libEGL = openFirst({ "libEGL.so.1", "libEGL.so" });
    static PFNEGLGETPROCADDRESSPROC eglGetProcAddressFunction = getProcAddress(libEGL);

    void* function = nullptr;
    if (library == Library::EGL)
        function = libEGL ? dlsym(libEGL, name) : nullptr;
    else {
        // Core functions from libGLESv2 (dispatched per context by libglvnd), then
        // extensions through eglGetProcAddress.
        static void* libGLES = openFirst({ "libGLESv2.so.2", "libGLESv2.so" });
        function = libGLES ? dlsym(libGLES, name) : nullptr;
    }
    if (!function && eglGetProcAddressFunction)
        function = reinterpret_cast<void*>(eglGetProcAddressFunction(name));
    return function;
}

} // namespace ZeroG::GL
