/*
 * Copyright (C) 2006 Eric Seidel (eric@webkit.org)
 * Copyright (C) 2008-2025 Apple Inc. All rights reserved.
 * Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2012 Samsung Electronics. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// Derived from EmptyChromeClient (Source/WebCore/loader/EmptyClients.h) at the
// pinned WebKit commit: the same defaults, with invalidation, rendering updates
// and geometry routed to the ZeroG view (docs/PROVENANCE.md).

#pragma once

#include <WebCore/ChromeClient.h>
#include <WebCore/FocusOptions.h>

namespace ZeroG {

class View;

class ChromeClientZeroG final : public WebCore::ChromeClient {
    WTF_MAKE_TZONE_ALLOCATED(ChromeClientZeroG);
public:
    explicit ChromeClientZeroG(View&);

    // The page can outlive its view while something still references it.
    void detachFromView() { m_view = nullptr; }

private:
    void chromeDestroyed() final { }

    void setWindowRect(const WebCore::FloatRect&) final { }
    WebCore::FloatRect windowRect() const final;
    WebCore::FloatRect pageRect() const final;

    void focus() final { }
    void unfocus() final { }
    bool canTakeFocus(WebCore::FocusDirection) const final { return false; }
    void takeFocus(WebCore::FocusDirection) final { }
    void focusedElementChanged(WebCore::Element*, WebCore::LocalFrame*, WebCore::FocusOptions) final { }
    void focusedFrameChanged(WebCore::Frame*) final { }

    RefPtr<WebCore::Page> createWindow(WebCore::LocalFrame&, const String&, const WebCore::WindowFeatures&, const WebCore::NavigationAction&) final { return nullptr; }
    void show() final { }
    bool canRunModal() const final { return false; }
    void runModal() final { }
    bool isPopup() const final { return false; }
    void setResizable(bool) final { }

    void addMessageToConsole(JSC::MessageSource, JSC::MessageLevel, const String&, unsigned, unsigned, const String&) final;
    void addConsoleMessage(const Inspector::ConsoleMessage&, const String&) final;

    bool canRunBeforeUnloadConfirmPanel() final { return false; }
    bool runBeforeUnloadConfirmPanel(String&&, WebCore::LocalFrame&) final { return true; }
    void closeWindow() final { }
    void rootFrameAdded(const WebCore::LocalFrame&) final { }
    void rootFrameRemoved(const WebCore::LocalFrame&) final { }

    void runJavaScriptAlert(WebCore::LocalFrame&, const String&) final;
    bool runJavaScriptConfirm(WebCore::LocalFrame&, const String&) final;
    bool runJavaScriptPrompt(WebCore::LocalFrame&, const String&, const String&, String&) final;

    RefPtr<WebCore::PopupMenu> createPopupMenu(WebCore::PopupMenuClient&) const final;
    RefPtr<WebCore::SearchPopupMenu> createSearchPopupMenu(WebCore::PopupMenuClient&) const final;

    WebCore::KeyboardUIMode keyboardUIMode() final { return WebCore::KeyboardAccessDefault; }

    bool hasAccessoryMousePointingDevice() const final { return true; }
    bool hoverSupportedByPrimaryPointingDevice() const final { return true; }
    bool hoverSupportedByAnyAvailablePointingDevice() const final { return true; }
    std::optional<WebCore::PointerCharacteristics> pointerCharacteristicsOfPrimaryPointingDevice() const final { return WebCore::PointerCharacteristics::Fine; }
    OptionSet<WebCore::PointerCharacteristics> pointerCharacteristicsOfAllAvailablePointingDevices() const final { return WebCore::PointerCharacteristics::Fine; }

    void invalidateRootView(const WebCore::IntRect&) final;
    void invalidateContentsAndRootView(const WebCore::IntRect&) final;
    void invalidateContentsForSlowScroll(const WebCore::IntRect&) final;
    void scroll(const WebCore::IntSize&, const WebCore::IntRect&, const WebCore::IntRect&) final;

    WebCore::IntPoint screenToRootView(const WebCore::IntPoint& point) const final { return point; }
    WebCore::IntPoint rootViewToScreen(const WebCore::IntPoint& point) const final { return point; }
    WebCore::IntRect rootViewToScreen(const WebCore::IntRect& rect) const final { return rect; }
    WebCore::IntPoint accessibilityScreenToRootView(const WebCore::IntPoint& point) const final { return point; }
    WebCore::IntRect rootViewToAccessibilityScreen(const WebCore::IntRect& rect) const final { return rect; }

    void didFinishLoadingImageForElement(WebCore::HTMLImageElement&) final { }

    PlatformPageClient platformPageClient() const final { return 0; }
    void contentsSizeChanged(WebCore::LocalFrame&, const WebCore::IntSize&) const final { }
    void intrinsicContentsSizeChanged(const WebCore::IntSize&) const final { }

    void mouseDidMoveOverElement(const WebCore::HitTestResult&, OptionSet<WebCore::PlatformEventModifier>, const String&, WebCore::TextDirection) final;
    void print(WebCore::LocalFrame&, const WebCore::StringWithDirection&) final { }
    void exceededDatabaseQuota(WebCore::LocalFrame&, const String&, WebCore::DatabaseDetails) final { }

    RefPtr<WebCore::ColorChooser> createColorChooser(WebCore::ColorChooserClient&, const WebCore::Color&) final { return nullptr; }
    RefPtr<WebCore::DataListSuggestionPicker> createDataListSuggestionPicker(WebCore::DataListSuggestionsClient&) final { return nullptr; }
    bool canShowDataListSuggestionLabels() const final { return false; }
    RefPtr<WebCore::DateTimeChooser> createDateTimeChooser(WebCore::DateTimeChooserClient&) final { return nullptr; }

    void setTextIndicator(RefPtr<WebCore::TextIndicator>&&) const final { }
    void updateTextIndicator(RefPtr<WebCore::TextIndicator>&&) const final { }

    // The view drives rendering updates itself (scheduleRenderingUpdate()).
    WebCore::DisplayRefreshMonitorFactory* displayRefreshMonitorFactory() const final { return nullptr; }

    void runOpenPanel(WebCore::LocalFrame&, WebCore::FileChooser&) final;
    void showShareSheet(WebCore::ShareDataWithParsedURL&&, CompletionHandler<void(bool)>&& completionHandler) final { completionHandler(false); }
    void loadIconForFiles(const Vector<String>&, WebCore::FileIconLoader&) final { }

    void elementDidFocus(WebCore::Element&, const WebCore::FocusOptions&) final { }
    void elementDidBlur(WebCore::Element&) final { }

    void setCursor(const WebCore::Cursor&) final;
    void setCursorHiddenUntilMouseMoves(bool) final { }

    void scrollContainingScrollViewsToRevealRect(const WebCore::IntRect&) const final { }
    void scrollMainFrameToRevealRect(const WebCore::IntRect&) const final { }

    void attachRootGraphicsLayer(WebCore::LocalFrame&, WebCore::GraphicsLayer*) final { }
    void attachViewOverlayGraphicsLayer(WebCore::GraphicsLayer*) final { }
    void setNeedsOneShotDrawingSynchronization() final { }
    void triggerRenderingUpdate() final;
    bool scheduleRenderingUpdate() final;

    void wheelEventHandlersChanged(bool) final { }

    void didAssociateFormControls(const Vector<Ref<WebCore::Element>>&, WebCore::LocalFrame&) final { }
    bool shouldNotifyOnFormChanges() final { return false; }

    RefPtr<WebCore::Icon> createIconForFiles(const Vector<String>&) final { return nullptr; }

    View* m_view;
    String m_toolTip;
};

} // namespace ZeroG
