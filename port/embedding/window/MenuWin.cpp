#include "config.h"
#include "Menu.h"

#include <SDL3/SDL.h>
#include <iterator>
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

// ---- Dark menus ----
//
// Windows has no dark menu bar, and themes popup menus by the process's
// "preferred app mode": both only through undocumented means, the ones File
// Explorer's own dark menus use. The mode and the menu theme flush are uxtheme
// exports known by ordinal only (no import library has them; ordinal 135 is
// SetPreferredAppMode since Windows 10 1903, on 1809 it takes a BOOL and popups
// then follow the desktop). The menu bar is drawn on WM_UAHDRAWMENU and
// WM_UAHDRAWMENUITEM, which Windows sends a window before drawing its menu bar.

namespace {

enum class PreferredAppMode { Default, AllowDark, ForceDark, ForceLight };

struct DarkModeFunctions {
    PreferredAppMode (WINAPI* setPreferredAppMode)(PreferredAppMode) { nullptr };
    bool (WINAPI* allowDarkModeForWindow)(HWND, bool) { nullptr };
    void (WINAPI* flushMenuThemes)() { nullptr };
};

const DarkModeFunctions& darkModeFunctions()
{
    static const DarkModeFunctions functions = [] {
        DarkModeFunctions result;
        HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!uxtheme)
            return result;
        result.setPreferredAppMode = reinterpret_cast<decltype(result.setPreferredAppMode)>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
        result.allowDarkModeForWindow = reinterpret_cast<decltype(result.allowDarkModeForWindow)>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(133)));
        result.flushMenuThemes = reinterpret_cast<decltype(result.flushMenuThemes)>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
        return result;
    }();
    return functions;
}

// Popup menus take the process's mode as they open: the window's, just before.
void applyMenuTheme(bool dark)
{
    auto& functions = darkModeFunctions();
    if (functions.setPreferredAppMode)
        functions.setPreferredAppMode(dark ? PreferredAppMode::ForceDark : PreferredAppMode::ForceLight);
    if (functions.flushMenuThemes)
        functions.flushMenuThemes();
}

constexpr UINT WM_UAHDRAWMENU = 0x0091;
constexpr UINT WM_UAHDRAWMENUITEM = 0x0092;

// What those messages point to (lParam).
struct UAHMENU {
    HMENU hmenu;
    HDC hdc;
    DWORD dwFlags;
};
union UAHMENUITEMMETRICS {
    struct { DWORD cx, cy; } rgsizeBar[2];
    struct { DWORD cx, cy; } rgsizePopup[4];
};
struct UAHMENUPOPUPMETRICS {
    DWORD rgcx[4];
    DWORD fUpdateMaxWidths : 2;
};
struct UAHMENUITEM {
    int iPosition;
    UAHMENUITEMMETRICS umim;
    UAHMENUPOPUPMETRICS umpm;
};
struct UAHDRAWMENUITEM {
    DRAWITEMSTRUCT dis;
    UAHMENU um;
    UAHMENUITEM umi;
};

constexpr COLORREF s_barColor = RGB(32, 32, 32); // the dark title bar's
constexpr COLORREF s_hotColor = RGB(61, 61, 61);
constexpr COLORREF s_textColor = RGB(255, 255, 255);
constexpr COLORREF s_disabledColor = RGB(109, 109, 109);

void fill(HDC hdc, const RECT& rect, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(hdc, &rect, brush);
    DeleteObject(brush);
}

// The menu bar's background, in the window's coordinates.
void drawMenuBar(HWND window, const UAHMENU& menu)
{
    MENUBARINFO info { sizeof info };
    if (!GetMenuBarInfo(window, OBJID_MENU, 0, &info))
        return;
    RECT windowRect;
    GetWindowRect(window, &windowRect);
    RECT bar = info.rcBar;
    OffsetRect(&bar, -windowRect.left, -windowRect.top);
    fill(menu.hdc, bar, s_barColor);
}

