#pragma once

// The handles of the C API (include/zerog.h) over the embedding layer.

#include "View.h"
#include "loader/Download.h"
#include "loader/MemoryFS.h"
#include "window/Window.h"
#include "zerog.h"
#include <JavaScriptCore/JSBase.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

struct zg_surface {
    ZeroG::Surface* surface { nullptr };
};

struct zg_binding {
    zg_view* view { nullptr }; // cleared when the view is destroyed
    std::string path;
    zg_bind_fn function { nullptr };
    void* user { nullptr };
};

class ViewClientAPI;

struct zg_view {
    zg_context* context { nullptr };
    zg_window* window { nullptr }; // set when the window owns the view
    std::unique_ptr<ZeroG::View> owned; // null when the window owns the view
    ZeroG::View* view { nullptr };
    zg_surface surface;
    zg_view_callbacks callbacks { };
    std::vector<std::shared_ptr<zg_binding>> bindings;
    std::vector<std::shared_ptr<zg_class>> classes; // Bindings.cpp
    std::vector<std::string> scripts; // zg_view_add_script, in order
    std::unique_ptr<ViewClientAPI> client;
};

struct zg_context {
    zg_runtime* runtime { nullptr };
    std::string appID;
    std::string dataDirectory;
    std::string devServerURL;
    bool ephemeral { false };
    bool allowExternalContent { false };
    zg_trust_fn trust { nullptr };
    void* trustUser { nullptr };
    std::vector<zg_view*> views;
    // Its schemes, cookies; and the file system of each scheme serving files.
    RefPtr<ZeroG::Session> session;
    HashMap<String, Ref<ZeroG::MemoryFS>> fileSystems;
};

struct zg_window {
    zg_context* context { nullptr };
    std::unique_ptr<ZeroG::Window> window;
    zg_view view;
};

struct zg_runtime {
    zg_runtime_config config { };
    std::vector<zg_context*> contexts;
    std::vector<zg_window*> windows;
    // The Web Inspector's windows (zg_view_show_inspector): the engine's own.
    std::vector<std::unique_ptr<ZeroG::Window>> inspectorWindows;
    std::atomic<bool> quit { false }; // zg_runtime_quit, from any thread
    zg_native_handle wakeUpHandle { 0 };
};

namespace ZeroG::API {

// Errors: every failing call sets the calling thread's message.
zg_result fail(zg_result, const String& message);
void clearError();
bool checkThread();

zg_runtime* runtime();

// Trust (docs/design/c-api.md, "Direct JavaScript bindings"): the application's own content (HTML it
// loads itself as about:blank, its schemes), the dev server in dev profiles, then
// the context's policy callback.
bool isTrusted(zg_context&, const URL&, zg_trust_request, bool isMainFrame);

// Bindings (Bindings.cpp).
void installBindings(zg_view&, WebCore::LocalFrame&);
// The view is going: its functions throw from now on, and the native pointers of
// its classes' live instances are finalized.
void detachBindings(zg_view&);
JSGlobalContextRef globalContext(WebCore::LocalFrame&);

} // namespace ZeroG::API
