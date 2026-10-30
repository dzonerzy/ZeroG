// What a drop inserts into editable content: the dragged HTML, else its text.
// Paste goes through editing/libwpe/EditorLibWPE.cpp, which reads the same.

#include "config.h"
#include "Editor.h"

#include "DocumentFragment.h"
#include "FrameDestructionObserverInlines.h"
#include "LocalFrameInlines.h"
#include "Pasteboard.h"
#include "SimpleRange.h"
#include "markup.h"

namespace WebCore {

RefPtr<DocumentFragment> Editor::webContentFromPasteboard(Pasteboard& pasteboard, const SimpleRange& context, bool allowPlainText, bool& chosePlainText)
{
    chosePlainText = false;
    auto types = pasteboard.typesForLegacyUnsafeBindings();
    if (types.contains("text/html"_s)) {
        auto markup = pasteboard.readString("text/html"_s);
        if (!markup.isEmpty())
            return createFragmentFromMarkup(protect(document()), markup, emptyString(), { });
    }
    if (!allowPlainText || !types.contains("text/plain"_s))
        return nullptr;
    chosePlainText = true;
    return createFragmentFromText(context, pasteboard.readString("text/plain"_s));
}

} // namespace WebCore
