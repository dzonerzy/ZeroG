#pragma once

// A media resource's bytes as they arrive from the page's loader (main thread),
// read by the player's decode thread at any position. Reads past what has
// arrived wait for it, so demuxers and decoders read a file that is still
// downloading as if it were all there.

#include <optional>
#include <span>
#include <wtf/Condition.h>
#include <wtf/Lock.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Vector.h>

namespace ZeroG::Media {

class MediaBytes : public ThreadSafeRefCounted<MediaBytes> {
public:
    static Ref<MediaBytes> create() { return adoptRef(*new MediaBytes); }

    // Main thread, from the loader.
    void setExpectedLength(std::optional<uint64_t>);
    void append(std::span<const uint8_t>);
    void finish(); // all the bytes are there
    void fail();

    // Any thread: ends every wait, for good (the player is going).
    void abort();

    // Decode thread. Copies up to `out.size()` bytes at `position`, waiting
    // until they have arrived; fewer at the end, 0 past it or when aborted.
    size_t read(uint64_t position, std::span<uint8_t> out);
    // The length once known: the response's, or what arrived once finished.
    std::optional<uint64_t> length() const;
    // What has arrived so far, and whether that is all.
    uint64_t available() const;
    bool isComplete() const;
    bool isFailed() const;
    bool isAborted() const;

private:
    MediaBytes() = default;

    mutable Lock m_lock;
    Condition m_condition;
    Vector<uint8_t> m_bytes WTF_GUARDED_BY_LOCK(m_lock);
    std::optional<uint64_t> m_expectedLength WTF_GUARDED_BY_LOCK(m_lock);
    bool m_complete WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_failed WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_aborted WTF_GUARDED_BY_LOCK(m_lock) { false };
};

// A read position over MediaBytes, for the libraries' read/seek/tell callbacks.
class MediaReader {
public:
    explicit MediaReader(MediaBytes& bytes)
        : m_bytes(bytes)
    {
    }

    size_t read(std::span<uint8_t> out)
    {
        size_t count = m_bytes.read(m_position, out);
        m_position += count;
        return count;
    }
    enum class Origin : uint8_t { Start, Current, End };
    bool seek(int64_t offset, Origin);
    uint64_t position() const { return m_position; }
    MediaBytes& bytes() const { return m_bytes; }

private:
    MediaBytes& m_bytes;
    uint64_t m_position { 0 };
};

} // namespace ZeroG::Media
