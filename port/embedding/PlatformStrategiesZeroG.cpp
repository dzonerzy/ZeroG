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
#include <WebCore/Pasteboard.h>
#include <WebCore/PasteboardCustomData.h>
#include <WebCore/PasteboardItemInfo.h>
#include <WebCore/PasteboardItemsZeroG.h>
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

// What navigator.clipboard reads through: the system clipboard
// (PasteboardItemsZeroG.h) as one item, its types those the page may see.
class PasteboardStrategyZeroG final : public PasteboardStrategy {
public:
    String readStringFromPasteboard(size_t index, const String& type, const String&, const PasteboardContext*) final
    {
        auto buffer = index ? nullptr : systemClipboardZeroG().read(type);
        return buffer ? String::fromUTF8(buffer->span()) : String();
    }

    RefPtr<SharedBuffer> readBufferFromPasteboard(std::optional<size_t> index, const String& type, const String&, const PasteboardContext*) final
    {
        return index.value_or(0) ? nullptr : systemClipboardZeroG().read(type);
    }

    URL readURLFromPasteboard(size_t index, const String& name, String& title, const PasteboardContext* context) final
    {
        title = { };
        auto list = readStringFromPasteboard(index, "text/uri-list"_s, name, context);
        for (auto line : StringView { list }.split('\n')) {
            auto trimmed = line.trim(isASCIIWhitespace<char16_t>);
            if (!trimmed.isEmpty() && !trimmed.startsWith('#'))
                return URL { trimmed.toString() };
        }
        return { };
    }

    std::optional<PasteboardItemInfo> informationForItemAtIndex(size_t index, const String&, int64_t, const PasteboardContext*) final
    {
        if (index)
            return std::nullopt;
        auto types = systemClipboardZeroG().types();
        if (types.isEmpty())
            return std::nullopt;
        PasteboardItemInfo info;
        for (auto& type : types) {
            if (type == customPasteboardDataTypeZeroG)
                continue;
            info.platformTypesByFidelity.append(type);
            if (isWebSafe(type))
                info.webSafeTypesByFidelity.append(type);
            if (!type.startsWith("text/"_s))
                info.isNonTextType = true;
        }
        return info;
    }

    std::optional<Vector<PasteboardItemInfo>> allPasteboardItemInfo(const String& name, int64_t changeCount, const PasteboardContext* context) final
    {
        Vector<PasteboardItemInfo> items;
        if (auto info = informationForItemAtIndex(0, name, changeCount, context))
            items.append(WTF::move(*info));
        return items;
    }

    int getPasteboardItemsCount(const String&, const PasteboardContext*) final
    {
        return systemClipboardZeroG().types().isEmpty() ? 0 : 1;
    }

    Vector<String> typesSafeForDOMToReadAndWrite(const String&, const String& origin, const PasteboardContext*) final
    {
        return Pasteboard::createForCopyAndPaste(nullptr)->typesSafeForBindings(origin);
    }

    int64_t writeCustomData(const Vector<PasteboardCustomData>& data, const String&, const PasteboardContext*) final
    {
        Pasteboard::createForCopyAndPaste(nullptr)->writeCustomData(data);
        return systemClipboardZeroG().changeCount();
    }

    bool containsStringSafeForDOMToReadForType(const String& type, const String&, const PasteboardContext*) final
    {
        return isWebSafe(type) && systemClipboardZeroG().types().contains(type);
    }

private:
    static bool isWebSafe(const String& type)
    {
        return type == "text/plain"_s || type == "text/html"_s || type == "text/uri-list"_s || type == "image/png"_s;
    }
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
