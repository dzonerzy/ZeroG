#include "config.h"
#include "desktop/Desktop.h"

#include <wtf/NeverDestroyed.h>

namespace ZeroG {

bool updateMenuItem(Vector<Menu::Item>& items, unsigned id, std::optional<bool> checked, std::optional<bool> enabled)
{
    bool found = false;
    for (auto& item : items) {
        if (item.kind == Menu::Item::Kind::Submenu) {
            found |= updateMenuItem(item.submenu, id, checked, enabled);
            continue;
        }
        if (item.kind != Menu::Item::Kind::Action || item.id != id)
            continue;
        if (checked)
            item.checked = *checked;
        if (enabled)
            item.enabled = *enabled;
        found = true;
    }
    return found;
}

namespace Shortcuts {

static Function<void(unsigned)>& callback()
{
    static NeverDestroyed<Function<void(unsigned)>> function;
    return function;
}

void setCallback(Function<void(unsigned)>&& function)
{
    callback() = WTF::move(function);
}

void pressed(unsigned id)
{
    if (callback())
        callback()(id);
}

} // namespace Shortcuts

} // namespace ZeroG
