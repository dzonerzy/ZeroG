#pragma once

// The application's files, in memory, served under a custom scheme (app://):
// archives generated at build time (tools/assets.py) and files the application
// adds at runtime. Files are write-once: a path, once added, never changes and is
// never removed, so a page always sees one version of it and nothing it loaded
// goes stale. Lookups ignore the URL's host; the path selects the file.
//
// Served with an explicit MIME type (the file's own, else from its extension),
// HTTP range requests (audio and video seek), HEAD, and index.html for
// directories. Adding files is thread-safe; loads run on the engine thread.

#include "Loader.h"
#include <expected>
#include <span>
#include <wtf/Lock.h>
#include <wtf/StdLibExtras.h>

namespace ZeroG {

class MemoryFS final : public SchemeHandler {
public:
    static Ref<MemoryFS> create() { return adoptRef(*new MemoryFS); }
    ~MemoryFS();

    enum class AddResult : uint8_t { Added, InvalidPath, AlreadyExists };

    // Copies the data.
    AddResult addFile(const String& path, std::span<const uint8_t>, const String& mimeType = { });
    // Keeps the caller's data until the file system and every load using it are
    // gone, then calls `release` (on any thread).
    AddResult addFile(const String& path, std::span<const uint8_t>, const String& mimeType, Function<void()>&& release);

    // Mounts an archive's files under `prefix` ("/" for the root). The archive is
    // not copied and must outlive the file system. Fails as a whole, adding
    // nothing, if the archive is malformed or one of its paths already exists.
    struct MountError {
        enum class Kind : uint8_t { Malformed, Conflict };
        Kind kind;
        String message;
    };
    std::expected<size_t, MountError> mountArchive(const String& prefix, std::span<const uint8_t>);

    // Single-page applications route with the URL's path: a request for a path
    // with no file and no extension in its last segment ("/settings/profile")
    // gets this file instead ("/index.html"), with status 200 and its own URL.
    // A missing "/app.js" is still a 404. Null turns it off.
    void setFallback(const String& path);

    bool contains(const String& path) const;
    // Bytes the file system holds itself: copied files and decompressed entries.
    size_t ownedBytes() const;

    // Normalizes a file path: "/" separated, no empty, "." or ".." segments, no
    // backslashes or control characters. Null if the path is not acceptable.
    static String normalizePath(StringView);
    // The MIME type ZeroG serves for a file name's extension.
    static String mimeTypeForPath(StringView);

private:
    MemoryFS();

    class File;
    struct Lookup;
    RefPtr<File> find(const String& path) const;
    Lookup lookup(const URL&) const;

    void start(SchemeTask&) final;
    bool loadSynchronously(const WebCore::ResourceRequest&, WebCore::ResourceResponse&, Vector<uint8_t>&) final;

    mutable Lock m_lock;
    HashMap<String, Ref<File>> m_files WTF_GUARDED_BY_LOCK(m_lock);
    String m_fallback WTF_GUARDED_BY_LOCK(m_lock);
};

} // namespace ZeroG