void drawMenuBarItem(const UAHDRAWMENUITEM& item)
{
    wchar_t text[256] { };
    MENUITEMINFOW info { sizeof info };
    info.fMask = MIIM_STRING;
    info.dwTypeData = text;
    info.cch = static_cast<UINT>(std::size(text) - 1);
    GetMenuItemInfoW(item.um.hmenu, item.umi.iPosition, TRUE, &info);
    UINT state = item.dis.itemState;
    fill(item.um.hdc, item.dis.rcItem, (state & (ODS_HOTLIGHT | ODS_SELECTED)) ? s_hotColor : s_barColor);
    SetBkMode(item.um.hdc, TRANSPARENT);
    SetTextColor(item.um.hdc, (state & (ODS_DISABLED | ODS_GRAYED)) ? s_disabledColor : s_textColor);
    // The access key underlined only while Alt is held, as Windows draws it.
    UINT format = DT_CENTER | DT_SINGLELINE | DT_VCENTER | ((state & ODS_NOACCEL) ? DT_HIDEPREFIX : 0);
    RECT rect = item.dis.rcItem;
    DrawTextW(item.um.hdc, text, -1, &rect, format);
}

// Windows draws a light line between the menu bar and the client area, in the
// non-client area: covered after it is drawn.
void drawLineUnderMenuBar(HWND window)
{
    MENUBARINFO info { sizeof info };
    if (!GetMenuBarInfo(window, OBJID_MENU, 0, &info))
        return;
    RECT client;
    GetClientRect(window, &client);
    MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&client), 2);
    RECT windowRect;
    GetWindowRect(window, &windowRect);
    OffsetRect(&client, -windowRect.left, -windowRect.top);
    RECT line = client;
    line.bottom = line.top;
    line.top--;
    HDC hdc = GetWindowDC(window);
    fill(hdc, line, s_barColor);
    ReleaseDC(window, hdc);
}

} // namespace

static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* menus = static_cast<NativeMenus*>(GetPropW(window, s_property));
    if (!menus)
        return DefWindowProcW(window, message, wParam, lParam);
    auto original = reinterpret_cast<WNDPROC>(menus->originalProcedure());
    // A menu item (not a control's notification, not an accelerator table's).
    if (message == WM_COMMAND && !HIWORD(wParam) && !lParam) {
        menus->chosen(LOWORD(wParam));
        return 0;
    }
    // The menu bar's dropdowns: in the window's scheme.
    if (message == WM_ENTERMENULOOP)
        applyMenuTheme(menus->isDark());
    if (menus->isDark()) {
        switch (message) {
        case WM_UAHDRAWMENU:
            drawMenuBar(window, *reinterpret_cast<const UAHMENU*>(lParam));
            return 0;
        case WM_UAHDRAWMENUITEM:
            drawMenuBarItem(*reinterpret_cast<const UAHDRAWMENUITEM*>(lParam));
            return 0;
        case WM_NCPAINT:
        case WM_NCACTIVATE: {
            LRESULT result = CallWindowProcW(original, window, message, wParam, lParam);
            drawLineUnderMenuBar(window);
            return result;
        }
        }
    }
    return CallWindowProcW(original, window, message, wParam, lParam);
}

void* createNativePopupMenu(const Menu& menu)
{
    HMENU popupMenu = CreatePopupMenu();
    appendItems(popupMenu, menu.items);
    return popupMenu;
}

void applyNativeMenuTheme(bool dark)
{
    applyMenuTheme(dark);
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

void NativeMenus::setDark(bool dark)
{
    if (m_dark == dark)
        return;
    m_dark = dark;
    auto hwnd = static_cast<HWND>(m_window);
    if (auto allow = darkModeFunctions().allowDarkModeForWindow)
        allow(hwnd, dark);
    // The menu bar and the line under it, in their new colors.
    DrawMenuBar(hwnd);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
}

void NativeMenus::popup(const Menu& menu, int x, int y)
{
    auto hwnd = static_cast<HWND>(m_window);
    applyMenuTheme(m_dark);
    HMENU popupMenu = CreatePopupMenu();
    appendItems(popupMenu, menu.items);
    POINT point { x, y };
    ClientToScreen(hwnd, &point);
    // The menu closes when the user clicks elsewhere only if its window is
    // the foreground one.
    SetForegroundWindow(hwnd);
    UINT id = TrackPopupMenuEx(popupMenu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN, point.x, point.y, hwnd, nullptr);
    DestroyMenu(popupMenu);
    // 0: dismissed.
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
