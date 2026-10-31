#include "config.h"
#include "storage/CacheStorageZeroG.h"

#include <WebCore/CacheQueryOptions.h>
#include <WebCore/CacheStorageConnection.h>
#include <WebCore/ClientOrigin.h>
#include <WebCore/CrossOriginAccessControl.h>
#include <WebCore/DOMCacheEngine.h>
#include <WebCore/FormData.h>
#include <WebCore/OriginAccessPatterns.h>
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceResponse.h>
#include <WebCore/RetrieveRecordsOptions.h>
#include <WebCore/SharedBuffer.h>
#include <WebCore/WebCorePersistentCoders.h>
#include <wtf/FileSystem.h>
#include <wtf/HashMap.h>
#include <wtf/MainThread.h>
#include <wtf/WorkQueue.h>
#include <wtf/persistence/PersistentCoders.h>
#include <wtf/persistence/PersistentDecoder.h>
#include <wtf/persistence/PersistentEncoder.h>
#include <wtf/text/Base64.h>

namespace ZeroG {

using namespace WebCore;
using namespace WebCore::DOMCacheEngine;

namespace {

// A file holds one origin's caches; the version changes with the format.
constexpr uint32_t fileVersion = 1;
// Space for one origin (WebKit2 asks the user beyond its quota; an
// application's caches have a fixed one).
constexpr uint64_t originQuota = 512 * 1024 * 1024;

struct StoredRecord {
    uint64_t identifier { 0 };
    FetchHeaders::Guard requestHeadersGuard { FetchHeaders::Guard::None };
    ResourceRequest request;
    FetchOptions options;
    String referrer;
    FetchHeaders::Guard responseHeadersGuard { FetchHeaders::Guard::None };
    ResourceResponse response;
    ResponseBody body { nullptr };
    uint64_t bodySize { 0 };
};

struct Cache {
    DOMCacheIdentifier identifier;
    String name;
    Vector<StoredRecord> records;
};

struct OriginCaches {
    Vector<Cache> caches; // in creation order, as caches.keys() lists them
    uint64_t updateCounter { 0 };
    uint64_t nextRecordIdentifier { 1 };
};

bool isGet(const ResourceRequest& request)
{
    return request.httpMethod() == "GET"_s;
}

std::span<const uint8_t> bodyBytes(const ResponseBody& body)
{
    if (auto* buffer = std::get_if<Ref<SharedBuffer>>(&body))
        return buffer->get().span();
    return { }; // form data bodies are not kept, as in WebKit2
}

class Engine : public RefCounted<Engine> {
public:
    static Ref<Engine> create(const String& directory) { return adoptRef(*new Engine(directory)); }

    CacheIdentifierOperationResult open(const ClientOrigin& clientOrigin, const String& name)
    {
        auto& origin = originFor(clientOrigin);
        for (auto& cache : origin.caches) {
            if (cache.name == name)
                return { cache.identifier, false };
        }
        auto identifier = DOMCacheIdentifier::generate();
        origin.caches.append({ identifier, name, { } });
        m_cacheOrigins.set(identifier, key(clientOrigin));
        ++origin.updateCounter;
        save(key(clientOrigin));
        return { identifier, false };
    }

    bool remove(DOMCacheIdentifier identifier)
    {
        auto originKey = m_cacheOrigins.take(identifier);
        auto origin = m_origins.find(originKey);
        if (origin == m_origins.end())
            return false;
        bool removed = origin->value.caches.removeFirstMatching([&](auto& cache) {
            return cache.identifier == identifier;
        });
        if (removed) {
            ++origin->value.updateCounter;
            save(originKey);
        }
        return removed;
    }

    CacheInfos caches(const ClientOrigin& clientOrigin)
    {
        auto& origin = originFor(clientOrigin);
        CacheInfos infos { { }, origin.updateCounter };
        for (auto& cache : origin.caches)
            infos.infos.append({ cache.identifier, cache.name });
        return infos;
    }

