#include "config.h"
#include "MemoryFS.h"

#include <WebCore/HTTPHeaderNames.h>
#include <WebCore/HTTPParsers.h>
#include <WebCore/MIMETypeRegistry.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>
#include <zlib.h>

namespace ZeroG {

using namespace WebCore;

// ---- Files ----

class MemoryFS::File : public ThreadSafeRefCounted<File> {
public:
    enum class Compression : uint8_t { None, Zlib };

    static Ref<File> copy(std::span<const uint8_t> data, const String& mimeType)
    {
        auto file = adoptRef(*new File(mimeType, data.size(), Compression::None));
        file->m_owned = data;
        file->m_data = file->m_owned.span();
        return file;
    }

    static Ref<File> external(std::span<const uint8_t> data, const String& mimeType, Function<void()>&& release)
    {
        auto file = adoptRef(*new File(mimeType, data.size(), Compression::None));
        file->m_data = data;
        file->m_release = WTF::move(release);
        return file;
    }

    static Ref<File> archived(std::span<const uint8_t> stored, size_t size, Compression compression, const String& mimeType)
    {
        auto file = adoptRef(*new File(mimeType, size, compression));
        file->m_stored = stored;
        if (compression == Compression::None)
            file->m_data = stored;
        return file;
    }

    ~File()
    {
        if (m_release)
            m_release();
    }

    const String& mimeType() const LIFETIME_BOUND { return m_mimeType; }
    size_t size() const { return m_size; }
    size_t ownedBytes() const { return m_owned.size(); }

    // The file's bytes; compressed archive entries are inflated on first use, on
    // the engine thread. Null if an entry does not inflate to its recorded size.
    std::optional<std::span<const uint8_t>> data()
    {
        ASSERT(isMainThread());
        if (m_compression == Compression::None || m_inflated)
            return m_data;
        if (m_inflateFailed)
            return std::nullopt;
        Vector<uint8_t> inflated(m_size);
        uLongf length = m_size;
        if (m_size && (uncompress(inflated.mutableSpan().data(), &length, m_stored.data(), m_stored.size()) != Z_OK || length != m_size)) {
            m_inflateFailed = true;
            return std::nullopt;
        }
        m_owned = WTF::move(inflated);
        m_data = m_owned.span();
        m_inflated = true;
        return m_data;
    }

private:
    File(const String& mimeType, size_t size, Compression compression)
        : m_mimeType(mimeType.isolatedCopy())
        , m_size(size)
        , m_compression(compression)
    {
    }

