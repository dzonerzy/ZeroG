/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

// Derived from Tools/TestWebKitAPI/Tests/WebCore/TestPlatformStrategies.cpp at the
// pinned WebKit commit, reduced to the platforms ZeroG builds (docs/PROVENANCE.md).

#include "config.h"
#include "PlatformStrategiesZeroG.h"

#include "loader/Loader.h"
#include <WebCore/BlobRegistry.h>
#include <WebCore/LoaderStrategy.h>
#include <WebCore/MediaStrategy.h>
#include <WebCore/PasteboardItemInfo.h>
#include <WebCore/PasteboardStrategy.h>
#include <WebCore/PlatformStrategies.h>
#include <WebCore/ResourceError.h>
#include <WebCore/SharedBuffer.h>
#include <WebCore/SubresourceLoader.h>
#include <mutex>
#include <wtf/URL.h>
#include <wtf/text/WTFString.h>

namespace ZeroG {

using namespace WebCore;

namespace {

class PasteboardStrategyZeroG final : public PasteboardStrategy {
public:
    String readStringFromPasteboard(size_t, const String&, const String&, const PasteboardContext*) final { return { }; }
    RefPtr<SharedBuffer> readBufferFromPasteboard(std::optional<size_t>, const String&, const String&, const PasteboardContext*) final { return nullptr; }
    URL readURLFromPasteboard(size_t, const String&, String&, const PasteboardContext*) final { return { }; }
    std::optional<PasteboardItemInfo> informationForItemAtIndex(size_t, const String&, int64_t, const PasteboardContext*) final { return std::nullopt; }
    std::optional<Vector<PasteboardItemInfo>> allPasteboardItemInfo(const String&, int64_t, const PasteboardContext*) final { return std::nullopt; }
    int getPasteboardItemsCount(const String&, const PasteboardContext*) final { return 0; }
    Vector<String> typesSafeForDOMToReadAndWrite(const String&, const String&, const PasteboardContext*) final { return { }; }
    int64_t writeCustomData(const Vector<PasteboardCustomData>&, const String&, const PasteboardContext*) final { return 0; }
    bool containsStringSafeForDOMToReadForType(const String&, const String&, const PasteboardContext*) final { return false; }
};

class MediaStrategyZeroG final : public MediaStrategy {
public:
#if ENABLE(WEB_AUDIO)
    Ref<AudioDestination> createAudioDestination(const AudioDestinationCreationOptions&) final { RELEASE_ASSERT_NOT_REACHED(); }
#endif
};

class PlatformStrategiesZeroG final : public PlatformStrategies {
private:
    LoaderStrategy* createLoaderStrategy() final { return ZeroG::createLoaderStrategy(); }
    PasteboardStrategy* createPasteboardStrategy() final { return new PasteboardStrategyZeroG; }
    MediaStrategy* createMediaStrategy() final { return new MediaStrategyZeroG; }
    BlobRegistry* createBlobRegistry() final { return ZeroG::createBlobRegistry(); }
#if ENABLE(DECLARATIVE_WEB_PUSH)
    PushStrategy* createPushStrategy() final { return nullptr; }
#endif
};

} // anonymous namespace

void installPlatformStrategies()
{
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        setPlatformStrategies(new PlatformStrategiesZeroG);
    });
}

} // namespace ZeroG
