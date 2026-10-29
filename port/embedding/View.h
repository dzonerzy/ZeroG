#pragma once

// A view: one page, its size and scale, and the surface it renders into
// (ZeroG.md, "Ciclo di un frame"). Engine thread only, except the surface's
// acquire/release.

#include "Surface.h"
#include <WebCore/IntRect.h>
#include <WebCore/Region.h>
#include <memory>
#include <wtf/MonotonicTime.h>
#include <wtf/Ref.h>
#include <wtf/text/WTFString.h>

namespace WebCore {
class Page;
}

namespace ZeroG {

class View {
    WTF_MAKE_NONCOPYABLE(View);
public:
    struct Configuration {
        WebCore::IntSize size { 800, 600 }; // logical pixels
        float scale { 1 };
        bool transparent { false };
        bool scriptEnabled { true };
        std::unique_ptr<Surface> surface; // null: a CPU surface
    };
    static std::unique_ptr<View> create(Configuration&&);
    ~View();

    WebCore::Page& page() { return m_page.get(); }
    Surface& surface() { return *m_surface; }
    WebCore::IntSize size() const { return m_size; }
    float scale() const { return m_scale; }

    void loadHTML(const String&);
    String evaluateScript(const String&);

    void resize(const WebCore::IntSize&);
    void setScale(float);

    // One frame of the application's loop: runs the rendering update (animations,
    // requestAnimationFrame at `frameTime`) if one is pending, lays out, and
    // repaints the damaged region into a surface buffer. Returns whether a frame
    // was published.
    bool render(MonotonicTime frameTime);

    // Number of pixels painted by the last published frame, for tests.
    uint64_t lastPaintedArea() const { return m_lastPaintedArea; }

    // From ChromeClientZeroG.
    void invalidate(const WebCore::IntRect& rootViewRect);
    void setNeedsRenderingUpdate() { m_needsRenderingUpdate = true; }

private:
    explicit View(Configuration&&);
    void invalidateAll() { invalidate({ { }, m_size }); }
    WebCore::IntSize physicalSize() const;

    const Ref<WebCore::Page> m_page;
    std::unique_ptr<Surface> m_surface;
    WebCore::IntSize m_size;
    float m_scale { 1 };
    bool m_transparent { false };
    bool m_needsRenderingUpdate { true };
    WebCore::Region m_damage; // logical pixels
    uint64_t m_lastPaintedArea { 0 };
};

} // namespace ZeroG
