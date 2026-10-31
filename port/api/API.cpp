// The C API (include/zerog.h): runtime, contexts, views, surfaces, input,
// callbacks and windows, over the embedding layer. Bindings and values are in
// Bindings.cpp.

#include "config.h"
#include "APIInternal.h"

#include "Embedding.h"
#include "graphics/Compositor.h"
#include "graphics/Surface.h"
#include "inspector/InspectorZeroG.h"
#include "window/ClipboardSDL.h"
#include "window/KeyboardSDL.h"
#include <WebCore/FileChooser.h>
#include <WebCore/PasteboardItemsZeroG.h>
#include <WebCore/SharedBuffer.h>
#include <JavaScriptCore/ConsoleTypes.h>
#include <JavaScriptCore/HeapInlines.h>
#include <JavaScriptCore/JSLock.h>
#include <JavaScriptCore/VM.h>
#include <WebCore/Color.h>
#include <WebCore/CommonVM.h>
#include <WebCore/CompositionHighlight.h>
#include <WebCore/CompositionUnderline.h>
#include <WebCore/Document.h>
#include <WebCore/DocumentView.h>
#include <WebCore/Editor.h>
#include <WebCore/FocusController.h>
#include <WebCore/FrameLoader.h>
#include <WebCore/FrameSelection.h>
#include <WebCore/GarbageCollectionController.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/MemoryCache.h>
#include <WebCore/Page.h>
#include <WebCore/SecurityOrigin.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <wtf/FileSystem.h>
#include <wtf/MainThread.h>
#include <wtf/MonotonicTime.h>
#include <wtf/RunLoop.h>
#include <wtf/text/MakeString.h>

#if OS(WINDOWS)
#include <windows.h>
#else
#include <csignal>
#include <cstdlib>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#endif

#ifndef ZEROG_WEBKIT_REVISION
#define ZEROG_WEBKIT_REVISION "unknown"
#endif
#ifndef ZEROG_PROFILE_NAME
#define ZEROG_PROFILE_NAME "unknown"
#endif

using namespace WebCore;
using namespace ZeroG;

namespace ZeroG::API {

static thread_local std::string t_lastError;

zg_result fail(zg_result result, const String& message)
{
    auto utf8 = message.utf8();
    t_lastError.assign(byteCast<char>(utf8.data()), utf8.length());
    return result;
}

void clearError()
{
    t_lastError.clear();
}

bool checkThread()
{
    if (isMainThread())
        return true;
    fail(ZG_ERROR_WRONG_THREAD, "called off the runtime thread"_s);
    return false;
}

static zg_runtime* s_runtime;
static bool s_runtimeCreated;

zg_runtime* runtime()
{
    return s_runtime;
}

#if defined(ZEROG_DEV_PROFILE)
static constexpr bool devProfile = true;
#else
static constexpr bool devProfile = false;
#endif

bool isTrusted(zg_context& context, const URL& url, zg_trust_request request, bool isMainFrame)
{
    // HTML the application loads itself (zg_view_load_html), and its own schemes.
    // data: is not: a page could build one from any string.
    if (url.isEmpty() || url.isAboutBlank() || url.protocolIsAbout())
        return true;
    if (context.session && context.session->hasScheme(url.protocol()))
        return true;
    if (devProfile && !context.devServerURL.empty()) {
        auto devServer = SecurityOrigin::create(URL { String::fromUTF8(context.devServerURL.c_str()) });
        if (devServer->isSameOriginAs(SecurityOrigin::create(url)))
            return true;
    }
    if (context.trust) {
        auto origin = SecurityOrigin::create(url)->toString().utf8();
        return context.trust(byteCast<char>(origin.data()), request, isMainFrame, context.trustUser);
    }
    return false;
}

} // namespace ZeroG::API

using namespace ZeroG::API;

namespace {

String stringFromUTF8(const char* text, size_t length)
{
    if (!text)
        return { };
    return String::fromUTF8(std::span(text, length));
}

String stringFromUTF8(const char* text)
{
    return text ? String::fromUTF8(text) : String { };
}

std::string utf8String(const String& string)
{
    auto utf8 = string.utf8();
    return { byteCast<char>(utf8.data()), utf8.length() };
}

template<typename T>
T copyConfig(const T* config)
{
    T copy { };
    if (config)
        std::memcpy(&copy, config, std::min<size_t>(config->size ? config->size : sizeof(T), sizeof(T)));
    copy.size = sizeof(T);
    return copy;
}

View::Modifiers modifiers(unsigned flags)
{
    View::Modifiers modifiers;
    if (flags & ZG_MOD_SHIFT)
        modifiers.add(PlatformEvent::Modifier::ShiftKey);
    if (flags & ZG_MOD_CTRL)
        modifiers.add(PlatformEvent::Modifier::ControlKey);
    if (flags & ZG_MOD_ALT)
        modifiers.add(PlatformEvent::Modifier::AltKey);
    if (flags & ZG_MOD_META)
        modifiers.add(PlatformEvent::Modifier::MetaKey);
    return modifiers;
}

zg_cursor cursorFor(PlatformCursorType type)
{
    switch (type) {
    case PlatformCursorType::Hand:
        return ZG_CURSOR_POINTER;
    case PlatformCursorType::IBeam:
    case PlatformCursorType::VerticalText:
        return ZG_CURSOR_TEXT;
    case PlatformCursorType::Wait:
    case PlatformCursorType::Progress:
        return ZG_CURSOR_WAIT;
    case PlatformCursorType::Cross:
    case PlatformCursorType::Cell:
        return ZG_CURSOR_CROSSHAIR;
    case PlatformCursorType::Move:
    case PlatformCursorType::Grab:
    case PlatformCursorType::Grabbing:
        return ZG_CURSOR_MOVE;
    case PlatformCursorType::NotAllowed:
    case PlatformCursorType::NoDrop:
        return ZG_CURSOR_NOT_ALLOWED;
    case PlatformCursorType::EastResize:
    case PlatformCursorType::WestResize:
    case PlatformCursorType::EastWestResize:
    case PlatformCursorType::ColumnResize:
        return ZG_CURSOR_RESIZE_EW;
    case PlatformCursorType::NorthResize:
    case PlatformCursorType::SouthResize:
    case PlatformCursorType::NorthSouthResize:
    case PlatformCursorType::RowResize:
        return ZG_CURSOR_RESIZE_NS;
    case PlatformCursorType::NorthEastResize:
    case PlatformCursorType::SouthWestResize:
    case PlatformCursorType::NorthEastSouthWestResize:
        return ZG_CURSOR_RESIZE_NESW;
    case PlatformCursorType::NorthWestResize:
    case PlatformCursorType::SouthEastResize:
    case PlatformCursorType::NorthWestSouthEastResize:
        return ZG_CURSOR_RESIZE_NWSE;
    case PlatformCursorType::None:
        return ZG_CURSOR_NONE;
    default:
        return ZG_CURSOR_DEFAULT;
    }
}

zg_log_level logLevel(JSC::MessageLevel level)
{
    switch (level) {
    case JSC::MessageLevel::Debug:
        return ZG_LOG_DEBUG;
    case JSC::MessageLevel::Warning:
        return ZG_LOG_WARNING;
    case JSC::MessageLevel::Error:
        return ZG_LOG_ERROR;
    default:
        return ZG_LOG_INFO;
    }
}

RefPtr<LocalFrame> mainFrame(zg_view& view)
{
    return view.view->page().localMainFrame();
}

} // namespace

// An HTTP challenge waiting for the application; answered on the runtime thread.
struct zg_auth_challenge {
    CompletionHandler<void(std::optional<Credential>&&)> completionHandler;
    std::string url;
    std::string realm;
    const char* scheme;
    unsigned failures;
};

// A download, as the application sees it; its strings are kept for its lifetime.
struct zg_download {
    Ref<ZeroG::Download> download;
    std::string url;
    std::string suggestedFilename;
    std::string mimeType;
};

// <input type=file> waiting for the application.
struct zg_file_chooser {
    CompletionHandler<void(std::optional<Vector<String>>&&)> completionHandler;
    bool allowsMultiple { false };
    bool allowsDirectories { false };
    std::vector<std::string> accept;
};

// A context menu waiting for the application; its strings live with it.
struct zg_context_menu {
    CompletionHandler<void(std::optional<unsigned>)> completionHandler;
    float x { 0 };
    float y { 0 };
    std::string linkURL, linkText, imageURL, mediaURL, selectedText, frameURL;
    bool isEditable { false };
    std::vector<std::string> labels;
    std::vector<zg_context_menu_item> items;
};

struct zg_clipboard_reply {
    RefPtr<SharedBuffer> data;
};

// The view's events, as zg_view_callbacks.
class ViewClientAPI final : public ViewClient {
public:
    explicit ViewClientAPI(zg_view& view)
        : m_view(view)
    {
    }

private:
    const zg_view_callbacks& callbacks() const { return m_view.callbacks; }

