#include "config.h"
#include "HTTPCache.h"

#include <WebCore/CacheValidation.h>
#include <WebCore/HTTPHeaderNames.h>
#include <WebCore/HTTPParsers.h>
#include <WebCore/ResourceRequest.h>
#include <filesystem>
#include <wtf/FileSystem.h>
#include <wtf/SHA1.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringToIntegerConversion.h>
#include <wtf/threads/BinarySemaphore.h>

namespace ZeroG {

using namespace WebCore;

// ---- Entries on disk ----
//
// One file per request, named after the SHA-1 of its URL: UTF-8 header lines,
// an empty line, then the body.
//
//   ZeroG HTTP cache 1
//   <URL> / <status code> / <status text> / <HTTP version>
//   <response time, ms since the epoch>
//   <n> varying request headers, then n name and value lines
//   <m> response headers, then m name and value lines
//   (empty line)
//   <body>

static constexpr auto entryMagic = "ZeroG HTTP cache 1"_s;

// One line, whatever the value holds.
static void appendLine(StringBuilder& builder, const String& line)
{
    for (unsigned i = 0; i < line.length(); ++i) {
        char16_t character = line[i];
        if (character == '\n' || character == '\r')
            character = ' ';
        builder.append(character);
    }
    builder.append('\n');
}

static String requestKey(const ResourceRequest& request)
{
    URL url = request.url();
    url.removeFragmentIdentifier();
    return url.string();
}

static Vector<uint8_t> encodeEntry(const HTTPCache::Entry& entry)
{
    auto& response = entry.response;
    StringBuilder header;
    appendLine(header, entryMagic);
    appendLine(header, response.url().string());
    appendLine(header, String::number(response.httpStatusCode()));
    appendLine(header, response.httpStatusText());
    appendLine(header, response.httpVersion());
    appendLine(header, String::number(static_cast<int64_t>(entry.responseTime.secondsSinceEpoch().milliseconds())));
    appendLine(header, String::number(entry.varyingRequestHeaders.size()));
    for (auto& [name, value] : entry.varyingRequestHeaders) {
        appendLine(header, name);
        appendLine(header, value);
    }
    // Cookies are the cookie jar's; a stored response does not set them again.
    Vector<std::pair<String, String>> headers;
    for (auto& field : response.httpHeaderFields()) {
        if (!equalLettersIgnoringASCIICase(field.key, "set-cookie"_s) && !equalLettersIgnoringASCIICase(field.key, "set-cookie2"_s))
            headers.append({ field.key, field.value });
    }
    appendLine(header, String::number(headers.size()));
    for (auto& [name, value] : headers) {
        appendLine(header, name);
        appendLine(header, value);
    }
    header.append('\n');

    auto text = header.toString().utf8();
    Vector<uint8_t> data;
    data.append(byteCast<uint8_t>(text.span()));
    data.append(entry.body->span());
    return data;
}

static std::optional<HTTPCache::Entry> decodeEntry(Vector<uint8_t>&& data)
{
    size_t position = 0;
    auto nextLine = [&]() -> std::optional<String> {
        auto rest = data.span().subspan(position);
        auto end = std::ranges::find(rest, '\n');
        if (end == rest.end())
            return std::nullopt;
        size_t length = end - rest.begin();
        position += length + 1;
        return String::fromUTF8(rest.first(length));
    };
    auto nextNumber = [&]() -> std::optional<int64_t> {
        auto line = nextLine();
        return line ? parseInteger<int64_t>(*line) : std::nullopt;
    };

    auto magic = nextLine();
    if (!magic || *magic != entryMagic)
        return std::nullopt;
    auto url = nextLine();
    auto status = nextNumber();
    auto statusText = nextLine();
    auto version = nextLine();
    auto responseTime = nextNumber();
    if (!url || !status || !statusText || !version || !responseTime)
        return std::nullopt;

    Vector<std::pair<String, String>> varying;
    auto varyingCount = nextNumber();
    if (!varyingCount || *varyingCount < 0 || *varyingCount > 64)
        return std::nullopt;
    for (int64_t i = 0; i < *varyingCount; ++i) {
        auto name = nextLine();
        auto value = nextLine();
        if (!name || !value)
            return std::nullopt;
        varying.append({ *name, *value });
    }

    Vector<std::pair<String, String>> headers;
    auto headerCount = nextNumber();
    if (!headerCount || *headerCount < 0 || *headerCount > 1024)
        return std::nullopt;
    for (int64_t i = 0; i < *headerCount; ++i) {
        auto name = nextLine();
        auto value = nextLine();
        if (!name || !value)
            return std::nullopt;
        headers.append({ *name, *value });
    }
    auto blank = nextLine();
    if (!blank || !blank->isEmpty())
        return std::nullopt;

    auto body = data.span().subspan(position);
    ResourceResponse response { URL { *url }, String { }, static_cast<long long>(body.size()), String { } };
    response.setHTTPStatusCode(static_cast<int>(*status));
    response.setHTTPStatusText(String { *statusText });
    response.setHTTPVersion(String { *version });
    for (auto& [name, value] : headers)
        response.addHTTPHeaderField(name, value);
    auto contentType = response.httpHeaderField(HTTPHeaderName::ContentType);
    response.setMimeType(extractMIMETypeFromMediaType(contentType).convertToASCIILowercase());
    response.setTextEncodingName(extractCharsetFromMediaType(contentType).toString());
    response.setSource(ResourceResponse::Source::DiskCache);

    return HTTPCache::Entry {
        WTF::move(response),
        SharedBuffer::create(body),
        WallTime::fromRawSeconds(Seconds::fromMilliseconds(*responseTime).seconds()),
        WTF::move(varying),
    };
}

// ---- The cache ----

HTTPCache::HTTPCache(const String& directory, uint64_t capacity)
    : m_directory(directory.isolatedCopy())
    , m_capacity(capacity)
    , m_queue(WorkQueue::create("ZeroG HTTP cache"_s))
{
    // What an earlier run left counts against the capacity.
    m_queue->dispatch([this, protectedThis = Ref { *this }] {
        FileSystem::makeAllDirectories(m_directory);
        uint64_t size = 0;
        for (auto& name : FileSystem::listDirectory(m_directory))
            size += FileSystem::fileSize(FileSystem::pathByAppendingComponent(m_directory, name)).value_or(0);
        {
            Locker locker { m_lock };
            m_size = size;
        }
        evictIfNeeded();
    });
}

String HTTPCache::pathForRequest(const ResourceRequest& request) const
{
    SHA1 sha1;
    auto key = requestKey(request).utf8();
    sha1.addBytes(byteCast<uint8_t>(key.span()));
    return FileSystem::pathByAppendingComponent(m_directory, String::fromLatin1(sha1.computeHexDigest().span()));
}

static bool hasConditionalOrRangeHeaders(const ResourceRequest& request)
{
    return request.hasHTTPHeaderField(HTTPHeaderName::IfNoneMatch)
        || request.hasHTTPHeaderField(HTTPHeaderName::IfModifiedSince)
        || request.hasHTTPHeaderField(HTTPHeaderName::IfMatch)
        || request.hasHTTPHeaderField(HTTPHeaderName::IfUnmodifiedSince)
        || request.hasHTTPHeaderField(HTTPHeaderName::IfRange)
        || request.hasHTTPHeaderField(HTTPHeaderName::Range);
}

bool HTTPCache::canUse(const ResourceRequest& request)
{
    if (!request.url().protocolIsInHTTPFamily() || request.httpMethod() != "GET"_s)
        return false;
    if (request.cachePolicy() == ResourceRequestCachePolicy::DoNotUseAnyCache)
        return false;
    // The page validates (WebCore's memory cache revalidating) or wants part of
    // the resource: the request goes as it is. Credentials are not shared.
    return !hasConditionalOrRangeHeaders(request) && !request.hasHTTPHeaderField(HTTPHeaderName::Authorization);
}

bool HTTPCache::canStore(const ResourceRequest& request, const ResourceResponse& response)
{
    if (!canUse(request))
        return false;
    // Final responses only: partial content and redirects are not kept.
    auto status = response.httpStatusCode();
    if (status == 206 || (status >= 300 && status < 400) || !isStatusCodeCacheableByDefault(status))
        return false;
    if (parseCacheControlDirectives(request.httpHeaderFields()).noStore || parseCacheControlDirectives(response.httpHeaderFields()).noStore)
        return false;
    if (response.httpHeaderField(HTTPHeaderName::Vary).contains('*'))
        return false;
    // Something that can be used again: freshness, or a validator.
    if (computeFreshnessLifetimeForHTTPFamily(response, WallTime::now()) > 0_s)
        return true;
    return !response.httpHeaderField(HTTPHeaderName::ETag).isEmpty() || !response.httpHeaderField(HTTPHeaderName::LastModified).isEmpty();
}

std::optional<HTTPCache::Entry> HTTPCache::lookup(const ResourceRequest& request)
{
    auto path = pathForRequest(request);
    auto data = FileSystem::readEntireFile(path);
    if (!data)
        return std::nullopt;
    auto entry = decodeEntry(WTF::move(*data));
    if (!entry || requestKey(request) != entry->response.url().string()) {
        // Unreadable, or a hash collision: not this request's.
        if (!entry)
            remove(request);
        return std::nullopt;
    }
    auto varies = verifyVaryingRequestHeaders(entry->varyingRequestHeaders, [&](const String& name) {
        return request.httpHeaderField(name);
    });
    if (!varies)
        return std::nullopt;
    // Least recently used goes first: a hit is a use.
    m_queue->dispatch([path = path.isolatedCopy()] {
        FileSystem::updateFileModificationTime(path);
    });
    return entry;
}

bool HTTPCache::isFresh(const Entry& entry, const ResourceRequest& request)
{
    auto requestDirectives = parseCacheControlDirectives(request.httpHeaderFields());
    auto responseDirectives = parseCacheControlDirectives(entry.response.httpHeaderFields());
    if (requestDirectives.noCache || responseDirectives.noCache)
        return false;
    if (request.cachePolicy() == ResourceRequestCachePolicy::RefreshAnyCacheData)
        return false;
    auto age = computeCurrentAge(entry.response, entry.responseTime);
    auto lifetime = computeFreshnessLifetimeForHTTPFamily(entry.response, entry.responseTime);
    if (requestDirectives.maxAge)
        lifetime = std::min(lifetime, *requestDirectives.maxAge);
    return age < lifetime;
}

std::optional<ResourceRequest> HTTPCache::conditionalRequest(const ResourceRequest& request, const Entry& entry)
{
    auto etag = entry.response.httpHeaderField(HTTPHeaderName::ETag);
    auto lastModified = entry.response.httpHeaderField(HTTPHeaderName::LastModified);
    if (etag.isEmpty() && lastModified.isEmpty())
        return std::nullopt;
    auto conditional = request;
    if (!etag.isEmpty())
        conditional.setHTTPHeaderField(HTTPHeaderName::IfNoneMatch, etag);
    if (!lastModified.isEmpty())
        conditional.setHTTPHeaderField(HTTPHeaderName::IfModifiedSince, lastModified);
    return conditional;
}

void HTTPCache::store(const ResourceRequest& request, const ResourceResponse& response, Ref<SharedBuffer>&& body, WallTime responseTime)
{
    // One entry may not take more than an eighth of the cache.
    if (body->size() > m_capacity / 8)
        return;
    auto varying = collectVaryingRequestHeaders(response, [&](StringView name) {
        return request.httpHeaderField(name.toString());
    });
    auto stored = response;
    // The response's own URL is the key it is found under.
    stored.setURL(URL { requestKey(request) });
    Entry entry { WTF::move(stored), WTF::move(body), responseTime, WTF::move(varying) };
    writeEntry(pathForRequest(request), encodeEntry(entry));
}

void HTTPCache::update(const ResourceRequest& request, Entry& entry)
{
    entry.response.setURL(URL { requestKey(request) });
    writeEntry(pathForRequest(request), encodeEntry(entry));
}

void HTTPCache::remove(const ResourceRequest& request)
{
    m_queue->dispatch([this, protectedThis = Ref { *this }, path = pathForRequest(request).isolatedCopy()] {
        auto size = FileSystem::fileSize(path).value_or(0);
        if (FileSystem::deleteFile(path)) {
            Locker locker { m_lock };
            m_size -= std::min(m_size, size);
        }
    });
}

void HTTPCache::writeEntry(const String& path, Vector<uint8_t>&& data)
{
    m_queue->dispatch([this, protectedThis = Ref { *this }, path = path.isolatedCopy(), data = WTF::move(data)] {
        // Written aside, then moved over the old entry: a reader never sees half.
        auto temporary = makeString(path, ".tmp"_s);
        auto previous = FileSystem::fileSize(path).value_or(0);
        if (FileSystem::overwriteEntireFile(temporary, data.span()) != data.size()) {
            FileSystem::deleteFile(temporary);
            return;
        }
        FileSystem::deleteFile(path);
        if (!FileSystem::moveFile(temporary, path)) {
            FileSystem::deleteFile(temporary);
            return;
        }
        {
            Locker locker { m_lock };
            m_size = m_size - std::min(m_size, previous) + data.size();
        }
        evictIfNeeded();
    });
}

// When an entry was last used (written, or found by a lookup). Not
// FileSystem::fileModificationTime: it has whole seconds, and entries written in
// the same second would leave in no particular order.
static std::filesystem::file_time_type lastUse(const String& path)
{
    auto utf8 = path.utf8();
    std::error_code error;
    auto time = std::filesystem::last_write_time(std::filesystem::path { std::u8string_view { byteCast<char8_t>(utf8.span()) } }, error);
    return error ? std::filesystem::file_time_type { } : time;
}

// On the work queue: the least recently used entries go until the cache is
// under its capacity.
void HTTPCache::evictIfNeeded()
{
    {
        Locker locker { m_lock };
        if (m_size <= m_capacity)
            return;
    }
    struct File {
        String path;
        std::filesystem::file_time_type used;
        uint64_t size;
    };
    Vector<File> files;
    uint64_t total = 0;
    for (auto& name : FileSystem::listDirectory(m_directory)) {
        auto path = FileSystem::pathByAppendingComponent(m_directory, name);
        auto size = FileSystem::fileSize(path).value_or(0);
        files.append({ path, lastUse(path), size });
        total += size;
    }
    std::ranges::sort(files, { }, &File::used);
    // Down to 90%, so that every store does not evict again.
    auto target = m_capacity / 10 * 9;
    for (auto& file : files) {
        if (total <= target)
            break;
        if (FileSystem::deleteFile(file.path))
            total -= file.size;
    }
    Locker locker { m_lock };
    m_size = total;
}

uint64_t HTTPCache::size() const
{
    Locker locker { m_lock };
    return m_size;
}

void HTTPCache::flush()
{
    BinarySemaphore semaphore;
    m_queue->dispatch([&semaphore] {
        semaphore.signal();
    });
    semaphore.wait();
}

} // namespace ZeroG
