// Fonts registered by the application (docs/design/text-fonts.md, "Embedded fonts"): its UI's
// fonts come from memory, not from the system. A session keeps one style sheet
// of @font-face rules, one per registered font, with the font inline as a data:
// URL; every document of the session's pages gets it through the pages' user
// content. A family registered here wins over a system font of the same name,
// as any @font-face family does, and only the session's pages see it.

#include "config.h"
#include "Loader.h"

#include <WebCore/DOMWrapperWorld.h>
#include <WebCore/FontCache.h>
#include <WebCore/SharedBuffer.h>
#include <WebCore/UserContentProvider.h>
#include <WebCore/UserStyleSheet.h>
// Not one of WebCore's exported headers; on its include path.
#include <WOFFFileFormat.h>
#include <wtf/HexNumber.h>
#include <wtf/text/Base64.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

#if ENABLE(CONTENT_EXTENSIONS)
#include <WebCore/ContentExtensionsBackend.h>
#endif

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkData.h>
#include <skia/core/SkFontMgr.h>
#include <skia/core/SkFontStyle.h>
#include <skia/core/SkString.h>
#include <skia/core/SkTypeface.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace ZeroG {

using namespace WebCore;

namespace {

class UserContentProviderZeroG final : public UserContentProvider {
public:
    static Ref<UserContentProviderZeroG> create() { return adoptRef(*new UserContentProviderZeroG); }

    void addRule(const String& rule)
    {
        m_rules.append(rule);
        StringBuilder source;
        for (auto& existing : m_rules)
            source.append(existing, '\n');
        // User level: user sheets are part of every document's style resolver from
        // its creation, and their @font-face rules join the document's fonts like
        // its own.
        m_styleSheet = UserStyleSheet { source.toString(), URL { "about:zerog-fonts"_s }, { }, { },
            UserContentInjectedFrames::InjectInAllFrames, UserContentMatchParentFrame::Never, UserStyleLevel::User };
        invalidateInjectedStyleSheetCacheInAllFramesInAllPages();
    }

private:
    void forEachUserScript(NOESCAPE const Function<void(DOMWrapperWorld&, const UserScript&)>&) const final { }
    void forEachUserStyleSheet(NOESCAPE const Function<void(const UserStyleSheet&)>& function) const final
    {
        if (m_styleSheet)
            function(*m_styleSheet);
    }
#if ENABLE(USER_MESSAGE_HANDLERS)
    void forEachUserMessageHandler(NOESCAPE const Function<void(const UserMessageHandlerDescriptor&)>&) const final { }
#endif
    bool hasBuffersForWorld(const DOMWrapperWorld&) const final { return false; }
    WebKitBuffer* buffer(const DOMWrapperWorld&, const String&) const final { return nullptr; }
#if ENABLE(CONTENT_EXTENSIONS)
    const ContentExtensions::ContentExtensionsBackend& userContentExtensionBackend() const final { return m_contentExtensions; }
    ContentExtensions::ContentExtensionsBackend m_contentExtensions;
#endif

    Vector<String> m_rules;
    std::optional<UserStyleSheet> m_styleSheet;
};

// A CSS string literal.
String quoted(const String& value)
{
    StringBuilder builder;
    builder.append('"');
    for (auto character : StringView { value }.codeUnits()) {
        if (character == '"' || character == '\\')
            builder.append('\\');
        if (character < 0x20) {
            builder.append('\\', hex(character), ' ');
            continue;
        }
        builder.append(character);
    }
    builder.append('"');
    return builder.toString();
}

ASCIILiteral mimeTypeForFont(std::span<const uint8_t> data)
{
    auto starts = [&](ASCIILiteral magic) {
        return data.size() >= magic.length() && equalSpans(data.first(magic.length()), magic.span8());
    };
    if (starts("wOF2"_s))
        return "font/woff2"_s;
    if (starts("wOFF"_s))
        return "font/woff"_s;
    if (starts("OTTO"_s))
        return "font/otf"_s;
    if (starts("ttcf"_s))
        return "font/collection"_s;
    return "font/ttf"_s;
}

} // anonymous namespace

UserContentProvider& Session::userContentProvider()
{
    if (!m_userContentProvider)
        m_userContentProvider = UserContentProviderZeroG::create();
    return *m_userContentProvider;
}

std::expected<String, String> Session::registerFont(std::span<const uint8_t> data)
{
    if (data.empty())
        return makeUnexpected("empty font data"_str);

    // The family and style come from the font itself. WOFF and WOFF2 are read
    // as sfnt, as WebCore reads web fonts.
    RefPtr<SharedBuffer> sfnt = SharedBuffer::create(data);
    convertWOFFToSfntIfNecessary(sfnt);
    if (!sfnt)
        return makeUnexpected("not a font (WOFF data that does not decode)"_str);
    auto bytes = sfnt->span();
    auto typeface = FontCache::forCurrentThread().fontManager().makeFromData(SkData::MakeWithCopy(bytes.data(), bytes.size()));
    if (!typeface)
        return makeUnexpected("not a font the engine can read (TrueType, OpenType, WOFF or WOFF2)"_str);

    SkString skFamily;
    typeface->getFamilyName(&skFamily);
    auto family = String::fromUTF8(std::span { skFamily.c_str(), skFamily.size() });
    if (family.isEmpty())
        return makeUnexpected("the font has no family name"_str);
    auto style = typeface->fontStyle();
    auto slant = style.slant() == SkFontStyle::kUpright_Slant ? "normal"_s : style.slant() == SkFontStyle::kItalic_Slant ? "italic"_s : "oblique"_s;
    // SkFontStyle widths 1 to 9 are CSS's ultra-condensed to ultra-expanded.
    static constexpr int stretches[] = { 50, 62, 75, 87, 100, 112, 125, 150, 200 };
    int stretch = stretches[std::clamp(style.width(), 1, 9) - 1];
    auto rule = makeString("@font-face { font-family: "_s, quoted(family),
        "; src: url(data:"_s, mimeTypeForFont(data), ";base64,"_s, base64EncodeToString(data),
        "); font-weight: "_s, style.weight(), "; font-style: "_s, slant, "; font-stretch: "_s, stretch, "%; }"_s);
    static_cast<UserContentProviderZeroG&>(userContentProvider()).addRule(rule);
    return family;
}

} // namespace ZeroG
