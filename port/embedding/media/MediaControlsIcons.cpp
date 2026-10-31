// The icons of WebCore's media controls (patch 0033): WebKit's Adwaita set,
// packed into the engine at build time (tools/assets.py) and served from an
// archive in memory, as GTK serves them from a GResource.

#include "config.h"

#include "loader/Loader.h"
#include "loader/MemoryFS.h"
#include <WebCore/ResourceRequest.h>
#include <WebCore/ResourceResponse.h>
#include <WebCore/SharedBuffer.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/MakeString.h>

extern "C" {
extern const unsigned char zerog_media_controls_icons[];
extern const size_t zerog_media_controls_icons_len;
}

namespace WebCore {

RefPtr<SharedBuffer> mediaControlsIconZeroG(const String& iconName, const String& iconType)
{
    static NeverDestroyed<RefPtr<ZeroG::MemoryFS>> icons = [] {
        auto files = ZeroG::MemoryFS::create();
        if (!files->mountArchive("/"_s, std::span { zerog_media_controls_icons, zerog_media_controls_icons_len }))
            return RefPtr<ZeroG::MemoryFS> { };
        return RefPtr<ZeroG::MemoryFS> { WTF::move(files) };
    }();
    if (!icons.get())
        return nullptr;
    ResourceResponse response;
    Vector<uint8_t> data;
    ResourceRequest request { URL { makeString("zerog-media-controls://localhost/"_s, iconName, '.', iconType) } };
    ZeroG::SchemeHandler& handler = *icons.get();
    if (!handler.loadSynchronously(request, response, data) || data.isEmpty())
        return nullptr;
    return SharedBuffer::create(WTF::move(data));
}

} // namespace WebCore