    void consoleMessage(JSC::MessageSource source, JSC::MessageLevel level, const String& message, unsigned line, unsigned column, const String& sourceURL, const String& stack) final
    {
        auto text = message.utf8();
        auto file = sourceURL.utf8();
        // The page's console API is CONSOLE; what the engine reports about the page
        // (network, security, content policy, script errors) is ENGINE.
        auto logSource = source == JSC::MessageSource::ConsoleAPI ? ZG_LOG_SOURCE_CONSOLE : ZG_LOG_SOURCE_ENGINE;
        zg_log_message log { logSource, logLevel(level), byteCast<char>(text.data()), text.length(), byteCast<char>(file.data()), file.length(), line, column, &m_view };
        if (callbacks().console)
            callbacks().console(&m_view, &log, callbacks().user);
        if (auto* rt = runtime(); rt && rt->config.log && log.level >= rt->config.log_level)
            rt->config.log(&log, rt->config.log_user);
        if (callbacks().js_error && source == JSC::MessageSource::JS && level == JSC::MessageLevel::Error) {
            auto stackText = stack.utf8();
            zg_js_error error { log.message, log.message_len, stack.isEmpty() ? nullptr : byteCast<char>(stackText.data()), stackText.length(),
                log.file, log.file_len, line, column, message.contains("Unhandled Promise Rejection"_s) };
            callbacks().js_error(&m_view, &error, callbacks().user);
        }
    }

    void cursorChanged(PlatformCursorType type) final
    {
        if (callbacks().cursor_changed)
            callbacks().cursor_changed(&m_view, cursorFor(type), callbacks().user);
    }

    void tooltipChanged(const String& text) final
    {
        if (!callbacks().tooltip_changed)
            return;
        auto utf8 = text.utf8();
        callbacks().tooltip_changed(&m_view, byteCast<char>(utf8.data()), utf8.length(), callbacks().user);
    }

    void titleChanged(const String& title) final
    {
        if (!callbacks().title_changed)
            return;
        auto utf8 = title.utf8();
        callbacks().title_changed(&m_view, byteCast<char>(utf8.data()), utf8.length(), callbacks().user);
    }

    void loadFinished(const URL& url, const ResourceError& error) final
    {
        if (!callbacks().load_finished)
            return;
        // The reason is zg_last_error() during the callback.
        if (!error.isNull())
            fail(ZG_ERROR_NOT_FOUND, error.localizedDescription().isEmpty() ? makeString("load failed ("_s, error.domain(), ' ', error.errorCode(), ')') : error.localizedDescription());
        auto utf8 = url.string().utf8();
        callbacks().load_finished(&m_view, byteCast<char>(utf8.data()), !error.isNull(), callbacks().user);
    }

    bool allowNavigation(const URL& url, bool isMainFrame) final
    {
        auto& context = *m_view.context;
        if (isTrusted(context, url, ZG_TRUST_NAVIGATE, isMainFrame) || context.allowExternalContent)
            return true;
        auto utf8 = url.string().utf8();
        if (callbacks().navigation_blocked && callbacks().navigation_blocked(&m_view, byteCast<char>(utf8.data()), callbacks().user))
            return true;
        if (isMainFrame && callbacks().open_external)
            callbacks().open_external(&m_view, byteCast<char>(utf8.data()), callbacks().user);
        return false;
    }

    void openExternal(const URL& url) final
    {
        if (!callbacks().open_external)
            return;
        auto utf8 = url.string().utf8();
        callbacks().open_external(&m_view, byteCast<char>(utf8.data()), callbacks().user);
    }

    void windowObjectCleared(LocalFrame& frame) final
    {
        if (frame.isMainFrame())
            installBindings(m_view, frame);
    }

    int dialog(zg_dialog_kind kind, const String& message, const String& defaultText, char** reply)
    {
        if (!callbacks().dialog)
            return 0;
        auto text = message.utf8();
        auto initial = defaultText.utf8();
        return callbacks().dialog(&m_view, kind, byteCast<char>(text.data()), byteCast<char>(initial.data()), reply, callbacks().user);
    }

    void alert(const String& message) final
    {
        dialog(ZG_DIALOG_ALERT, message, { }, nullptr);
    }

    bool confirm(const String& message) final
    {
        return dialog(ZG_DIALOG_CONFIRM, message, { }, nullptr);
    }

    std::optional<String> prompt(const String& message, const String& defaultValue) final
    {
        char* reply = nullptr;
        if (!dialog(ZG_DIALOG_PROMPT, message, defaultValue, &reply))
            return std::nullopt;
        String result = reply ? String::fromUTF8(reply) : emptyString();
        free(reply);
        return result;
    }

    void authenticationChallenge(const AuthenticationChallenge& challenge, CompletionHandler<void(std::optional<Credential>&&)>&& completionHandler) final
    {
        if (!callbacks().auth_challenge) {
            completionHandler(std::nullopt);
            return;
        }
        auto& space = challenge.protectionSpace();
        auto scheme = [&] {
            switch (space.authenticationScheme()) {
            case ProtectionSpace::AuthenticationScheme::HTTPBasic: return "basic";
            case ProtectionSpace::AuthenticationScheme::HTTPDigest: return "digest";
            case ProtectionSpace::AuthenticationScheme::NTLM: return "ntlm";
            case ProtectionSpace::AuthenticationScheme::Negotiate: return "negotiate";
            default: return "other";
            }
        }();
        auto* request = new zg_auth_challenge { WTF::move(completionHandler), toUTF8(challenge.failureResponse().url().string()),
            toUTF8(space.realm()), scheme, challenge.previousFailureCount() };
        callbacks().auth_challenge(&m_view, request, callbacks().user);
    }

    static std::string toUTF8(const String& string)
    {
        auto utf8 = string.utf8();
        return { byteCast<char>(utf8.data()), utf8.length() };
    }

    void downloadStarted(ZeroG::Download& download) final
    {
        if (!callbacks().download_started)
            return;
        auto handle = std::unique_ptr<zg_download>(new zg_download { download, toUTF8(download.url().string()), toUTF8(download.suggestedFilename()), toUTF8(download.mimeType()) });
        auto* pointer = handle.get();
        callbacks().download_started(&m_view, pointer, callbacks().user);
        // Accepted: kept until it ends.
        if (!download.destination().isEmpty())
            m_downloads.add(&download, WTF::move(handle));
    }

    void downloadProgress(ZeroG::Download& download) final
    {
        if (auto* handle = m_downloads.get(&download); handle && callbacks().download_progress)
            callbacks().download_progress(&m_view, handle, callbacks().user);
    }

    void downloadFinished(ZeroG::Download& download, const ResourceError& error) final
    {
        auto handle = m_downloads.take(&download);
        if (!handle || !callbacks().download_finished)
            return;
        if (!error.isNull())
            fail(ZG_ERROR_INTERNAL, error.localizedDescription().isEmpty() ? "download failed"_s : error.localizedDescription());
        callbacks().download_finished(&m_view, handle.get(), !error.isNull(), callbacks().user);
    }

    void runOpenPanel(const FileChooserSettings& settings, CompletionHandler<void(std::optional<Vector<String>>&&)>&& completionHandler) final
    {
        if (callbacks().file_chooser) {
            auto* chooser = new zg_file_chooser;
            chooser->completionHandler = WTF::move(completionHandler);
            chooser->allowsMultiple = settings.allowsMultipleFiles;
            chooser->allowsDirectories = settings.allowsDirectories;
            for (auto& type : settings.acceptMIMETypes)
                chooser->accept.push_back(toUTF8(type));
            for (auto& extension : settings.acceptFileExtensions)
                chooser->accept.push_back(toUTF8(extension));
            callbacks().file_chooser(&m_view, chooser, callbacks().user);
            return;
        }
        // Window mode: the system's dialog.
        if (m_view.window) {
            m_view.window->window->showOpenDialog(settings, WTF::move(completionHandler));
            return;
        }
        completionHandler(std::nullopt);
    }

