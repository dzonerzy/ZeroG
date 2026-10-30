#include "config.h"
#include "inspector/InspectorZeroG.h"

#include <WebCore/InspectorBackendClient.h>

#if ZEROG_INSPECTOR

#include "View.h"
#include "inspector/LegacyWebPageInspectorController.h"
#include "loader/MemoryFS.h"
#include <JavaScriptCore/InspectorFrontendChannel.h>
#include <WebCore/InspectorFrontendClientLocal.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/Page.h>
#include <WebCore/PageInspectorController.h>
#include <WebCore/Settings.h>
#include <wtf/HashSet.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>

#if ENABLE(REMOTE_INSPECTOR)
#include "inspector/LegacyWebPageDebuggable.h"
#include <JavaScriptCore/RemoteInspector.h>
#include <JavaScriptCore/RemoteInspectorServer.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/ResourceResponse.h>
#include <filesystem>
#include <wtf/FileSystem.h>
#endif

// WebInspectorUI's resources, packed by tools/assets.py at build time.
extern "C" {
extern const unsigned char zerog_inspector_resources[];
extern const size_t zerog_inspector_resources_len;
}

namespace ZeroG {

using namespace WebCore;

namespace {

constexpr auto frontendURL = "inspector://localhost/Main.html"_s;

class ViewInspectorImpl;

// The frontend page's side: InspectorFrontendHost calls into it.
class FrontendClient final : public InspectorFrontendClientLocal {
public:
    FrontendClient(ViewInspectorImpl&, PageInspectorController& inspected, Page& frontendPage);

private:
    // Commands go to the multi-target controller, not to the page's own.
    void sendMessageToBackend(const String&) final;
    void frontendLoaded() final;
    void closeWindow() final;
    void bringToFront() final;
    void reopen() final;

    String localizedStringsURL() const final { return "inspector://localhost/Localizations/en.lproj/localizedStrings.js"_s; }
    Inspector::DebuggableType debuggableType() const final { return Inspector::DebuggableType::WebPage; }
    String targetPlatformName() const final
    {
#if OS(WINDOWS)
        return "Windows"_s;
#elif OS(LINUX)
        return "Linux"_s;
#else
        return "Unknown"_s;
#endif
    }
    String targetBuildVersion() const final { return "Unknown"_s; }
    String targetProductVersion() const final { return "Unknown"_s; }
    bool targetIsSimulator() const final { return false; }

    // Always a window of its own: no docking.
    void setForcedAppearance(Appearance) final { }
    bool supportsDockSide(DockSide side) final { return side == DockSide::Undocked; }
    void attachWindow(DockSide) final { }
    void detachWindow() final { }
    void setAttachedWindowHeight(unsigned) final { }
    void setAttachedWindowWidth(unsigned) final { }
    void setSheetRect(const FloatRect&) final { }
    void inspectedURLChanged(const String&) final { }
    void showCertificate(const CertificateInfo&) final { }

    ViewInspectorImpl& m_inspector;
};

class ViewInspectorImpl final : public ViewInspector, public Inspector::FrontendChannel {
public:
    // For tasks that may run after an inspector is gone.
    static HashSet<ViewInspectorImpl*>& live()
    {
        static NeverDestroyed<HashSet<ViewInspectorImpl*>> inspectors;
        return inspectors;
    }

    explicit ViewInspectorImpl(View& view)
        : m_view(view)
        , m_controller(LegacyWebPageInspectorController::create(view.page()))
    {
        live().add(this);
#if ENABLE(REMOTE_INSPECTOR)
        m_debuggable = LegacyWebPageDebuggable::create(m_controller, view.page());
        m_debuggable->init();
        m_debuggable->setInspectable(true);
#endif
    }

    ~ViewInspectorImpl()
    {
        live().remove(this);
        close();
#if ENABLE(REMOTE_INSPECTOR)
        m_debuggable->detachFromPage();
        m_debuggable = nullptr;
#endif
        m_controller->willDestroyPage(m_view.page());
    }

    LegacyWebPageInspectorController& controller() { return m_controller; }

