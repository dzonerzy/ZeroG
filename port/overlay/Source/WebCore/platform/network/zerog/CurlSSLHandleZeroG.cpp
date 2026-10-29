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
#include "CurlSSLHandle.h"

#include <wtf/FileSystem.h>
#include <wtf/text/WTFString.h>

namespace WebCore {

// Root CAs on Unix (ZeroG.md, "Certificati"): BoringSSL does not read the system
// store by itself, so point it at the distribution's CA bundle. Windows uses
// CurlSSLHandleWin.cpp (CryptoAPI). An embedded bundle, for systems without one,
// is set through the API.
void CurlSSLHandle::platformInitialize()
{
    static constexpr ASCIILiteral bundles[] = {
        "/etc/ssl/certs/ca-certificates.crt"_s, // Debian, Ubuntu, Arch, Gentoo
        "/etc/pki/tls/certs/ca-bundle.crt"_s, // Fedora, RHEL, CentOS
        "/etc/ssl/ca-bundle.pem"_s, // openSUSE
        "/etc/ssl/cert.pem"_s, // Alpine, macOS, BSDs
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem"_s, // RHEL 8+
    };
    for (auto bundle : bundles) {
        String path { bundle };
        if (FileSystem::fileExists(path)) {
            setCACertPath(WTF::move(path));
            return;
        }
    }
}

} // namespace WebCore
