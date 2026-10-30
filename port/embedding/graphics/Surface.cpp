#include "config.h"
#include "Surface.h"

#include <WebCore/FontRenderOptions.h>
#include <wtf/StdLibExtras.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkCanvas.h>
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkImage.h>
#include <skia/core/SkImageInfo.h>
#include <skia/core/SkPaint.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace ZeroG {

using namespace WebCore;

Surface::Surface(SurfaceKind kind, unsigned bufferCount)
    : m_kind(kind)
{
    bufferCount = std::clamp(bufferCount ? bufferCount : 2u, 2u, 3u);
    Locker locker { m_lock };
    for (unsigned i = 0; i < bufferCount; ++i)
        m_buffers.append(std::make_unique<Buffer>());
}

Surface::~Surface() = default;

void Surface::resize(const IntSize& physicalSize)
{
    {
        Locker locker { m_lock };
        if (physicalSize == m_size)
            return;
        m_size = physicalSize;
        ++m_generation;
        for (auto& buffer : m_buffers) {
            if (buffer->state == BufferState::Acquired)
                m_retired.append(WTF::move(buffer));
            else if (buffer->skSurface)
                destroyBuffer(*buffer);
            buffer = std::make_unique<Buffer>();
            buffer->generation = m_generation;
        }
        m_newest = nullptr;
        m_rendering = nullptr;
        m_hasUnacquiredFrame = false;
        m_dirtySinceAcquire = { { }, m_size };
        m_privateTarget = nullptr;
    }
    collectGarbage();
}

void Surface::destroyAllBuffers()
{
    Locker locker { m_lock };
    for (auto* list : { &m_buffers, &m_retired }) {
        for (auto& buffer : *list) {
            if (buffer->skSurface || buffer->backend)
                destroyBuffer(*buffer);
        }
    }
    m_retired.clear();
    m_newest = nullptr;
    m_rendering = nullptr;
    m_hasUnacquiredFrame = false;
    m_privateTarget = nullptr;
}

void Surface::collectGarbage()
{
    Locker locker { m_lock };
    m_retired.removeAllMatching([&](auto& buffer) {
        if (buffer->state == BufferState::Acquired)
            return false;
        if (buffer->skSurface)
            destroyBuffer(*buffer);
        return true;
    });
}

Surface::Buffer* Surface::pickBuffer()
{
    // A free buffer first; otherwise the newest frame if the application has not
    // taken it yet, which a newer frame replaces anyway.
    for (auto& buffer : m_buffers) {
        if (buffer->state == BufferState::Free)
            return buffer.get();
    }
    for (auto& buffer : m_buffers) {
        if (buffer->state == BufferState::Ready)
            return buffer.get();
    }
    return nullptr;
}

std::optional<Surface::Target> Surface::beginFrame(const IntRect& damage)
{
    collectGarbage();

    Buffer* buffer;
    Region paintRegion;
    {
        Locker locker { m_lock };
        RELEASE_ASSERT(!m_rendering);
        if (m_size.isEmpty())
            return std::nullopt;
        buffer = pickBuffer();
        if (!buffer)
            return std::nullopt;
        if (!buffer->skSurface) {
            if (!createBuffer(*buffer, m_size))
                return std::nullopt;
            buffer->missedDamage = IntRect { { }, m_size };
        }
        if (buffer == m_newest) {
            m_newest = nullptr;
            m_hasUnacquiredFrame = false;
        }

        IntRect bounds { { }, m_size };
        auto frameDamage = intersection(damage, bounds);
        if (usesPrivateTarget()) {
            // Only the persistent private target is painted incrementally; the
            // buffer is overwritten whole in endFrame().
            if (!m_privateTarget) {
                m_privateTarget = createPrivateTarget(m_size);
                if (!m_privateTarget)
                    return std::nullopt;
                frameDamage = bounds;
            }
            paintRegion = frameDamage;
        } else {
            paintRegion = buffer->missedDamage;
            paintRegion.unite(frameDamage);
            paintRegion.intersect(bounds);
            for (auto& other : m_buffers) {
                if (other.get() != buffer)
                    other->missedDamage.unite(frameDamage);
            }
            buffer->missedDamage = { };
        }
        buffer->state = BufferState::Rendering;
        m_rendering = buffer;
        // The application sees what changed in the page, not what this buffer
        // had to catch up on.
        m_pendingDamage = frameDamage;
    }

    // Outside the lock: a backend may wait here for the GPU or for the
    // application's last use of this buffer.
    if (!willRender(*buffer)) {
        Locker locker { m_lock };
        buffer->missedDamage = IntRect { { }, m_size };
        buffer->state = BufferState::Free;
        m_rendering = nullptr;
        return std::nullopt;
    }
    auto& canvas = m_privateTarget ? *m_privateTarget->getCanvas() : *buffer->skSurface->getCanvas();
    return Target { canvas, WTF::move(paintRegion) };
}