    void showContextMenu(ZeroG::ContextMenu&& request, CompletionHandler<void(std::optional<unsigned>)>&& completionHandler) final
    {
        if (!callbacks().context_menu) {
            completionHandler(std::nullopt);
            return;
        }
        auto* menu = new zg_context_menu;
        menu->completionHandler = WTF::move(completionHandler);
        menu->x = request.position.x();
        menu->y = request.position.y();
        auto urlString = [](const URL& url) { return url.isEmpty() ? std::string { } : toUTF8(url.string()); };
        menu->linkURL = urlString(request.linkURL);
        menu->linkText = toUTF8(request.linkText);
        menu->imageURL = urlString(request.imageURL);
        menu->mediaURL = urlString(request.mediaURL);
        menu->selectedText = toUTF8(request.selectedText);
        menu->frameURL = urlString(request.frameURL);
        menu->isEditable = request.isEditable;
        // The labels first: the items point into them.
        menu->labels.reserve(request.items.size());
        for (auto& item : request.items)
            menu->labels.push_back(toUTF8(item.label));
        for (size_t i = 0; i < request.items.size(); ++i) {
            auto& item = request.items[i];
            zg_context_menu_item_kind kind = ZG_CONTEXT_MENU_ITEM_ACTION;
            switch (item.kind) {
            case ZeroG::ContextMenu::ItemKind::Action: kind = ZG_CONTEXT_MENU_ITEM_ACTION; break;
            case ZeroG::ContextMenu::ItemKind::Checkbox: kind = ZG_CONTEXT_MENU_ITEM_CHECKBOX; break;
            case ZeroG::ContextMenu::ItemKind::Separator: kind = ZG_CONTEXT_MENU_ITEM_SEPARATOR; break;
            case ZeroG::ContextMenu::ItemKind::Submenu: kind = ZG_CONTEXT_MENU_ITEM_SUBMENU; break;
            }
            menu->items.push_back({ item.id, item.parent, kind, menu->labels[i].c_str(), item.enabled, item.checked });
        }
        callbacks().context_menu(&m_view, menu, callbacks().user);
    }

    // The Web Inspector's frontend: a window of the runtime's, whatever the
    // inspected view's mode.
    View* createInspectorView(Session& session) final
    {
        auto* rt = runtime();
        if (!rt)
            return nullptr;
        auto window = Window::create({ "Web Inspector"_s, { 1000, 700 }, true, false, true, &session });
        if (!window) {
            fail(ZG_ERROR_GPU, makeString("cannot create the inspector's window: "_s, Window::lastError()));
            return nullptr;
        }
        auto* view = &window->view();
        rt->inspectorWindows.push_back(WTF::move(window));
        return view;
    }

    // The loop destroys closed inspector windows.
    void closeInspectorView(View& view) final
    {
        if (auto* window = inspectorWindow(view))
            window->quit();
    }

    void bringInspectorViewToFront(View& view) final
    {
        if (auto* window = inspectorWindow(view))
            window->raise();
    }

    static Window* inspectorWindow(View& view)
    {
        auto* rt = runtime();
        if (!rt)
            return nullptr;
        for (auto& window : rt->inspectorWindows) {
            if (&window->view() == &view)
                return window.get();
        }
        return nullptr;
    }

    zg_view& m_view;
    HashMap<ZeroG::Download*, std::unique_ptr<zg_download>> m_downloads;
};

