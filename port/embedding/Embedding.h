#pragma once

// The minimal embedding layer of phase 1 (ZeroG.md, "Roadmap"): one process, one
// page with empty clients, HTML loaded from a string, painted on the CPU through
// Skia. The zg_* C API is built on top of this once loading and input exist.

#include <WebCore/IntSize.h>
#include <wtf/Forward.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>

namespace WebCore {
class ImageBuffer;
class Page;
}

namespace ZeroG {

// Initializes WTF, JSC and WebCore for this process. Call once, on the thread that
// becomes the main thread; later calls do nothing.
void initializeProcess();

// Runs the engine's pending work on the main thread without waiting: expired
// timers (animations, setTimeout) and tasks posted from other threads. Texture
// and CPU modes call it once per application tick (zg_runtime_update); window mode
// does it in its loop.
void runPendingWork();

struct PageOptions {
    WebCore::IntSize viewportSize { 800, 600 };
    bool scriptEnabled { true };
};

Ref<WebCore::Page> createPage(const PageOptions&);

// Replaces the main frame's document with `html` and lays it out.
void loadHTML(WebCore::Page&, const String& html);

// Runs `script` in the main frame's normal world and returns the result as a
// string, or a null string when scripts are disabled or the script throws.
String evaluateScript(WebCore::Page&, const String& script);

// Opens the GPU display Skia renders through (ANGLE on Windows, the system EGL
// driver on Linux). Call after initializeProcess(), once; returns whether a GPU is
// available. Without it every paint happens on the CPU. On Linux `hostDisplay`
// may be the application's EGLDisplay, needed for GL texture-mode surfaces.
bool initializeGPU(void* hostDisplay = nullptr);

// The GL_RENDERER string of Skia's GPU context, or a null string without a GPU.
String gpuRenderer();

enum class Rendering : bool { CPU, GPU };

// Updates style and layout, then paints the main frame into a new image. A GPU
// paint falls back to the CPU when no GPU is available; the image's
// renderingMode() tells which one was used.
RefPtr<WebCore::ImageBuffer> paint(WebCore::Page&, Rendering = Rendering::CPU, float deviceScaleFactor = 1);

Vector<uint8_t> encodePNG(WebCore::ImageBuffer&);

} // namespace ZeroG
