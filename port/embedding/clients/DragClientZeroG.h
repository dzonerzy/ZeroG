#pragma once

// Drags that start in a ZeroG page (a selection, a link, an image, an element with
// draggable): there is no system drag loop to hand them to, so the view drives
// them with the mouse events that follow (View::startInternalDrag), as WebKit2's
// UI process drives a web process's drag.

#include <WebCore/DragClient.h>

namespace ZeroG {

class DragClientZeroG final : public WebCore::DragClient {
public:
    void willPerformDragDestinationAction(WebCore::DragDestinationAction, const WebCore::DragData&) final { }
    void willPerformDragSourceAction(WebCore::DragSourceAction, const WebCore::IntPoint&, WebCore::DataTransfer&) final { }
    OptionSet<WebCore::DragSourceAction> dragSourceActionMaskForPoint(const WebCore::IntPoint&) final { return WebCore::anyDragSourceAction(); }
    void startDrag(WebCore::DragItem, WebCore::DataTransfer&, WebCore::Frame&, const std::optional<WebCore::NodeIdentifier>&) final;
};

} // namespace ZeroG
