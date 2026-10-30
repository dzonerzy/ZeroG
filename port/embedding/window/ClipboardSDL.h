#pragma once

// The system clipboard through SDL3, for window mode (SDL's video subsystem is
// running): text, HTML, URLs and images by MIME type. Installed when a window is
// created, unless the application provides its own clipboard.

#include <WebCore/PasteboardItemsZeroG.h>

namespace ZeroG {

class ClipboardSDL final : public WebCore::PasteboardItemsZeroG {
public:
    static Ref<ClipboardSDL> create() { return adoptRef(*new ClipboardSDL); }

    Vector<String> types() final;
    RefPtr<WebCore::SharedBuffer> read(const String& type) final;
    void write(Vector<Item>&&) final;
    int64_t changeCount() final { return m_changeCount; }

    // SDL_EVENT_CLIPBOARD_UPDATE: another application changed it.
    void didChange() { ++m_changeCount; }

private:
    ClipboardSDL() = default;

    int64_t m_changeCount { 0 };
};

// The clipboard WebCore uses: the application's own if it set one, else SDL's
// once a window exists (SDL's video subsystem running), else one in process.
void setApplicationClipboard(RefPtr<WebCore::PasteboardItemsZeroG>&&);
void useSDLClipboard();
// SDL_EVENT_CLIPBOARD_UPDATE.
void sdlClipboardChanged();

} // namespace ZeroG