    bool show() final
    {
        if (m_frontendView) {
            m_view.client().bringInspectorViewToFront(*m_frontendView);
            return true;
        }
        auto* frontend = m_view.client().createInspectorView(inspectorSession());
        if (!frontend)
            return false;
        m_frontendView = frontend;
        // The embedder may destroy it (its window closed): the inspector closes then.
        frontend->setWillDestroyCallback([this] {
            disconnect();
            m_frontendView = nullptr;
        });
        Ref frontendPage = frontend->page();
        m_frontendClient = std::make_unique<FrontendClient>(*this, protect(m_view.page())->inspectorController(), frontendPage);
        frontendPage->inspectorController().setInspectorFrontendClient(m_frontendClient.get());
        m_controller->connectFrontend(*this);
        frontend->loadURL(URL { frontendURL });
        return true;
    }

    void close() final
    {
        auto* frontend = std::exchange(m_frontendView, nullptr);
        if (!frontend)
            return;
        frontend->setWillDestroyCallback(nullptr);
        disconnect(*frontend);
        m_view.client().closeInspectorView(*frontend);
    }

    // From the frontend's own script (InspectorFrontendHost.closeWindow()): its
    // view goes once that script has returned.
    void closeLater()
    {
        callOnMainThread([inspector = this] {
            if (live().contains(inspector))
                inspector->close();
        });
    }

    View* frontendView() const final { return m_frontendView; }

    void paintHighlight(GraphicsContext& context) final
    {
        auto& controller = m_view.page().inspectorController();
        if (controller.shouldShowOverlay())
            controller.drawHighlight(context);
    }

    void pageChanged() final
    {
#if ENABLE(REMOTE_INSPECTOR)
        m_debuggable->update();
#endif
    }

    void bringToFront()
    {
        if (m_frontendView)
            m_view.client().bringInspectorViewToFront(*m_frontendView);
    }

private:
    // Inspector::FrontendChannel: the controller's messages for the frontend.
    ConnectionType connectionType() const final { return ConnectionType::Local; }
    void sendMessageToFrontend(const String& message) final
    {
        if (m_frontendClient)
            protect(m_frontendClient->frontendAPIDispatcher())->dispatchMessageAsync(message);
    }

    void disconnect()
    {
        if (m_frontendView)
            disconnect(*m_frontendView);
    }

    void disconnect(View& frontend)
    {
        if (!m_frontendClient)
            return;
        protect(frontend.page())->inspectorController().setInspectorFrontendClient(nullptr);
        m_controller->disconnectFrontend(*this);
        m_frontendClient = nullptr;
    }

    View& m_view;
    const Ref<LegacyWebPageInspectorController> m_controller;
#if ENABLE(REMOTE_INSPECTOR)
    RefPtr<LegacyWebPageDebuggable> m_debuggable;
#endif
    View* m_frontendView { nullptr };
    std::unique_ptr<FrontendClient> m_frontendClient;
};

FrontendClient::FrontendClient(ViewInspectorImpl& inspector, PageInspectorController& inspected, Page& frontendPage)
    : InspectorFrontendClientLocal(&inspected, &frontendPage, makeUnique<InspectorFrontendClientLocal::Settings>())
    , m_inspector(inspector)
{
}

void FrontendClient::sendMessageToBackend(const String& message)
{
    m_inspector.controller().dispatchMessageFromFrontend(message);
}

void FrontendClient::frontendLoaded()
{
    InspectorFrontendClientLocal::frontendLoaded();
    setAttachedWindow(DockSide::Undocked);
}

void FrontendClient::closeWindow()
{
    m_inspector.closeLater();
}

void FrontendClient::bringToFront()
{
    m_inspector.bringToFront();
}

void FrontendClient::reopen()
{
    m_inspector.closeLater();
}

// The inspected page's side: WebCore asks it to open the frontend and to show
// the highlight. Owned by the page, which can outlive its view briefly.
class BackendClient final : public InspectorBackendClient {
public:
    explicit BackendClient(View& view)
        : m_view(&view)
    {
    }

    void detach() { m_view = nullptr; }

private:
    ViewInspector* inspector() const { return m_view ? m_view->inspector() : nullptr; }

