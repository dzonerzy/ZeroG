// Global shortcuts on Windows: hotkeys of the process's hidden window
// (RegisterHotKey), WM_HOTKEY to the shortcut callback.

#include "config.h"
#include "desktop/Desktop.h"

#include <SDL3/SDL.h>
#include <windows.h>
#include <wtf/text/MakeString.h>

namespace ZeroG::Shortcuts {

// An SDL keycode as a virtual key.
static std::optional<UINT> virtualKey(uint32_t key)
{
    if (key >= 'a' && key <= 'z')
        return 'A' + (key - 'a');
    if (key >= '0' && key <= '9')
        return key;
    for (int i = 0; i < 12; ++i) {
        if (key == SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F1 + i))
            return VK_F1 + i;
        if (key == SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F13 + i))
            return VK_F13 + i;
    }
    static constexpr std::pair<uint32_t, UINT> named[] = {
        { SDLK_SPACE, VK_SPACE }, { SDLK_RETURN, VK_RETURN }, { SDLK_ESCAPE, VK_ESCAPE }, { SDLK_TAB, VK_TAB },
        { SDLK_BACKSPACE, VK_BACK }, { SDLK_DELETE, VK_DELETE }, { SDLK_INSERT, VK_INSERT }, { SDLK_HOME, VK_HOME },
        { SDLK_END, VK_END }, { SDLK_PAGEUP, VK_PRIOR }, { SDLK_PAGEDOWN, VK_NEXT }, { SDLK_UP, VK_UP },
        { SDLK_DOWN, VK_DOWN }, { SDLK_LEFT, VK_LEFT }, { SDLK_RIGHT, VK_RIGHT }, { SDLK_PRINTSCREEN, VK_SNAPSHOT },
        { SDLK_PAUSE, VK_PAUSE },
    };
    for (auto [keycode, vk] : named) {
        if (key == keycode)
            return vk;
    }
    // Punctuation, as the keyboard layout has it.
    if (key < 0x80) {
        SHORT scan = VkKeyScanW(static_cast<WCHAR>(key));
        if (scan != -1)
            return LOBYTE(scan);
    }
    return std::nullopt;
}

static void ensureHandler()
{
    static bool added = false;
    if (added)
        return;
    added = true;
    addDesktopMessageHandler([](unsigned message, uintptr_t wParam, intptr_t, intptr_t&) {
        if (message != WM_HOTKEY)
            return false;
        pressed(static_cast<unsigned>(wParam));
        return true;
    });
}

Result add(unsigned id, const Accelerator& accelerator, const String& text, String& error)
{
    auto vk = virtualKey(accelerator.key);
    if (!vk) {
        error = makeString("no key for \""_s, text, "\" on this keyboard"_s);
        return Result::Invalid;
    }
    UINT modifiers = MOD_NOREPEAT;
    if (accelerator.modifiers & SDL_KMOD_CTRL)
        modifiers |= MOD_CONTROL;
    if (accelerator.modifiers & SDL_KMOD_SHIFT)
        modifiers |= MOD_SHIFT;
    if (accelerator.modifiers & SDL_KMOD_ALT)
        modifiers |= MOD_ALT;
    if (accelerator.modifiers & SDL_KMOD_GUI)
        modifiers |= MOD_WIN;
    ensureHandler();
    if (!RegisterHotKey(static_cast<HWND>(desktopWindow()), static_cast<int>(id), modifiers, *vk)) {
        error = makeString('"', text, "\" is taken (another application's hotkey, or this id's)"_s);
        return Result::Taken;
    }
    return Result::Ok;
}

bool remove(unsigned id)
{
    return UnregisterHotKey(static_cast<HWND>(desktopWindow()), static_cast<int>(id));
}

} // namespace ZeroG::Shortcuts
