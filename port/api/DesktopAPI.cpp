// The desktop around the windows (zerog.h 0.5.3): opening with the system,
// the system tray, notifications and global shortcuts, over desktop/Desktop.h.

#include "config.h"
#include "APIInternal.h"

#include "desktop/Desktop.h"
#include <SDL3/SDL.h>
#include <wtf/MainThread.h>
#include <wtf/text/MakeString.h>

using namespace ZeroG;
using namespace ZeroG::API;

struct zg_tray {
    zg_runtime* runtime { nullptr };
    zg_tray_callbacks callbacks { };
    std::unique_ptr<Tray> tray;
};

namespace {

String text(const char* utf8)
{
    return utf8 ? String::fromUTF8(utf8) : String { };
}

zg_result checkRuntime(zg_runtime* rt)
{
    clearError();
    if (!rt || rt != runtime())
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no runtime"_s);
    if (!checkThread())
        return ZG_ERROR_WRONG_THREAD;
    return ZG_OK;
}

zg_result checkTray(zg_tray* tray)
{
    clearError();
    if (!tray || !tray->tray)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no tray"_s);
    if (!checkThread())
        return ZG_ERROR_WRONG_THREAD;
    return ZG_OK;
}

std::optional<TrayIcon> trayIcon(const void* rgba, unsigned width, unsigned height)
{
    if (!rgba || !width || !height)
        return std::nullopt;
    TrayIcon icon;
    icon.width = width;
    icon.height = height;
    icon.rgba.append(std::span { static_cast<const uint8_t*>(rgba), static_cast<size_t>(width) * height * 4 });
    return icon;
}

} // namespace

zg_result zg_runtime_post(zg_runtime* rt, zg_task_fn task, void* user)
{
    clearError();
    if (!rt || !task)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no runtime or task"_s);
    // The engine thread's queue, which wakes its loop.
    callOnMainThread([task, user] {
        if (runtime())
            task(user);
    });
    return ZG_OK;
}

zg_result zg_open_url(zg_runtime* rt, const char* url)
{
    if (auto result = checkRuntime(rt))
        return result;
    if (!url || !*url)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no URL"_s);
    if (!SDL_OpenURL(url))
        return fail(ZG_ERROR_INTERNAL, makeString("the desktop did not open it: "_s, text(SDL_GetError())));
    return ZG_OK;
}

// ---- System tray ----

zg_tray* zg_tray_create(zg_runtime* rt, const zg_tray_config* config, const zg_tray_callbacks* callbacks)
{
    if (checkRuntime(rt))
        return nullptr;
    if (!config || config->size < sizeof(zg_tray_config)) {
        fail(ZG_ERROR_INVALID_ARGUMENT, "no tray config"_s);
        return nullptr;
    }
    auto icon = trayIcon(config->icon_rgba, config->icon_width, config->icon_height);
    if (!icon) {
        fail(ZG_ERROR_INVALID_ARGUMENT, "a tray needs an icon"_s);
        return nullptr;
    }
    auto* tray = new zg_tray;
    tray->runtime = rt;
    if (callbacks)
        std::memcpy(&tray->callbacks, callbacks, std::min(callbacks->size, sizeof(zg_tray_callbacks)));
    Tray::Callbacks hooks;
    hooks.activated = [tray](int x, int y) {
        if (tray->callbacks.activated)
            tray->callbacks.activated(tray, x, y, tray->callbacks.user);
    };
    hooks.menuItem = [tray](unsigned id) {
        if (tray->callbacks.menu_item)
            tray->callbacks.menu_item(tray, id, tray->callbacks.user);
    };
    String error;
    tray->tray = Tray::create(config->id ? text(config->id) : "zerog"_s, WTF::move(*icon), text(config->tooltip), WTF::move(hooks), error);
    if (!tray->tray) {
        delete tray;
        fail(ZG_ERROR_UNSUPPORTED, error);
        return nullptr;
    }
    rt->trays.push_back(tray);
    return tray;
}

void zg_tray_destroy(zg_tray* tray)
{
    if (!tray || !checkThread())
        return;
    auto& trays = tray->runtime->trays;
    trays.erase(std::remove(trays.begin(), trays.end(), tray), trays.end());
    delete tray;
}

zg_result zg_tray_set_icon(zg_tray* tray, const void* rgba, unsigned width, unsigned height)
{
    if (auto result = checkTray(tray))
        return result;
    auto icon = trayIcon(rgba, width, height);
    if (!icon)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no icon"_s);
    tray->tray->setIcon(WTF::move(*icon));
    return ZG_OK;
}

zg_result zg_tray_set_tooltip(zg_tray* tray, const char* tooltip)
{
    if (auto result = checkTray(tray))
        return result;
    tray->tray->setTooltip(text(tooltip));
    return ZG_OK;
}

zg_result zg_tray_set_menu(zg_tray* tray, const zg_menu* menu)
{
    if (auto result = checkTray(tray))
        return result;
    tray->tray->setMenu(menu ? std::optional<Menu> { menu->menu } : std::nullopt);
    return ZG_OK;
}

