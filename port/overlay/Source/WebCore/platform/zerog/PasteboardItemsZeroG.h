#pragma once

// What a ZeroG pasteboard reads and writes: typed data, by MIME type (text is
// UTF-8). The system clipboard is one (the embedding layer's: the host's, SDL's
// in window mode, or one in process); each drag has its own, in memory, which
// can also carry files.
//
// Main thread only.

#include <WebCore/SharedBuffer.h>
#include <wtf/RefCounted.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace WebCore {

class PasteboardItemsZeroG : public RefCounted<PasteboardItemsZeroG> {
public:
    virtual ~PasteboardItemsZeroG() = default;

    using Item = std::pair<String, Ref<SharedBuffer>>;

    virtual Vector<String> types() = 0;
    virtual RefPtr<SharedBuffer> read(const String& type) = 0;
    // Replaces everything (one change of the clipboard).
    virtual void write(Vector<Item>&&) = 0;
    // Grows with each write, here or elsewhere (another application).
    virtual int64_t changeCount() = 0;
    // Files dropped from outside the page; the clipboard has none.
    virtual Vector<String> filePaths() { return { }; }
};

// Data in memory: a drag's, and the clipboard when nothing else provides one.
class MemoryPasteboardItemsZeroG final : public PasteboardItemsZeroG {
public:
    static Ref<MemoryPasteboardItemsZeroG> create() { return adoptRef(*new MemoryPasteboardItemsZeroG); }

    Vector<String> types() final;
    RefPtr<SharedBuffer> read(const String& type) final;
    void write(Vector<Item>&&) final;
    int64_t changeCount() final { return m_changeCount; }
    Vector<String> filePaths() final { return m_filePaths; }
    void setFilePaths(Vector<String>&& paths) { m_filePaths = WTF::move(paths); }

private:
    MemoryPasteboardItemsZeroG() = default;

    Vector<Item> m_items;
    Vector<String> m_filePaths;
    int64_t m_changeCount { 0 };
};

// The system clipboard; null puts back the one in process.
WEBCORE_EXPORT void setSystemClipboardZeroG(RefPtr<PasteboardItemsZeroG>&&);
WEBCORE_EXPORT PasteboardItemsZeroG& systemClipboardZeroG();

// The type custom pasteboard data (DataTransfer.setData with other types,
// navigator.clipboard) is kept under.
static constexpr auto customPasteboardDataTypeZeroG = "org.zerog.custom-pasteboard-data"_s;

} // namespace WebCore
