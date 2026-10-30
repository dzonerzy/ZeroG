#include "config.h"
#include "ClipboardSDL.h"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_stdinc.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/CString.h>

namespace ZeroG {

using namespace WebCore;

// Text goes as SDL's text type, which each platform maps to its own (UTF-16 on
// Windows, UTF8_STRING on X11, text/plain on Wayland).
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
