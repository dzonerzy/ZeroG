#include "config.h"
#include "Menu.h"

#include <SDL3/SDL.h>
#include <wtf/text/CString.h>

namespace ZeroG {

// Either side's modifier, as one.
static uint16_t normalizedModifiers(uint16_t modifiers)
{
    uint16_t result = 0;
    if (modifiers & SDL_KMOD_CTRL)
        result |= SDL_KMOD_CTRL;
    if (modifiers & SDL_KMOD_SHIFT)
        result |= SDL_KMOD_SHIFT;
    if (modifiers & SDL_KMOD_ALT)
        result |= SDL_KMOD_ALT;
    if (modifiers & SDL_KMOD_GUI)
        result |= SDL_KMOD_GUI;
    return result;
}

std::optional<Accelerator> parseAccelerator(const String& text)
{
    if (text.isEmpty())
        return std::nullopt;
    // "Ctrl++": the key is '+'.
    String keyName;
    String modifiersText = text;
    if (text.endsWith("++"_s)) {
        keyName = "+"_s;
        modifiersText = text.left(text.length() - 2);
    } else if (auto plus = text.reverseFind('+'); plus != notFound) {
        keyName = text.substring(plus + 1);
        modifiersText = text.left(plus);
    } else {
        keyName = text;
        modifiersText = { };
    }
    Accelerator accelerator;
    for (auto& part : modifiersText.split('+')) {
        auto name = part.trim(isASCIIWhitespace<char16_t>);
        if (equalLettersIgnoringASCIICase(name, "ctrl"_s) || equalLettersIgnoringASCIICase(name, "control"_s))
            accelerator.modifiers |= SDL_KMOD_CTRL;
        else if (equalLettersIgnoringASCIICase(name, "shift"_s))
            accelerator.modifiers |= SDL_KMOD_SHIFT;
        else if (equalLettersIgnoringASCIICase(name, "alt"_s) || equalLettersIgnoringASCIICase(name, "option"_s))
            accelerator.modifiers |= SDL_KMOD_ALT;
        else if (equalLettersIgnoringASCIICase(name, "super"_s) || equalLettersIgnoringASCIICase(name, "win"_s)
            || equalLettersIgnoringASCIICase(name, "meta"_s) || equalLettersIgnoringASCIICase(name, "cmd"_s))
            accelerator.modifiers |= SDL_KMOD_GUI;
        else
            return std::nullopt;
    }
    keyName = keyName.trim(isASCIIWhitespace<char16_t>);
    // Letters as keycodes are lowercase (SDLK_N is 'n').
    if (keyName.length() == 1)
        keyName = keyName.convertToASCIILowercase();
    auto name = keyName.utf8();
    accelerator.key = SDL_GetKeyFromName(byteCast<char>(name.data()));
    if (accelerator.key == SDLK_UNKNOWN)
        return std::nullopt;
    return accelerator;
}

Accelerator acceleratorForKey(uint32_t scancode, uint16_t modifiers)
{
    // The key as named, whatever Shift does to it ("Ctrl+Shift+N" is N).
    return { SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(scancode), SDL_KMOD_NONE, true), normalizedModifiers(modifiers) };
}

} // namespace ZeroG