void Surface::endFrame()
{
    Buffer* buffer;
    {
        Locker locker { m_lock };
        buffer = std::exchange(m_rendering, nullptr);
        RELEASE_ASSERT(buffer);
    }

    if (m_privateTarget) {
        SkPaint paint;
        paint.setBlendMode(SkBlendMode::kSrc);
        buffer->skSurface->getCanvas()->drawImage(m_privateTarget->makeImageSnapshot(), 0, 0, SkSamplingOptions(), &paint);
    }
    didRender(*buffer);

    Locker locker { m_lock };
    if (m_newest && m_newest->state == BufferState::Ready)
        m_newest->state = BufferState::Free;
    buffer->state = BufferState::Ready;
    buffer->published = true;
    buffer->acquiredSinceRender = false;
    m_newest = buffer;
    m_hasUnacquiredFrame = true;
    m_dirtySinceAcquire.unite(m_pendingDamage);
}

bool Surface::acquire(Frame& frame)
{
    Buffer* buffer;
    {
        Locker locker { m_lock };
        if (!m_hasUnacquiredFrame || !m_newest)
            return false;
        buffer = m_newest;
        buffer->state = BufferState::Acquired;
        buffer->acquiredSinceRender = true;
        m_hasUnacquiredFrame = false;

        frame = { };
        frame.width = m_size.width();
        frame.height = m_size.height();
        frame.dirty = std::exchange(m_dirtySinceAcquire, { });
        frame.handle = buffer;
    }
    willAcquire(*buffer);
    fillFrame(*buffer, frame);
    return true;
}

void Surface::release(const Frame& frame)
{
    auto* buffer = static_cast<Buffer*>(frame.handle);
    if (!buffer)
        return;
    didRelease(*buffer);
    Locker locker { m_lock };
    RELEASE_ASSERT(buffer->state == BufferState::Acquired);
    buffer->state = BufferState::Free;
}

// CPU surface: pixels in memory, which Skia rasterizes into directly.

namespace {

class CPUSurface final : public Surface {
public:
    explicit CPUSurface(unsigned bufferCount)
        : Surface(SurfaceKind::CPU, bufferCount)
    {
    }

    ~CPUSurface()
    {
        destroyAllBuffers();
    }

private:
    struct Pixels {
        Vector<uint8_t> bytes;
        size_t stride { 0 };
    };

    bool createBuffer(Buffer& buffer, const IntSize& size) final
    {
        auto pixels = std::make_unique<Pixels>();
        auto info = SkImageInfo::MakeN32Premul(size.width(), size.height(), SkColorSpace::MakeSRGB());
        pixels->stride = info.minRowBytes();
        pixels->bytes.grow(pixels->stride * size.height());
        auto properties = FontRenderOptions::singleton().createSurfaceProps();
        buffer.skSurface = SkSurfaces::WrapPixels(info, pixels->bytes.mutableSpan().data(), pixels->stride, &properties);
        if (!buffer.skSurface)
            return false;
        buffer.backend = pixels.release();
        return true;
    }

    void destroyBuffer(Buffer& buffer) final
    {
        buffer.skSurface = nullptr;
        delete static_cast<Pixels*>(std::exchange(buffer.backend, nullptr));
    }

    void fillFrame(Buffer& buffer, Frame& frame) final
    {
        auto& pixels = *static_cast<Pixels*>(buffer.backend);
        frame.pixels = pixels.bytes.span().data();
        frame.stride = pixels.stride;
    }
};

} // namespace

std::unique_ptr<Surface> createCPUSurface(unsigned bufferCount)
{
    return std::make_unique<CPUSurface>(bufferCount);
}

} // namespace ZeroG
