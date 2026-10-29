// First version of the pasteboard: one in-process store shared by every page, so
// copy and paste work inside the application. The system clipboard is reached
// later through the embedding API, which the application can connect to its own.

#include "config.h"
#include "Pasteboard.h"

#include "Color.h"
#include "NotImplemented.h"
#include "PasteboardStrategy.h"
#include <wtf/HashMap.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/StringHash.h>

namespace WebCore {

static constexpr auto textPlainType = "text/plain;charset=utf-8"_s;
static constexpr auto textHTMLType = "text/html"_s;
static constexpr auto textURIListType = "text/uri-list"_s;

static HashMap<String, String>& store()
{
    ASSERT(isMainThread());
    static NeverDestroyed<HashMap<String, String>> store;
    return store;
}

std::unique_ptr<Pasteboard> Pasteboard::createForCopyAndPaste(std::unique_ptr<PasteboardContext>&& context)
{
    return makeUnique<Pasteboard>(WTF::move(context));
}

Pasteboard::Pasteboard(std::unique_ptr<PasteboardContext>&& context)
    : m_context(WTF::move(context))
{
}

bool Pasteboard::hasData()
{
    return !store().isEmpty();
}

Vector<String> Pasteboard::typesSafeForBindings(const String&)
{
    return typesForLegacyUnsafeBindings();
}

Vector<String> Pasteboard::typesForLegacyUnsafeBindings()
{
    Vector<String> types;
    for (auto& type : store().keys())
        types.append(type == textPlainType ? "text/plain"_s : type);
    if (store().contains(textPlainType))
        types.append(textPlainType);
    return types;
}

String Pasteboard::readOrigin()
{
    notImplemented();
    return { };
}

String Pasteboard::readString(const String& type)
{
    if (type == "text/plain"_s)
        return store().get(textPlainType);
    return store().get(type);
}

String Pasteboard::readStringInCustomData(const String&)
{
    notImplemented();
    return { };
}

void Pasteboard::writeString(const String& type, const String& data)
{
    store().set(type == "text/plain"_s ? String(textPlainType) : type, data);
}

void Pasteboard::clear()
{
    store().clear();
}

void Pasteboard::clear(const String& type)
{
    store().remove(type == "text/plain"_s ? String(textPlainType) : type);
}

void Pasteboard::read(PasteboardPlainText& text, PlainTextURLReadingPolicy, std::optional<size_t>)
{
    text.text = store().get(textPlainType);
}

void Pasteboard::read(PasteboardWebContentReader&, WebContentReadingPolicy, std::optional<size_t>)
{
    notImplemented();
}

void Pasteboard::read(PasteboardFileReader&, std::optional<size_t>)
{
}

void Pasteboard::write(const PasteboardURL& url)
{
    store().clear();
    store().set(textURIListType, url.url.string());
    store().set(textPlainType, url.url.string());
}

void Pasteboard::writeTrustworthyWebURLsPboardType(const PasteboardURL&)
{
    notImplemented();
}

void Pasteboard::write(PasteboardImage&&)
{
    notImplemented();
}

void Pasteboard::write(const PasteboardBuffer&)
{
    notImplemented();
}

void Pasteboard::write(const PasteboardWebContent& content)
{
    store().clear();
    store().set(textPlainType, content.text);
    store().set(textHTMLType, content.markup);
}

Pasteboard::FileContentState Pasteboard::fileContentState()
{
    return FileContentState::NoFileOrImageData;
}

bool Pasteboard::canSmartReplace()
{
    return false;
}

void Pasteboard::writeMarkup(const String& markup)
{
    store().set(textHTMLType, markup);
}

void Pasteboard::writePlainText(const String& text, SmartReplaceOption)
{
    store().clear();
    store().set(textPlainType, text);
}

void Pasteboard::writeCustomData(const Vector<PasteboardCustomData>&, PasteboardWriteType)
{
    notImplemented();
}

void Pasteboard::write(const Color&)
{
    notImplemented();
}

} // namespace WebCore
