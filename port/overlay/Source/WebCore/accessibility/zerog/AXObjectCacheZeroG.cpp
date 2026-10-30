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
#include "AXObjectCache.h"

#include "AXNotifications.h"
#include "AccessibilityClientZeroG.h"
#include "AccessibilityObject.h"
#include "Document.h"
#include "DocumentPage.h"
#include "Page.h"
#include "RenderView.h"

// WebCore builds the accessibility tree; the per-OS bridges that hand it to
// assistive technologies (UI Automation, AT-SPI) live in the embedding layer
// (port/embedding/accessibility) and read the tree themselves. Here platform
// notifications become AccessibilityClientZeroG events. Objects are identified
// by their AXID: wrappers stay placeholders (AccessibilityObjectWrapper,
// AXCoreObject.h).

namespace WebCore {

static AccessibilityClientZeroG* s_client;

void setAccessibilityClientZeroG(AccessibilityClientZeroG* client)
{
    s_client = client;
}

static void notifyClient(AccessibilityObject* object, AccessibilityEventZeroG event)
{
    if (!s_client || !object)
        return;
    RefPtr document = object->document();
    RefPtr page = document ? document->page() : nullptr;
    if (page)
        s_client->accessibilityEvent(*page, *object, event);
}

void AXObjectCache::attachWrapper(AccessibilityObject& object)
{
    auto wrapper = adoptRef(*new AccessibilityObjectWrapper());
    object.setWrapper(wrapper.ptr());
}

void AXObjectCache::detachWrapper(AXCoreObject* object, AccessibilityDetachmentType)
{
    if (s_client && object)
        s_client->accessibilityObjectDetached(object->objectID());
}

void AXObjectCache::postPlatformNotification(AccessibilityObject& object, AXNotification notification)
{
    switch (notification) {
    case AXNotification::ChildrenChanged:
        notifyClient(&object, AccessibilityEventZeroG::ChildrenChanged);
        break;
    case AXNotification::NameChanged:
    case AXNotification::LabelChanged:
        notifyClient(&object, AccessibilityEventZeroG::NameChanged);
        break;
    case AXNotification::ValueChanged:
    case AXNotification::TextChanged:
    case AXNotification::MenuListValueChanged:
        notifyClient(&object, AccessibilityEventZeroG::ValueChanged);
        break;
    case AXNotification::CheckedStateChanged:
    case AXNotification::SelectedStateChanged:
    case AXNotification::ExpandedChanged:
    case AXNotification::RowExpanded:
    case AXNotification::RowCollapsed:
    case AXNotification::PressedStateChanged:
    case AXNotification::DisabledStateChanged:
    case AXNotification::ReadOnlyStatusChanged:
    case AXNotification::RequiredStatusChanged:
        notifyClient(&object, AccessibilityEventZeroG::StateChanged);
        break;
    default:
        break;
    }
}

void AXObjectCache::nodeTextChangePlatformNotification(AccessibilityObject* object, AXTextChange, unsigned, const String&)
{
    notifyClient(object, AccessibilityEventZeroG::ValueChanged);
}

void AXObjectCache::frameLoadingEventPlatformNotification(RenderView* renderView, AXLoadingEvent loadingEvent)
{
    if (!renderView || loadingEvent != AXLoadingEvent::Finished)
        return;
    RefPtr object = getOrCreate(*renderView);
    if (object && object->role() == AccessibilityRole::WebArea)
        notifyClient(object.get(), AccessibilityEventZeroG::LoadComplete);
}

void AXObjectCache::handleScrolledToAnchor(const Node&)
{
}

void AXObjectCache::platformHandleFocusedUIElementChanged(AccessibilityObject*, AccessibilityObject* newFocus)
{
    notifyClient(newFocus, AccessibilityEventZeroG::FocusChanged);
}

void AXObjectCache::platformPerformDeferredCacheUpdate()
{
}

} // namespace WebCore
