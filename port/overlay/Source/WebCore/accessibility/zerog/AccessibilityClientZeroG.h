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

#pragma once

#include <WebCore/AXID.h>

namespace WebCore {

class AccessibilityObject;
class Page;

// What changed on an accessibility object, as the OS bridges (UI Automation,
// AT-SPI; port/embedding/accessibility) report it to assistive technologies.
enum class AccessibilityEventZeroG : uint8_t {
    FocusChanged, // the object got the focus
    ChildrenChanged,
    NameChanged,
    ValueChanged,
    StateChanged, // checked, selected, expanded, pressed, enabled, read-only, required
    LoadComplete, // a web area finished loading
};

// AXObjectCacheZeroG.cpp's platform notifications go to the embedding layer
// through this. Calls come on the main thread, from inside WebCore's own
// notification processing: a client must not change the document there.
class AccessibilityClientZeroG {
public:
    virtual ~AccessibilityClientZeroG() = default;
    virtual void accessibilityEvent(Page&, AccessibilityObject&, AccessibilityEventZeroG) = 0;
    // The object with this ID is gone (its page may be too).
    virtual void accessibilityObjectDetached(AXID) = 0;
};

// One client for the process; null: notifications go nowhere.
WEBCORE_EXPORT void setAccessibilityClientZeroG(AccessibilityClientZeroG*);

} // namespace WebCore
