// The system tray on Windows: a notification area icon (Shell_NotifyIcon,
// version 4 callbacks) of the process's hidden window, its menu a popup built
// as the windows' menus (MenuWin.cpp), light or dark as the taskbar.

#include "config.h"
#include "desktop/Desktop.h"

#include "View.h"
#include <shellapi.h>
#include <windows.h>
#include <windowsx.h>

namespace ZeroG {

namespace {

constexpr UINT s_trayMessage = WM_APP + 0x5A; // the icons' callbacks

// The icon as an HICON: a 32-bit top-down BGRA bitmap with its alpha.
HICON createIcon(const TrayIcon& icon)
{
    if (!icon.width || !icon.height || icon.rgba.size() < static_cast<size_t>(icon.width) * icon.height * 4)
        return nullptr;
    BITMAPV5HEADER header { };
    header.bV5Size = sizeof header;
    header.bV5Width = static_cast<LONG>(icon.width);
    header.bV5Height = -static_cast<LONG>(icon.height);
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color)
        return nullptr;
    auto* out = static_cast<uint8_t*>(bits);
    for (size_t i = 0; i < static_cast<size_t>(icon.width) * icon.height; ++i) {
        out[i * 4 + 0] = icon.rgba[i * 4 + 2];
        out[i * 4 + 1] = icon.rgba[i * 4 + 1];
        out[i * 4 + 2] = icon.rgba[i * 4 + 0];
        out[i * 4 + 3] = icon.rgba[i * 4 + 3];
    }
    HBITMAP mask = CreateBitmap(icon.width, icon.height, 1, 1, nullptr);
    ICONINFO info { TRUE, 0, 0, mask, color };
    HICON result = CreateIconIndirect(&info);
    DeleteObject(mask);
    DeleteObject(color);
    return result;
}

class TrayWin final : public Tray {
public:
    TrayWin(TrayIcon&& icon, const String& tooltip, Callbacks&& callbacks)
        : m_callbacks(WTF::move(callbacks))
        , m_tooltip(tooltip)
    {
        static UINT nextID = 1;
        m_id = nextID++;
        m_icon = createIcon(icon);
        m_handler = addDesktopMessageHandler([this](unsigned message, uintptr_t wParam, intptr_t lParam, intptr_t&) {
            return handle(message, wParam, lParam);
        });
    }

    ~TrayWin()
    {
        removeDesktopMessageHandler(m_handler);
        auto data = notifyData(0);
        Shell_NotifyIconW(NIM_DELETE, &data);
        if (m_icon)
            DestroyIcon(m_icon);
    }

    bool add()
    {
        auto data = notifyData(NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP);
        if (!Shell_NotifyIconW(NIM_ADD, &data))
            return false;
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
        return true;
    }

    void setIcon(TrayIcon&& icon) final
    {
        HICON previous = m_icon;
        m_icon = createIcon(icon);
        auto data = notifyData(NIF_ICON);
        Shell_NotifyIconW(NIM_MODIFY, &data);
        if (previous)
            DestroyIcon(previous);
    }

    void setTooltip(const String& tooltip) final
    {
        m_tooltip = tooltip;
        auto data = notifyData(NIF_TIP | NIF_SHOWTIP);
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }

    void setMenu(std::optional<Menu>&& menu) final { m_menu = WTF::move(menu); }

    void setChecked(unsigned id, bool checked) final
    {
        if (m_menu)
            updateMenuItem(m_menu->items, id, checked, std::nullopt);
    }

    void setEnabled(unsigned id, bool enabled) final
    {
        if (m_menu)
            updateMenuItem(m_menu->items, id, std::nullopt, enabled);
    }

    std::optional<TrayRect> rect() final
    {
        NOTIFYICONIDENTIFIER identifier { sizeof identifier };
        identifier.hWnd = static_cast<HWND>(desktopWindow());
        identifier.uID = m_id;
        RECT rect;
        if (FAILED(Shell_NotifyIconGetRect(&identifier, &rect)))
            return std::nullopt;
        return TrayRect { rect.left, rect.top, static_cast<unsigned>(rect.right - rect.left), static_cast<unsigned>(rect.bottom - rect.top) };
    }

private:
    NOTIFYICONDATAW notifyData(UINT flags) const
    {
        NOTIFYICONDATAW data { };
        data.cbSize = sizeof data;
        data.hWnd = static_cast<HWND>(desktopWindow());
        data.uID = m_id;
        data.uFlags = flags;
        data.uCallbackMessage = s_trayMessage;
        data.hIcon = m_icon;
        auto tip = m_tooltip.wideCharacters();
        size_t length = std::min<size_t>(tip.size(), std::size(data.szTip) - 1);
        std::copy_n(tip.begin(), length, data.szTip);
        data.szTip[length] = 0;
        return data;
    }

    bool handle(unsigned message, uintptr_t wParam, intptr_t lParam)
    {
        // Explorer started again: the icons are gone.
        static const UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        if (message == taskbarCreated) {
            add();
            return false; // the other icons too
        }
        if (message != s_trayMessage || HIWORD(lParam) != m_id)
            return false;
        int x = GET_X_LPARAM(wParam);
        int y = GET_Y_LPARAM(wParam);
        switch (LOWORD(lParam)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            if (m_callbacks.activated)
                m_callbacks.activated(x, y);
            return true;
        case WM_CONTEXTMENU:
            showMenu(x, y);
            return true;
        }
        return false;
    }

    void showMenu(int x, int y)
    {
        if (!m_menu)
            return;
        auto window = static_cast<HWND>(desktopWindow());
        applyNativeMenuTheme(View::systemDarkAppearance());
        auto popup = static_cast<HMENU>(createNativePopupMenu(*m_menu));
        // A tray menu closes on a click elsewhere only with its window in
        // front, and needs a message after it (Shell_NotifyIcon's documented quirk).
        SetForegroundWindow(window);
        UINT id = TrackPopupMenuEx(popup, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | GetSystemMetrics(SM_MENUDROPALIGNMENT), x, y, window, nullptr);
        PostMessageW(window, WM_NULL, 0, 0);
        DestroyMenu(popup);
        if (id && m_callbacks.menuItem)
            m_callbacks.menuItem(id);
    }

    Callbacks m_callbacks;
    String m_tooltip;
    std::optional<Menu> m_menu;
    HICON m_icon { nullptr };
    UINT m_id { 0 };
    unsigned m_handler { 0 };
};

} // namespace

std::unique_ptr<Tray> Tray::create(const String&, TrayIcon&& icon, const String& tooltip, Callbacks&& callbacks, String& error)
{
    auto tray = std::make_unique<TrayWin>(WTF::move(icon), tooltip, WTF::move(callbacks));
    if (!tray->add()) {
        error = "the notification area refused the icon (no Explorer shell?)"_s;
        return nullptr;
    }
    return tray;
}

} // namespace ZeroG
