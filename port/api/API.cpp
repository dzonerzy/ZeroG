// The C API (include/zerog.h): runtime, contexts, views, surfaces, input,
// callbacks and windows, over the embedding layer. Bindings and values are in
// Bindings.cpp.

#include "config.h"
#include "APIInternal.h"

#include "Embedding.h"
#include "KeyboardSDL.h"
#include "Surface.h"
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

// The view's events, as zg_view_callbacks.
class ViewClientAPI final : public ViewClient {
public:
    explicit ViewClientAPI(zg_view& view)
        : m_view(view)
    {
    }

private:
    const zg_view_callbacks& callbacks() const { return m_view.callbacks; }

    void consoleMessage(JSC::MessageSource source, JSC::MessageLevel level, const String& message, unsigned line, unsigned column, const String& sourceURL) final
    {
        auto text = message.utf8();
        auto file = sourceURL.utf8();
        zg_log_message log { ZG_LOG_SOURCE_CONSOLE, logLevel(level), byteCast<char>(text.data()), text.length(), byteCast<char>(file.data()), file.length(), line, column, &m_view };
        if (callbacks().console)
            callbacks().console(&m_view, &log, callbacks().user);
        if (auto* rt = runtime(); rt && rt->config.log && log.level >= rt->config.log_level)
            rt->config.log(&log, rt->config.log_user);
        if (callbacks().js_error && source == JSC::MessageSource::JS && level == JSC::MessageLevel::Error) {
            zg_js_error error { log.message, log.message_len, nullptr, 0, log.file, log.file_len, line, column, message.contains("Unhandled Promise Rejection"_s) };
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

    zg_view& m_view;
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

    initializeProcess({ config.jit != ZG_JIT_DISABLED });
    s_runtimeCreated = true;
    s_runtime = new zg_runtime { config };

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
    while (!rt->windows.empty())
        zg_window_destroy(rt->windows.back());
    while (!rt->contexts.empty())
        zg_context_destroy(rt->contexts.back());
    // The wake-up observers outlive the runtime: the handle stays open, as the
    // runtime cannot be created again.
    s_runtime = nullptr;
    delete rt;
}

zg_result zg_runtime_run(zg_runtime* rt)
{
    clearError();
    if (!rt)
        return fail(ZG_ERROR_INVALID_ARGUMENT, "no runtime"_s);
    if (!checkThread())
        return ZG_ERROR_WRONG_THREAD;
    rt->quit = false;
    while (!rt->quit) {
        std::vector<zg_window*> open;
        for (auto* window : rt->windows) {
            if (!window->window->isClosed())
                open.push_back(window);
        }
        if (open.empty())
            break;
        // One window waits for its events; several take turns without blocking.
        for (auto* window : open)
            window->window->runOnce(open.size() == 1 ? std::nullopt : std::optional { 1_ms });
    }
    rt->quit = false;
    return ZG_OK;
}

void zg_runtime_quit(zg_runtime* rt)
{
    if (rt)
        rt->quit = true;
}

void zg_runtime_update(zg_runtime* rt)
{
    if (!rt || !checkThread())
        return;
#if !OS(WINDOWS)
    uint64_t count;
    while (read(static_cast<int>(rt->wakeUpHandle), &count, sizeof(count)) > 0) { }
#endif
    runPendingWork();
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

    // Cookies persist in the data directory unless the context is ephemeral or
    // has none (no data_dir and no app_id): then they live in memory.
    String cookieDatabase;
    if (!context->ephemeral && !context->dataDirectory.empty()) {
        auto directory = String::fromUTF8(context->dataDirectory.c_str());
        if (FileSystem::makeAllDirectories(directory))
            cookieDatabase = FileSystem::pathByAppendingComponent(directory, "Cookies.sqlite"_s);
    }
    context->session = Session::create(cookieDatabase);
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
    if (!surface || !surface->surface || !frame || !frame->internal)
        return;
    ZeroG::Frame internal;
    internal.handle = frame->internal;
    surface->surface->release(internal);
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
        if (!initializeGPU(surfaceConfig.device)) {
            fail(ZG_ERROR_GPU, "no GPU on the EGLDisplay (or the engine uses another display)"_s);
            return nullptr;
        }
        surface = createGLSurface(surfaceConfig.device, buffers);
        if (!surface) {
            fail(ZG_ERROR_GPU, "cannot create GL textures on the EGLDisplay"_s);
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
    view->view->setClient(nullptr);
    // Functions bound in pages still referenced elsewhere throw from now on.
    for (auto& binding : view->bindings)
        binding->view = nullptr;
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
    auto window = Window::create({ stringFromUTF8(config.title), size, !config.fixed_size, !!config.frameless, true, context->session });
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
    window->view.view->setClient(nullptr);
    for (auto& binding : window->view.bindings)
        binding->view = nullptr;
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
