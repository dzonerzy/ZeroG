#pragma once

// The desktop around the windows (zerog.h 0.5.3): the system tray,
// notifications and global shortcuts, on the engine thread. Windows:
// TrayWin.cpp, NotificationsWin.cpp, ShortcutsWin.cpp over one hidden window
// (DesktopWin.cpp). Linux: StatusNotifierItem and org.freedesktop.
// Notifications over the session bus (SessionBus.cpp, libdbus loaded at run
// time), key grabs over an X11 connection of its own, the GlobalShortcuts
// portal on Wayland.

#include "window/Menu.h"
#include <memory>
#include <optional>
#include <wtf/Function.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace ZeroG {

struct TrayIcon {
    Vector<uint8_t> rgba; // RGBA8, rows top to bottom
    unsigned width { 0 };
    unsigned height { 0 };
};

struct TrayRect {
    int x { 0 };
    int y { 0 };
    unsigned width { 0 };
    unsigned height { 0 };
};

class Tray {
    WTF_MAKE_NONCOPYABLE(Tray);
public:
    struct Callbacks {
        Function<void(int x, int y)> activated;
        Function<void(unsigned id)> menuItem;
    };
    // Null with `error` set where there is no tray.
    static std::unique_ptr<Tray> create(const String& id, TrayIcon&&, const String& tooltip, Callbacks&&, String& error);
    virtual ~Tray() = default;

    virtual void setIcon(TrayIcon&&) = 0;
    virtual void setTooltip(const String&) = 0;
    virtual void setMenu(std::optional<Menu>&&) = 0;
    virtual void setChecked(unsigned id, bool) = 0;
    virtual void setEnabled(unsigned id, bool) = 0;
    virtual std::optional<TrayRect> rect() = 0;

protected:
    Tray() = default;
};

#if OS(WINDOWS)
// The process's hidden window for the tray icons and hotkeys (DesktopWin.cpp),
// created on first use: a top-level one, never shown, so that it gets the
// broadcast TaskbarCreated as well. Its messages go to the handlers added,
// first true wins.
void* desktopWindow(); // HWND
using DesktopMessageHandler = Function<bool(unsigned message, uintptr_t wParam, intptr_t lParam, intptr_t& result)>;
unsigned addDesktopMessageHandler(DesktopMessageHandler&&);
void removeDesktopMessageHandler(unsigned);
#endif

// Changes the item `id` of `items` (submenus too); whether there was one.
bool updateMenuItem(Vector<Menu::Item>& items, unsigned id, std::optional<bool> checked, std::optional<bool> enabled);

namespace Notifications {

struct Config {
    String appId;
    String appName;
    String iconPath;
    Function<void(const String& tag, int action)> activated;
};

enum class Result : uint8_t { Ok, Unsupported, Failed };

Result configure(Config&&, String& error);
Result show(const String& title, const String& body, const String& tag, const Vector<String>& actions, String& error);
Result remove(const String& tag, String& error);

} // namespace Notifications

namespace Shortcuts {

enum class Result : uint8_t { Ok, Taken, Unsupported, Invalid };

void setCallback(Function<void(unsigned id)>&&);
Result add(unsigned id, const Accelerator&, const String& accelerator, String& error);
bool remove(unsigned id);
// To the callback, on the engine thread.
void pressed(unsigned id);

} // namespace Shortcuts

} // namespace ZeroG
