/*
 * Copyright (C) 2026 The ZeroG Authors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "FontManagerZeroG.h"

#if PLATFORM(ZEROG)

#include <array>
#include <span>
#include <string>
#include <string_view>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkData.h>
#include <skia/core/SkFontStyle.h>
#include <skia/core/SkStream.h>
#include <skia/core/SkString.h>
#include <skia/core/SkTypeface.h>
#include <skia/ports/SkFontMgr_directory.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

#if OS(WINDOWS)
#include <windows.h>
#endif

namespace WebCore {

// First version: the OS font directory, read with FreeType. The embedded default
// font set and fonts registered by the application (zg_context_register_font) come
// with the platform layer.
static std::string systemFontDirectory()
{
#if OS(WINDOWS)
    char windowsDirectory[MAX_PATH];
    UINT length = GetWindowsDirectoryA(windowsDirectory, MAX_PATH);
    if (!length || length >= MAX_PATH)
        return "C:\\Windows\\Fonts\\";
    return std::string(windowsDirectory, length) + "\\Fonts\\";
#elif OS(DARWIN)
    return "/System/Library/Fonts/";
#else
    return "/usr/share/fonts/";
#endif
}

// CSS generic families (and the names WebCore passes for them) mapped to installed
// families, first match wins. Skia's custom manager only knows real family names.
struct GenericFamily {
    std::string_view name;
    std::span<const char* const> candidates;
};

#if OS(WINDOWS)
static constexpr std::array sansSerifFamilies { "Arial", "Segoe UI", "Tahoma", "Verdana" };
static constexpr std::array serifFamilies { "Times New Roman", "Georgia", "Cambria" };
static constexpr std::array monospaceFamilies { "Consolas", "Courier New", "Lucida Console" };
static constexpr std::array cursiveFamilies { "Comic Sans MS", "Segoe Script" };
static constexpr std::array fantasyFamilies { "Impact", "Gabriola" };
static constexpr std::array systemUIFamilies { "Segoe UI", "Tahoma", "Arial" };
static constexpr std::array mathFamilies { "Cambria Math" };
#elif OS(DARWIN)
static constexpr std::array sansSerifFamilies { "Helvetica", "Helvetica Neue", "Arial" };
static constexpr std::array serifFamilies { "Times", "Times New Roman", "Georgia" };
static constexpr std::array monospaceFamilies { "Menlo", "Monaco", "Courier" };
static constexpr std::array cursiveFamilies { "Apple Chancery", "Snell Roundhand" };
static constexpr std::array fantasyFamilies { "Papyrus", "Impact" };
static constexpr std::array systemUIFamilies { "Helvetica Neue", "Helvetica" };
static constexpr std::array mathFamilies { "STIX Two Math" };
#else
static constexpr std::array sansSerifFamilies { "DejaVu Sans", "Liberation Sans", "Noto Sans", "FreeSans", "Arial" };
static constexpr std::array serifFamilies { "DejaVu Serif", "Liberation Serif", "Noto Serif", "FreeSerif", "Times New Roman" };
static constexpr std::array monospaceFamilies { "DejaVu Sans Mono", "Liberation Mono", "Noto Sans Mono", "FreeMono", "Courier New" };
static constexpr std::array cursiveFamilies { "Comic Neue", "URW Chancery L" };
static constexpr std::array fantasyFamilies { "Impact" };
static constexpr std::array systemUIFamilies { "Cantarell", "Ubuntu", "Noto Sans", "DejaVu Sans" };
static constexpr std::array mathFamilies { "STIX Two Math", "Latin Modern Math", "DejaVu Math TeX Gyre" };
#endif

static constexpr std::array genericFamilies {
    GenericFamily { "sans-serif", sansSerifFamilies },
    GenericFamily { "serif", serifFamilies },
    GenericFamily { "monospace", monospaceFamilies },
    GenericFamily { "cursive", cursiveFamilies },
    GenericFamily { "fantasy", fantasyFamilies },
    GenericFamily { "system-ui", systemUIFamilies },
    GenericFamily { "ui-sans-serif", sansSerifFamilies },
    GenericFamily { "ui-serif", serifFamilies },
    GenericFamily { "ui-monospace", monospaceFamilies },
    GenericFamily { "math", mathFamilies },
};

class FontManagerZeroG final : public SkFontMgr {
public:
    explicit FontManagerZeroG(sk_sp<SkFontMgr>&& fonts)
        : m_fonts(std::move(fonts))
    {
        m_defaultFamily = matchCandidates(sansSerifFamilies);
        for (int i = 0; !m_defaultFamily && i < m_fonts->countFamilies(); ++i)
            m_defaultFamily = m_fonts->createStyleSet(i);
    }

private:
    sk_sp<SkFontStyleSet> matchCandidates(std::span<const char* const> candidates) const
    {
        for (auto* candidate : candidates) {
            if (auto set = m_fonts->matchFamily(candidate); set && set->count())
                return set;
        }
        return nullptr;
    }

    // Null when the name is not a generic family and is not installed. A null or
    // empty name asks for the default family, as Skia's managers do.
    sk_sp<SkFontStyleSet> styleSetForFamily(const char familyName[]) const
    {
        if (!familyName || !*familyName)
            return m_defaultFamily;
        if (auto set = m_fonts->matchFamily(familyName); set && set->count())
            return set;
        for (auto& generic : genericFamilies) {
            if (generic.name != familyName)
                continue;
            if (auto set = matchCandidates(generic.candidates))
                return set;
            // Generic families always resolve to something.
            return m_defaultFamily;
        }
        return nullptr;
    }

    int onCountFamilies() const final { return m_fonts->countFamilies(); }
    void onGetFamilyName(int index, SkString* familyName) const final { m_fonts->getFamilyName(index, familyName); }
    sk_sp<SkFontStyleSet> onCreateStyleSet(int index) const final { return m_fonts->createStyleSet(index); }

    sk_sp<SkFontStyleSet> onMatchFamily(const char familyName[]) const final
    {
        return styleSetForFamily(familyName);
    }

    sk_sp<SkTypeface> onMatchFamilyStyle(const char familyName[], const SkFontStyle& style) const final
    {
        auto set = styleSetForFamily(familyName);
        return set ? set->matchStyle(style) : nullptr;
    }

    // Character fallback: the default family first, then every installed family in
    // order. WebCore caches the result per character and font (SystemFallbackFontCache).
    sk_sp<SkTypeface> onMatchFamilyStyleCharacter(const char familyName[], const SkFontStyle& style, const char*[], int, SkUnichar character) const final
    {
        auto hasCharacter = [character](const sk_sp<SkTypeface>& typeface) {
            return typeface && typeface->unicharToGlyph(character);
        };
        if (auto set = styleSetForFamily(familyName)) {
            if (auto typeface = set->matchStyle(style); hasCharacter(typeface))
                return typeface;
        }
        if (m_defaultFamily) {
            if (auto typeface = m_defaultFamily->matchStyle(style); hasCharacter(typeface))
                return typeface;
        }
        for (int i = 0; i < m_fonts->countFamilies(); ++i) {
            if (auto typeface = m_fonts->createStyleSet(i)->matchStyle(style); hasCharacter(typeface))
                return typeface;
        }
        return nullptr;
    }

    sk_sp<SkTypeface> onMakeFromData(sk_sp<SkData> data, int ttcIndex) const final { return m_fonts->makeFromData(std::move(data), ttcIndex); }
    sk_sp<SkTypeface> onMakeFromStreamIndex(std::unique_ptr<SkStreamAsset> stream, int ttcIndex) const final { return m_fonts->makeFromStream(std::move(stream), ttcIndex); }
    sk_sp<SkTypeface> onMakeFromStreamArgs(std::unique_ptr<SkStreamAsset> stream, const SkFontArguments& arguments) const final { return m_fonts->makeFromStream(std::move(stream), arguments); }
    sk_sp<SkTypeface> onMakeFromFile(const char path[], int ttcIndex) const final { return m_fonts->makeFromFile(path, ttcIndex); }

    sk_sp<SkTypeface> onLegacyMakeTypeface(const char familyName[], SkFontStyle style) const final
    {
        if (auto typeface = onMatchFamilyStyle(familyName, style))
            return typeface;
        return m_defaultFamily ? m_defaultFamily->matchStyle(style) : nullptr;
    }

    sk_sp<SkFontMgr> m_fonts;
    sk_sp<SkFontStyleSet> m_defaultFamily;
};

sk_sp<SkFontMgr> createFontManagerZeroG()
{
    return sk_make_sp<FontManagerZeroG>(SkFontMgr_New_Custom_Directory(systemFontDirectory().c_str()));
}

} // namespace WebCore

#endif // PLATFORM(ZEROG)