extern "C" {

// ---- Errors and version ----

const char* zg_last_error(void)
{
    return t_lastError.c_str();
}

zg_version_info zg_version(void)
{
    return { ZG_VERSION_MAJOR, ZG_VERSION_MINOR, ZG_VERSION_PATCH, ZEROG_WEBKIT_REVISION, ZEROG_PROFILE_NAME };
}

// ---- Runtime ----

zg_runtime* zg_runtime_create(const zg_runtime_config* configuration)
{
    clearError();
    if (s_runtimeCreated) {
        fail(ZG_ERROR_ALREADY_EXISTS, "the runtime exists for the whole process and cannot be created again"_s);
        return nullptr;
    }
    auto config = copyConfig(configuration);

#if !OS(WINDOWS)
    // JSC suspends threads with a signal during concurrent GC. Pick a real-time
    // signal nobody handles instead of SIGUSR1, which hosts often use.
    if (!getenv("JSC_SIGNAL_FOR_GC")) {
        int signal = config.gc_signal;
        for (int candidate = SIGRTMIN + 4; !signal && candidate <= SIGRTMAX; ++candidate) {
            struct sigaction action { };
            if (!sigaction(candidate, nullptr, &action) && action.sa_handler == SIG_DFL)
                signal = candidate;
        }
        if (signal)
            setenv("JSC_SIGNAL_FOR_GC", std::to_string(signal).c_str(), 1);
    }
#endif

    ProcessOptions options;
    options.jit = config.jit != ZG_JIT_DISABLED;
    options.gcThreads = config.gc_threads;
    options.jitThreads = config.jit_threads;
    options.heapLimit = config.js_heap_limit;
    options.synchronousImageDecoding = config.sync_image_decoding;
    initializeProcess(options);
    if (config.cache_limit) {
        auto limit = static_cast<unsigned>(std::min<size_t>(config.cache_limit, std::numeric_limits<unsigned>::max()));
        MemoryCache::singleton().setCapacities(0, limit, limit);
    }
    s_runtimeCreated = true;
    s_runtime = new zg_runtime { config };

    // The engine's own diagnostics go to the log callback (or stderr without one).
    if (config.log) {
        setLogHandler([](LogLevel level, const String& message) {
            auto* rt = runtime();
            if (!rt || !rt->config.log)
                return;
            static constexpr zg_log_level levels[] = { ZG_LOG_DEBUG, ZG_LOG_INFO, ZG_LOG_WARNING, ZG_LOG_ERROR };
            zg_log_message log { ZG_LOG_SOURCE_ENGINE, levels[static_cast<int>(level)], nullptr, 0, nullptr, 0, 0, 0, nullptr };
            if (log.level < rt->config.log_level)
                return;
            auto text = message.utf8();
            log.message = byteCast<char>(text.data());
            log.message_len = text.length();
            rt->config.log(&log, rt->config.log_user);
        });
    }

    // Remote inspection, local to the machine (an SSH tunnel reaches it from
    // another): dev and debug profiles only.
    if (config.inspector_port && ViewInspector::isAvailable()) {
        bool listening = config.inspector_port <= std::numeric_limits<uint16_t>::max()
            && startRemoteInspectorServer("127.0.0.1"_s, static_cast<uint16_t>(config.inspector_port));
        if (!listening)
            ZeroG::log(LogLevel::Warning, makeString("the remote inspector cannot listen on 127.0.0.1:"_s, config.inspector_port));
    }

#if OS(WINDOWS)
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s_runtime->wakeUpHandle = reinterpret_cast<zg_native_handle>(event);
    addWakeUpObserver([event] {
        SetEvent(event);
    });
#else
    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    s_runtime->wakeUpHandle = fd;
    addWakeUpObserver([fd] {
        uint64_t one = 1;
        [[maybe_unused]] auto written = write(fd, &one, sizeof(one));
    });
#endif
    return s_runtime;
}

void zg_runtime_destroy(zg_runtime* rt)
{
    if (!rt || !checkThread())
        return;
    rt->inspectorWindows.clear();
    while (!rt->windows.empty())
        zg_window_destroy(rt->windows.back());
    while (!rt->contexts.empty())
        zg_context_destroy(rt->contexts.back());
    // The wake-up observers outlive the runtime: the handle stays open, as the
    // runtime cannot be created again.
    s_runtime = nullptr;
    delete rt;
}

// Sleeps on the wake-up handle, up to `milliseconds` (-1: until woken).
static void waitForWork(zg_runtime& rt, int64_t milliseconds)
{
#if OS(WINDOWS)
    WaitForSingleObject(reinterpret_cast<HANDLE>(rt.wakeUpHandle), milliseconds < 0 ? INFINITE : static_cast<DWORD>(milliseconds));
#else
    pollfd descriptor { static_cast<int>(rt.wakeUpHandle), POLLIN, 0 };
    poll(&descriptor, 1, milliseconds < 0 ? -1 : static_cast<int>(std::min<int64_t>(milliseconds, std::numeric_limits<int>::max())));
#endif
}

// Due timers, network responses and pending tasks.
static void runEngineWork(zg_runtime& rt)
{
#if !OS(WINDOWS)
    uint64_t count;
    while (read(static_cast<int>(rt.wakeUpHandle), &count, sizeof(count)) > 0) { }
#else
    UNUSED_PARAM(rt);
#endif
    runPendingWork();
}

// The inspector's windows: closed ones go (their views first tell the inspector),
// open ones get `maxWait` each for their events and frames.
static void runInspectorWindows(zg_runtime& rt, std::optional<Seconds> maxWait)
{
    std::erase_if(rt.inspectorWindows, [](auto& window) {
        return window->isClosed();
    });
    for (size_t i = 0; i < rt.inspectorWindows.size(); ++i)
        rt.inspectorWindows[i]->runOnce(maxWait);
}

zg_result zg_runtime_run(zg_runtime* rt)
{
    clearError();
    if (!rt)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no runtime"_s);
    if (!checkThread())
        return ZG_ERROR_WRONG_THREAD;
    // A quit asked for before the loop started ends it at once.
    bool hadWindows = !rt->windows.empty();
    while (!rt->quit) {
        std::vector<zg_window*> open;
        for (auto* window : rt->windows) {
            if (!window->window->isClosed())
                open.push_back(window);
        }
        // The last window was closed: the application is done.
        if (open.empty() && hadWindows)
            break;
        runInspectorWindows(*rt, 1_ms);
        if (open.empty()) {
            // No window (CPU and texture views): the engine's own work, sleeping
            // until a timer is due or something wakes the loop; with an
            // inspector window, its events are polled.
            runEngineWork(*rt);
            if (!rt->quit) {
                auto timeout = zg_runtime_next_timeout(rt);
                if (!rt->inspectorWindows.empty())
                    timeout = timeout < 0 ? 4 : std::min<int64_t>(timeout, 4);
                waitForWork(*rt, timeout);
            }
            continue;
        }
        // One window waits for its events; several take turns without blocking.
        bool alone = open.size() == 1 && rt->inspectorWindows.empty();
        for (auto* window : open)
            window->window->runOnce(alone ? std::nullopt : std::optional { 1_ms });
    }
    rt->quit = false;
    return ZG_OK;
}

void zg_runtime_quit(zg_runtime* rt)
{
    if (!rt)
        return;
    rt->quit = true;
    // From another thread the loop may be asleep: an empty task wakes it (the
    // wake-up handle, and the window loop's SDL event).
    if (!isMainThread())
        callOnMainThread([] { });
}

void zg_runtime_update(zg_runtime* rt)
{
    if (!rt || !checkThread())
        return;
    runEngineWork(*rt);
    // Own-loop hosts drive the inspector's windows from here.
    if (!rt->inspectorWindows.empty())
        runInspectorWindows(*rt, 0_s);
}

zg_native_handle zg_runtime_wakeup_handle(zg_runtime* rt)
{
    return rt ? rt->wakeUpHandle : 0;
}

int64_t zg_runtime_next_timeout(zg_runtime* rt)
{
    if (!rt)
        return -1;
    auto next = RunLoop::mainSingleton().nextWakeUpTime();
    if (!next)
        return -1;
    return std::max<int64_t>(0, static_cast<int64_t>(std::ceil((*next - MonotonicTime::now()).milliseconds())));
}

double zg_runtime_now_ms(void)
{
    return MonotonicTime::now().secondsSinceEpoch().milliseconds();
}

void zg_runtime_memory_stats(zg_runtime* rt, zg_memory_stats* out)
{
    if (!rt || !out || !checkThread())
        return;
    zg_memory_stats stats { };
    stats.size = sizeof(stats);
    {
        auto& vm = commonVM();
        JSC::JSLockHolder lock(vm);
        stats.js_heap_bytes = vm.heap.size();
        stats.js_heap_capacity = vm.heap.capacity();
    }
    stats.resource_cache_bytes = MemoryCache::singleton().size();
    for (auto* context : rt->contexts) {
        for (auto& fileSystem : context->fileSystems.values())
            stats.asset_bytes += fileSystem->ownedBytes();
        for (auto* view : context->views) {
            auto size = view->view->surface().size();
            stats.surface_bytes += static_cast<size_t>(size.width()) * size.height() * 4 * 2;
        }
    }
    for (auto* window : rt->windows) {
        auto size = window->view.view->surface().size();
        stats.surface_bytes += static_cast<size_t>(size.width()) * size.height() * 4 * 2;
    }
    std::memcpy(out, &stats, std::min<size_t>(out->size ? out->size : sizeof(stats), sizeof(stats)));
}

void zg_runtime_collect_garbage(zg_runtime* rt)
{
    if (rt && checkThread())
        GarbageCollectionController::singleton().garbageCollectNow();
}

// ---- Contexts ----

static std::string toStdString(const String& string)
{
    auto utf8 = string.utf8();
    return { byteCast<char>(utf8.data()), utf8.length() };
}

// The app_id names a directory: reverse-DNS characters only.
static bool isValidAppID(const std::string& appID)
{
    if (!appID.empty() && appID.front() == '.')
        return false;
    return std::all_of(appID.begin(), appID.end(), [](char character) {
        return isASCIIAlphanumeric(character) || character == '.' || character == '-' || character == '_';
    });
}

// The per-app data directory: %LOCALAPPDATA%\<app_id> on Windows,
// $XDG_DATA_HOME/<app_id> (default ~/.local/share) elsewhere. Empty without an app_id.
static std::string defaultDataDirectory(const std::string& appID)
{
    if (appID.empty())
        return { };
#if OS(WINDOWS)
    if (auto* localAppData = std::getenv("LOCALAPPDATA"); localAppData && *localAppData)
        return std::string(localAppData) + "\\" + appID;
#else
    if (auto* dataHome = std::getenv("XDG_DATA_HOME"); dataHome && *dataHome)
        return std::string(dataHome) + "/" + appID;
    if (auto* home = std::getenv("HOME"); home && *home)
        return std::string(home) + "/.local/share/" + appID;
#endif
    return { };
}

zg_context* zg_context_create(zg_runtime* rt, const zg_context_config* configuration)
{
    clearError();
    if (!rt) {
        fail(ZG_ERROR_INVALID_ARGUMENT, "no runtime"_s);
        return nullptr;
    }
    if (!checkThread())
        return nullptr;
    auto config = copyConfig(configuration);
    std::string appID = config.app_id ? config.app_id : "";
    if (!isValidAppID(appID)) {
        fail(ZG_ERROR_INVALID_ARGUMENT, makeString("invalid app_id (letters, digits, '.', '-' and '_'): "_s, String::fromUTF8(appID.c_str())));
        return nullptr;
    }
    auto* context = new zg_context;
    context->runtime = rt;
    context->appID = appID;
    context->dataDirectory = config.data_dir ? config.data_dir : defaultDataDirectory(appID);
    context->devServerURL = config.dev_server_url ? config.dev_server_url : "";
    context->ephemeral = config.ephemeral;
    context->allowExternalContent = config.allow_external_content;
    context->trust = config.trust;
    context->trustUser = config.trust_user;

    // Cookies, local storage and IndexedDB persist in the data directory unless
    // the context is ephemeral or has none (no data_dir and no app_id): then they
    // live in memory.
    String cookieDatabase;
    String storageDirectory;
    if (!context->ephemeral && !context->dataDirectory.empty()) {
        auto directory = String::fromUTF8(context->dataDirectory.c_str());
        if (FileSystem::makeAllDirectories(directory)) {
            cookieDatabase = FileSystem::pathByAppendingComponent(directory, "Cookies.sqlite"_s);
            storageDirectory = directory;
        }
    }
    uint64_t httpCacheCapacity = config.http_cache_limit < 0 ? 0
        : config.http_cache_limit ? static_cast<uint64_t>(config.http_cache_limit) : Session::defaultHTTPCacheCapacity;
    context->session = Session::create(cookieDatabase, storageDirectory, httpCacheCapacity);
    rt->contexts.push_back(context);
    return context;
}

void zg_context_destroy(zg_context* context)
{
    if (!context || !checkThread())
        return;
    // Its windows and views go first.
    auto& windows = context->runtime->windows;
    for (auto iterator = windows.begin(); iterator != windows.end();) {
        if ((*iterator)->context == context) {
            zg_window_destroy(*iterator);
            iterator = windows.begin();
        } else
            ++iterator;
    }
    while (!context->views.empty())
        zg_view_destroy(context->views.back());
    context->session->close();
    context->fileSystems.clear();
    auto& contexts = context->runtime->contexts;
    contexts.erase(std::remove(contexts.begin(), contexts.end(), context), contexts.end());
    delete context;
}

// ---- Schemes and files ----

static zg_result registrationFailure(Session::RegisterResult result, const String& scheme)
{
    switch (result) {
    case Session::RegisterResult::Registered:
        return ZG_OK;
    case Session::RegisterResult::InvalidScheme:
        return fail(ZG_ERROR_INVALID_ARGUMENT, makeString("invalid scheme name (lowercase letter, then letters, digits, '+', '-', '.'): "_s, scheme));
    case Session::RegisterResult::BuiltinScheme:
        return fail(ZG_ERROR_INVALID_ARGUMENT, makeString("built-in scheme: "_s, scheme));
    case Session::RegisterResult::AlreadyRegistered:
        return fail(ZG_ERROR_ALREADY_EXISTS, makeString("scheme already registered: "_s, scheme));
    }
    return ZG_ERROR_INTERNAL;
}

// The file system serving `scheme` in the context, created and registered on
// first use. Null if the scheme cannot serve files; `result` says why.
static MemoryFS* fileSystem(zg_context& context, const String& scheme, zg_result& result)
{
    result = ZG_OK;
    if (auto* fileSystem = context.fileSystems.get(scheme))
        return fileSystem;
    if (context.session->hasScheme(scheme)) {
        result = fail(ZG_ERROR_ALREADY_EXISTS, makeString("scheme has a handler: "_s, scheme));
        return nullptr;
    }
    auto fileSystem = MemoryFS::create();
    result = registrationFailure(context.session->registerScheme(scheme, fileSystem.copyRef()), scheme);
    if (result != ZG_OK)
        return nullptr;
    auto* pointer = fileSystem.ptr();
    context.fileSystems.add(scheme, WTF::move(fileSystem));
    return pointer;
}

zg_result zg_context_mount_assets(zg_context* context, const char* scheme, const void* archive, size_t length)
{
    clearError();
    if (!context || !scheme || (!archive && length))
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no context, scheme or archive"_s);
    if (!checkThread())
        return ZG_ERROR_WRONG_THREAD;
    zg_result result;
    auto* files = fileSystem(*context, stringFromUTF8(scheme), result);
    if (!files)
        return result;
    auto mounted = files->mountArchive("/"_s, std::span { static_cast<const uint8_t*>(archive), length });
    if (!mounted)
        return fail(mounted.error().kind == MemoryFS::MountError::Kind::Conflict ? ZG_ERROR_ALREADY_EXISTS : ZG_ERROR_INVALID_ARGUMENT, mounted.error().message);
    return ZG_OK;
}

zg_result zg_context_add_file(zg_context* context, const char* scheme, const char* path, const void* data, size_t length, const char* mimeType, zg_release_fn release, void* user)
{
    clearError();
    if (!context || !scheme || !path || (!data && length))
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no context, scheme, path or data"_s);
    if (!checkThread())
        return ZG_ERROR_WRONG_THREAD;
    zg_result fileSystemResult;
    auto* files = fileSystem(*context, stringFromUTF8(scheme), fileSystemResult);
    if (!files)
        return fileSystemResult;
    std::span bytes { static_cast<const uint8_t*>(data), length };
    auto filePath = stringFromUTF8(path);
    auto type = stringFromUTF8(mimeType);
    auto result = release
        ? files->addFile(filePath, bytes, type, [release, user] { release(user); })
        : files->addFile(filePath, bytes, type);
    switch (result) {
    case MemoryFS::AddResult::Added:
        return ZG_OK;
    case MemoryFS::AddResult::InvalidPath:
        return fail(ZG_ERROR_INVALID_ARGUMENT, makeString("invalid path (absolute, '/' separated, no '.' or '..' segments): "_s, filePath));
    case MemoryFS::AddResult::AlreadyExists:
        return fail(ZG_ERROR_ALREADY_EXISTS, makeString("file already exists: "_s, filePath));
    }
    return ZG_ERROR_INTERNAL;
}

zg_result zg_context_set_fallback(zg_context* context, const char* scheme, const char* path)
{
    clearError();
    if (!context || !scheme)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no context or scheme"_s);
    if (!checkThread())
        return ZG_ERROR_WRONG_THREAD;
    auto filePath = stringFromUTF8(path);
    if (path && (MemoryFS::normalizePath(filePath).isNull() || filePath.endsWith('/')))
        return fail(ZG_ERROR_INVALID_ARGUMENT, makeString("invalid fallback path: "_s, filePath));
    zg_result result;
    auto* files = fileSystem(*context, stringFromUTF8(scheme), result);
    if (!files)
        return result;
    files->setFallback(filePath);
    return ZG_OK;
}

// A request to the application's scheme handler. Owned by the application until
// it answers; the task is only touched on the runtime thread.
struct zg_scheme_request {
    RefPtr<SchemeTask> task;
    std::string url;
    std::string method;
    std::vector<std::pair<std::string, std::string>> headers; // names in lowercase
    Vector<std::pair<String, String>> responseHeaders;
};

namespace {

class SchemeHandlerAPI final : public SchemeHandler {
public:
    SchemeHandlerAPI(zg_scheme_fn handler, void* user)
        : m_handler(handler)
        , m_user(user)
    {
    }

private:
    void start(SchemeTask& task) final
    {
        auto* request = new zg_scheme_request;
        request->task = &task;
        auto& resourceRequest = task.request();
        request->url = toStdString(resourceRequest.url().string());
        request->method = toStdString(resourceRequest.httpMethod());
        for (auto& header : resourceRequest.httpHeaderFields())
            request->headers.emplace_back(toStdString(header.key.convertToASCIILowercase()), toStdString(header.value));
        m_handler(request, m_user);
    }

    zg_scheme_fn m_handler;
    void* m_user;
};

// Answers arrive from any thread; the task is used, and the request freed, on the
// runtime thread.
void answer(zg_scheme_request* request, Function<void(SchemeTask&)>&& reply)
{
    ensureOnMainThread([request, reply = WTF::move(reply)] {
        if (!request->task->isStopped())
            reply(*request->task);
        delete request;
    });
}

} // anonymous namespace

const char* zg_scheme_request_url(const zg_scheme_request* request, size_t* length)
{
    if (length)
        *length = request ? request->url.size() : 0;
    return request ? request->url.c_str() : nullptr;
}

const char* zg_scheme_request_method(const zg_scheme_request* request)
{
    return request ? request->method.c_str() : nullptr;
}

const char* zg_scheme_request_header(const zg_scheme_request* request, const char* name, size_t* length)
{
    if (length)
        *length = 0;
    if (!request || !name)
        return nullptr;
    std::string lowercase = name;
    std::transform(lowercase.begin(), lowercase.end(), lowercase.begin(), [](char character) { return toASCIILower(character); });
    for (auto& [headerName, value] : request->headers) {
        if (headerName == lowercase) {
            if (length)
                *length = value.size();
            return value.c_str();
        }
    }
    return nullptr;
}

void zg_scheme_request_add_header(zg_scheme_request* request, const char* name, const char* value)
{
    if (!request || !name || !*name || !value)
        return;
    request->responseHeaders.append({ stringFromUTF8(name).isolatedCopy(), stringFromUTF8(value).isolatedCopy() });
}

void zg_scheme_request_respond(zg_scheme_request* request, int status, const char* mimeType, const void* body, size_t length)
{
    if (!request)
        return;
    Vector<uint8_t> bytes(std::span { static_cast<const uint8_t*>(body), body ? length : 0 });
    auto headers = WTF::move(request->responseHeaders);
    answer(request, [status, mimeType = stringFromUTF8(mimeType).isolatedCopy(), bytes = WTF::move(bytes), headers = WTF::move(headers)](SchemeTask& task) mutable {
        task.respond(status ? status : 200, mimeType, SharedBuffer::create(WTF::move(bytes)), headers);
    });
}

void zg_scheme_request_fail(zg_scheme_request* request, const char* reason)
{
    if (!request)
        return;
    answer(request, [reason = stringFromUTF8(reason).isolatedCopy()](SchemeTask& task) {
        task.didFail(failedError(task.request().url(), reason.isEmpty() ? "The scheme handler failed the request"_s : reason));
    });
}

// ---- Downloads ----

const char* zg_download_url(const zg_download* download)
{
    return download ? download->url.c_str() : nullptr;
}

const char* zg_download_suggested_filename(const zg_download* download)
{
    return download ? download->suggestedFilename.c_str() : nullptr;
}

const char* zg_download_mime_type(const zg_download* download)
{
    return download ? download->mimeType.c_str() : nullptr;
}

int64_t zg_download_expected_length(const zg_download* download)
{
    return download ? download->download->expectedLength() : -1;
}

uint64_t zg_download_received_length(const zg_download* download)
{
    return download ? download->download->receivedLength() : 0;
}

zg_result zg_download_set_destination(zg_download* download, const char* path)
{
    clearError();
    if (!download || !path || !*path)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no download or path"_s);
    if (!checkThread())
        return ZG_ERROR_WRONG_THREAD;
    download->download->setDestination(stringFromUTF8(path));
    return ZG_OK;
}

void zg_download_cancel(zg_download* download)
{
    if (download && checkThread())
        Ref { download->download }->cancel();
}

// ---- HTTP authentication ----

const char* zg_auth_challenge_url(const zg_auth_challenge* challenge)
{
    return challenge ? challenge->url.c_str() : nullptr;
}

const char* zg_auth_challenge_realm(const zg_auth_challenge* challenge)
{
    return challenge ? challenge->realm.c_str() : nullptr;
}

const char* zg_auth_challenge_scheme(const zg_auth_challenge* challenge)
{
    return challenge ? challenge->scheme : nullptr;
}

unsigned zg_auth_challenge_failures(const zg_auth_challenge* challenge)
{
    return challenge ? challenge->failures : 0;
}

void zg_auth_challenge_respond(zg_auth_challenge* challenge, const char* user, const char* password)
{
    if (!challenge)
        return;
    ensureOnMainThread([challenge, user = stringFromUTF8(user).isolatedCopy(), password = stringFromUTF8(password).isolatedCopy()] {
        challenge->completionHandler(Credential { user, password, CredentialPersistence::ForSession });
        delete challenge;
    });
}

void zg_auth_challenge_cancel(zg_auth_challenge* challenge)
{
    if (!challenge)
        return;
    ensureOnMainThread([challenge] {
        challenge->completionHandler(std::nullopt);
        delete challenge;
    });
}

// ---- File chooser ----

int zg_file_chooser_allows_multiple(const zg_file_chooser* chooser)
{
    return chooser && chooser->allowsMultiple;
}

int zg_file_chooser_allows_directories(const zg_file_chooser* chooser)
{
    return chooser && chooser->allowsDirectories;
}

size_t zg_file_chooser_accept_count(const zg_file_chooser* chooser)
{
    return chooser ? chooser->accept.size() : 0;
}

const char* zg_file_chooser_accept(const zg_file_chooser* chooser, size_t index)
{
    return chooser && index < chooser->accept.size() ? chooser->accept[index].c_str() : nullptr;
}

void zg_file_chooser_choose(zg_file_chooser* chooser, size_t count, const char* const* paths)
{
    if (!chooser)
        return;
    Vector<String> chosen;
    for (size_t i = 0; paths && i < count; ++i) {
        if (paths[i])
            chosen.append(String::fromUTF8(paths[i]).isolatedCopy());
    }
    ensureOnMainThread([chooser, chosen = WTF::move(chosen)] mutable {
        chooser->completionHandler(chosen.isEmpty() ? std::nullopt : std::optional { WTF::move(chosen) });
        delete chooser;
    });
}

void zg_file_chooser_cancel(zg_file_chooser* chooser)
{
    zg_file_chooser_choose(chooser, 0, nullptr);
}

// ---- Context menus ----

void zg_context_menu_position(const zg_context_menu* menu, float* x, float* y)
{
    if (x)
        *x = menu ? menu->x : 0;
    if (y)
        *y = menu ? menu->y : 0;
}

static const char* nullIfEmpty(const std::string& string)
{
    return string.empty() ? nullptr : string.c_str();
}

const char* zg_context_menu_link_url(const zg_context_menu* menu) { return menu ? nullIfEmpty(menu->linkURL) : nullptr; }
const char* zg_context_menu_link_text(const zg_context_menu* menu) { return menu ? nullIfEmpty(menu->linkText) : nullptr; }
const char* zg_context_menu_image_url(const zg_context_menu* menu) { return menu ? nullIfEmpty(menu->imageURL) : nullptr; }
const char* zg_context_menu_media_url(const zg_context_menu* menu) { return menu ? nullIfEmpty(menu->mediaURL) : nullptr; }
const char* zg_context_menu_selected_text(const zg_context_menu* menu) { return menu ? nullIfEmpty(menu->selectedText) : nullptr; }
const char* zg_context_menu_frame_url(const zg_context_menu* menu) { return menu ? nullIfEmpty(menu->frameURL) : nullptr; }

int zg_context_menu_is_editable(const zg_context_menu* menu)
{
    return menu && menu->isEditable;
}

size_t zg_context_menu_item_count(const zg_context_menu* menu)
{
    return menu ? menu->items.size() : 0;
}

const zg_context_menu_item* zg_context_menu_item_at(const zg_context_menu* menu, size_t index)
{
    return menu && index < menu->items.size() ? &menu->items[index] : nullptr;
}

void zg_context_menu_select(zg_context_menu* menu, unsigned id)
{
    if (!menu)
        return;
    ensureOnMainThread([menu, id] {
        menu->completionHandler(id ? std::optional<unsigned> { id } : std::nullopt);
        delete menu;
    });
}

void zg_context_menu_cancel(zg_context_menu* menu)
{
    zg_context_menu_select(menu, 0);
}

// ---- Clipboard ----

namespace {

// The application's clipboard, through its callbacks.
class HostClipboard final : public PasteboardItemsZeroG {
public:
    static Ref<HostClipboard> create(const zg_clipboard& clipboard) { return adoptRef(*new HostClipboard(clipboard)); }

    Vector<String> types() final
    {
        // The application says which it holds of those a page can use.
        static constexpr std::array<ASCIILiteral, 9> candidates = { "text/plain"_s, "text/html"_s, "text/uri-list"_s,
            "image/png"_s, "image/jpeg"_s, "image/gif"_s, "image/webp"_s, "image/bmp"_s, customPasteboardDataTypeZeroG };
        Vector<String> types;
        if (!m_clipboard.has)
            return types;
        for (auto type : candidates) {
            if (m_clipboard.has(type.characters(), m_clipboard.user))
                types.append(type);
        }
        return types;
    }

    RefPtr<SharedBuffer> read(const String& type) final
    {
        if (!m_clipboard.read)
            return nullptr;
        zg_clipboard_reply reply;
        auto utf8 = type.utf8();
        m_clipboard.read(byteCast<char>(utf8.data()), &reply, m_clipboard.user);
        return WTF::move(reply.data);
    }

    void write(Vector<Item>&& items) final
    {
        ++m_changeCount;
        if (!m_clipboard.write)
            return;
        Vector<std::string> types;
        Vector<const char*> typePointers;
        Vector<const void*> data;
        Vector<size_t> lengths;
        for (auto& [type, buffer] : items)
            types.append(utf8String(type));
        for (size_t i = 0; i < items.size(); ++i) {
            typePointers.append(types[i].c_str());
            data.append(items[i].second->span().data());
            lengths.append(items[i].second->size());
        }
        m_clipboard.write(items.size(), typePointers.span().data(), data.span().data(), lengths.span().data(), m_clipboard.user);
    }

    int64_t changeCount() final { return m_changeCount; }

private:
    explicit HostClipboard(const zg_clipboard& clipboard)
        : m_clipboard(clipboard)
    {
    }

    zg_clipboard m_clipboard;
    int64_t m_changeCount { 0 };
};

} // namespace

zg_result zg_runtime_add_ca_certificates(zg_runtime* rt, const void* pem, size_t length)
{
    clearError();
    if (!rt || !pem || !length)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no certificates"_s);
    if (!addTrustedCertificates(std::span { static_cast<const uint8_t*>(pem), length }))
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no PEM certificate could be read"_s);
    return ZG_OK;
}

void zg_clipboard_reply_data(zg_clipboard_reply* reply, const void* data, size_t length)
{
    if (reply && data)
        reply->data = SharedBuffer::create(std::span { static_cast<const uint8_t*>(data), length });
}

zg_result zg_runtime_set_clipboard(zg_runtime* rt, const zg_clipboard* clipboard)
{
    clearError();
    if (!rt)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no runtime"_s);
    if (!checkThread())
        return ZG_ERROR_WRONG_THREAD;
    if (!clipboard) {
        setApplicationClipboard(nullptr);
        return ZG_OK;
    }
    setApplicationClipboard(HostClipboard::create(copyConfig(clipboard)));
    return ZG_OK;
}

zg_result zg_context_register_scheme(zg_context* context, const char* scheme, zg_scheme_fn handler, void* user)
{
    clearError();
    if (!context || !scheme || !handler)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no context, scheme or handler"_s);
    if (!checkThread())
        return ZG_ERROR_WRONG_THREAD;
    auto name = stringFromUTF8(scheme);
    if (context->fileSystems.contains(name))
        return fail(ZG_ERROR_ALREADY_EXISTS, makeString("scheme serves files: "_s, name));
    return registrationFailure(context->session->registerScheme(name, adoptRef(*new SchemeHandlerAPI(handler, user))), name);
}

zg_result zg_context_register_font(zg_context* context, const void* data, size_t length)
{
    clearError();
    if (!context || !data || !length)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no context or font data"_s);
    if (!checkThread())
        return ZG_ERROR_WRONG_THREAD;
    auto family = context->session->registerFont(std::span { static_cast<const uint8_t*>(data), length });
    if (!family)
        return fail(ZG_ERROR_INVALID_ARGUMENT, family.error());
    return ZG_OK;
}

// ---- Surfaces ----

zg_result zg_surface_acquire(zg_surface* surface, zg_frame* out)
{
    clearError();
    if (!surface || !surface->surface || !out)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no surface or frame"_s);
    ZeroG::Frame frame;
    if (!surface->surface->acquire(frame))
        return fail(ZG_ERROR_NOT_FOUND, "no new frame"_s);
    *out = { };
    out->width = frame.width;
    out->height = frame.height;
    out->pixels = frame.pixels;
    out->stride = frame.stride;
    out->texture = frame.texture;
    out->fence = frame.fence;
    out->fence_value = frame.fenceValue;
    out->shared_handle = reinterpret_cast<zg_native_handle>(frame.sharedHandle);
    out->dirty_x = frame.dirty.x();
    out->dirty_y = frame.dirty.y();
    out->dirty_w = frame.dirty.width();
    out->dirty_h = frame.dirty.height();
    out->internal = frame.handle;
    return ZG_OK;
}

void zg_surface_release(zg_surface* surface, const zg_frame* frame)
{
    zg_surface_release_with_fence(surface, frame, -1);
}

void zg_surface_release_with_fence(zg_surface* surface, const zg_frame* frame, int releaseFence)
{
    if (!surface || !surface->surface) {
        // Nothing to give it to: the descriptor is closed all the same.
#if !OS(WINDOWS)
        if (releaseFence >= 0)
            close(releaseFence);
#endif
        return;
    }
    // Without a frame the surface closes the descriptor.
    ZeroG::Frame internal;
    if (frame)
        internal.handle = frame->internal;
    surface->surface->release(internal, releaseFence);
}

zg_result zg_surface_dmabuf(zg_surface* surface, const zg_frame* frame, zg_dmabuf* out)
{
    clearError();
    if (!surface || !surface->surface || !frame || !frame->internal || !out)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no surface, frame or output"_s);
    ZeroG::Frame internal;
    internal.handle = frame->internal;
    ZeroG::DMABufFrame dmabuf;
    if (!surface->surface->dmabuf(internal, dmabuf))
        return fail(ZG_ERROR_UNSUPPORTED, "frames of this surface are not dma-bufs"_s);
    zg_dmabuf result { };
    result.size = sizeof(zg_dmabuf);
    result.fourcc = dmabuf.fourcc;
    result.modifier = dmabuf.modifier;
    result.plane_count = dmabuf.planeCount;
    for (unsigned i = 0; i < 4; ++i) {
        result.fd[i] = i < dmabuf.planeCount ? dmabuf.fds[i] : -1;
        result.offset[i] = dmabuf.offsets[i];
        result.stride[i] = dmabuf.strides[i];
    }
    result.ready_fence_fd = dmabuf.readyFence;
    result.buffer_id = dmabuf.bufferID;
    // As much as the caller's struct holds (an older one is shorter).
    size_t size = std::min(out->size ? out->size : sizeof(zg_dmabuf), sizeof(zg_dmabuf));
    memcpy(out, &result, size);
    out->size = size;
    return ZG_OK;
}

void* zg_surface_device(zg_surface* surface)
{
    return surface && surface->surface ? surface->surface->device() : nullptr;
}

// ---- Views ----

static zg_view* viewOrFail(zg_view* view)
{
    if (!view) {
        fail(ZG_ERROR_INVALID_ARGUMENT, "no view"_s);
        return nullptr;
    }
    return checkThread() ? view : nullptr;
}

static void attachView(zg_view& view)
{
    view.client = std::make_unique<ViewClientAPI>(view);
    view.view->setClient(view.client.get());
    view.callbacks.size = sizeof(view.callbacks);
}

zg_view* zg_view_create(zg_context* context, const zg_view_config* configuration)
{
    clearError();
    if (!context) {
        fail(ZG_ERROR_INVALID_ARGUMENT, "no context"_s);
        return nullptr;
    }
    if (!checkThread())
        return nullptr;
    auto config = copyConfig(configuration);
    auto surfaceConfig = copyConfig(config.surface);
    unsigned buffers = surfaceConfig.buffer_count;

    std::unique_ptr<Surface> surface;
    switch (surfaceConfig.kind) {
    case ZG_SURFACE_CPU:
        surface = createCPUSurface(buffers);
        break;
#if OS(WINDOWS)
    case ZG_SURFACE_D3D11:
        if (!initializeGPU()) {
            fail(ZG_ERROR_GPU, "no GPU (ANGLE over D3D11)"_s);
            return nullptr;
        }
        surface = createD3D11Surface(surfaceConfig.device, buffers);
        if (!surface) {
            fail(ZG_ERROR_GPU, "cannot share D3D11 textures with the device (it must be on ANGLE's adapter)"_s);
            return nullptr;
        }
        break;
#else
    case ZG_SURFACE_GL:
        if (!surfaceConfig.device) {
            fail(ZG_ERROR_INVALID_ARGUMENT, "ZG_SURFACE_GL needs the application's EGLDisplay as its device"_s);
            return nullptr;
        }
        if (!initializeGPU(surfaceConfig.device, surfaceConfig.egl_platform, surfaceConfig.egl_native_display)) {
            fail(ZG_ERROR_GPU, "no GPU on the EGLDisplay (or the engine uses another display)"_s);
            return nullptr;
        }
        surface = createGLSurface(surfaceConfig.device, buffers);
        if (!surface) {
            fail(ZG_ERROR_GPU, "cannot create GL textures on the EGLDisplay"_s);
            return nullptr;
        }
        break;
    case ZG_SURFACE_VULKAN:
        // The engine's own display (a GL surface's, if one was made first).
        if (!initializeGPU()) {
            fail(ZG_ERROR_GPU, "no GPU"_s);
            return nullptr;
        }
        surface = createDMABufSurface(buffers);
        if (!surface) {
            fail(ZG_ERROR_UNSUPPORTED, "no dma-buf frames: the GPU driver exports none, and neither GBM nor a DRM device gives one it imports"_s);
            return nullptr;
        }
        break;
#endif
    default:
        fail(ZG_ERROR_UNSUPPORTED, "surface kind not supported on this platform"_s);
        return nullptr;
    }

    IntSize size { static_cast<int>(config.width ? config.width : 800), static_cast<int>(config.height ? config.height : 600) };
    auto* view = new zg_view;
    view->context = context;
    view->owned = View::create({ size, config.scale > 0 ? config.scale : 1, !!config.transparent, true, WTF::move(surface), context->session });
    view->view = view->owned.get();
    view->surface.surface = &view->view->surface();
    attachView(*view);
    context->views.push_back(view);
    return view;
}

void zg_view_destroy(zg_view* view)
{
    if (!view || !checkThread() || view->window)
        return;
    auto& views = view->context->views;
    views.erase(std::remove(views.begin(), views.end(), view), views.end());
    // Its inspector's window goes with it, while the client can close it.
    if (auto* inspector = view->view->inspector())
        inspector->close();
    view->view->setClient(nullptr);
    detachBindings(*view);
    delete view;
}

zg_surface* zg_view_surface(zg_view* view)
{
    // A window's frames go to the window.
    return view && !view->window ? &view->surface : nullptr;
}

zg_result zg_view_load_url(zg_view* view, const char* url)
{
    clearError();
    if (!viewOrFail(view))
        return t_lastError.empty() ? ZG_ERROR_INVALID_ARGUMENT : ZG_ERROR_WRONG_THREAD;
    URL target { stringFromUTF8(url) };
    if (!url || !target.isValid())
        return fail(ZG_ERROR_INVALID_ARGUMENT, makeString("invalid URL: "_s, stringFromUTF8(url)));
    // The navigation policy applies the trust rules (allowNavigation).
    view->view->loadURL(target);
    return ZG_OK;
}

zg_result zg_view_load_html(zg_view* view, const char* html, size_t length, const char* baseURL)
{
    clearError();
    if (!viewOrFail(view))
        return t_lastError.empty() ? ZG_ERROR_INVALID_ARGUMENT : ZG_ERROR_WRONG_THREAD;
    if (!html)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no HTML"_s);
    URL base { stringFromUTF8(baseURL) };
    if (baseURL && !base.isValid())
        return fail(ZG_ERROR_INVALID_ARGUMENT, makeString("invalid base URL: "_s, stringFromUTF8(baseURL)));
    view->view->loadHTML(stringFromUTF8(html, length), base);
    return ZG_OK;
}

void zg_view_reload(zg_view* view)
{
    if (viewOrFail(view)) {
        if (RefPtr frame = mainFrame(*view))
            frame->loader().reload();
    }
}

void zg_view_stop(zg_view* view)
{
    if (viewOrFail(view)) {
        if (RefPtr frame = mainFrame(*view))
            frame->loader().stopAllLoaders();
    }
}

void zg_view_resize(zg_view* view, unsigned width, unsigned height)
{
    if (viewOrFail(view) && width && height)
        view->view->resize({ static_cast<int>(width), static_cast<int>(height) });
}

void zg_view_set_scale(zg_view* view, float scale)
{
    if (viewOrFail(view) && scale > 0)
        view->view->setScale(scale);
}

void zg_view_set_focus(zg_view* view, int focused)
{
    if (viewOrFail(view))
        view->view->setFocused(focused);
}

zg_result zg_view_render(zg_view* view, double frameTimeMilliseconds)
{
    clearError();
    if (!viewOrFail(view))
        return t_lastError.empty() ? ZG_ERROR_INVALID_ARGUMENT : ZG_ERROR_WRONG_THREAD;
    if (view->window)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "a window renders its view itself"_s);
    view->view->render(MonotonicTime::fromRawSeconds(frameTimeMilliseconds / 1000));
    return ZG_OK;
}

