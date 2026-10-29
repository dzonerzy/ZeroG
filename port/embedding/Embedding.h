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

// Updates style and layout, then paints the main frame into a new BGRA image.
RefPtr<WebCore::ImageBuffer> paint(WebCore::Page&, float deviceScaleFactor = 1);

Vector<uint8_t> encodePNG(WebCore::ImageBuffer&);

} // namespace ZeroG
