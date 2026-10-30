// WebGL (the standard, dev and debug profiles build it): a WebGL canvas needs a
// composited layer to show its drawing buffer, and accelerated compositing is
// off until the compositor (compositing version 2) replaces GraphicsLayerZeroG.
// Until then no context can be created: getContext('webgl') returns null, as in a
// browser whose GPU is blocklisted. Upstream's EGL implementation
// (platform/graphics/egl/GraphicsContextGLEGL.cpp) needs coordinated graphics.

#include "config.h"
#include "GraphicsContextGL.h"

#if ENABLE(WEBGL)

namespace WebCore {

RefPtr<GraphicsContextGL> createWebProcessGraphicsContextGL(const GraphicsContextGLAttributes&)
{
    return nullptr;
}

} // namespace WebCore

#endif // ENABLE(WEBGL)
