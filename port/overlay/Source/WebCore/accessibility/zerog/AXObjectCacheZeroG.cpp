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

#include "AccessibilityObject.h"

// WebCore builds the accessibility tree; delivering it to a screen reader needs a
// per-OS bridge (AT-SPI, UI Automation, NSAccessibility), which ZeroG.md places
// after version 1 ("Accessibilità"). Until then platform notifications go nowhere
// and wrappers are placeholders (AccessibilityObjectWrapper, AXCoreObject.h).

namespace WebCore {

void AXObjectCache::attachWrapper(AccessibilityObject& object)
{
    auto wrapper = adoptRef(*new AccessibilityObjectWrapper());
    object.setWrapper(wrapper.ptr());
}

void AXObjectCache::detachWrapper(AXCoreObject*, AccessibilityDetachmentType)
{
}

void AXObjectCache::postPlatformNotification(AccessibilityObject&, AXNotification)
{
}

void AXObjectCache::nodeTextChangePlatformNotification(AccessibilityObject*, AXTextChange, unsigned, const String&)
{
}

void AXObjectCache::frameLoadingEventPlatformNotification(RenderView*, AXLoadingEvent)
{
}

void AXObjectCache::handleScrolledToAnchor(const Node&)
{
}

void AXObjectCache::platformHandleFocusedUIElementChanged(AccessibilityObject*, AccessibilityObject*)
{
}

void AXObjectCache::platformPerformDeferredCacheUpdate()
{
}

} // namespace WebCore
