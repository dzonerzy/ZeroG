// SDL keyboard events as the View's key events: the physical key (scancode) gives
// the DOM `code` and the Windows virtual key code WebCore's keyCode and editing
// bindings use; the layout's key gives the DOM `key`.

#include "config.h"
#include "KeyboardSDL.h"

#include <SDL3/SDL.h>
#include <WebCore/WindowsKeyboardCodes.h>
#include <wtf/text/MakeString.h>

namespace ZeroG {

using namespace WebCore;

namespace {

struct KeyInfo {
    SDL_Scancode scancode;
    ASCIILiteral code;
    int windowsKeyCode;
    ASCIILiteral key; // for keys that produce no character
    bool isKeypad { false };
};

const KeyInfo keys[] = {
    { SDL_SCANCODE_RETURN, "Enter"_s, VK_RETURN, "Enter"_s },
    { SDL_SCANCODE_ESCAPE, "Escape"_s, VK_ESCAPE, "Escape"_s },
    { SDL_SCANCODE_BACKSPACE, "Backspace"_s, VK_BACK, "Backspace"_s },
    { SDL_SCANCODE_TAB, "Tab"_s, VK_TAB, "Tab"_s },
    { SDL_SCANCODE_SPACE, "Space"_s, VK_SPACE, { } },
    { SDL_SCANCODE_MINUS, "Minus"_s, VK_OEM_MINUS, { } },
    { SDL_SCANCODE_EQUALS, "Equal"_s, VK_OEM_PLUS, { } },
    { SDL_SCANCODE_LEFTBRACKET, "BracketLeft"_s, VK_OEM_4, { } },
    { SDL_SCANCODE_RIGHTBRACKET, "BracketRight"_s, VK_OEM_6, { } },
    { SDL_SCANCODE_BACKSLASH, "Backslash"_s, VK_OEM_5, { } },
    { SDL_SCANCODE_SEMICOLON, "Semicolon"_s, VK_OEM_1, { } },
    { SDL_SCANCODE_APOSTROPHE, "Quote"_s, VK_OEM_7, { } },
    { SDL_SCANCODE_GRAVE, "Backquote"_s, VK_OEM_3, { } },
    { SDL_SCANCODE_COMMA, "Comma"_s, VK_OEM_COMMA, { } },
    { SDL_SCANCODE_PERIOD, "Period"_s, VK_OEM_PERIOD, { } },
    { SDL_SCANCODE_SLASH, "Slash"_s, VK_OEM_2, { } },
    { SDL_SCANCODE_NONUSBACKSLASH, "IntlBackslash"_s, VK_OEM_102, { } },
    { SDL_SCANCODE_CAPSLOCK, "CapsLock"_s, VK_CAPITAL, "CapsLock"_s },
    { SDL_SCANCODE_PRINTSCREEN, "PrintScreen"_s, VK_SNAPSHOT, "PrintScreen"_s },
    { SDL_SCANCODE_SCROLLLOCK, "ScrollLock"_s, VK_SCROLL, "ScrollLock"_s },
    { SDL_SCANCODE_PAUSE, "Pause"_s, VK_PAUSE, "Pause"_s },
    { SDL_SCANCODE_INSERT, "Insert"_s, VK_INSERT, "Insert"_s },
    { SDL_SCANCODE_HOME, "Home"_s, VK_HOME, "Home"_s },
    { SDL_SCANCODE_PAGEUP, "PageUp"_s, VK_PRIOR, "PageUp"_s },
    { SDL_SCANCODE_DELETE, "Delete"_s, VK_DELETE, "Delete"_s },
    { SDL_SCANCODE_END, "End"_s, VK_END, "End"_s },
    { SDL_SCANCODE_PAGEDOWN, "PageDown"_s, VK_NEXT, "PageDown"_s },
    { SDL_SCANCODE_RIGHT, "ArrowRight"_s, VK_RIGHT, "ArrowRight"_s },
    { SDL_SCANCODE_LEFT, "ArrowLeft"_s, VK_LEFT, "ArrowLeft"_s },
    { SDL_SCANCODE_DOWN, "ArrowDown"_s, VK_DOWN, "ArrowDown"_s },
    { SDL_SCANCODE_UP, "ArrowUp"_s, VK_UP, "ArrowUp"_s },
    { SDL_SCANCODE_NUMLOCKCLEAR, "NumLock"_s, VK_NUMLOCK, "NumLock"_s },
    { SDL_SCANCODE_KP_DIVIDE, "NumpadDivide"_s, VK_DIVIDE, { }, true },
    { SDL_SCANCODE_KP_MULTIPLY, "NumpadMultiply"_s, VK_MULTIPLY, { }, true },
    { SDL_SCANCODE_KP_MINUS, "NumpadSubtract"_s, VK_SUBTRACT, { }, true },
    { SDL_SCANCODE_KP_PLUS, "NumpadAdd"_s, VK_ADD, { }, true },
    { SDL_SCANCODE_KP_ENTER, "NumpadEnter"_s, VK_RETURN, "Enter"_s, true },
    { SDL_SCANCODE_KP_PERIOD, "NumpadDecimal"_s, VK_DECIMAL, { }, true },
    { SDL_SCANCODE_APPLICATION, "ContextMenu"_s, VK_APPS, "ContextMenu"_s },
    { SDL_SCANCODE_LCTRL, "ControlLeft"_s, VK_CONTROL, "Control"_s },
    { SDL_SCANCODE_LSHIFT, "ShiftLeft"_s, VK_SHIFT, "Shift"_s },
    { SDL_SCANCODE_LALT, "AltLeft"_s, VK_MENU, "Alt"_s },
    { SDL_SCANCODE_LGUI, "MetaLeft"_s, VK_LWIN, "Meta"_s },
    { SDL_SCANCODE_RCTRL, "ControlRight"_s, VK_CONTROL, "Control"_s },
    { SDL_SCANCODE_RSHIFT, "ShiftRight"_s, VK_SHIFT, "Shift"_s },
    { SDL_SCANCODE_RALT, "AltRight"_s, VK_MENU, "Alt"_s },
    { SDL_SCANCODE_RGUI, "MetaRight"_s, VK_RWIN, "Meta"_s },
};

ASCIILiteral functionKeyName(int index)
{
    static constexpr ASCIILiteral names[] = { "F1"_s, "F2"_s, "F3"_s, "F4"_s, "F5"_s, "F6"_s, "F7"_s, "F8"_s, "F9"_s, "F10"_s, "F11"_s, "F12"_s };
    return names[index];
}

} // namespace

View::Modifiers modifiersFromSDL(unsigned sdlModifiers)
{
    View::Modifiers modifiers;
    if (sdlModifiers & SDL_KMOD_SHIFT)
        modifiers.add(PlatformEvent::Modifier::ShiftKey);
    if (sdlModifiers & SDL_KMOD_CTRL)
        modifiers.add(PlatformEvent::Modifier::ControlKey);
    if (sdlModifiers & SDL_KMOD_ALT)
        modifiers.add(PlatformEvent::Modifier::AltKey);
    if (sdlModifiers & SDL_KMOD_GUI)
        modifiers.add(PlatformEvent::Modifier::MetaKey);
    if (sdlModifiers & SDL_KMOD_CAPS)
        modifiers.add(PlatformEvent::Modifier::CapsLockKey);
    return modifiers;
}

View::KeyEvent keyEventFromSDL(unsigned scancodeValue, unsigned sdlModifiers, bool repeat)
{
    auto scancode = static_cast<SDL_Scancode>(scancodeValue);
    View::KeyEvent event;
    event.modifiers = modifiersFromSDL(sdlModifiers);
    event.isAutoRepeat = repeat;

    if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) {
        char letter = 'A' + (scancode - SDL_SCANCODE_A);
        event.code = makeString("Key"_s, letter);
        event.windowsKeyCode = letter;
    } else if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_0) {
        char digit = scancode == SDL_SCANCODE_0 ? '0' : '1' + (scancode - SDL_SCANCODE_1);
        event.code = makeString("Digit"_s, digit);
        event.windowsKeyCode = digit;
    } else if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12) {
        int index = scancode - SDL_SCANCODE_F1;
        event.code = functionKeyName(index);
        event.key = functionKeyName(index);
        event.windowsKeyCode = VK_F1 + index;
        return event;
    } else if (scancode >= SDL_SCANCODE_KP_1 && scancode <= SDL_SCANCODE_KP_0) {
        int digit = scancode == SDL_SCANCODE_KP_0 ? 0 : 1 + (scancode - SDL_SCANCODE_KP_1);
        event.code = makeString("Numpad"_s, digit);
        event.windowsKeyCode = VK_NUMPAD0 + digit;
        event.isKeypad = true;
    } else {
        for (auto& info : keys) {
            if (info.scancode != scancode)
                continue;
            event.code = info.code;
            event.windowsKeyCode = info.windowsKeyCode;
            event.isKeypad = info.isKeypad;
            if (!info.key.isNull()) {
                event.key = info.key;
                return event;
            }
            break;
        }
    }

    // A key that types a character: the character in the current layout, with
    // the modifiers applied (Shift+A is "A").
    SDL_Keycode keycode = SDL_GetKeyFromScancode(scancode, static_cast<SDL_Keymod>(sdlModifiers), false);
    if (keycode && !(keycode & SDLK_SCANCODE_MASK))
        event.key = String::fromCodePoint(keycode);
    else
        event.key = "Unidentified"_s;
    return event;
}

} // namespace ZeroG