zg_result zg_tray_menu_set_checked(zg_tray* tray, unsigned id, int checked)
{
    if (auto result = checkTray(tray))
        return result;
    tray->tray->setChecked(id, checked);
    return ZG_OK;
}

zg_result zg_tray_menu_set_enabled(zg_tray* tray, unsigned id, int enabled)
{
    if (auto result = checkTray(tray))
        return result;
    tray->tray->setEnabled(id, enabled);
    return ZG_OK;
}

zg_result zg_tray_get_rect(zg_tray* tray, int* x, int* y, unsigned* width, unsigned* height)
{
    if (auto result = checkTray(tray))
        return result;
    auto rect = tray->tray->rect();
    if (!rect)
        return fail(ZG_ERROR_NOT_FOUND, "the tray's place is not known (yet)"_s);
    if (x)
        *x = rect->x;
    if (y)
        *y = rect->y;
    if (width)
        *width = rect->width;
    if (height)
        *height = rect->height;
    return ZG_OK;
}

// ---- Notifications ----

static zg_result notificationResult(Notifications::Result result, const String& error)
{
    switch (result) {
    case Notifications::Result::Ok:
        return ZG_OK;
    case Notifications::Result::Unsupported:
        return fail(ZG_ERROR_UNSUPPORTED, error);
    case Notifications::Result::Failed:
        break;
    }
    return fail(ZG_ERROR_INTERNAL, error);
}

zg_result zg_runtime_set_notifications(zg_runtime* rt, const zg_notifications_config* config)
{
    if (auto result = checkRuntime(rt))
        return result;
    if (!config || config->size < sizeof(zg_notifications_config) || !config->app_id || !*config->app_id)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "notifications need an app_id"_s);
    Notifications::Config settings;
    settings.appId = text(config->app_id);
    settings.appName = config->app_name ? text(config->app_name) : settings.appId;
    settings.iconPath = text(config->icon_path);
    if (auto activated = config->activated) {
        settings.activated = [rt, activated, user = config->user](const String& tag, int action) {
            auto utf8 = tag.utf8();
            activated(rt, byteCast<char>(utf8.data()), action, user);
        };
    }
    String error;
    return notificationResult(Notifications::configure(WTF::move(settings), error), error);
}

zg_result zg_notification_show(zg_runtime* rt, const zg_notification* notification)
{
    if (auto result = checkRuntime(rt))
        return result;
    if (!notification || notification->size < sizeof(zg_notification) || !notification->title)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "a notification needs a title"_s);
    if (notification->action_count > 3 || (notification->action_count && !notification->actions))
        return fail(ZG_ERROR_INVALID_ARGUMENT, "up to 3 buttons"_s);
    Vector<String> actions;
    for (size_t i = 0; i < notification->action_count; ++i)
        actions.append(text(notification->actions[i]));
    String error;
    return notificationResult(Notifications::show(text(notification->title), text(notification->body), text(notification->tag), actions, error), error);
}

zg_result zg_notification_remove(zg_runtime* rt, const char* tag)
{
    if (auto result = checkRuntime(rt))
        return result;
    if (!tag || !*tag)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no tag"_s);
    String error;
    return notificationResult(Notifications::remove(text(tag), error), error);
}

// ---- Global shortcuts ----

zg_result zg_runtime_set_shortcut_callback(zg_runtime* rt, zg_shortcut_fn pressed, void* user)
{
    if (auto result = checkRuntime(rt))
        return result;
    if (!pressed)
        Shortcuts::setCallback(nullptr);
    else {
        Shortcuts::setCallback([rt, pressed, user](unsigned id) {
            pressed(rt, id, user);
        });
    }
    return ZG_OK;
}

zg_result zg_shortcut_register(zg_runtime* rt, unsigned id, const char* accelerator)
{
    if (auto result = checkRuntime(rt))
        return result;
    // Windows' hotkey ids for applications.
    if (!id || id > 0xBFFF)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "a shortcut's id is from 1 to 49151"_s);
    auto parsed = parseAccelerator(text(accelerator));
    if (!parsed || !parsed->modifiers)
        return fail(ZG_ERROR_INVALID_ARGUMENT, makeString("not a key with modifiers: \""_s, text(accelerator), '"'));
    String error;
    switch (Shortcuts::add(id, *parsed, text(accelerator), error)) {
    case Shortcuts::Result::Ok:
        return ZG_OK;
    case Shortcuts::Result::Taken:
        return fail(ZG_ERROR_ALREADY_EXISTS, error);
    case Shortcuts::Result::Unsupported:
        return fail(ZG_ERROR_UNSUPPORTED, error);
    case Shortcuts::Result::Invalid:
        break;
    }
    return fail(ZG_ERROR_INVALID_ARGUMENT, error);
}

zg_result zg_shortcut_unregister(zg_runtime* rt, unsigned id)
{
    if (auto result = checkRuntime(rt))
        return result;
    if (!Shortcuts::remove(id))
        return fail(ZG_ERROR_NOT_FOUND, "no such shortcut"_s);
    return ZG_OK;
}
