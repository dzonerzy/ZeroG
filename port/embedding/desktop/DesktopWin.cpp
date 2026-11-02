#include "config.h"
#include "desktop/Desktop.h"

#include <windows.h>
#include <wtf/NeverDestroyed.h>

namespace ZeroG {

namespace {

struct Handler {
    unsigned id;
    DesktopMessageHandler function;
};

Vector<Handler>& handlers()
{
    static NeverDestroyed<Vector<Handler>> list;
    return list;
}

LRESULT CALLBACK desktopProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    // By index: a handler may add or remove handlers.
    for (size_t i = 0; i < handlers().size(); ++i) {
        intptr_t result = 0;
        if (handlers()[i].function(message, wParam, lParam, result))
            return result;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

void* desktopWindow()
{
    // The engine thread's: SDL's message loop dispatches its messages.
    static HWND window = [] {
        WNDCLASSEXW windowClass { sizeof windowClass };
        windowClass.lpfnWndProc = desktopProcedure;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"ZeroGDesktop";
        RegisterClassExW(&windowClass);
        return CreateWindowExW(WS_EX_TOOLWINDOW, L"ZeroGDesktop", L"ZeroG", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, windowClass.hInstance, nullptr);
    }();
    return window;
}

unsigned addDesktopMessageHandler(DesktopMessageHandler&& function)
{
    static unsigned nextID = 1;
    unsigned id = nextID++;
    handlers().append({ id, WTF::move(function) });
    return id;
}

void removeDesktopMessageHandler(unsigned id)
{
    handlers().removeFirstMatching([id](auto& handler) {
        return handler.id == id;
    });
}

} // namespace ZeroG
