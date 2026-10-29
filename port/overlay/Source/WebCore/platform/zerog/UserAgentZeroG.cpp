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
#include "UserAgent.h"

#include <wtf/NeverDestroyed.h>
#include <wtf/text/MakeString.h>

// The user agent is fragile: sites sniff it. This follows the shape WebKit's other
// non-Apple ports use (WebCore/platform/glib/UserAgentGLib.cpp): the real OS, the
// frozen AppleWebKit/605.1.15 token, and "Version/X Safari/Y", without which many
// sites serve broken content to WebKit.

namespace WebCore {

static constexpr ASCIILiteral platformForUAString()
{
#if OS(WINDOWS)
    return "Windows NT 10.0; Win64; x64"_s;
#elif OS(DARWIN)
    return "Macintosh; Intel Mac OS X 10_15_7"_s;
#elif CPU(ARM64)
    return "X11; Linux aarch64"_s;
#else
    return "X11; Linux x86_64"_s;
#endif
}

static constexpr ASCIILiteral versionForUAString()
{
    // https://bugs.webkit.org/show_bug.cgi?id=180365
    return "605.1.15"_s;
}

static const String& standardUserAgentStatic()
{
    static NeverDestroyed<String> userAgent(makeString("Mozilla/5.0 ("_s, platformForUAString(), ") AppleWebKit/"_s,
        versionForUAString(), " (KHTML, like Gecko) Version/18.0 Safari/"_s, versionForUAString()));
    return userAgent;
}

String standardUserAgent(const String& applicationName, const String& applicationVersion)
{
    if (applicationName.isEmpty())
        return standardUserAgentStatic();
    return makeString(standardUserAgentStatic(), ' ', applicationName, '/',
        applicationVersion.isEmpty() ? String(versionForUAString()) : applicationVersion);
}

String standardUserAgentForURL(const URL&)
{
    return { };
}

} // namespace WebCore
