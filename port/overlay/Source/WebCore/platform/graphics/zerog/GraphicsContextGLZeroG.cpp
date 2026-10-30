/*
 * Copyright (C) 2023 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// WebGL (the standard, dev and debug profiles build it, on Windows): upstream's
// coordinated graphics context over ANGLE (GraphicsContextGLCoordinated, created by
// platform/graphics/egl/GraphicsContextGLEGL.cpp), its drawing buffer shown as a
// layer by the compositor. Upstream sets up the shared texture with libepoxy
// only (GraphicsContextGLCoordinatedEpoxy.cpp); this is the same through ANGLE's
// entry points, which on Windows are the engine's GL.

#include "config.h"
#include "GraphicsContextGLCoordinated.h"

#if ENABLE(WEBGL) && USE(COORDINATED_GRAPHICS) && !USE(LIBEPOXY)
#include "ANGLEHeaders.h"

namespace WebCore {

GCGLuint GraphicsContextGLCoordinated::setupCurrentTexture() const
{
    // The drawing buffer's texture, as ANGLE bound it.
    GLint texture;
    GL_GetIntegerv(GL_TEXTURE_BINDING_2D, &texture);

    // The parameters the compositor samples it with, applied now: ANGLE caches
    // them until another call syncs the texture's state.
    GL_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GL_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GL_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    GL_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return texture;
}

} // namespace WebCore

#endif // ENABLE(WEBGL) && USE(COORDINATED_GRAPHICS) && !USE(LIBEPOXY)
