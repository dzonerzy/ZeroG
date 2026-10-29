#pragma once

// Every EGL/GLES prototype is visible to the generated entry points, which must
// match it exactly.
#ifndef EGL_EGL_PROTOTYPES
#define EGL_EGL_PROTOTYPES 1
#endif
#ifndef EGL_EGLEXT_PROTOTYPES
#define EGL_EGLEXT_PROTOTYPES 1
#endif
#ifndef GL_GLES_PROTOTYPES
#define GL_GLES_PROTOTYPES 1
#endif
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif

namespace ZeroG::GL {

enum class Library { EGL, GLES };

// The driver's implementation of `name`, or null when the system has no EGL/GLES
// driver or the driver lacks the function. A missing function returns zero
// (EGL_NO_DISPLAY, EGL_FALSE, 0) so the engine falls back to CPU rendering.
void* resolve(Library, const char* name);

} // namespace ZeroG::GL