zg_result zg_view_set_frame_rate(zg_view* view, double framesPerSecond)
{
    clearError();
    if (!viewOrFail(view))
        return t_lastError.empty() ? ZG_ERROR_INVALID_ARGUMENT : ZG_ERROR_WRONG_THREAD;
    if (view->window)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "a window follows its display's refresh rate"_s);
    if (!(framesPerSecond >= 0) || !std::isfinite(framesPerSecond))
        return fail(ZG_ERROR_INVALID_ARGUMENT, "the frame rate must not be negative"_s);
    if (auto* compositor = view->view->compositor())
        compositor->setFrameInterval(framesPerSecond ? 1_s / framesPerSecond : 0_s);
    return ZG_OK;
}

void zg_view_set_callbacks(zg_view* view, const zg_view_callbacks* callbacks)
{
    if (viewOrFail(view))
        view->callbacks = copyConfig(callbacks);
}

// ---- Input ----

static View::MouseButton mouseButton(zg_mouse_button button)
{
    switch (button) {
    case ZG_MOUSE_LEFT:
        return View::MouseButton::Left;
    case ZG_MOUSE_MIDDLE:
        return View::MouseButton::Middle;
    case ZG_MOUSE_RIGHT:
        return View::MouseButton::Right;
    default:
        return View::MouseButton::None;
    }
}

