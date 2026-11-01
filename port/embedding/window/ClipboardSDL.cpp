#include "config.h"
#include "ClipboardSDL.h"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_stdinc.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

#if OS(WINDOWS)
#include <windows.h>
#include <cstdio>
#include <string>
#endif

namespace ZeroG {

using namespace WebCore;

#if OS(WINDOWS)

// Windows: the clipboard directly, in the system's own formats. SDL's would
// keep only one text type (every text/* is its plain text there), no HTML,
// and give up when another application has the clipboard open for a moment,
// as every SDL application does on each change. SDL still reports changes
// (SDL_EVENT_CLIPBOARD_UPDATE).
//
//   text/plain  CF_UNICODETEXT (CRLF line ends)
//   text/html   "HTML Format", what browsers and Office read and write
//   image/png   "PNG"
//   image/bmp   CF_DIB (the file without its header)
//   other types a format registered under the type's name, the length first

namespace {

UINT htmlFormat()
{
    static UINT format = RegisterClipboardFormatW(L"HTML Format");
    return format;
}

UINT pngFormat()
{
    static UINT format = RegisterClipboardFormatW(L"PNG");
    return format;
}

UINT namedFormat(const String& type)
{
    return RegisterClipboardFormatW(type.wideCharacters().span().data());
}

// The clipboard's owner when it writes: a window of its own (message-only).
HWND ownerWindow()
{
    static HWND window = CreateWindowExW(0, L"STATIC", L"ZeroG clipboard", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    return window;
}

// Open for this scope. Another application may hold it for a moment (every
// SDL application lists the formats on each change): tries for 250 ms.
class OpenClipboard {
public:
    explicit OpenClipboard(HWND owner)
    {
        for (int attempt = 0; attempt < 50 && !m_open; ++attempt) {
            m_open = ::OpenClipboard(owner);
            if (!m_open)
                Sleep(5);
        }
    }
    ~OpenClipboard()
    {
        if (m_open)
            CloseClipboard();
    }
    explicit operator bool() const { return m_open; }

private:
    bool m_open { false };
};

HGLOBAL globalCopy(std::span<const uint8_t> bytes, std::span<const uint8_t> prefix = { })
{
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, prefix.size() + bytes.size());
    if (!memory)
        return nullptr;
    auto* destination = static_cast<uint8_t*>(GlobalLock(memory));
    if (!prefix.empty())
        std::memcpy(destination, prefix.data(), prefix.size());
    if (!bytes.empty())
        std::memcpy(destination + prefix.size(), bytes.data(), bytes.size());
    GlobalUnlock(memory);
    return memory;
}

HGLOBAL textData(std::span<const uint8_t> utf8)
{
    // Windows text ends lines with CRLF.
    auto text = String::fromUTF8(utf8);
    StringBuilder builder;
    for (unsigned i = 0; i < text.length(); ++i) {
        if (text[i] == '\n' && (!i || text[i - 1] != '\r'))
            builder.append('\r');
        builder.append(text[i]);
    }
    auto wide = builder.toString().wideCharacters();
    wide.append(0);
    return globalCopy(asBytes(wide.span()));
}

HGLOBAL htmlData(std::span<const uint8_t> fragment)
{
    static constexpr char headerFormat[] = "Version:0.9\r\nStartHTML:%010zu\r\nEndHTML:%010zu\r\nStartFragment:%010zu\r\nEndFragment:%010zu\r\n";
    static constexpr char prefix[] = "<html><body>\r\n<!--StartFragment-->";
    static constexpr char suffix[] = "<!--EndFragment-->\r\n</body>\r\n</html>";
    char header[160];
    size_t headerLength = std::snprintf(header, sizeof header, headerFormat, size_t { 0 }, size_t { 0 }, size_t { 0 }, size_t { 0 });
    size_t startFragment = headerLength + sizeof prefix - 1;
    size_t endFragment = startFragment + fragment.size();
    size_t endHTML = endFragment + sizeof suffix - 1;
    std::snprintf(header, sizeof header, headerFormat, headerLength, endHTML, startFragment, endFragment);
    std::string html;
    html.reserve(endHTML + 1);
    html.append(header, headerLength).append(prefix).append(reinterpret_cast<const char*>(fragment.data()), fragment.size()).append(suffix);
    return globalCopy(byteCast<uint8_t>(std::span { html.c_str(), html.size() + 1 }));
}

// A BMP file is a BITMAPFILEHEADER and a DIB.
HGLOBAL dibData(std::span<const uint8_t> bmp)
{
    if (bmp.size() <= sizeof(BITMAPFILEHEADER) || bmp[0] != 'B' || bmp[1] != 'M')
        return nullptr;
    return globalCopy(bmp.subspan(sizeof(BITMAPFILEHEADER)));
}

HGLOBAL namedData(std::span<const uint8_t> bytes)
{
    uint64_t length = bytes.size();
    return globalCopy(bytes, asBytes(std::span { &length, 1 }));
}

// The bytes of a format; the clipboard must be open.
std::span<const uint8_t> lockedBytes(HANDLE memory)
{
    auto* data = static_cast<const uint8_t*>(memory ? GlobalLock(memory) : nullptr);
    return data ? std::span { data, GlobalSize(memory) } : std::span<const uint8_t> { };
}

RefPtr<SharedBuffer> readText()
{
    HANDLE memory = GetClipboardData(CF_UNICODETEXT);
    auto bytes = lockedBytes(memory);
    if (bytes.empty())
        return nullptr;
    auto* characters = reinterpret_cast<const char16_t*>(bytes.data());
    size_t length = 0;
    while (length < bytes.size() / 2 && characters[length])
        ++length;
    auto text = String({ characters, length });
    GlobalUnlock(memory);
    // Back to LF line ends, as pages write them.
    text = makeStringByReplacingAll(text, "\r\n"_s, "\n"_s);
    if (text.isEmpty())
        return nullptr;
    auto utf8 = text.utf8();
    return SharedBuffer::create(byteCast<uint8_t>(utf8.span()));
}

// The fragment, as UTF-8.
RefPtr<SharedBuffer> readHTML()
{
    HANDLE memory = GetClipboardData(htmlFormat());
    auto bytes = lockedBytes(memory);
    if (bytes.empty())
        return nullptr;
    std::string_view html(reinterpret_cast<const char*>(bytes.data()), strnlen(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    auto offset = [&](std::string_view name) -> std::optional<size_t> {
        auto at = html.find(name);
        if (at == std::string_view::npos)
            return std::nullopt;
        return static_cast<size_t>(std::strtoull(html.data() + at + name.size(), nullptr, 10));
    };
    auto start = offset("StartFragment:");
    auto end = offset("EndFragment:");
    if (!start || !end || *start > *end || *end > html.size()) {
        start = offset("StartHTML:");
        end = offset("EndHTML:");
    }
    RefPtr<SharedBuffer> result;
    if (start && end && *start <= *end && *end <= html.size())
        result = SharedBuffer::create(byteCast<uint8_t>(std::span { html.data() + *start, *end - *start }));
    GlobalUnlock(memory);
    return result;
}

RefPtr<SharedBuffer> readBMP()
{
    HANDLE memory = GetClipboardData(CF_DIB);
    auto dib = lockedBytes(memory);
    if (dib.size() < sizeof(BITMAPINFOHEADER)) {
        if (memory)
            GlobalUnlock(memory);
        return nullptr;
    }
    // The file header: where the pixels start, after the info header and the
    // color table (or the bit field masks).
    auto& info = *reinterpret_cast<const BITMAPINFOHEADER*>(dib.data());
    size_t colors = info.biClrUsed ? info.biClrUsed : (info.biBitCount <= 8 ? (1u << info.biBitCount) : 0);
    size_t masks = info.biCompression == BI_BITFIELDS && info.biSize == sizeof(BITMAPINFOHEADER) ? 12 : 0;
    BITMAPFILEHEADER file { };
    file.bfType = 0x4D42;
    file.bfSize = static_cast<DWORD>(sizeof file + dib.size());
    file.bfOffBits = static_cast<DWORD>(sizeof file + info.biSize + masks + colors * 4);
    Vector<uint8_t> bmp;
    bmp.append(asBytes(std::span { &file, 1 }));
    bmp.append(dib);
    GlobalUnlock(memory);
    return SharedBuffer::create(WTF::move(bmp));
}

RefPtr<SharedBuffer> readNamed(UINT format, bool lengthFirst)
{
    HANDLE memory = GetClipboardData(format);
    auto bytes = lockedBytes(memory);
    if (bytes.empty())
        return nullptr;
    RefPtr<SharedBuffer> result;
    if (!lengthFirst)
        result = SharedBuffer::create(bytes);
    else if (bytes.size() >= sizeof(uint64_t)) {
        uint64_t length;
        std::memcpy(&length, bytes.data(), sizeof length);
        if (length <= bytes.size() - sizeof length)
            result = SharedBuffer::create(bytes.subspan(sizeof length, length));
    }
    GlobalUnlock(memory);
    return result;
}

} // namespace

Vector<String> ClipboardSDL::types()
{
    Vector<String> types;
    OpenClipboard clipboard(nullptr);
    if (!clipboard)
        return types;
    auto add = [&](const String& type) {
        if (!types.contains(type))
            types.append(type);
    };
    for (UINT format = EnumClipboardFormats(0); format; format = EnumClipboardFormats(format)) {
        if (format == CF_UNICODETEXT)
            add("text/plain"_s);
        else if (format == CF_DIB)
            add("image/bmp"_s);
        else if (format == htmlFormat())
            add("text/html"_s);
        else if (format == pngFormat())
            add("image/png"_s);
        else if (format >= 0xC000) {
            wchar_t name[256];
            int length = GetClipboardFormatNameW(format, name, std::size(name));
            auto type = String({ reinterpret_cast<const char16_t*>(name), static_cast<size_t>(std::max(length, 0)) });
            // MIME types, written here or by other applications.
            if (type.contains('/') && !type.contains(' '))
                add(type);
        }
    }
    return types;
}

RefPtr<SharedBuffer> ClipboardSDL::read(const String& type)
{
    OpenClipboard clipboard(nullptr);
    if (!clipboard)
        return nullptr;
    if (type == "text/plain"_s)
        return readText();
    if (type == "text/html"_s)
        return readHTML();
    if (type == "image/png"_s)
        return readNamed(pngFormat(), false);
    if (type == "image/bmp"_s)
        return readBMP();
    return readNamed(namedFormat(type), true);
}

void ClipboardSDL::write(Vector<Item>&& items)
{
    ++m_changeCount;
    OpenClipboard clipboard(ownerWindow());
    if (!clipboard)
        return;
    // One change of the clipboard: everything at once, owned here.
    EmptyClipboard();
    for (auto& [type, buffer] : items) {
        auto bytes = buffer->span();
        HGLOBAL memory = nullptr;
        UINT format = 0;
        if (type == "text/plain"_s) {
            memory = textData(bytes);
            format = CF_UNICODETEXT;
        } else if (type == "text/html"_s) {
            memory = htmlData(bytes);
            format = htmlFormat();
        } else if (type == "image/png"_s) {
            memory = globalCopy(bytes);
            format = pngFormat();
        } else if (type == "image/bmp"_s) {
            memory = dibData(bytes);
            format = CF_DIB;
        } else {
            memory = namedData(bytes);
            format = namedFormat(type);
        }
        if (memory && (!format || !SetClipboardData(format, memory)))
            GlobalFree(memory);
    }
}

#else

// Text goes as SDL's text type, which each platform maps to its own
// (UTF8_STRING on X11, text/plain on Wayland).
static constexpr auto sdlTextType = "text/plain;charset=utf-8"_s;

Vector<String> ClipboardSDL::types()
{
    Vector<String> types;
    if (SDL_HasClipboardText())
        types.append("text/plain"_s);
    size_t count = 0;
    if (char** mimeTypes = SDL_GetClipboardMimeTypes(&count)) {
        for (size_t i = 0; i < count; ++i) {
            auto type = String::fromUTF8(mimeTypes[i]);
            // Text, under any of its names, is "text/plain" already.
            if (type.startsWith("text/plain"_s) || type == "UTF8_STRING"_s || type == "TEXT"_s || type == "STRING"_s)
                continue;
            if (type.contains('/') && !types.contains(type))
                types.append(type);
        }
        SDL_free(mimeTypes);
    }
    return types;
}

RefPtr<SharedBuffer> ClipboardSDL::read(const String& type)
{
    if (type == "text/plain"_s) {
        char* text = SDL_GetClipboardText();
        if (!text)
            return nullptr;
        RefPtr<SharedBuffer> buffer;
        if (*text)
            buffer = SharedBuffer::create(byteCast<uint8_t>(unsafeSpan(text)));
        SDL_free(text);
        return buffer;
    }
    size_t size = 0;
    auto mimeType = type.utf8();
    void* data = SDL_GetClipboardData(byteCast<char>(mimeType.data()), &size);
    if (!data)
        return nullptr;
    auto buffer = SharedBuffer::create(std::span { static_cast<const uint8_t*>(data), size });
    SDL_free(data);
    return buffer;
}

namespace {

// What SDL reads from when another application pastes: the items written,
// until the clipboard changes again.
struct Offer {
    Vector<std::pair<std::string, Ref<SharedBuffer>>> items;

    static const void* SDLCALL data(void* user, const char* mimeType, size_t* size)
    {
        auto& offer = *static_cast<Offer*>(user);
        std::string_view requested = mimeType;
        for (auto& [type, buffer] : offer.items) {
            bool isText = type == sdlTextType.characters() && requested.starts_with("text/plain");
            if (isText || requested == type) {
                *size = buffer->size();
                return buffer->span().data();
            }
        }
        *size = 0;
        return nullptr;
    }

    static void SDLCALL cleanup(void* user)
    {
        delete static_cast<Offer*>(user);
    }
};

} // namespace

void ClipboardSDL::write(Vector<Item>&& items)
{
    ++m_changeCount;
    if (items.isEmpty()) {
        SDL_ClearClipboardData();
        return;
    }
    auto* offer = new Offer;
    Vector<const char*> mimeTypes;
    for (auto& [type, buffer] : items) {
        auto utf8 = type.utf8();
        offer->items.append({ type == "text/plain"_s ? std::string { sdlTextType.characters() } : std::string { byteCast<char>(utf8.data()), utf8.length() }, WTF::move(buffer) });
    }
    for (auto& item : offer->items) {
        mimeTypes.append(item.first.c_str());
        // The other names text is asked for.
        if (item.first == sdlTextType.characters())
            mimeTypes.append("text/plain");
    }
    // SDL copies the type list and owns `offer` from here (cleanup frees it).
    if (!SDL_SetClipboardData(Offer::data, Offer::cleanup, offer, mimeTypes.span().data(), mimeTypes.size()))
        delete offer;
}

#endif

// ---- Which clipboard ----

static RefPtr<PasteboardItemsZeroG>& applicationClipboard()
{
    static NeverDestroyed<RefPtr<PasteboardItemsZeroG>> clipboard;
    return clipboard;
}

static RefPtr<ClipboardSDL>& sdlClipboard()
{
    static NeverDestroyed<RefPtr<ClipboardSDL>> clipboard;
    return clipboard;
}

static void updateSystemClipboard()
{
    if (auto& application = applicationClipboard())
        setSystemClipboardZeroG(application.copyRef());
    else if (auto& sdl = sdlClipboard())
        setSystemClipboardZeroG(sdl.copyRef());
    else
        setSystemClipboardZeroG(nullptr);
}

void setApplicationClipboard(RefPtr<PasteboardItemsZeroG>&& clipboard)
{
    applicationClipboard() = WTF::move(clipboard);
    updateSystemClipboard();
}

void useSDLClipboard()
{
    if (!sdlClipboard()) {
        sdlClipboard() = ClipboardSDL::create();
        updateSystemClipboard();
    }
}

void sdlClipboardChanged()
{
    if (auto& clipboard = sdlClipboard())
        clipboard->didChange();
}

} // namespace ZeroG
