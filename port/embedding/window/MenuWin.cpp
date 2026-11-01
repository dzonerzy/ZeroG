#include "config.h"
#include "Menu.h"

#include <SDL3/SDL.h>
#include <windows.h>
#include <wtf/text/MakeString.h>

namespace ZeroG {

static constexpr const wchar_t* s_property = L"ZeroGMenus";

// NUL-terminated UTF-16, empty strings included.
static Vector<wchar_t> wide(const String& string)
{
    auto result = string.wideCharacters();
    if (result.isEmpty())
        result.append(0);
    return result;
}

static void appendItems(HMENU menu, const Vector<Menu::Item>& items)
{
    for (auto& item : items) {
        UINT disabled = item.enabled ? 0 : MF_GRAYED;
        switch (item.kind) {
        case Menu::Item::Kind::Separator:
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            break;
        case Menu::Item::Kind::Submenu: {
            HMENU submenu = CreatePopupMenu();
            appendItems(submenu, item.submenu);
            AppendMenuW(menu, MF_POPUP | MF_STRING | disabled, reinterpret_cast<UINT_PTR>(submenu), wide(item.label).span().data());
            break;
        }
        case Menu::Item::Kind::Action: {
            // The accelerator right-aligned after a tab, as Windows draws them.
            auto label = item.accelerator.isEmpty() ? item.label : makeString(item.label, '\t', item.accelerator);
            UINT checked = item.checked ? MF_CHECKED : MF_UNCHECKED;
            AppendMenuW(menu, MF_STRING | checked | disabled, item.id, wide(label).span().data());
            break;
        }
        }
    }
}

static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* menus = static_cast<NativeMenus*>(GetPropW(window, s_property));
    if (!menus)
        return DefWindowProcW(window, message, wParam, lParam);
    // A menu item (not a control's notification, not an accelerator table's).
    if (message == WM_COMMAND && !HIWORD(wParam) && !lParam) {
        menus->chosen(LOWORD(wParam));
        return 0;
    }
    return CallWindowProcW(reinterpret_cast<WNDPROC>(menus->originalProcedure()), window, message, wParam, lParam);
}

NativeMenus::NativeMenus(void* window, uint32_t windowID, uint32_t menuEvent)
    : m_window(window)
    , m_windowID(windowID)
    , m_menuEvent(menuEvent)
{
    // WM_COMMAND comes to this window procedure, ahead of SDL's. The window's
    // other one (accessibility) is installed before and removed after it.
    auto hwnd = static_cast<HWND>(m_window);
    SetPropW(hwnd, s_property, this);
    m_originalProcedure = SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(windowProcedure));
}

NativeMenus::~NativeMenus()
{
    setMenuBar(nullptr);
    auto hwnd = static_cast<HWND>(m_window);
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, m_originalProcedure);
    RemovePropW(hwnd, s_property);
}

void NativeMenus::setMenuBar(const Menu* menu)
{
    HMENU menuBar = nullptr;
    if (menu) {
        menuBar = CreateMenu();
        appendItems(menuBar, menu->items);
    }
    // The client area changes size: SDL resizes the view.
    auto hwnd = static_cast<HWND>(m_window);
    SetMenu(hwnd, menuBar);
    if (m_menuBar)
        DestroyMenu(static_cast<HMENU>(m_menuBar));
    m_menuBar = menuBar;
    DrawMenuBar(hwnd);
}

void NativeMenus::setChecked(unsigned id, bool checked)
{
    // By command: submenus are searched too.
    if (m_menuBar)
        CheckMenuItem(static_cast<HMENU>(m_menuBar), id, MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED));
}

void NativeMenus::setEnabled(unsigned id, bool enabled)
{
    if (!m_menuBar)
        return;
    EnableMenuItem(static_cast<HMENU>(m_menuBar), id, MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
    DrawMenuBar(static_cast<HWND>(m_window));
}

void NativeMenus::popup(const Menu& menu, int x, int y)
{
    auto hwnd = static_cast<HWND>(m_window);
    HMENU popupMenu = CreatePopupMenu();
    appendItems(popupMenu, menu.items);
    POINT point { x, y };
    ClientToScreen(hwnd, &point);
    // The menu closes when the user clicks elsewhere only if its window is
    // the foreground one.
    SetForegroundWindow(hwnd);
    UINT id = TrackPopupMenuEx(popupMenu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN, point.x, point.y, hwnd, nullptr);
    DestroyMenu(popupMenu);
    if (id)
        chosen(id);
}

// To the engine thread's loop, with the window's other events.
void NativeMenus::chosen(unsigned id)
{
    SDL_Event event { };
    event.type = m_menuEvent;
    event.user.windowID = m_windowID;
    event.user.code = static_cast<Sint32>(id);
    SDL_PushEvent(&event);
}

} // namespace ZeroG