void zg_view_mouse_event(zg_view* view, zg_mouse_event_type type, float x, float y, zg_mouse_button button, unsigned clickCount, unsigned flags)
{
    if (!viewOrFail(view))
        return;
    FloatPoint point { x, y };
    switch (type) {
    case ZG_MOUSE_MOVE:
        view->view->mouseMove(point, mouseButton(button), modifiers(flags));
        break;
    case ZG_MOUSE_DOWN:
        view->view->mouseDown(point, mouseButton(button), std::max(1u, clickCount), modifiers(flags));
        break;
    case ZG_MOUSE_UP:
        view->view->mouseUp(point, mouseButton(button), std::max(1u, clickCount), modifiers(flags));
        break;
    }
}

void zg_view_scroll_event(zg_view* view, float x, float y, float deltaX, float deltaY, int precise, unsigned flags)
{
    if (!viewOrFail(view))
        return;
    float scale = precise ? 1 : 100; // one notch, 100 pixels
    // DOM signs in, WebCore's out: positive scrolls left and up.
    view->view->wheel({ x, y }, { -deltaX * scale, -deltaY * scale }, modifiers(flags));
}

void zg_view_key_event(zg_view* view, zg_key_event_type type, const char* key, const char* code, const char* text, unsigned flags, int isRepeat)
{
    if (!viewOrFail(view))
        return;
    if (type == ZG_KEY_CHAR) {
        view->view->insertText(stringFromUTF8(text), modifiers(flags));
        return;
    }
    View::KeyEvent event;
    event.key = stringFromUTF8(key);
    event.code = stringFromUTF8(code);
    event.windowsKeyCode = windowsKeyCodeForDOMCode(event.code);
    // Without a code, a single ASCII letter or digit key still has its key code.
    if (!event.windowsKeyCode && event.key.length() == 1 && isASCIIAlphanumeric(event.key[0]))
        event.windowsKeyCode = toASCIIUpper(event.key[0]);
    event.isAutoRepeat = isRepeat;
    event.isKeypad = event.code.startsWith("Numpad"_s);
    event.modifiers = modifiers(flags);
    if (type == ZG_KEY_DOWN)
        view->view->keyDown(event);
    else
        view->view->keyUp(event);
}

