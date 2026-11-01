#pragma once

// Native menus of window mode: the description an application gives (zg_menu),
// the accelerators of its items, and on Windows the window's menu bar and popup
// menus built from it (MenuWin.cpp).

#include <optional>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace ZeroG {

struct Menu {
    struct Item {
        enum class Kind : uint8_t { Action, Separator, Submenu };
        Kind kind { Kind::Action };
        unsigned id { 0 };
        String label;
        String accelerator;
        bool checkbox { false };
        bool checked { false };
        bool enabled { true };
        Vector<Item> submenu;
    };
    Vector<Item> items;
};

// A key with modifiers ("Ctrl+Shift+N"): an SDL keycode, without modifiers
// applied, and SDL_KMOD_CTRL/SHIFT/ALT/GUI (either side).
struct Accelerator {
    uint32_t key { 0 };
    uint16_t modifiers { 0 };
    bool operator==(const Accelerator&) const = default;
};
std::optional<Accelerator> parseAccelerator(const String&);
// What a key press is as an accelerator.
Accelerator acceleratorForKey(uint32_t scancode, uint16_t modifiers);

#if OS(WINDOWS)
// A window's menu bar and popup menus (Win32 HMENUs; the handles are opaque
// here, for the files that include this without windows.h). Chosen items come
// back as SDL events of type `menuEvent` for the window, the item in `code`.
class NativeMenus {
    WTF_MAKE_NONCOPYABLE(NativeMenus);
public:
    NativeMenus(void* hwnd, uint32_t windowID, uint32_t menuEvent);
    ~NativeMenus();

    void setMenuBar(const Menu*);
    void setChecked(unsigned id, bool);
    void setEnabled(unsigned id, bool);
    // Shows a popup menu at a point of the window's client area (pixels);
    // returns once it is closed.
    void popup(const Menu&, int x, int y);

    // For the window procedure (MenuWin.cpp).
    void chosen(unsigned id);
    intptr_t originalProcedure() const { return m_originalProcedure; }

private:
    void* m_window { nullptr }; // HWND
    uint32_t m_windowID { 0 };
    uint32_t m_menuEvent { 0 };
    void* m_menuBar { nullptr }; // HMENU
    intptr_t m_originalProcedure { 0 }; // WNDPROC
};
#endif

} // namespace ZeroG
