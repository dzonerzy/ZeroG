/*
 * Copyright (C) 2006 Eric Seidel (eric@webkit.org)
 * Copyright (C) 2008-2025 Apple Inc. All rights reserved.
 * Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2012 Samsung Electronics. All rights reserved.
 * Copyright (C) 2017 Sony Interactive Entertainment Inc.
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

// Derived from EmptyEditorClient (Source/WebCore/loader/EmptyClients.cpp) and the
// key bindings of the Windows WebPage (Source/WebKit/WebProcess/WebPage/win/
// WebPageWin.cpp) at the pinned WebKit commit: editing is allowed, key events
// become editing commands, and undo/redo keep a stack (docs/PROVENANCE.md).

#pragma once

#include <WebCore/DOMPasteAccess.h>
#include <WebCore/EditorClient.h>
#include <WebCore/TextCheckerClient.h>
#include <WebCore/UndoStep.h>
#include <wtf/Vector.h>

namespace ZeroG {

class EditorClientZeroG final : public WebCore::EditorClient {
    WTF_MAKE_TZONE_ALLOCATED(EditorClientZeroG);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(EditorClientZeroG);
public:
    EditorClientZeroG() = default;

private:
    bool shouldDeleteRange(const std::optional<WebCore::SimpleRange>&) final { return true; }
    bool smartInsertDeleteEnabled() final { return false; }
    bool isSelectTrailingWhitespaceEnabled() const final { return false; }
    bool isContinuousSpellCheckingEnabled() final { return false; }
    void toggleContinuousSpellChecking() final { }
    bool isGrammarCheckingEnabled() final { return false; }
    void toggleGrammarChecking() final { }
    int spellCheckerDocumentTag() final { return -1; }

    bool shouldBeginEditing(const WebCore::SimpleRange&) final { return true; }
    bool shouldEndEditing(const WebCore::SimpleRange&) final { return true; }
    bool shouldInsertNode(WebCore::Node&, const std::optional<WebCore::SimpleRange>&, WebCore::EditorInsertAction) final { return true; }
    bool shouldInsertText(const String&, const std::optional<WebCore::SimpleRange>&, WebCore::EditorInsertAction) final { return true; }
    bool shouldChangeSelectedRange(const std::optional<WebCore::SimpleRange>&, const std::optional<WebCore::SimpleRange>&, WebCore::Affinity, bool) final { return true; }

    bool shouldApplyStyle(const WebCore::StyleProperties&, const std::optional<WebCore::SimpleRange>&) final { return true; }
    void didApplyStyle() final { }
    bool shouldMoveRangeAfterDelete(const WebCore::SimpleRange&, const WebCore::SimpleRange&) final { return true; }

    void didBeginEditing() final { }
    void respondToChangedContents() final { }
    void respondToChangedSelection(WebCore::LocalFrame*) final { }
    void updateEditorStateAfterLayoutIfEditabilityChanged() final { }
    void discardedComposition(const WebCore::Document&) final { }
    void canceledComposition() final { }
    void didUpdateComposition() final { }
    void didEndEditing() final { }
    void didEndUserTriggeredSelectionChanges() final { }
    void willWriteSelectionToPasteboard(const std::optional<WebCore::SimpleRange>&) final { }
    void didWriteSelectionToPasteboard() final { }
    void getClientPasteboardData(const std::optional<WebCore::SimpleRange>&, Vector<std::pair<String, RefPtr<WebCore::SharedBuffer>>>&) final { }
    void requestCandidatesForSelection(const WebCore::VisibleSelection&) final { }
    void handleAcceptedCandidateWithSoftSpaces(const WebCore::TextCheckingResult&) final { }

    void registerUndoStep(WebCore::UndoStep&) final;
    void registerRedoStep(WebCore::UndoStep&) final;
    void clearUndoRedoOperations() final;

    WebCore::DOMPasteAccessResponse requestDOMPasteAccess(WebCore::DOMPasteAccessCategory, WebCore::FrameIdentifier, const String&) final { return WebCore::DOMPasteAccessResponse::GrantedForGesture; }

    bool canCopyCut(WebCore::LocalFrame*, bool defaultValue) const final { return defaultValue; }
    bool canPaste(WebCore::LocalFrame*, bool defaultValue) const final { return defaultValue; }
    bool canUndo() const final { return !m_undoStack.isEmpty(); }
    bool canRedo() const final { return !m_redoStack.isEmpty(); }
    void undo() final;
    void redo() final;

    void handleKeyboardEvent(WebCore::KeyboardEvent&) final;
    void handleInputMethodKeydown(WebCore::KeyboardEvent&) final { }

    void textFieldDidBeginEditing(WebCore::Element&) final { }
    void textFieldDidEndEditing(WebCore::Element&) final { }
    void textDidChangeInTextField(WebCore::Element&) final { }
    bool doTextFieldCommandFromEvent(WebCore::Element&, WebCore::KeyboardEvent*) final { return false; }
    void textWillBeDeletedInTextField(WebCore::Element&) final { }
    void textDidChangeInTextArea(WebCore::Element&) final { }
    void overflowScrollPositionChanged() final { }
    void subFrameScrollPositionChanged() final { }

    bool performTwoStepDrop(WebCore::DocumentFragment&, const WebCore::SimpleRange&, bool) final { return false; }

    WebCore::TextCheckerClient* textChecker() final { return &m_textChecker; }
    void updateSpellingUIWithGrammarString(const String&, const WebCore::GrammarDetail&) final { }
    void updateSpellingUIWithMisspelledWord(const String&) final { }
    void showSpellingUI(bool) final { }
    bool spellingUIIsShowing() final { return false; }
    void setInputMethodState(WebCore::Element*) final { }

    class TextChecker final : public WebCore::TextCheckerClient {
        bool shouldEraseMarkersAfterChangeSelection(WebCore::TextCheckingType) const final { return true; }
        void ignoreWordInSpellDocument(const String&) final { }
        void learnWord(const String&) final { }
        void checkSpellingOfString(StringView, int*, int*) final { }
        void checkGrammarOfString(StringView, Vector<WebCore::GrammarDetail>&, int*, int*) final { }
        void getGuessesForWord(const String&, const String&, const WebCore::VisibleSelection&, Vector<String>&) final { }
        void requestCheckingOfString(WebCore::TextCheckingRequest&, const WebCore::VisibleSelection&) final { }
        void requestExtendedCheckingOfString(WebCore::TextCheckingRequest&, const WebCore::VisibleSelection&) final { }
    };

    TextChecker m_textChecker;
    Vector<Ref<WebCore::UndoStep>> m_undoStack;
    Vector<Ref<WebCore::UndoStep>> m_redoStack;
    bool m_isUndoing { false };
};

} // namespace ZeroG