void zg_view_ime_set_composition(zg_view* view, const char* text, size_t length, unsigned cursorBegin, unsigned cursorEnd)
{
    if (!viewOrFail(view))
        return;
    RefPtr frame = view->view->page().focusController().focusedOrMainFrame();
    if (!frame)
        return;
    auto composition = stringFromUTF8(text, length);
    Vector<CompositionUnderline> underlines { CompositionUnderline(0, composition.length(), CompositionUnderlineColor::TextColor, Color::black, false) };
    frame->editor().setComposition(composition, underlines, { }, { }, cursorBegin, cursorEnd);
    if (view->callbacks.ime_caret_rect) {
        auto caret = frame->selection().absoluteCaretBounds();
        view->callbacks.ime_caret_rect(view, caret.x(), caret.y(), caret.width(), caret.height(), view->callbacks.user);
    }
}

void zg_view_ime_commit(zg_view* view, const char* text, size_t length)
{
    if (!viewOrFail(view))
        return;
    RefPtr frame = view->view->page().focusController().focusedOrMainFrame();
    if (!frame)
        return;
    auto committed = stringFromUTF8(text, length);
    if (frame->editor().hasComposition())
        frame->editor().confirmComposition(committed);
    else
        view->view->insertText(committed);
}

void zg_view_ime_cancel(zg_view* view)
{
    if (!viewOrFail(view))
        return;
    if (RefPtr frame = view->view->page().focusController().focusedOrMainFrame())
        frame->editor().cancelComposition();
}