    std::expected<Vector<CrossThreadRecord>, Error> records(DOMCacheIdentifier identifier, const RetrieveRecordsOptions& options)
    {
        auto* cache = cacheFor(identifier);
        if (!cache)
            return makeUnexpected(Error::Internal);
        Vector<CrossThreadRecord> result;
        bool matchAll = options.request.url().isNull();
        if (!matchAll && !options.ignoreMethod && !isGet(options.request))
            return result;
        CacheQueryOptions queryOptions { options.ignoreSearch, options.ignoreMethod, options.ignoreVary };
        for (auto& record : cache->records) {
            if (!matchAll && !queryCacheMatch(options.request, record.request, record.response, queryOptions))
                continue;
            // An opaque response is only for those its resource policy allows.
            if (record.response.type() == ResourceResponse::Type::Opaque
                && validateCrossOriginResourcePolicy(options.crossOriginEmbedderPolicy.value, options.documentIsolationPolicy, options.sourceOrigin, record.request.url(), record.response, ForNavigation::No, EmptyOriginAccessPatterns::singleton()))
                return makeUnexpected(Error::CORP);
            CrossThreadRecord copy { record.identifier, 0, record.requestHeadersGuard, ResourceRequest { record.request }, record.options, String { record.referrer },
                record.responseHeadersGuard, { }, nullptr, 0 };
            if (options.shouldProvideResponse) {
                copy.response = record.response.crossThreadData();
                copy.responseBody = copyResponseBody(record.body);
                copy.responseBodySize = record.bodySize;
            }
            result.append(WTF::move(copy).isolatedCopy());
        }
        return result;
    }

    std::expected<Vector<uint64_t>, Error> remove(DOMCacheIdentifier identifier, const ResourceRequest& request, const CacheQueryOptions& options)
    {
        auto* cache = cacheFor(identifier);
        if (!cache)
            return makeUnexpected(Error::Internal);
        Vector<uint64_t> removed;
        if (!options.ignoreMethod && !isGet(request))
            return removed;
        cache->records.removeAllMatching([&](auto& record) {
            if (!queryCacheMatch(request, record.request, record.response, options))
                return false;
            removed.append(record.identifier);
            return true;
        });
        if (!removed.isEmpty())
            save(m_cacheOrigins.get(identifier));
        return removed;
    }

    std::expected<Vector<uint64_t>, Error> put(DOMCacheIdentifier identifier, Vector<CrossThreadRecord>&& records)
    {
        auto* cache = cacheFor(identifier);
        auto originKey = m_cacheOrigins.get(identifier);
        auto origin = m_origins.find(originKey);
        if (!cache || origin == m_origins.end())
            return makeUnexpected(Error::Internal);
        // The quota: what the origin keeps, with these records replacing the
        // ones they match.
        uint64_t used = 0;
        for (auto& each : origin->value.caches) {
            for (auto& record : each.records)
                used += record.bodySize;
        }
        Vector<uint64_t> identifiers;
        for (auto& incoming : records) {
            StoredRecord record;
            record.requestHeadersGuard = incoming.requestHeadersGuard;
            record.request = WTF::move(incoming.request);
            record.options = incoming.options;
            record.referrer = WTF::move(incoming.referrer);
            record.responseHeadersGuard = incoming.responseHeadersGuard;
            record.response = ResourceResponse::fromCrossThreadData(WTF::move(incoming.response));
            record.body = isolatedResponseBody(incoming.responseBody);
            record.bodySize = incoming.responseBodySize;
            // A record for the same request replaces the one there.
            auto existing = cache->records.findIf([&](auto& stored) {
                return queryCacheMatch(record.request, stored.request, stored.response, { });
            });
            uint64_t replaced = existing == notFound ? 0 : cache->records[existing].bodySize;
            if (used - replaced + record.bodySize > originQuota)
                return makeUnexpected(Error::QuotaExceeded);
            used = used - replaced + record.bodySize;
            if (existing != notFound) {
                record.identifier = cache->records[existing].identifier;
                cache->records[existing] = WTF::move(record);
            } else {
                record.identifier = origin->value.nextRecordIdentifier++;
                cache->records.append(WTF::move(record));
            }
            identifiers.append(cache->records[existing == notFound ? cache->records.size() - 1 : existing].identifier);
        }
        save(originKey);
        return identifiers;
    }

private:
    explicit Engine(const String& directory)
        : m_directory(directory)
        , m_queue(WorkQueue::create("ZeroG cache storage"_s))
    {
    }

