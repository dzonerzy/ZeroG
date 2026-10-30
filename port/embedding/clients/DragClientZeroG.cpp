#include "config.h"
#include "DragClientZeroG.h"

#include "View.h"
#include <WebCore/DataTransfer.h>
#include <WebCore/DocumentPage.h>
#include <WebCore/Frame.h>
#include <WebCore/Page.h>
#include <WebCore/Pasteboard.h>
#include <WebCore/PasteboardItemsZeroG.h>

namespace ZeroG {

using namespace WebCore;

void DragClientZeroG::startDrag(DragItem, DataTransfer& dataTransfer, WebCore::Frame& frame, const std::optional<NodeIdentifier>&)
{
    // The drag's data is what the page put in the DataTransfer (dragstart) and
    // what WebCore added for links, images and selections.
    RefPtr page = frame.page();
    auto* view = page ? View::fromPage(*page) : nullptr;
    auto* items = dataTransfer.pasteboard().zerogItems();
    if (view && items)
        view->startInternalDrag(*items);
}

} // namespace ZeroG