int zg_view_drag(zg_view* view, zg_drag_event event, float x, float y, const zg_drag_data* data)
{
    if (!viewOrFail(view))
        return 0;
    View::DragItems items;
    if (data) {
        auto config = copyConfig(data);
        for (size_t i = 0; config.files && i < config.file_count; ++i) {
            if (config.files[i])
                items.files.append(String::fromUTF8(config.files[i]));
        }
        items.text = stringFromUTF8(config.text);
        items.url = stringFromUTF8(config.url);
        items.html = stringFromUTF8(config.html);
    }
    View::DragEvent dragEvent = View::DragEvent::Over;
    switch (event) {
    case ZG_DRAG_ENTER:
        dragEvent = View::DragEvent::Enter;
        break;
    case ZG_DRAG_OVER:
        dragEvent = View::DragEvent::Over;
        break;
    case ZG_DRAG_LEAVE:
        dragEvent = View::DragEvent::Leave;
        break;
    case ZG_DRAG_DROP:
        dragEvent = View::DragEvent::Drop;
        break;
    }
    return view->view->drag(dragEvent, { x, y }, items);
}

// ---- Web Inspector ----

zg_result zg_view_show_inspector(zg_view* view)
{
    clearError();
    if (!view)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no view"_s);
    if (!checkThread())
        return ZG_ERROR_WRONG_THREAD;
    if (!ViewInspector::isAvailable())
        return fail(ZG_ERROR_UNSUPPORTED, makeString("the Web Inspector is in the dev and debug profiles, not in "_s, ZEROG_PROFILE_NAME ""_s));
    if (!view->view->showInspector())
        return t_lastError.empty() ? fail(ZG_ERROR_GPU, "cannot open the inspector"_s) : ZG_ERROR_GPU;
    return ZG_OK;
}

// ---- Windows ----

zg_window* zg_window_create(zg_context* context, const zg_window_config* configuration)
{
    clearError();
    if (!context) {
        fail(ZG_ERROR_INVALID_ARGUMENT, "no context"_s);
        return nullptr;
    }
    if (!checkThread())
        return nullptr;
    auto config = copyConfig(configuration);
    IntSize size { static_cast<int>(config.width ? config.width : 1024), static_cast<int>(config.height ? config.height : 768) };
    auto window = Window::create({ stringFromUTF8(config.title), size, !config.fixed_size, !!config.frameless, true, context->session, !!config.transparent });
    if (!window) {
        fail(ZG_ERROR_GPU, makeString("cannot create the window: "_s, Window::lastError()));
        return nullptr;
    }
    if (config.fullscreen)
        window->setFullscreen(true);
    auto* result = new zg_window;
    result->context = context;
    result->window = WTF::move(window);
    result->view.context = context;
    result->view.window = result;
    result->view.view = &result->window->view();
    attachView(result->view);
    context->runtime->windows.push_back(result);
    return result;
}

void zg_window_destroy(zg_window* window)
{
    if (!window || !checkThread())
        return;
    auto& windows = window->context->runtime->windows;
    windows.erase(std::remove(windows.begin(), windows.end(), window), windows.end());
    if (auto* inspector = window->view.view->inspector())
        inspector->close();
    window->view.view->setClient(nullptr);
    detachBindings(window->view);
    delete window;
}

zg_view* zg_window_view(zg_window* window)
{
    return window ? &window->view : nullptr;
}

void zg_window_set_title(zg_window* window, const char* title)
{
    if (window && checkThread())
        window->window->setTitle(stringFromUTF8(title));
}

void zg_window_set_fullscreen(zg_window* window, int fullscreen)
{
    if (window && checkThread())
        window->window->setFullscreen(fullscreen);
}

void* zg_window_native_handle(zg_window* window)
{
    return window ? window->window->nativeHandle() : nullptr;
}

} // extern "C"