    void inspectedPageDestroyed() final { }
    // The frontend connects to the multi-target controller (ViewInspectorImpl::show),
    // not to the page's controller: no channel for WebCore to connect here.
    Inspector::FrontendChannel* openLocalFrontend(PageInspectorController*) final
    {
        if (auto* inspector = this->inspector())
            inspector->show();
        return nullptr;
    }
    void bringFrontendToFront() final
    {
        if (auto* inspector = this->inspector())
            inspector->show();
    }
    void highlight() final { repaint(); }
    void hideHighlight() final { repaint(); }
    void repaint()
    {
        if (m_view)
            m_view->invalidate({ { }, m_view->size() });
    }

    View* m_view;
};

} // namespace

bool ViewInspector::isAvailable()
{
    return true;
}

std::unique_ptr<InspectorBackendClient> ViewInspector::createBackendClient(View& view)
{
    return std::make_unique<BackendClient>(view);
}

std::unique_ptr<ViewInspector> ViewInspector::create(View& view)
{
    return std::make_unique<ViewInspectorImpl>(view);
}

void ViewInspector::detachBackendClient(InspectorBackendClient& client)
{
    static_cast<BackendClient&>(client).detach();
}

Session& inspectorSession()
{
    static NeverDestroyed<Ref<Session>> session = [] {
        Ref session = Session::create();
        Ref files = MemoryFS::create();
        auto mounted = files->mountArchive("/"_s, std::span { zerog_inspector_resources, zerog_inspector_resources_len });
        RELEASE_ASSERT(mounted);
        session->registerScheme("inspector"_s, WTF::move(files));
        return session;
    }();
    return session.get();
}

#if ENABLE(REMOTE_INSPECTOR)
// The protocol the backend speaks, for remote frontends (sent on connection).
// The remote inspector reads it from a file: the compiled-in copy is written to
// the temporary directory once, under a name that is the same on every run.
static void provideBackendCommands()
{
    Ref files = MemoryFS::create();
    if (!files->mountArchive("/"_s, std::span { zerog_inspector_resources, zerog_inspector_resources_len }))
        return;
    ResourceResponse response;
    Vector<uint8_t> commands;
    auto& handler = static_cast<SchemeHandler&>(files.get());
    if (!handler.loadSynchronously(ResourceRequest { URL { "inspector://localhost/Protocol/InspectorBackendCommands.js"_s } }, response, commands) || commands.isEmpty())
        return;
    std::error_code error;
    auto directory = std::filesystem::temp_directory_path(error);
    if (error)
        return;
    auto utf8Path = (directory / "zerog-inspector-backend-commands.js").u8string();
    auto path = String::fromUTF8(std::span<const char8_t> { utf8Path.data(), utf8Path.size() });
    auto existing = FileSystem::readEntireFile(path);
    if (!existing || existing->span().size() != commands.size() || !equalSpans(existing->span(), commands.span())) {
        if (!FileSystem::overwriteEntireFile(path, commands.span()))
            return;
    }
    Inspector::RemoteInspector::singleton().setBackendCommandsPath(path);
}
#endif

bool startRemoteInspectorServer(const String& address, uint16_t port)
{
#if ENABLE(REMOTE_INSPECTOR)
    provideBackendCommands();
    auto utf8 = address.utf8();
    return Inspector::RemoteInspectorServer::singleton().start(UTF8CStringView::unsafeFromUTF8(byteCast<char>(utf8.data())), port);
#else
    UNUSED_PARAM(address);
    UNUSED_PARAM(port);
    return false;
#endif
}

} // namespace ZeroG

#else // !ZEROG_INSPECTOR

namespace ZeroG {

bool ViewInspector::isAvailable()
{
    return false;
}

std::unique_ptr<WebCore::InspectorBackendClient> ViewInspector::createBackendClient(View&)
{
    return nullptr;
}

std::unique_ptr<ViewInspector> ViewInspector::create(View&)
{
    return nullptr;
}

void ViewInspector::detachBackendClient(WebCore::InspectorBackendClient&)
{
}

bool startRemoteInspectorServer(const String&, uint16_t)
{
    return false;
}

} // namespace ZeroG

#endif // ZEROG_INSPECTOR
