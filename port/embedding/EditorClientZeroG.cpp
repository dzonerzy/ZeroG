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

#include "config.h"
#include "EditorClientZeroG.h"

#include <WebCore/Document.h>
#include <WebCore/DocumentInlines.h>
#include <WebCore/FrameDestructionObserverInlines.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/NodeInlines.h>
#include <WebCore/Editor.h>
#include <WebCore/EventNames.h>
#include <WebCore/KeyboardEvent.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/PlatformKeyboardEvent.h>
#include <WebCore/WindowsKeyboardCodes.h>
#include <wtf/HashMap.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMallocInlines.h>

namespace ZeroG {

using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(EditorClientZeroG);

// Undo: steps registered while undoing are redo steps, and a new edit clears the
// redo stack.
void EditorClientZeroG::registerUndoStep(UndoStep& step)
{
    if (!m_isUndoing)
        m_redoStack.clear();
    m_undoStack.append(step);
}

void EditorClientZeroG::registerRedoStep(UndoStep& step)
{
    m_redoStack.append(step);
}

void EditorClientZeroG::clearUndoRedoOperations()
{
    m_undoStack.clear();
    m_redoStack.clear();
}

void EditorClientZeroG::undo()
{
    if (m_undoStack.isEmpty())
        return;
    Ref step = m_undoStack.takeLast();
    SetForScope undoing(m_isUndoing, true);
    step->unapply();
}

void EditorClientZeroG::redo()
{
    if (m_redoStack.isEmpty())
        return;
    Ref step = m_redoStack.takeLast();
    SetForScope undoing(m_isUndoing, true);
    step->reapply();
}

// Key bindings: the Windows port's table, used on every OS since ZeroG gives
// WebCore Windows virtual key codes everywhere.
namespace {

constexpr unsigned CtrlKey = 1 << 0;
constexpr unsigned AltKey = 1 << 1;
constexpr unsigned ShiftKey = 1 << 2;

struct KeyEntry {
    unsigned key;
    unsigned modifiers;
    ASCIILiteral name;
};

constexpr KeyEntry keyDownEntries[] = {
    { VK_LEFT, 0, "MoveLeft"_s },
    { VK_LEFT, ShiftKey, "MoveLeftAndModifySelection"_s },
    { VK_LEFT, CtrlKey, "MoveWordLeft"_s },
    { VK_LEFT, CtrlKey | ShiftKey, "MoveWordLeftAndModifySelection"_s },
    { VK_RIGHT, 0, "MoveRight"_s },
    { VK_RIGHT, ShiftKey, "MoveRightAndModifySelection"_s },
    { VK_RIGHT, CtrlKey, "MoveWordRight"_s },
    { VK_RIGHT, CtrlKey | ShiftKey, "MoveWordRightAndModifySelection"_s },
    { VK_UP, 0, "MoveUp"_s },
    { VK_UP, ShiftKey, "MoveUpAndModifySelection"_s },
    { VK_PRIOR, ShiftKey, "MovePageUpAndModifySelection"_s },
    { VK_DOWN, 0, "MoveDown"_s },
    { VK_DOWN, ShiftKey, "MoveDownAndModifySelection"_s },
    { VK_NEXT, ShiftKey, "MovePageDownAndModifySelection"_s },
    { VK_PRIOR, 0, "MovePageUp"_s },
    { VK_NEXT, 0, "MovePageDown"_s },
    { VK_HOME, 0, "MoveToBeginningOfLine"_s },
    { VK_HOME, ShiftKey, "MoveToBeginningOfLineAndModifySelection"_s },
    { VK_HOME, CtrlKey, "MoveToBeginningOfDocument"_s },
    { VK_HOME, CtrlKey | ShiftKey, "MoveToBeginningOfDocumentAndModifySelection"_s },
    { VK_END, 0, "MoveToEndOfLine"_s },
    { VK_END, ShiftKey, "MoveToEndOfLineAndModifySelection"_s },
    { VK_END, CtrlKey, "MoveToEndOfDocument"_s },
    { VK_END, CtrlKey | ShiftKey, "MoveToEndOfDocumentAndModifySelection"_s },
    { VK_BACK, 0, "DeleteBackward"_s },
    { VK_BACK, ShiftKey, "DeleteBackward"_s },
    { VK_DELETE, 0, "DeleteForward"_s },
    { VK_BACK, CtrlKey, "DeleteWordBackward"_s },
    { VK_DELETE, CtrlKey, "DeleteWordForward"_s },
    { 'B', CtrlKey, "ToggleBold"_s },
    { 'I', CtrlKey, "ToggleItalic"_s },
    { VK_ESCAPE, 0, "Cancel"_s },
    { VK_OEM_PERIOD, CtrlKey, "Cancel"_s },
    { VK_TAB, 0, "InsertTab"_s },
    { VK_TAB, ShiftKey, "InsertBacktab"_s },
    { VK_RETURN, 0, "InsertNewline"_s },
    { VK_RETURN, CtrlKey, "InsertNewline"_s },
    { VK_RETURN, AltKey, "InsertNewline"_s },
    { VK_RETURN, ShiftKey, "InsertNewline"_s },
    { VK_RETURN, AltKey | ShiftKey, "InsertNewline"_s },
    { 'C', CtrlKey, "Copy"_s },
    { 'V', CtrlKey, "Paste"_s },
    { 'X', CtrlKey, "Cut"_s },
    { 'A', CtrlKey, "SelectAll"_s },
    { VK_INSERT, CtrlKey, "Copy"_s },
    { VK_DELETE, ShiftKey, "Cut"_s },
    { VK_INSERT, ShiftKey, "Paste"_s },
    { 'Z', CtrlKey, "Undo"_s },
    { 'Z', CtrlKey | ShiftKey, "Redo"_s },
    { 'Y', CtrlKey, "Redo"_s },
};

constexpr KeyEntry keyPressEntries[] = {
    { '\t', 0, "InsertTab"_s },
    { '\t', ShiftKey, "InsertBacktab"_s },
    { '\r', 0, "InsertNewline"_s },
    { '\r', CtrlKey, "InsertNewline"_s },
    { '\r', AltKey, "InsertNewline"_s },
    { '\r', ShiftKey, "InsertNewline"_s },
    { '\r', AltKey | ShiftKey, "InsertNewline"_s },
};

ASCIILiteral commandForKeyEvent(const KeyboardEvent& event)
{
    static NeverDestroyed<HashMap<unsigned, ASCIILiteral>> keyDownCommands = [] {
        HashMap<unsigned, ASCIILiteral> map;
        for (auto& entry : keyDownEntries)
            map.add(entry.modifiers << 16 | entry.key, entry.name);
        return map;
    }();
    static NeverDestroyed<HashMap<unsigned, ASCIILiteral>> keyPressCommands = [] {
        HashMap<unsigned, ASCIILiteral> map;
        for (auto& entry : keyPressEntries)
            map.add(entry.modifiers << 16 | entry.key, entry.name);
        return map;
    }();

    unsigned modifiers = 0;
    if (event.shiftKey())
        modifiers |= ShiftKey;
    if (event.altKey())
        modifiers |= AltKey;
    if (event.ctrlKey())
        modifiers |= CtrlKey;

    unsigned key = event.type() == eventNames().keydownEvent ? event.keyCode() : event.charCode();
    if (!key)
        return { };
    auto& commands = event.type() == eventNames().keydownEvent ? keyDownCommands.get() : keyPressCommands.get();
    return commands.get(modifiers << 16 | key);
}

} // namespace

void EditorClientZeroG::handleKeyboardEvent(KeyboardEvent& event)
{
    auto* platformEvent = event.underlyingPlatformEvent();
    if (!platformEvent || platformEvent->isSystemKey())
        return;
    if (event.type() != eventNames().keydownEvent && event.type() != eventNames().keypressEvent)
        return;
    RefPtr node = dynamicDowncast<Node>(event.target());
    RefPtr frame = node ? node->document().frame() : nullptr;
    if (!frame)
        return;

    auto command = frame->editor().command(String(commandForKeyEvent(event)));
    if (platformEvent->type() == PlatformEvent::Type::RawKeyDown) {
        // Commands that insert text (Tab, Enter) wait for the keypress, where
        // WebCore decides between inserting and moving focus.
        if (!command.isTextInsertion() && command.execute(&event))
            event.setDefaultHandled();
        return;
    }

    if (command.execute(&event)) {
        event.setDefaultHandled();
        return;
    }
    // Control characters are not text.
    if (event.charCode() < ' ')
        return;
    if (frame->editor().insertText(platformEvent->text(), &event))
        event.setDefaultHandled();
}

} // namespace ZeroG
