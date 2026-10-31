#include "config.h"
#include "media/MediaBytes.h"

#if ENABLE(VIDEO) && defined(ZEROG_MEDIA)

namespace ZeroG::Media {

void MediaBytes::setExpectedLength(std::optional<uint64_t> length)
{
    Locker locker { m_lock };
    m_expectedLength = length;
    if (length)
        m_bytes.reserveCapacity(std::min<uint64_t>(*length, 256 * 1024 * 1024));
    m_condition.notifyAll();
}

void MediaBytes::append(std::span<const uint8_t> data)
{
    Locker locker { m_lock };
    m_bytes.append(data);
    m_condition.notifyAll();
}

void MediaBytes::finish()
{
    Locker locker { m_lock };
    m_complete = true;
    m_condition.notifyAll();
}

void MediaBytes::fail()
{
    Locker locker { m_lock };
    m_failed = true;
    m_complete = true;
    m_condition.notifyAll();
}

void MediaBytes::abort()
{
    Locker locker { m_lock };
    m_aborted = true;
    m_condition.notifyAll();
}

size_t MediaBytes::read(uint64_t position, std::span<uint8_t> out)
{
    Locker locker { m_lock };
    while (!m_aborted && !m_complete && m_bytes.size() < position + out.size())
        m_condition.wait(m_lock);
    if (m_aborted || position >= m_bytes.size())
        return 0;
    size_t count = std::min<uint64_t>(out.size(), m_bytes.size() - position);
    memcpy(out.data(), m_bytes.span().subspan(position, count).data(), count);
    return count;
}

std::optional<uint64_t> MediaBytes::length() const
{
    Locker locker { m_lock };
    if (m_complete)
        return m_bytes.size();
    return m_expectedLength;
}

uint64_t MediaBytes::available() const
{
    Locker locker { m_lock };
    return m_bytes.size();
}

bool MediaBytes::isComplete() const
{
    Locker locker { m_lock };
    return m_complete;
}

bool MediaBytes::isFailed() const
{
    Locker locker { m_lock };
    return m_failed;
}

bool MediaBytes::isAborted() const
{
    Locker locker { m_lock };
    return m_aborted;
}

bool MediaReader::seek(int64_t offset, Origin origin)
{
    int64_t base = 0;
    switch (origin) {
    case Origin::Start:
        break;
    case Origin::Current:
        base = m_position;
        break;
    case Origin::End: {
        // The end is known once the length is: the response's, or after the
        // last byte arrived.
        auto length = m_bytes.length();
        if (!length) {
            uint8_t byte;
            while (!m_bytes.isComplete() && !m_bytes.isAborted() && m_bytes.read(m_bytes.available(), { &byte, 1 })) { }
            length = m_bytes.length();
        }
        if (!length)
            return false;
        base = *length;
        break;
    }
    }
    if (base + offset < 0)
        return false;
    m_position = base + offset;
    return true;
}

} // namespace ZeroG::Media

#endif
