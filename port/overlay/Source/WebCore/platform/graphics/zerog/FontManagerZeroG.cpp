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

#include <string>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/ports/SkFontMgr_directory.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

#if OS(WINDOWS)
#include <windows.h>
#endif

namespace WebCore {

// First version: the OS font directory, read with FreeType. The embedded default
// font set, fonts registered by the application (zg_context_register_font) and the
// character fallback table come with the platform layer; Skia's custom manager has
// no character fallback of its own.
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

sk_sp<SkFontMgr> createFontManagerZeroG()
{
    return SkFontMgr_New_Custom_Directory(systemFontDirectory().c_str());
}

} // namespace WebCore

#endif // PLATFORM(ZEROG)
