#pragma once

// A session's HTTP disk cache (ZeroG.md, "Cache HTTP"): WebKit's disk cache lives
// in WebKit2's network process, and curl has none, so the port keeps its own,
// under the session's directory (HTTPCache/). Freshness and revalidation follow
// HTTP caching as WebCore's CacheValidation implements it: max-age, Expires,
// heuristic freshness from Last-Modified, no-store, no-cache, Vary, and
// conditional requests with ETag and Last-Modified, a 304 refreshing the stored
// headers. GET responses only; least recently used entries go first.
//
// Lookups read on the engine thread; writes and eviction run on a work queue.

#include <WebCore/ResourceResponse.h>
#include <WebCore/SharedBuffer.h>
#include <optional>
#include <wtf/Lock.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/WallTime.h>
#include <wtf/WorkQueue.h>
#include <wtf/text/WTFString.h>

namespace WebCore {
class ResourceRequest;
}

namespace ZeroG {

class HTTPCache final : public ThreadSafeRefCounted<HTTPCache> {
public:
    static Ref<HTTPCache> create(const String& directory, uint64_t capacity) { return adoptRef(*new HTTPCache(directory, capacity)); }

    struct Entry {
        WebCore::ResourceResponse response;
        Ref<WebCore::SharedBuffer> body;
        WallTime responseTime; // when the response (or its last revalidation) arrived
        Vector<std::pair<String, String>> varyingRequestHeaders;
    };

    // The request may use the cache (GET over http/https, a cache policy that
    // allows it, no conditional headers of the page's own).
    static bool canUse(const WebCore::ResourceRequest&);
    // The response may be stored for the request.
    static bool canStore(const WebCore::ResourceRequest&, const WebCore::ResourceResponse&);

    // The stored response for the request, if its Vary headers match.
    std::optional<Entry> lookup(const WebCore::ResourceRequest&);
    // Usable without asking the server.
    static bool isFresh(const Entry&, const WebCore::ResourceRequest&);
    // The request with the entry's validators (If-None-Match, If-Modified-Since);
    // nullopt if it has none.
    static std::optional<WebCore::ResourceRequest> conditionalRequest(const WebCore::ResourceRequest&, const Entry&);

    void store(const WebCore::ResourceRequest&, const WebCore::ResourceResponse&, Ref<WebCore::SharedBuffer>&& body, WallTime responseTime);
    // After a 304: the entry's headers merged with the validating response's.
    void update(const WebCore::ResourceRequest&, Entry&);
    void remove(const WebCore::ResourceRequest&);

    uint64_t capacity() const { return m_capacity; }
    // Stored bytes (approximate while writes are queued).
    uint64_t size() const;
    // Waits for queued writes (tests).
    void flush();

private:
    HTTPCache(const String& directory, uint64_t capacity);

    String pathForRequest(const WebCore::ResourceRequest&) const;
    void writeEntry(const String& path, Vector<uint8_t>&&);
    void evictIfNeeded();

    const String m_directory;
    const uint64_t m_capacity;
    const Ref<WorkQueue> m_queue;
    mutable Lock m_lock;
    uint64_t m_size WTF_GUARDED_BY_LOCK(m_lock) { 0 };
};

} // namespace ZeroG