    static String key(const ClientOrigin& origin)
    {
        return origin.loggingString();
    }

    String pathFor(const String& originKey) const
    {
        // The origin's name, made a file name.
        auto name = base64URLEncodeToString(asBytes(originKey.utf8().span()));
        return FileSystem::pathByAppendingComponent(m_directory, makeString(name, ".cache"_s));
    }

    OriginCaches& originFor(const ClientOrigin& clientOrigin)
    {
        auto originKey = key(clientOrigin);
        auto result = m_origins.ensure(originKey, [] {
            return OriginCaches { };
        });
        if (result.isNewEntry && !m_directory.isEmpty())
            load(originKey, result.iterator->value);
        return result.iterator->value;
    }

    Cache* cacheFor(DOMCacheIdentifier identifier)
    {
        auto origin = m_origins.find(m_cacheOrigins.get(identifier));
        if (origin == m_origins.end())
            return nullptr;
        for (auto& cache : origin->value.caches) {
            if (cache.identifier == identifier)
                return &cache;
        }
        return nullptr;
    }

    // The whole origin is written again on each change, off the main thread.
    void save(const String& originKey)
    {
        if (m_directory.isEmpty())
            return;
        auto origin = m_origins.find(originKey);
        if (origin == m_origins.end())
            return;
        WTF::Persistence::Encoder encoder;
        encoder << fileVersion;
        encoder << static_cast<uint64_t>(origin->value.caches.size());
        for (auto& cache : origin->value.caches) {
            encoder << cache.name;
            encoder << static_cast<uint64_t>(cache.records.size());
            for (auto& record : cache.records) {
                encoder << record.requestHeadersGuard;
                encoder << record.request;
                record.options.encodePersistent(encoder);
                encoder << record.referrer;
                encoder << record.responseHeadersGuard;
                encoder << record.response.crossThreadData();
                encoder << record.bodySize;
                encoder << Vector<uint8_t> { bodyBytes(record.body) };
            }
        }
        encoder.encodeChecksum();
        m_queue->dispatch([directory = m_directory.isolatedCopy(), path = pathFor(originKey).isolatedCopy(), data = Vector<uint8_t> { encoder.span() }] {
            FileSystem::makeAllDirectories(directory);
            auto temporary = makeString(path, ".tmp"_s);
            if (FileSystem::overwriteEntireFile(temporary, data.span()))
                FileSystem::moveFile(temporary, path);
        });
    }

    void load(const String& originKey, OriginCaches& origin)
    {
        // A write may be pending for the file: wait for it.
        m_queue->dispatchSync([] { });
        auto data = FileSystem::readEntireFile(pathFor(originKey));
        if (!data)
            return;
        WTF::Persistence::Decoder decoder(data->span());
        std::optional<uint32_t> version;
        decoder >> version;
        std::optional<uint64_t> cacheCount;
        decoder >> cacheCount;
        if (!version || *version != fileVersion || !cacheCount)
            return;
        Vector<Cache> caches;
        uint64_t nextRecordIdentifier = 1;
        for (uint64_t i = 0; i < *cacheCount; ++i) {
            std::optional<String> name;
            decoder >> name;
            std::optional<uint64_t> recordCount;
            decoder >> recordCount;
            if (!name || !recordCount)
                return;
            Cache cache { DOMCacheIdentifier::generate(), WTF::move(*name), { } };
            for (uint64_t j = 0; j < *recordCount; ++j) {
                StoredRecord record;
                std::optional<FetchHeaders::Guard> requestHeadersGuard;
                decoder >> requestHeadersGuard;
                std::optional<ResourceRequest> request;
                decoder >> request;
                if (!requestHeadersGuard || !request || !FetchOptions::decodePersistent(decoder, record.options))
                    return;
                std::optional<String> referrer;
                decoder >> referrer;
                std::optional<FetchHeaders::Guard> responseHeadersGuard;
                decoder >> responseHeadersGuard;
                std::optional<ResourceResponseData> response;
                decoder >> response;
                std::optional<uint64_t> bodySize;
                decoder >> bodySize;
                std::optional<Vector<uint8_t>> body;
                decoder >> body;
                if (!referrer || !responseHeadersGuard || !response || !bodySize || !body)
                    return;
                record.identifier = nextRecordIdentifier++;
                record.requestHeadersGuard = *requestHeadersGuard;
                record.request = WTF::move(*request);
                record.referrer = WTF::move(*referrer);
                record.responseHeadersGuard = *responseHeadersGuard;
                record.response = ResourceResponse::fromCrossThreadData(WTF::move(*response));
                record.body = SharedBuffer::create(WTF::move(*body));
                record.bodySize = *bodySize;
                cache.records.append(WTF::move(record));
            }
            caches.append(WTF::move(cache));
        }
        if (!decoder.verifyChecksum())
            return;
        for (auto& cache : caches)
            m_cacheOrigins.set(cache.identifier, originKey);
        origin.caches = WTF::move(caches);
        origin.nextRecordIdentifier = nextRecordIdentifier;
        // DOMCacheStorage starts at counter 0 and keeps its (empty) list while the
        // counter is unchanged.
        origin.updateCounter = 1;
    }

