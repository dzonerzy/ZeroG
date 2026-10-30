// The pasteboard over typed items (PasteboardItemsZeroG.h): the system clipboard
// for copy and paste, a drag's own data for drag and drop. It follows the GLib
// port's (platform/glib/PasteboardGLib.cpp): plain text, HTML, URLs and images
// as MIME types, and custom data (DataTransfer types of the page's own,
// navigator.clipboard) in one serialized item, readable by its origin only.

#include "config.h"
#include "Pasteboard.h"

#include "Color.h"
#include "CommonAtomStrings.h"
#include "DragData.h"
#include "MIMETypeRegistry.h"
#include "NotImplemented.h"
#include "PasteboardCustomData.h"
#include "PasteboardItemsZeroG.h"
#include "SharedBuffer.h"
#include <wtf/ListHashSet.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/StringHash.h>

namespace WebCore {

static constexpr auto textPlainType = "text/plain"_s; // UTF-8
static constexpr auto textHTMLType = "text/html"_s;
static constexpr auto uriListType = "text/uri-list"_s;
static constexpr std::array<ASCIILiteral, 5> imageTypes = { "image/png"_s, "image/jpeg"_s, "image/gif"_s, "image/webp"_s, "image/bmp"_s };

// ---- Items ----

Vector<String> MemoryPasteboardItemsZeroG::types()
{
    return m_items.map([](auto& item) { return item.first; });
}

RefPtr<SharedBuffer> MemoryPasteboardItemsZeroG::read(const String& type)
{
    for (auto& item : m_items) {
        if (item.first == type)
            return item.second.ptr();
    }
    return nullptr;
}

void MemoryPasteboardItemsZeroG::write(Vector<Item>&& items)
{
    m_items = WTF::move(items);
    ++m_changeCount;
}

static RefPtr<PasteboardItemsZeroG>& systemClipboardSlot()
{
    ASSERT(isMainThread());
    static NeverDestroyed<RefPtr<PasteboardItemsZeroG>> clipboard;
    return clipboard;
}

void setSystemClipboardZeroG(RefPtr<PasteboardItemsZeroG>&& clipboard)
{
    systemClipboardSlot() = WTF::move(clipboard);
}

PasteboardItemsZeroG& systemClipboardZeroG()
{
    if (auto& clipboard = systemClipboardSlot())
        return *clipboard;
    static NeverDestroyed<Ref<MemoryPasteboardItemsZeroG>> inProcess = MemoryPasteboardItemsZeroG::create();
    return inProcess.get();
}

static Ref<SharedBuffer> bufferFromString(const String& text)
{
    auto utf8 = text.utf8();
    return SharedBuffer::create(byteCast<uint8_t>(utf8.span()));
}

static String stringFromBuffer(const RefPtr<SharedBuffer>& buffer)
{
    return buffer ? String::fromUTF8(buffer->span()) : String();
}

// DataTransfer's and the legacy names for the types kept.
static String normalizedType(const String& type)
{
    if (equalLettersIgnoringASCIICase(type, "text"_s) || startsWithLettersIgnoringASCIICase(type, "text/plain"_s))
        return textPlainType;
    if (equalLettersIgnoringASCIICase(type, "url"_s))
        return uriListType;
    return type.convertToASCIILowercase();
}

// The items with one type replaced (or removed, with no value).
static void replaceItem(PasteboardItemsZeroG& items, const String& type, RefPtr<SharedBuffer>&& value)
{
    Vector<PasteboardItemsZeroG::Item> result;
    for (auto& existing : items.types()) {
        if (existing == type)
            continue;
        if (auto buffer = items.read(existing))
            result.append({ existing, buffer.releaseNonNull() });
    }
    if (value)
        result.append({ type, value.releaseNonNull() });
    items.write(WTF::move(result));
}

static PasteboardCustomData customData(PasteboardItemsZeroG& items)
{
    if (auto buffer = items.read(customPasteboardDataTypeZeroG))
        return PasteboardCustomData::fromSharedBuffer(*buffer);
    return { };
}

// ---- The pasteboard ----

std::unique_ptr<Pasteboard> Pasteboard::createForCopyAndPaste(std::unique_ptr<PasteboardContext>&& context)
{
    return makeUnique<Pasteboard>(WTF::move(context), Ref { systemClipboardZeroG() });
}

#if ENABLE(DRAG_SUPPORT)
std::unique_ptr<Pasteboard> Pasteboard::createForDragAndDrop(std::unique_ptr<PasteboardContext>&& context)
{
    return makeUnique<Pasteboard>(WTF::move(context), MemoryPasteboardItemsZeroG::create());
}

std::unique_ptr<Pasteboard> Pasteboard::create(const DragData& dragData)
{
    // DragDataRef on ZeroG: the drag's items (View.cpp makes them).
    auto* items = static_cast<PasteboardItemsZeroG*>(dragData.platformData());
    if (!items)
        return makeUnique<Pasteboard>(dragData.createPasteboardContext(), MemoryPasteboardItemsZeroG::create());
    return makeUnique<Pasteboard>(dragData.createPasteboardContext(), Ref { *items });
}

void Pasteboard::setDragImage(DragImage&&, const IntPoint&)
{
}
#endif

Pasteboard::Pasteboard(std::unique_ptr<PasteboardContext>&& context)
    : m_context(WTF::move(context))
{
}

Pasteboard::Pasteboard(std::unique_ptr<PasteboardContext>&& context, Ref<PasteboardItemsZeroG>&& items)
    : m_context(WTF::move(context))
    , m_items(WTF::move(items))
    , m_changeCount(m_items->changeCount())
{
}

Pasteboard::~Pasteboard() = default;

int64_t Pasteboard::changeCount() const
{
    return m_items ? m_items->changeCount() : 0;
}

bool Pasteboard::hasData()
{
    return m_items && (!m_items->types().isEmpty() || !m_items->filePaths().isEmpty());
}

Vector<String> Pasteboard::typesSafeForBindings(const String& origin)
{
    if (!m_items)
        return { };
    ListHashSet<String> types;
    auto custom = customData(*m_items);
    if (custom.origin() == origin) {
        for (auto& type : custom.orderedTypes())
            types.add(type);
    }
    auto present = m_items->types();
    if (present.contains(textPlainType))
        types.add(textPlainContentTypeAtom());
    if (present.contains(textHTMLType))
        types.add(textHTMLContentTypeAtom());
    if (present.contains(uriListType))
        types.add(uriListType);
    return copyToVector(types);
}

Vector<String> Pasteboard::typesForLegacyUnsafeBindings()
{
    if (!m_items)
        return { };
    Vector<String> types;
    for (auto& type : m_items->types()) {
        if (type == customPasteboardDataTypeZeroG)
            continue;
        types.append(type);
        if (type == textPlainType) {
            // The name editing/libwpe/EditorLibWPE.cpp pastes plain text under.
            types.append("text/plain;charset=utf-8"_s);
            types.append("Text"_s);
            types.append("text"_s);
        } else if (type == uriListType)
            types.append("URL"_s);
    }
    return types;
}

String Pasteboard::readOrigin()
{
    return m_items ? customData(*m_items).origin() : String();
}

String Pasteboard::readString(const String& type)
{
    if (!m_items)
        return { };
    auto normalized = normalizedType(type);
    if (normalized == uriListType && type.convertToASCIILowercase() == "url"_s) {
        // "URL": the first URL of the list.
        for (auto line : StringView { stringFromBuffer(m_items->read(uriListType)) }.split('\n')) {
            auto trimmed = line.trim(isASCIIWhitespace<char16_t>);
            if (!trimmed.isEmpty() && !trimmed.startsWith('#'))
                return trimmed.toString();
        }
        return { };
    }
    return stringFromBuffer(m_items->read(normalized));
}

String Pasteboard::readStringInCustomData(const String& type)
{
    return m_items ? customData(*m_items).readStringInCustomData(type) : String();
}

void Pasteboard::writeString(const String& type, const String& data)
{
    if (m_items)
        replaceItem(*m_items, normalizedType(type), bufferFromString(data));
}

void Pasteboard::clear()
{
    // A drag's files stay: "clearData() does not affect whether any files were
    // included in the drag".
    if (m_items)
        m_items->write({ });
}

void Pasteboard::clear(const String& type)
{
    if (m_items)
        replaceItem(*m_items, normalizedType(type), nullptr);
}

void Pasteboard::read(PasteboardPlainText& text, PlainTextURLReadingPolicy policy, std::optional<size_t>)
{
    if (!m_items)
        return;
    text.text = stringFromBuffer(m_items->read(textPlainType));
    if (text.text.isEmpty() && policy == PlainTextURLReadingPolicy::AllowURL)
        text.text = readString("URL"_s);
}

void Pasteboard::read(PasteboardWebContentReader& reader, WebContentReadingPolicy, std::optional<size_t>)
{
    // ZeroG's editor reads HTML and text itself (editing/libwpe/EditorLibWPE.cpp).
    reader.setContentOrigin(readOrigin());
}

void Pasteboard::read(PasteboardFileReader& reader, std::optional<size_t>)
{
    if (!m_items)
        return;
    auto paths = m_items->filePaths();
    if (!paths.isEmpty()) {
        for (auto& path : paths)
            reader.readFilename(path);
        return;
    }
    // An image on the clipboard, as a file named after its type.
    for (auto& type : imageTypes) {
        if (!reader.shouldReadBuffer(type))
            continue;
        if (auto buffer = m_items->read(type)) {
            reader.readBuffer(makeString("image."_s, StringView { type }.substring(6)), type, buffer.releaseNonNull());
            return;
        }
    }
}

void Pasteboard::write(const PasteboardURL& url)
{
    if (!m_items)
        return;
    Vector<PasteboardItemsZeroG::Item> items;
    items.append({ uriListType, bufferFromString(url.url.string()) });
    items.append({ textPlainType, bufferFromString(url.url.string()) });
    m_items->write(WTF::move(items));
}

void Pasteboard::writeTrustworthyWebURLsPboardType(const PasteboardURL&)
{
}

void Pasteboard::write(PasteboardImage&& image)
{
    if (!m_items)
        return;
    Vector<PasteboardItemsZeroG::Item> items;
    // The image's own bytes, as the page had them.
    if (image.resourceData && image.resourceMIMEType.startsWith("image/"_s))
        items.append({ image.resourceMIMEType.convertToASCIILowercase(), image.resourceData.releaseNonNull() });
    if (!image.url.url.isEmpty()) {
        items.append({ uriListType, bufferFromString(image.url.url.string()) });
        items.append({ textPlainType, bufferFromString(image.url.url.string()) });
    }
    m_items->write(WTF::move(items));
}

void Pasteboard::write(const PasteboardBuffer& buffer)
{
    if (m_items && buffer.data)
        replaceItem(*m_items, normalizedType(buffer.type), buffer.data.copyRef());
}

void Pasteboard::write(const PasteboardWebContent& content)
{
    if (!m_items)
        return;
    Vector<PasteboardItemsZeroG::Item> items;
    items.append({ textPlainType, bufferFromString(content.text) });
    if (!content.markup.isEmpty())
        items.append({ textHTMLType, bufferFromString(content.markup) });
    m_items->write(WTF::move(items));
}

Pasteboard::FileContentState Pasteboard::fileContentState()
{
    if (!m_items)
        return FileContentState::NoFileOrImageData;
    if (!m_items->filePaths().isEmpty())
        return FileContentState::MayContainFilePaths;
    for (auto& type : m_items->types()) {
        if (MIMETypeRegistry::isSupportedImageMIMEType(type))
            return FileContentState::InMemoryImage;
    }
    return FileContentState::NoFileOrImageData;
}

bool Pasteboard::canSmartReplace()
{
    return false;
}

void Pasteboard::writeMarkup(const String& markup)
{
    if (m_items)
        replaceItem(*m_items, textHTMLType, bufferFromString(markup));
}

void Pasteboard::writePlainText(const String& text, SmartReplaceOption)
{
    if (!m_items)
        return;
    Vector<PasteboardItemsZeroG::Item> items;
    items.append({ textPlainType, bufferFromString(text) });
    m_items->write(WTF::move(items));
}

void Pasteboard::writeCustomData(const Vector<PasteboardCustomData>& data, PasteboardWriteType)
{
    if (!m_items || data.isEmpty())
        return;
    // The first item: ZeroG's clipboard holds one, as GTK's does.
    auto& custom = data[0];
    Vector<PasteboardItemsZeroG::Item> items;
    custom.forEachPlatformStringOrBuffer([&](auto& type, auto& value) {
        auto normalized = normalizedType(type);
        WTF::switchOn(value,
            [&](const String& string) { items.append({ normalized, bufferFromString(string) }); },
            [&](const Ref<SharedBuffer>& buffer) { items.append({ normalized, buffer.copyRef() }); });
    });
    if (custom.hasSameOriginCustomData() || !custom.origin().isEmpty())
        items.append({ customPasteboardDataTypeZeroG, custom.createSharedBuffer() });
    m_items->write(WTF::move(items));
}

void Pasteboard::write(const Color&)
{
    notImplemented();
}

} // namespace WebCore
