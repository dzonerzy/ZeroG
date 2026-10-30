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

#include "config.h"
#include "ChromeClientZeroG.h"

#include "View.h"
#include <JavaScriptCore/ConsoleMessage.h>
#include <JavaScriptCore/ScriptCallStack.h>
#include <wtf/text/StringBuilder.h>
#include <JavaScriptCore/ConsoleTypes.h>
#include <WebCore/Cursor.h>
#include <WebCore/FileChooser.h>
#include <WebCore/HitTestResult.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/PopupMenu.h>
#include <WebCore/SearchPopupMenu.h>
#include <wtf/TZoneMallocInlines.h>

namespace ZeroG {

using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(ChromeClientZeroG);

namespace {

// Select popups come with the platform layer's form controls.
class PopupMenuZeroG final : public PopupMenu {
    void show(const IntRect&, LocalFrameView&, int) final { }
    void hide() final { }
    void updateFromElement() final { }
    void disconnectClient() final { }
};

class SearchPopupMenuZeroG final : public SearchPopupMenu {
public:
    SearchPopupMenuZeroG()
        : m_popup(adoptRef(*new PopupMenuZeroG))
    {
    }

private:
    PopupMenu* popupMenu() final { return m_popup.ptr(); }
    void saveRecentSearches(const AtomString&, const Vector<RecentSearch>&) final { }
    void loadRecentSearches(const AtomString&, Vector<RecentSearch>&) final { }
    bool enabled() final { return false; }

    const Ref<PopupMenuZeroG> m_popup;
};

} // namespace

ChromeClientZeroG::ChromeClientZeroG(View& view)
    : m_view(&view)
{
}

FloatRect ChromeClientZeroG::windowRect() const
{
    return m_view ? FloatRect { { }, m_view->size() } : FloatRect { };
}

FloatRect ChromeClientZeroG::pageRect() const
{
    return windowRect();
}

void ChromeClientZeroG::invalidateRootView(const IntRect&)
{
    // The root view is only the page's contents; nothing outside them to repaint.
}

void ChromeClientZeroG::invalidateContentsAndRootView(const IntRect& rect)
{
    if (m_view)
        m_view->invalidate(rect);
}

void ChromeClientZeroG::invalidateContentsForSlowScroll(const IntRect& rect)
{
    if (m_view)
        m_view->invalidate(rect);
}

void ChromeClientZeroG::scroll(const IntSize&, const IntRect& rectToScroll, const IntRect& clipRect)
{
    // Repaint instead of moving pixels: buffers are reused across frames, so a
    // blit would need every buffer's contents to be current.
    if (m_view)
        m_view->invalidate(intersection(rectToScroll, clipRect));
}

RefPtr<PopupMenu> ChromeClientZeroG::createPopupMenu(PopupMenuClient&) const
{
    return adoptRef(*new PopupMenuZeroG);
}

RefPtr<SearchPopupMenu> ChromeClientZeroG::createSearchPopupMenu(PopupMenuClient&) const
{
    return adoptRef(*new SearchPopupMenuZeroG);
}

void ChromeClientZeroG::addMessageToConsole(JSC::MessageSource source, JSC::MessageLevel level, const String& message, unsigned line, unsigned column, const String& sourceURL)
{
    if (m_view)
        m_view->client().consoleMessage(source, level, message, line, column, sourceURL, { });
}

// Messages with their call stack (patch 0018): exceptions and errors.
void ChromeClientZeroG::addConsoleMessage(const Inspector::ConsoleMessage& consoleMessage, const String& message)
{
    if (!m_view)
        return;
    StringBuilder stack;
    if (auto* callStack = consoleMessage.callStack()) {
        for (size_t i = 0; i < callStack->size(); ++i) {
            auto& frame = callStack->at(i);
            if (i)
                stack.append('\n');
            stack.append(frame.functionName().isEmpty() ? "(anonymous)"_s : frame.functionName(), '@', frame.sourceURL(), ':', frame.lineNumber(), ':', frame.columnNumber());
        }
    }
    m_view->client().consoleMessage(consoleMessage.source(), consoleMessage.level(), message, consoleMessage.line(), consoleMessage.column(), consoleMessage.url(), stack.toString());
}

// <input type=file>: the view's client answers, later if it wants (a dialog).
void ChromeClientZeroG::runOpenPanel(LocalFrame&, FileChooser& chooser)
{
    if (!m_view) {
        chooser.cancelFileChoosing();
        return;
    }
    m_view->client().runOpenPanel(chooser.settings(), [chooser = Ref { chooser }](std::optional<Vector<String>>&& paths) {
        if (paths && !paths->isEmpty())
            chooser->chooseFiles(*paths);
        else
            chooser->cancelFileChoosing();
    });
}

void ChromeClientZeroG::runJavaScriptAlert(LocalFrame&, const String& message)
{
    if (m_view)
        m_view->client().alert(message);
}

bool ChromeClientZeroG::runJavaScriptConfirm(LocalFrame&, const String& message)
{
    return m_view && m_view->client().confirm(message);
}

bool ChromeClientZeroG::runJavaScriptPrompt(LocalFrame&, const String& message, const String& defaultValue, String& result)
{
    if (!m_view)
        return false;
    auto reply = m_view->client().prompt(message, defaultValue);
    if (!reply)
        return false;
    result = WTF::move(*reply);
    return true;
}

void ChromeClientZeroG::mouseDidMoveOverElement(const HitTestResult&, OptionSet<PlatformEventModifier>, const String& toolTip, TextDirection)
{
    if (!m_view || toolTip == m_toolTip)
        return;
    m_toolTip = toolTip;
    m_view->client().tooltipChanged(toolTip);
}

void ChromeClientZeroG::setCursor(const Cursor& cursor)
{
    if (m_view)
        m_view->setCursor(cursor.type());
}

void ChromeClientZeroG::triggerRenderingUpdate()
{
    if (m_view)
        m_view->setNeedsRenderingUpdate();
}

bool ChromeClientZeroG::scheduleRenderingUpdate()
{
    // Rendering updates happen in View::render(), paced by the application (texture
    // and CPU modes) or by the window's vsync.
    if (m_view)
        m_view->setNeedsRenderingUpdate();
    return true;
}

} // namespace ZeroG