    const String m_directory;
    const Ref<WorkQueue> m_queue;
    HashMap<String, OriginCaches> m_origins;
    HashMap<DOMCacheIdentifier, String> m_cacheOrigins;
};

class Connection final : public CacheStorageConnection {
public:
    static Ref<Connection> create(Engine& engine) { return adoptRef(*new Connection(engine)); }

private:
    explicit Connection(Engine& engine)
        : m_engine(engine)
    {
    }

    Ref<OpenPromise> open(const ClientOrigin& origin, const String& name) final
    {
        ASSERT(isMainThread());
        // Settled through a function, as WebKit2's IPC replies are: the result has
        // no isolatedCopy() and never leaves the main thread (workers' connections
        // copy what they forward).
        OpenPromise::Producer producer;
        producer.settle([result = m_engine->open(origin, name)]() -> OpenPromise::Result { return result; });
        return producer.promise();
    }

    Ref<RemovePromise> remove(DOMCacheIdentifier identifier) final
    {
        return RemovePromise::createAndResolve(m_engine->remove(identifier));
    }

    Ref<RetrieveCachesPromise> retrieveCaches(const ClientOrigin& origin, uint64_t) final
    {
        return RetrieveCachesPromise::createAndResolve(m_engine->caches(origin));
    }

    Ref<RetrieveRecordsPromise> retrieveRecords(DOMCacheIdentifier identifier, RetrieveRecordsOptions&& options) final
    {
        auto result = m_engine->records(identifier, options);
        if (!result)
            return RetrieveRecordsPromise::createAndReject(result.error());
        return RetrieveRecordsPromise::createAndResolve(WTF::move(*result));
    }

    Ref<BatchPromise> batchDeleteOperation(DOMCacheIdentifier identifier, const ResourceRequest& request, CacheQueryOptions&& options) final
    {
        auto result = m_engine->remove(identifier, request, options);
        if (!result)
            return BatchPromise::createAndReject(result.error());
        return BatchPromise::createAndResolve(WTF::move(*result));
    }

    Ref<BatchPromise> batchPutOperation(DOMCacheIdentifier identifier, Vector<CrossThreadRecord>&& records) final
    {
        auto result = m_engine->put(identifier, WTF::move(records));
        if (!result)
            return BatchPromise::createAndReject(result.error());
        return BatchPromise::createAndResolve(WTF::move(*result));
    }

    // Caches live until deleted, referenced or not; one process, no locking.
    void reference(DOMCacheIdentifier) final { }
    void dereference(DOMCacheIdentifier) final { }
    void lockStorage(const ClientOrigin&) final { }
    void unlockStorage(const ClientOrigin&) final { }

    const Ref<Engine> m_engine;
};

class Provider final : public CacheStorageProvider {
public:
    static Ref<Provider> create(const String& directory) { return adoptRef(*new Provider(directory)); }

    Ref<CacheStorageConnection> createCacheStorageConnection() final { return Connection::create(m_engine); }

private:
    explicit Provider(const String& directory)
        : m_engine(Engine::create(directory))
    {
    }

    const Ref<Engine> m_engine;
};

} // namespace

Ref<CacheStorageProvider> createCacheStorageProvider(const String& directory)
{
    return Provider::create(directory);
}

} // namespace ZeroG