    const String m_mimeType;
    const size_t m_size;
    const Compression m_compression;
    std::span<const uint8_t> m_stored;
    std::span<const uint8_t> m_data;
    Vector<uint8_t> m_owned;
    Function<void()> m_release;
    bool m_inflated { false };
    bool m_inflateFailed { false };
};

MemoryFS::MemoryFS() = default;
MemoryFS::~MemoryFS() = default;

// ---- Paths and MIME types ----

String MemoryFS::normalizePath(StringView path)
{
    if (!path.startsWith('/'))
        return { };
    for (auto character : path.codeUnits()) {
        if (character < 0x20 || character == 0x7f || character == '\\')
            return { };
    }
    StringBuilder builder;
    bool trailingSlash = path.length() > 1 && path.endsWith('/');
    for (auto segment : path.substring(1).split('/')) {
        if (segment == "."_s || segment == ".."_s)
            return { };
        builder.append('/', segment);
    }
    // split() drops empty segments: a path with "//" is not normalized.
    String normalized = builder.isEmpty() ? "/"_s : builder.toString();
    if (trailingSlash)
        normalized = makeString(normalized, '/');
    return normalized == path ? normalized : String { };
}

String MemoryFS::mimeTypeForPath(StringView path)
{
    auto slash = path.reverseFind('/');
    auto name = slash == notFound ? path : path.substring(slash + 1);
    auto dot = name.reverseFind('.');
    if (dot == notFound)
        return "application/octet-stream"_s;
    auto extension = name.substring(dot + 1).convertToASCIILowercase();

    // Text is UTF-8: a page without a charset would otherwise be decoded as
    // windows-1252.
    static constexpr std::pair<ASCIILiteral, ASCIILiteral> types[] = {
        { "html"_s, "text/html; charset=utf-8"_s },
        { "htm"_s, "text/html; charset=utf-8"_s },
        { "js"_s, "text/javascript; charset=utf-8"_s },
        { "mjs"_s, "text/javascript; charset=utf-8"_s },
        { "cjs"_s, "text/javascript; charset=utf-8"_s },
        { "css"_s, "text/css; charset=utf-8"_s },
        { "json"_s, "application/json; charset=utf-8"_s },
        { "map"_s, "application/json; charset=utf-8"_s },
        { "webmanifest"_s, "application/manifest+json; charset=utf-8"_s },
        { "txt"_s, "text/plain; charset=utf-8"_s },
        { "md"_s, "text/markdown; charset=utf-8"_s },
        { "csv"_s, "text/csv; charset=utf-8"_s },
        { "xml"_s, "application/xml; charset=utf-8"_s },
        { "xhtml"_s, "application/xhtml+xml; charset=utf-8"_s },
        { "svg"_s, "image/svg+xml; charset=utf-8"_s },
        { "wasm"_s, "application/wasm"_s },
        { "png"_s, "image/png"_s },
        { "jpg"_s, "image/jpeg"_s },
        { "jpeg"_s, "image/jpeg"_s },
        { "gif"_s, "image/gif"_s },
        { "webp"_s, "image/webp"_s },
        { "avif"_s, "image/avif"_s },
        { "bmp"_s, "image/bmp"_s },
        { "ico"_s, "image/x-icon"_s },
        { "woff"_s, "font/woff"_s },
        { "woff2"_s, "font/woff2"_s },
        { "ttf"_s, "font/ttf"_s },
        { "otf"_s, "font/otf"_s },
        { "mp3"_s, "audio/mpeg"_s },
        { "wav"_s, "audio/wav"_s },
        { "ogg"_s, "audio/ogg"_s },
        { "oga"_s, "audio/ogg"_s },
        { "opus"_s, "audio/ogg"_s },
        { "m4a"_s, "audio/mp4"_s },
        { "flac"_s, "audio/flac"_s },
        { "mp4"_s, "video/mp4"_s },
        { "webm"_s, "video/webm"_s },
        { "ogv"_s, "video/ogg"_s },
        { "pdf"_s, "application/pdf"_s },
    };
    for (auto& [knownExtension, type] : types) {
        if (extension == knownExtension)
            return type;
    }
    auto type = MIMETypeRegistry::mimeTypeForExtension(extension);
    return type.isEmpty() ? "application/octet-stream"_s : type;
}

// ---- Adding files ----

MemoryFS::AddResult MemoryFS::addFile(const String& path, std::span<const uint8_t> data, const String& mimeType)
{
    auto normalized = normalizePath(path);
    if (normalized.isNull() || normalized.endsWith('/'))
        return AddResult::InvalidPath;
    auto file = File::copy(data, mimeType.isEmpty() ? mimeTypeForPath(normalized) : mimeType);
    Locker locker { m_lock };
    return m_files.add(normalized.isolatedCopy(), WTF::move(file)).isNewEntry ? AddResult::Added : AddResult::AlreadyExists;
}

MemoryFS::AddResult MemoryFS::addFile(const String& path, std::span<const uint8_t> data, const String& mimeType, Function<void()>&& release)
{
    auto normalized = normalizePath(path);
    if (normalized.isNull() || normalized.endsWith('/'))
        return AddResult::InvalidPath;
    Locker locker { m_lock };
    if (m_files.contains(normalized))
        return AddResult::AlreadyExists;
    // Created under the lock: a file that is not added must not call `release`.
    m_files.add(normalized.isolatedCopy(), File::external(data, mimeType.isEmpty() ? mimeTypeForPath(normalized) : mimeType, WTF::move(release)));
    return AddResult::Added;
}

bool MemoryFS::contains(const String& path) const
{
    Locker locker { m_lock };
    return m_files.contains(path);
}

size_t MemoryFS::ownedBytes() const
{
    Locker locker { m_lock };
    size_t bytes = 0;
    for (auto& file : m_files.values())
        bytes += file->ownedBytes();
    return bytes;
}

RefPtr<MemoryFS::File> MemoryFS::find(const String& path) const
{
    Locker locker { m_lock };
    return m_files.get(path);
}

// ---- Archives ----
//
// Format (little-endian), written by tools/assets.py:
//   header, 16 bytes:  "ZGAR", u32 version (1), u32 file count, u32 flags (0)
//   entries, 40 bytes each:
//     u64 data offset, u64 stored size, u64 size,
//     u32 path offset, u32 path length, u32 MIME offset, u16 MIME length,
//     u8 compression (0 stored, 1 zlib), u8 reserved
//   then strings (UTF-8 paths and MIME types) and data, at any offsets.
// Offsets are from the start of the archive. Paths are normalized ("/a/b.js").

namespace {

template<typename T> T readLittleEndian(std::span<const uint8_t> bytes)
{
    T value = 0;
    for (size_t i = 0; i < sizeof(T); ++i)
        value |= static_cast<T>(bytes[i]) << (8 * i);
    return value;
}

std::optional<std::span<const uint8_t>> slice(std::span<const uint8_t> archive, uint64_t offset, uint64_t length)
{
    if (offset > archive.size() || length > archive.size() - offset)
        return std::nullopt;
    return archive.subspan(offset, length);
}

} // anonymous namespace

std::expected<size_t, MemoryFS::MountError> MemoryFS::mountArchive(const String& prefix, std::span<const uint8_t> archive)
{
    auto malformed = [](String&& message) {
        return makeUnexpected(MountError { MountError::Kind::Malformed, WTF::move(message) });
    };

    auto mountPoint = normalizePath(prefix);
    if (mountPoint.isNull())
        return malformed(makeString("invalid mount point: "_s, prefix));
    if (mountPoint.endsWith('/'))
        mountPoint = mountPoint.left(mountPoint.length() - 1);

    static constexpr size_t headerSize = 16;
    static constexpr size_t entrySize = 40;
    if (archive.size() < headerSize || !equalSpans(archive.first(4), "ZGAR"_s.span8()))
        return malformed("not a ZeroG asset archive"_s);
    if (auto version = readLittleEndian<uint32_t>(archive.subspan(4)); version != 1)
        return malformed(makeString("unsupported archive version "_s, version));
    uint32_t count = readLittleEndian<uint32_t>(archive.subspan(8));
    auto entries = slice(archive, headerSize, static_cast<uint64_t>(count) * entrySize);
    if (!entries)
        return malformed("truncated archive"_s);

    Vector<std::pair<String, Ref<File>>> files;
    files.reserveInitialCapacity(count);
    HashSet<String> paths;
    for (uint32_t i = 0; i < count; ++i) {
        auto entry = entries->subspan(i * entrySize, entrySize);
        auto dataOffset = readLittleEndian<uint64_t>(entry);
        auto storedSize = readLittleEndian<uint64_t>(entry.subspan(8));
        auto size = readLittleEndian<uint64_t>(entry.subspan(16));
        auto pathOffset = readLittleEndian<uint32_t>(entry.subspan(24));
        auto pathLength = readLittleEndian<uint32_t>(entry.subspan(28));
        auto mimeOffset = readLittleEndian<uint32_t>(entry.subspan(32));
        auto mimeLength = readLittleEndian<uint16_t>(entry.subspan(36));
        auto compression = entry[38];

        auto stored = slice(archive, dataOffset, storedSize);
        auto pathBytes = slice(archive, pathOffset, pathLength);
        auto mimeBytes = slice(archive, mimeOffset, mimeLength);
        if (!stored || !pathBytes || !mimeBytes || compression > 1 || (!compression && storedSize != size) || size > std::numeric_limits<uint32_t>::max())
            return malformed(makeString("malformed entry "_s, i));

        auto path = String::fromUTF8(*pathBytes);
        if (path.isNull() || normalizePath(path) != path || path.endsWith('/'))
            return malformed(makeString("invalid path in entry "_s, i));
        auto fullPath = makeString(mountPoint, path);
        if (!paths.add(fullPath).isNewEntry)
            return malformed(makeString("duplicate path "_s, fullPath));
        auto mimeType = String::fromUTF8(*mimeBytes);
        if (mimeType.isEmpty())
            mimeType = mimeTypeForPath(fullPath);
        files.append({ fullPath.isolatedCopy(), File::archived(*stored, size, compression ? File::Compression::Zlib : File::Compression::None, mimeType) });
    }

    Locker locker { m_lock };
    for (auto& [path, file] : files) {
        if (m_files.contains(path))
            return makeUnexpected(MountError { MountError::Kind::Conflict, makeString("path already exists: "_s, path) });
    }
    for (auto& [path, file] : files)
        m_files.add(path, WTF::move(file));
    return files.size();
}

// ---- Serving ----

namespace {

String percentDecodedPath(const URL& url)
{
    auto path = url.path();
    Vector<uint8_t> bytes;
    auto utf8 = path.utf8();
    auto span = utf8.span();
    for (size_t i = 0; i < span.size(); ++i) {
        if (span[i] == '%' && i + 2 < span.size() && isASCIIHexDigit(span[i + 1]) && isASCIIHexDigit(span[i + 2])) {
            bytes.append(toASCIIHexValue(span[i + 1], span[i + 2]));
            i += 2;
        } else
            bytes.append(span[i]);
    }
    return String::fromUTF8(bytes.span());
}

struct Answer {
    int status { 200 };
    String mimeType;
    RefPtr<SharedBuffer> body;
    Vector<std::pair<String, String>> headers;
    URL redirect;
};

Answer textAnswer(int status, ASCIILiteral text, Vector<std::pair<String, String>>&& headers = { })
{
    return { status, "text/plain; charset=utf-8"_s, SharedBuffer::create(text.span8()), WTF::move(headers), { } };
}

// "bytes=first-last", "bytes=first-" or "bytes=-suffix"; one range only.
std::optional<std::pair<uint64_t, uint64_t>> parseRange(StringView header, uint64_t size, bool& unsatisfiable)
{
    unsatisfiable = false;
    if (!header.startsWith("bytes="_s) || header.contains(','))
        return std::nullopt;
    auto spec = header.substring(6).trim(isASCIIWhitespace<char16_t>);
    auto dash = spec.find('-');
    if (dash == notFound)
        return std::nullopt;
    auto firstText = spec.left(dash);
    auto lastText = spec.substring(dash + 1);
    uint64_t first, last;
    if (firstText.isEmpty()) {
        auto suffix = parseInteger<uint64_t>(lastText);
        if (!suffix)
            return std::nullopt;
        if (!*suffix || !size) {
            unsatisfiable = true;
            return std::nullopt;
        }
        first = size - std::min(*suffix, size);
        last = size - 1;
    } else {
        auto parsedFirst = parseInteger<uint64_t>(firstText);
        if (!parsedFirst)
            return std::nullopt;
        first = *parsedFirst;
        if (lastText.isEmpty())
            last = size ? size - 1 : 0;
        else {
            auto parsedLast = parseInteger<uint64_t>(lastText);
            if (!parsedLast || *parsedLast < first)
                return std::nullopt;
            last = std::min(*parsedLast, size ? size - 1 : 0);
        }
        if (first >= size) {
            unsatisfiable = true;
            return std::nullopt;
        }
    }
    return std::pair { first, last };
}

} // anonymous namespace

struct MemoryFS::Lookup {
    RefPtr<File> file;
    String path;
    URL redirect;
};

MemoryFS::Lookup MemoryFS::lookup(const URL& url) const
{
    auto path = normalizePath(percentDecodedPath(url));
    if (path.isNull())
        return { };
    if (path.endsWith('/'))
        return { find(makeString(path, "index.html"_s)), path, { } };
    if (auto file = find(path))
        return { WTF::move(file), path, { } };
    // A directory named without its slash: redirect, so its relative URLs resolve
    // inside it.
    if (find(makeString(path, "/index.html"_s))) {
        URL redirect = url;
        redirect.setPath(makeString(url.path(), '/'));
        return { nullptr, path, WTF::move(redirect) };
    }
    return { };
}

void MemoryFS::start(SchemeTask& task)
{
    auto& request = task.request();
    auto found = lookup(request.url());
    if (found.redirect.isValid()) {
        task.redirect(found.redirect);
        return;
    }

    auto& method = request.httpMethod();
    if (method != "GET"_s && method != "HEAD"_s) {
        auto result = textAnswer(405, "Method Not Allowed"_s, { { "Allow"_s, "GET, HEAD"_s } });
        task.respond(result.status, result.mimeType, result.body.releaseNonNull(), result.headers);
        return;
    }
    if (!found.file) {
        auto result = textAnswer(404, "Not Found"_s);
        task.respond(result.status, result.mimeType, result.body.releaseNonNull(), result.headers);
        return;
    }

    auto& file = *found.file;
    auto data = file.data();
    if (!data) {
        auto result = textAnswer(500, "Corrupt archive entry"_s);
        task.respond(result.status, result.mimeType, result.body.releaseNonNull(), result.headers);
        return;
    }

    uint64_t offset = 0;
    uint64_t length = data->size();
    int status = 200;
    Vector<std::pair<String, String>> headers { { "Accept-Ranges"_s, "bytes"_s } };
    auto rangeHeader = request.httpHeaderField(HTTPHeaderName::Range);
    if (!rangeHeader.isEmpty()) {
        bool unsatisfiable = false;
        if (auto range = parseRange(rangeHeader, data->size(), unsatisfiable)) {
            status = 206;
            offset = range->first;
            length = range->second - range->first + 1;
            headers.append({ "Content-Range"_s, makeString("bytes "_s, range->first, '-', range->second, '/', data->size()) });
        } else if (unsatisfiable) {
            auto result = textAnswer(416, "Range Not Satisfiable"_s, { { "Content-Range"_s, makeString("bytes */"_s, data->size()) } });
            task.respond(result.status, result.mimeType, result.body.releaseNonNull(), result.headers);
            return;
        }
    }

    bool isHead = method == "HEAD"_s;
    auto bytes = data->subspan(offset, isHead ? 0 : length);
    // The response reads the file in place; it keeps the file alive.
    auto body = SharedBuffer::create(DataSegment::Provider { [file = Ref { file }, bytes] { return bytes; } });
    if (isHead)
        headers.append({ "Content-Length"_s, String::number(length) });
    task.respond(status, file.mimeType(), WTF::move(body), headers);
}

bool MemoryFS::loadSynchronously(const ResourceRequest& request, ResourceResponse& response, Vector<uint8_t>& data)
{
    auto found = lookup(request.url());
    URL url = request.url();
    if (found.redirect.isValid()) {
        url = found.redirect;
        found = lookup(url);
    }
    if (!found.file) {
        response = ResourceResponse { URL { url }, "text/plain"_s, 0, "utf-8"_s };
        response.setHTTPStatusCode(404);
        response.setHTTPStatusText("Not Found"_s);
        return true;
    }
    auto bytes = found.file->data();
    if (!bytes)
        return false;
    auto mediaType = found.file->mimeType();
    response = ResourceResponse { URL { url }, extractMIMETypeFromMediaType(mediaType), static_cast<long long>(bytes->size()), extractCharsetFromMediaType(mediaType).toString() };
    response.setHTTPStatusCode(200);
    response.setHTTPStatusText("OK"_s);
    response.setHTTPHeaderField(HTTPHeaderName::ContentType, mediaType);
    response.setHTTPHeaderField(HTTPHeaderName::ContentLength, String::number(bytes->size()));
    data = *bytes;
    return true;
}

} // namespace ZeroG
