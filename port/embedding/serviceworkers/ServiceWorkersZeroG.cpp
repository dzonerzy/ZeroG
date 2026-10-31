#include "config.h"
#include "serviceworkers/ServiceWorkersZeroG.h"

#include "loader/Loader.h"
#include "loader/WebSocketChannelZeroG.h"
#include <WebCore/BackgroundFetchInformation.h>
#include <WebCore/BackgroundFetchOptions.h>
#include <WebCore/BackgroundFetchRecordInformation.h>
#include <WebCore/BackgroundFetchRequest.h>
#include <WebCore/BackgroundFetchStore.h>
#include <WebCore/CacheStorageProvider.h>
#include <WebCore/CookieChangeSubscription.h>
#include <WebCore/CookieJar.h>
#include <WebCore/DatabaseProvider.h>
#include <WebCore/NotificationData.h>
#include <WebCore/NotificationPayload.h>
#include <WebCore/RetrieveRecordsOptions.h>
#include <WebCore/SocketProvider.h>
#include <WebCore/StorageNamespaceProvider.h>
#include <WebCore/UserContentProvider.h>
#include <WebCore/DocumentLoader.h>
#include <WebCore/EmptyClients.h>
#include <WebCore/EmptyFrameLoaderClient.h>
#include <WebCore/ExceptionData.h>
#include <WebCore/FetchLoader.h>
#include <WebCore/FetchLoaderClient.h>
#include <WebCore/FormData.h>
#include <WebCore/FrameLoader.h>
#include <WebCore/DocumentInlines.h>
#include <WebCore/DocumentPage.h>
#include <WebCore/FrameInlines.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/MessageWithMessagePorts.h>
#include <WebCore/NavigationPreloadState.h>
#include <WebCore/NetworkLoadMetrics.h>
#include <WebCore/Page.h>
#include <WebCore/PageConfiguration.h>
#include <WebCore/ProcessIdentifier.h>
#include <WebCore/PushPermissionState.h>
#include <WebCore/PushSubscriptionData.h>
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceLoader.h>
#include <WebCore/SWClientConnection.h>
#include <WebCore/SWContextManager.h>
#include <WebCore/SWOriginStore.h>
#include <WebCore/SWRegistrationDatabase.h>
#include <WebCore/SWRegistrationStore.h>
#include <WebCore/SWServer.h>
#include <WebCore/SWServerDelegate.h>
#include <WebCore/SWServerRegistration.h>
#include <WebCore/SWServerToContextConnection.h>
#include <WebCore/SWServerWorker.h>
#include <WebCore/ServiceWorkerClientData.h>
#include <WebCore/ServiceWorkerContextData.h>
#include <WebCore/ServiceWorkerFetch.h>
#include <WebCore/ServiceWorkerJobData.h>
#include <WebCore/ServiceWorkerProvider.h>
#include <WebCore/ServiceWorkerRegistrationData.h>
#include <WebCore/ServiceWorkerRegistrationKey.h>
#include <WebCore/ServiceWorkerRoute.h>
#include <WebCore/ServiceWorkerThreadProxy.h>
#include <WebCore/Settings.h>
#include <WebCore/SharedBuffer.h>
#include <WebCore/Timer.h>
#include <WebCore/UserAgent.h>
#include <WebCore/WorkerFetchResult.h>
#include <WebCore/WorkerScriptLoader.h>
#include <wtf/FileSystem.h>
#include <wtf/HashCountedSet.h>
#include <wtf/HashMap.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>
#include <wtf/WorkQueue.h>

namespace ZeroG {

using namespace WebCore;

namespace {

// Every call from one side to the other: later, on the main run loop, as an
// IPC message would arrive.
void post(Function<void()>&& task)
{
    RunLoop::mainSingleton().dispatch(WTF::move(task));
}

class ClientConnection;
class ServerConnection;
class Workers;

// ---- The context side's connection: the process's one, for every session ----

// The workers report to their server through it (SWContextManager calls it on
// the main thread); it finds the server by the worker.
class ContextManagerConnection final : public SWContextManager::Connection, public RefCounted<ContextManagerConnection> {
public:
    static void ensure()
    {
        static bool installed;
        if (installed)
            return;
        installed = true;
        SWContextManager::singleton().setConnection(adoptRef(*new ContextManagerConnection));
    }

    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

private:
    static SWServerToContextConnection* contextConnection(ServiceWorkerIdentifier identifier)
    {
        RefPtr worker = SWServerWorker::existingWorkerForIdentifier(identifier);
        return worker ? worker->contextConnection() : nullptr;
    }

    void establishConnection(CompletionHandler<void()>&& completionHandler) final { completionHandler(); }

    void postMessageToServiceWorkerClient(const ScriptExecutionContextIdentifier& destination, const MessageWithMessagePorts& message, ServiceWorkerIdentifier source, const SecurityOriginData& sourceOrigin) final;

    void serviceWorkerStarted(std::optional<ServiceWorkerJobDataIdentifier> job, ServiceWorkerIdentifier identifier, bool doesHandleFetch) final
    {
        if (RefPtr connection = contextConnection(identifier))
            connection->scriptContextStarted(job, identifier, doesHandleFetch);
    }
    void serviceWorkerFailedToStart(std::optional<ServiceWorkerJobDataIdentifier> job, ServiceWorkerIdentifier identifier, const String& message) final
    {
        if (RefPtr connection = contextConnection(identifier))
            connection->scriptContextFailedToStart(job, identifier, message);
    }
    void didFinishInstall(std::optional<ServiceWorkerJobDataIdentifier> job, ServiceWorkerIdentifier identifier, bool wasSuccessful) final
    {
        RefPtr worker = SWServerWorker::existingWorkerForIdentifier(identifier);
        if (!worker || worker->state() == ServiceWorkerState::Redundant)
            return;
        if (RefPtr connection = worker->contextConnection())
            connection->didFinishInstall(job, identifier, wasSuccessful);
    }
    void didFinishActivation(ServiceWorkerIdentifier identifier) final
    {
        RefPtr worker = SWServerWorker::existingWorkerForIdentifier(identifier);
        if (!worker || worker->state() == ServiceWorkerState::Redundant)
            return;
        if (RefPtr connection = worker->contextConnection())
            connection->didFinishActivation(identifier);
    }
    void setServiceWorkerHasPendingEvents(ServiceWorkerIdentifier identifier, bool hasPendingEvents) final
    {
        if (RefPtr connection = contextConnection(identifier))
            connection->setServiceWorkerHasPendingEvents(identifier, hasPendingEvents);
    }
    void workerTerminated(ServiceWorkerIdentifier identifier) final
    {
        if (RefPtr connection = contextConnection(identifier))
            connection->workerTerminated(identifier);
    }
    void skipWaiting(ServiceWorkerIdentifier identifier, CompletionHandler<void()>&& completionHandler) final
    {
        if (RefPtr worker = SWServerWorker::existingWorkerForIdentifier(identifier))
            worker->skipWaiting();
        completionHandler();
    }
    void setScriptResource(ServiceWorkerIdentifier identifier, const URL& url, const ServiceWorkerContextData::ImportedScript& script) final
    {
        if (RefPtr connection = contextConnection(identifier))
            connection->setScriptResource(identifier, URL { url }, ServiceWorkerContextData::ImportedScript { script });
    }
    void findClientByVisibleIdentifier(ServiceWorkerIdentifier identifier, const String& clientIdentifier, FindClientByIdentifierCallback&& callback) final
    {
        if (RefPtr connection = contextConnection(identifier))
            return connection->findClientByVisibleIdentifier(identifier, clientIdentifier, WTF::move(callback));
        callback(std::nullopt);
    }
    void matchAll(ServiceWorkerIdentifier identifier, const ServiceWorkerClientQueryOptions& options, ServiceWorkerClientsMatchAllCallback&& callback) final
    {
        if (RefPtr connection = contextConnection(identifier))
            return connection->matchAll(identifier, options, WTF::move(callback));
        callback({ });
    }
    void claim(ServiceWorkerIdentifier identifier, CompletionHandler<void(ExceptionOr<void>&&)>&& callback) final
    {
        RefPtr connection = contextConnection(identifier);
        if (!connection)
            return callback(Exception { ExceptionCode::InvalidStateError });
        connection->claim(identifier, [callback = WTF::move(callback)](auto&& error) mutable {
            callback(error ? error->toException() : ExceptionOr<void> { });
        });
    }
    // An application engine has no other windows to focus, open or navigate.
    void focus(ScriptExecutionContextIdentifier, CompletionHandler<void(std::optional<ServiceWorkerClientData>&&)>&& callback) final { callback(std::nullopt); }
    void openWindow(ServiceWorkerIdentifier, const URL&, OpenWindowCallback&& callback) final { callback(Exception { ExceptionCode::NotSupportedError, "clients.openWindow() is not supported"_s }); }
    void navigate(ScriptExecutionContextIdentifier, ServiceWorkerIdentifier, const URL&, NavigateCallback&& callback) final { callback(Exception { ExceptionCode::NotSupportedError, "WindowClient.navigate() is not supported"_s }); }

    void didFailHeartBeatCheck(ServiceWorkerIdentifier identifier) final
    {
        if (RefPtr connection = contextConnection(identifier))
            connection->didFailHeartBeatCheck(identifier);
    }
    void setAsInspected(ServiceWorkerIdentifier identifier, bool isInspected) final
    {
        if (RefPtr connection = contextConnection(identifier))
            connection->setAsInspected(identifier, isInspected);
    }
    bool isThrottleable() const final { return false; }
    PageIdentifier pageIdentifier() const final { return m_pageIdentifier; }
    void stop() final { }
    void reportConsoleMessage(ServiceWorkerIdentifier, MessageSource, MessageLevel, const String&, unsigned long) final { }
    void removeNavigationFetch(SWServerConnectionIdentifier, FetchIdentifier) final { }
    // Streamed request bodies (ReadableStream uploads) do not reach workers.
    void startPendingStreamUploadForwarding(PendingStreamState&) final { }
    void cancelPendingStreamUploadForwarding(PendingStreamState&) final { }

    const PageIdentifier m_pageIdentifier { PageIdentifier::generate() };
};

// ---- A service worker's page: its frame loader client keeps the session ----

HashMap<const LocalFrameLoaderClient*, Session*>& serviceWorkerFrameLoaderClients()
{
    static NeverDestroyed<HashMap<const LocalFrameLoaderClient*, Session*>> clients;
    return clients;
}

class ServiceWorkerFrameLoaderClient final : public EmptyFrameLoaderClient {
public:
    ServiceWorkerFrameLoaderClient(FrameLoader& frameLoader, Session& session, const String& userAgent)
        : EmptyFrameLoaderClient(frameLoader)
        , m_session(session)
        , m_userAgent(userAgent)
    {
        serviceWorkerFrameLoaderClients().set(this, m_session.ptr());
    }
    ~ServiceWorkerFrameLoaderClient()
    {
        serviceWorkerFrameLoaderClients().remove(this);
    }

private:
    Ref<DocumentLoader> createDocumentLoader(ResourceRequest&& request, SubstituteData&& substituteData, ResourceRequest&& originalRequest) final
    {
        return DocumentLoader::create(WTF::move(request), WTF::move(substituteData), WTF::move(originalRequest));
    }
    Ref<DocumentLoader> createDocumentLoader(ResourceRequest&& request, SubstituteData&& substituteData) final
    {
        return createDocumentLoader(WTF::move(request), WTF::move(substituteData), { });
    }
    bool shouldUseCredentialStorage(DocumentLoader*, ResourceLoaderIdentifier) final { return true; }
    String userAgent(const URL&) const final { return m_userAgent; }

    const Ref<Session> m_session;
    const String m_userAgent;
};

// ---- The server to a site's workers ----

class ServerToContextConnection final : public SWServerToContextConnection {
public:
    static Ref<ServerToContextConnection> create(Workers& workers, SWServer& server, Site&& site, std::optional<ScriptExecutionContextIdentifier> serviceWorkerPageIdentifier)
    {
        return adoptRef(*new ServerToContextConnection(workers, server, WTF::move(site), serviceWorkerPageIdentifier));
    }

private:
    ServerToContextConnection(Workers& workers, SWServer& server, Site&& site, std::optional<ScriptExecutionContextIdentifier> serviceWorkerPageIdentifier)
        : SWServerToContextConnection(server, WTF::move(site), serviceWorkerPageIdentifier)
        , m_workers(workers)
    {
    }

    static RefPtr<ServiceWorkerThreadProxy> proxy(ServiceWorkerIdentifier identifier)
    {
        return SWContextManager::singleton().serviceWorkerThreadProxy(identifier);
    }

    void installServiceWorkerContext(const ServiceWorkerContextData&, const ServiceWorkerData&, const String& userAgent, WorkerThreadMode, OptionSet<AdvancedPrivacyProtections>) final;

    void updateAppInitiatedValue(ServiceWorkerIdentifier identifier, LastNavigationWasAppInitiated value) final
    {
        post([identifier, value] {
            if (RefPtr worker = proxy(identifier))
                worker->setLastNavigationWasAppInitiated(value == LastNavigationWasAppInitiated::Yes);
        });
    }
    void fireInstallEvent(ServiceWorkerIdentifier identifier) final
    {
        post([identifier] {
            if (RefPtr worker = proxy(identifier))
                worker->fireInstallEvent();
        });
    }
    void fireActivateEvent(ServiceWorkerIdentifier identifier) final
    {
        post([identifier] {
            if (RefPtr worker = proxy(identifier))
                worker->fireActivateEvent();
        });
    }
    void terminateWorker(ServiceWorkerIdentifier identifier) final
    {
        post([identifier] {
            SWContextManager::singleton().terminateWorker(identifier, SWContextManager::workerTerminationTimeout, nullptr);
        });
    }
    void didSaveScriptsToDisk(ServiceWorkerIdentifier, const ScriptBuffer&, const MemoryCompactRobinHoodHashMap<URL, ScriptBuffer>&) final { }
    // No push service, notifications or background fetch in an application engine.
    void firePushEvent(ServiceWorkerIdentifier, const std::optional<Vector<uint8_t>>&, std::optional<NotificationPayload>&&, CompletionHandler<void(bool, std::optional<NotificationPayload>&&)>&& callback) final { callback(false, std::nullopt); }
    void fireNotificationEvent(ServiceWorkerIdentifier, const NotificationData&, NotificationEventType, CompletionHandler<void(bool)>&& callback) final { callback(false); }
    void fireBackgroundFetchEvent(ServiceWorkerIdentifier, const BackgroundFetchInformation&, CompletionHandler<void(bool)>&& callback) final { callback(false); }
    void fireBackgroundFetchClickEvent(ServiceWorkerIdentifier, const BackgroundFetchInformation&, CompletionHandler<void(bool)>&& callback) final { callback(false); }
    ProcessIdentifier webProcessIdentifier() const final { return Process::identifier(); }
    void openWindow(ServiceWorkerIdentifier, const URL&, OpenWindowCallback&& callback) final
    {
        callback(makeUnexpected(ExceptionData { ExceptionCode::NotSupportedError, "clients.openWindow() is not supported"_s }));
    }
    void connectionIsNoLongerNeeded() final { }
    void terminateDueToUnresponsiveness() final { }
    void setInspectable(ServiceWorkerIsInspectable) final { }
#if ENABLE(CONTENT_EXTENSIONS)
    void reportNetworkUsageToWorkerClient(const ScriptExecutionContextIdentifier, uint64_t) final { }
#endif

    WeakPtr<Workers> m_workers;
};

// ---- A session's connections: the pages' side, and the server's ----

class ClientConnection final : public SWClientConnection {
public:
    static Ref<ClientConnection> create(ServerConnection& server) { return adoptRef(*new ClientConnection(server)); }

    void close()
    {
        setIsClosed();
        clearPendingJobs();
        m_server = nullptr;
    }

    // The server's messages (SWClientConnection's handlers).
    using SWClientConnection::jobRejectedInServer;
    using SWClientConnection::registrationJobResolvedInServer;
    using SWClientConnection::startScriptFetchForServer;
    using SWClientConnection::dispatchMessageToServiceWorkerClient;
    using SWClientConnection::updateRegistrationState;
    using SWClientConnection::updateWorkerState;
    using SWClientConnection::fireUpdateFoundEvent;
    using SWClientConnection::setRegistrationLastUpdateTime;
    using SWClientConnection::setRegistrationUpdateViaCache;
    using SWClientConnection::notifyClientsOfControllerChange;
    using SWClientConnection::updateBackgroundFetchRegistration;

    // A navigation a worker controls (WebSWClientConnection's).
    static bool setServiceWorkerClientIsControlled(ScriptExecutionContextIdentifier identifier, ServiceWorkerRegistrationData&& data)
    {
        if (RefPtr loader = DocumentLoader::fromScriptExecutionContextIdentifier(identifier))
            return loader->setControllingServiceWorkerRegistration(WTF::move(data));
        if (auto manager = WorkerScriptLoader::serviceWorkerDataManagerFromIdentifier(identifier)) {
            if (data.activeWorker) {
                manager->setData(WTF::move(*data.activeWorker));
                return true;
            }
        }
        return false;
    }

private:
    explicit ClientConnection(ServerConnection& server)
        : m_server(&server)
    {
    }

    template<typename Function> void toServer(Function&& function);

    void scheduleJobInServer(const ServiceWorkerJobData&) final;
    void finishFetchingScriptInServer(const ServiceWorkerJobDataIdentifier&, ServiceWorkerRegistrationKey&&, WorkerFetchResult&&) final;
    void addServiceWorkerRegistrationInServer(ServiceWorkerRegistrationIdentifier) final;
    void removeServiceWorkerRegistrationInServer(ServiceWorkerRegistrationIdentifier) final;
    void scheduleUnregisterJobInServer(ServiceWorkerRegistrationIdentifier, ServiceWorkerOrClientIdentifier, CompletionHandler<void(ExceptionOr<bool>&&)>&&) final;
    void registerServiceWorkerInServer(ServiceWorkerIdentifier) final;
    void unregisterServiceWorkerInServer(ServiceWorkerIdentifier) final;
    void didResolveRegistrationPromise(const ServiceWorkerRegistrationKey&) final;
    void postMessageToServiceWorker(ServiceWorkerIdentifier, MessageWithMessagePorts&&, const ServiceWorkerOrClientIdentifier&) final;
    SWServerConnectionIdentifier serverConnectionIdentifier() const final { return Process::identifier(); }
    bool mayHaveServiceWorkerRegisteredForOrigin(const SecurityOriginData&) const final { return true; }
    void registerServiceWorkerClient(const ClientOrigin&, ServiceWorkerClientData&&, const std::optional<ServiceWorkerRegistrationIdentifier>&, String&& userAgent) final;
    void unregisterServiceWorkerClient(ScriptExecutionContextIdentifier) final;
    void matchRegistration(SecurityOriginData&& topOrigin, const URL& clientURL, RegistrationCallback&&) final;
    void getRegistrations(SecurityOriginData&& topOrigin, const URL& clientURL, GetRegistrationsCallback&&) final;
    void whenRegistrationReady(const SecurityOriginData& topOrigin, const URL& clientURL, WhenRegistrationReadyCallback&&) final;
    void storeRegistrationsOnDiskForTesting(CompletionHandler<void()>&&) final;

    // No push service.
    void subscribeToPushService(ServiceWorkerRegistrationIdentifier, const Vector<uint8_t>&, SubscribeToPushServiceCallback&& callback) final { callback(Exception { ExceptionCode::NotAllowedError, "Push is not supported"_s }); }
    void unsubscribeFromPushService(ServiceWorkerRegistrationIdentifier, PushSubscriptionIdentifier, UnsubscribeFromPushServiceCallback&& callback) final { callback(false); }
    void getPushSubscription(ServiceWorkerRegistrationIdentifier, GetPushSubscriptionCallback&& callback) final { callback(std::optional<PushSubscriptionData> { }); }
    void getPushPermissionState(ServiceWorkerRegistrationIdentifier, GetPushPermissionStateCallback&& callback) final { callback(PushPermissionState::Denied); }
#if ENABLE(NOTIFICATION_EVENT)
    void getNotifications(const URL&, const String&, GetNotificationsCallback&& callback) final { callback(Vector<NotificationData> { }); }
#endif

    void enableNavigationPreload(ServiceWorkerRegistrationIdentifier, ExceptionOrVoidCallback&&) final;
    void disableNavigationPreload(ServiceWorkerRegistrationIdentifier, ExceptionOrVoidCallback&&) final;
    void setNavigationPreloadHeaderValue(ServiceWorkerRegistrationIdentifier, String&&, ExceptionOrVoidCallback&&) final;
    void getNavigationPreloadState(ServiceWorkerRegistrationIdentifier, ExceptionOrNavigationPreloadStateCallback&&) final;

    // No background fetch.
    void startBackgroundFetch(ServiceWorkerRegistrationIdentifier, const String&, Vector<BackgroundFetchRequest>&&, BackgroundFetchOptions&&, ExceptionOrBackgroundFetchInformationCallback&& callback) final { callback(Exception { ExceptionCode::NotSupportedError, "Background fetch is not supported"_s }); }
    void backgroundFetchInformation(ServiceWorkerRegistrationIdentifier, const String&, ExceptionOrBackgroundFetchInformationCallback&& callback) final { callback(std::optional<BackgroundFetchInformation> { }); }
    void backgroundFetchIdentifiers(ServiceWorkerRegistrationIdentifier, BackgroundFetchIdentifiersCallback&& callback) final { callback({ }); }
    void abortBackgroundFetch(ServiceWorkerRegistrationIdentifier, const String&, AbortBackgroundFetchCallback&& callback) final { callback(false); }
    void matchBackgroundFetch(ServiceWorkerRegistrationIdentifier, const String&, RetrieveRecordsOptions&&, MatchBackgroundFetchCallback&& callback) final { callback({ }); }
    void retrieveRecordResponse(BackgroundFetchRecordIdentifier, RetrieveRecordResponseCallback&& callback) final { callback(Exception { ExceptionCode::NotSupportedError }); }
    void retrieveRecordResponseBody(BackgroundFetchRecordIdentifier, RetrieveRecordResponseBodyCallback&& callback) final { callback(makeUnexpected(ResourceError { errorDomainWebKitInternal, 0, { }, "Background fetch is not supported"_s })); }

    // No cookie change events in workers.
    void addCookieChangeSubscriptions(ServiceWorkerRegistrationIdentifier, Vector<CookieChangeSubscription>&&, ExceptionOrVoidCallback&& callback) final { callback({ }); }
    void removeCookieChangeSubscriptions(ServiceWorkerRegistrationIdentifier, Vector<CookieChangeSubscription>&&, ExceptionOrVoidCallback&& callback) final { callback({ }); }
    void cookieChangeSubscriptions(ServiceWorkerRegistrationIdentifier, ExceptionOrCookieChangeSubscriptionsCallback&& callback) final { callback(Vector<CookieChangeSubscription> { }); }

    Ref<AddRoutePromise> addRoutes(ServiceWorkerRegistrationIdentifier, Vector<ServiceWorkerRoute>&&) final;

    RefPtr<ServerConnection> m_server;
    // As WebProcess counts them: the server hears of a registration or worker
    // once, however many objects of the pages hold it.
    HashCountedSet<ServiceWorkerRegistrationIdentifier> m_registrations;
    HashCountedSet<ServiceWorkerIdentifier> m_workers;
};

class ServerConnection final : public SWServer::Connection {
public:
    static Ref<ServerConnection> create(SWServer& server) { return adoptRef(*new ServerConnection(server)); }

    void setClient(ClientConnection* client) { m_client = client; }

    // The client's calls (WebSWServerConnection's handlers).
    using SWServer::Connection::finishFetchingScriptInServer;
    using SWServer::Connection::addServiceWorkerRegistrationInServer;
    using SWServer::Connection::removeServiceWorkerRegistrationInServer;
    using SWServer::Connection::registerServiceWorkerInServer;
    using SWServer::Connection::unregisterServiceWorkerInServer;
    using SWServer::Connection::whenRegistrationReady;
    using SWServer::Connection::storeRegistrationsOnDisk;

    void scheduleJobInServer(ServiceWorkerJobData&& jobData)
    {
        RefPtr server = this->server();
        if (!server)
            return;
        if (jobData.scopeURL.isNull())
            return rejectJobInClient(jobData.identifier().jobIdentifier, ExceptionData { ExceptionCode::InvalidStateError, "Scope URL is empty"_s });
        server->scheduleJob(WTF::move(jobData));
    }

    using UnregisterJobResult = std::expected<bool, ExceptionData>;
    void scheduleUnregisterJobInServer(ServiceWorkerJobIdentifier jobIdentifier, ServiceWorkerRegistrationIdentifier registrationIdentifier, ServiceWorkerOrClientIdentifier contextIdentifier, CompletionHandler<void(UnregisterJobResult&&)>&& completionHandler)
    {
        RefPtr server = this->server();
        RefPtr registration = server ? server->getRegistration(registrationIdentifier) : nullptr;
        if (!registration)
            return completionHandler(false);
        auto clientURL = clientURLFromIdentifier(contextIdentifier);
        if (!clientURL.isValid())
            return completionHandler(makeUnexpected(ExceptionData { ExceptionCode::InvalidStateError, "Client is unknown"_s }));
        m_unregisterJobs.add(jobIdentifier, WTF::move(completionHandler));
        server->scheduleUnregisterJob(ServiceWorkerJobDataIdentifier { identifier(), jobIdentifier }, *registration, contextIdentifier, WTF::move(clientURL));
    }

    void postMessageToServiceWorker(ServiceWorkerIdentifier destination, MessageWithMessagePorts&& message, const ServiceWorkerOrClientIdentifier& source)
    {
        RefPtr server = this->server();
        RefPtr destinationWorker = server ? server->workerByID(destination) : nullptr;
        if (!destinationWorker)
            return;
        std::optional<ServiceWorkerOrClientData> sourceData;
        WTF::switchOn(source, [&](ServiceWorkerIdentifier identifier) {
            if (RefPtr sourceWorker = server->workerByID(identifier))
                sourceData = ServiceWorkerOrClientData { sourceWorker->data() };
        }, [&](ScriptExecutionContextIdentifier identifier) {
            if (auto clientData = destinationWorker->findClientByIdentifier(identifier))
                sourceData = ServiceWorkerOrClientData { *clientData };
        });
        if (!sourceData)
            return;
        server->runServiceWorkerIfNecessary(destination, [destination, message = WTF::move(message), sourceData = WTF::move(*sourceData)](auto* contextConnection) mutable {
            if (!contextConnection)
                return;
            post([destination, message = WTF::move(message), sourceData = WTF::move(sourceData)]() mutable {
                if (RefPtr worker = SWContextManager::singleton().serviceWorkerThreadProxy(destination))
                    worker->fireMessageEvent(WTF::move(message), WTF::move(sourceData), { });
            });
        });
    }

    void registerServiceWorkerClient(ClientOrigin&& clientOrigin, ServiceWorkerClientData&& data, const std::optional<ServiceWorkerRegistrationIdentifier>& controllingRegistration, String&& userAgent, SWServer::IsBeingCreatedClient isBeingCreatedClient)
    {
        auto& contextOrigin = clientOrigin.clientOrigin;
        // Sandboxed documents are not clients.
        if (data.url.protocolIsInHTTPFamily() && contextOrigin != SecurityOriginData::fromURLWithoutStrictOpaqueness(data.url))
            return;
        if (contextOrigin.isNull())
            return;
        RefPtr server = this->server();
        if (!server)
            return;
        m_clientOrigins.add(data.identifier, clientOrigin);
        if (isBeingCreatedClient == SWServer::IsBeingCreatedClient::No) {
            for (auto&& pendingMessage : server->releaseServiceWorkerClientPendingMessage(data.identifier))
                sendMessageToClient(data.identifier, pendingMessage.message, ServiceWorkerData { pendingMessage.sourceData }, pendingMessage.sourceOrigin);
        }
        server->registerServiceWorkerClient(WTF::move(clientOrigin), WTF::move(data), controllingRegistration, WTF::move(userAgent), isBeingCreatedClient);
    }

    void unregisterServiceWorkerClient(ScriptExecutionContextIdentifier clientIdentifier)
    {
        auto clientOrigin = m_clientOrigins.take(clientIdentifier);
        if (clientOrigin.clientOrigin.isNull())
            return;
        if (RefPtr server = this->server())
            server->unregisterServiceWorkerClient(clientOrigin, clientIdentifier);
    }

    // A navigation or worker script a registration controls (WebSWServerConnection::controlClient).
    void controlClient(SWServerRegistration& registration, const ResourceRequest& request, const ResourceLoaderOptions& options, std::optional<PageIdentifier> pageIdentifier, std::optional<FrameIdentifier> frameIdentifier)
    {
        ServiceWorkerClientType clientType = ServiceWorkerClientType::Window;
        if (options.destination == FetchOptions::Destination::Worker)
            clientType = ServiceWorkerClientType::Worker;
        else if (options.destination == FetchOptions::Destination::Sharedworker)
            clientType = ServiceWorkerClientType::Sharedworker;

        ScriptExecutionContextIdentifier clientIdentifier { *options.resultingClientIdentifier, Process::identifier() };
        // The document loader takes it at once; WebKit2 sends it there and back.
        if (!ClientConnection::setServiceWorkerClientIsControlled(clientIdentifier, ServiceWorkerRegistrationData { registration.data() }))
            return;

        RefPtr server = this->server();
        if (!server)
            return;
        auto advancedPrivacyProtections = server->advancedPrivacyProtectionsFromClient(registration.key().clientOrigin());
        ServiceWorkerClientData data { clientIdentifier, clientType, ServiceWorkerClientFrameType::None, request.url(), URL(), pageIdentifier, frameIdentifier,
            request.isAppInitiated() ? LastNavigationWasAppInitiated::Yes : LastNavigationWasAppInitiated::No, advancedPrivacyProtections, false, false, 0, { }, std::nullopt };
        registerServiceWorkerClient(ClientOrigin { registration.key().topOrigin(), SecurityOriginData::fromURLWithoutStrictOpaqueness(request.url()) }, WTF::move(data), registration.identifier(), request.httpUserAgent(), SWServer::IsBeingCreatedClient::Yes);
    }

    void enableNavigationPreload(ServiceWorkerRegistrationIdentifier identifier, CompletionHandler<void(std::optional<ExceptionData>&&)>&& callback)
    {
        RefPtr registration = this->registration(identifier);
        callback(registration ? registration->enableNavigationPreload() : noRegistration());
    }
    void disableNavigationPreload(ServiceWorkerRegistrationIdentifier identifier, CompletionHandler<void(std::optional<ExceptionData>&&)>&& callback)
    {
        RefPtr registration = this->registration(identifier);
        callback(registration ? registration->disableNavigationPreload() : noRegistration());
    }
    void setNavigationPreloadHeaderValue(ServiceWorkerRegistrationIdentifier identifier, String&& value, CompletionHandler<void(std::optional<ExceptionData>&&)>&& callback)
    {
        RefPtr registration = this->registration(identifier);
        callback(registration ? registration->setNavigationPreloadHeaderValue(WTF::move(value)) : noRegistration());
    }
    std::optional<NavigationPreloadState> navigationPreloadState(ServiceWorkerRegistrationIdentifier identifier)
    {
        if (RefPtr registration = this->registration(identifier))
            return registration->navigationPreloadState();
        return std::nullopt;
    }

private:
    explicit ServerConnection(SWServer& server)
        : SWServer::Connection(server, Process::identifier())
    {
    }

    SWServerRegistration* registration(ServiceWorkerRegistrationIdentifier identifier)
    {
        RefPtr server = this->server();
        return server ? server->getRegistration(identifier) : nullptr;
    }
    static std::optional<ExceptionData> noRegistration() { return ExceptionData { ExceptionCode::InvalidStateError, "No registration"_s }; }

    URL clientURLFromIdentifier(ServiceWorkerOrClientIdentifier contextIdentifier)
    {
        RefPtr server = this->server();
        if (!server)
            return { };
        return WTF::switchOn(contextIdentifier, [&](ScriptExecutionContextIdentifier clientIdentifier) -> URL {
            auto iterator = m_clientOrigins.find(clientIdentifier);
            if (iterator == m_clientOrigins.end())
                return { };
            auto clientData = server->serviceWorkerClientWithOriginByID(iterator->value, clientIdentifier);
            return clientData ? clientData->url : URL { };
        }, [&](ServiceWorkerIdentifier serviceWorkerIdentifier) -> URL {
            RefPtr worker = server->workerByID(serviceWorkerIdentifier);
            return worker ? worker->data().scriptURL : URL { };
        });
    }

    template<typename Function> void toClient(Function&& function)
    {
        post([client = m_client, function = std::forward<Function>(function)]() mutable {
            if (client && !client->isClosed())
                function(*client);
        });
    }

    void sendMessageToClient(ScriptExecutionContextIdentifier destination, const MessageWithMessagePorts& message, ServiceWorkerData&& source, const SecurityOriginData& sourceOrigin)
    {
        toClient([destination, message = MessageWithMessagePorts { message }, source = WTF::move(source), sourceOrigin](ClientConnection& client) mutable {
            client.dispatchMessageToServiceWorkerClient(destination, WTF::move(message), WTF::move(source), sourceOrigin, { });
        });
    }

    // Messages to the pages, and to the workers' own registration and
    // ServiceWorker objects (SWContextManager), as WebKit2 sends both.
    void rejectJobInClient(ServiceWorkerJobIdentifier jobIdentifier, const ExceptionData& exception) final
    {
        if (auto completionHandler = m_unregisterJobs.take(jobIdentifier))
            return completionHandler(makeUnexpected(exception));
        toClient([jobIdentifier, exception = ExceptionData { exception }](ClientConnection& client) mutable {
            client.jobRejectedInServer(jobIdentifier, WTF::move(exception));
        });
    }
    void resolveRegistrationJobInClient(ServiceWorkerJobIdentifier jobIdentifier, const ServiceWorkerRegistrationData& data, ShouldNotifyWhenResolved shouldNotify) final
    {
        toClient([jobIdentifier, data = ServiceWorkerRegistrationData { data }, shouldNotify](ClientConnection& client) mutable {
            client.registrationJobResolvedInServer(jobIdentifier, WTF::move(data), shouldNotify);
        });
    }
    void resolveUnregistrationJobInClient(ServiceWorkerJobIdentifier jobIdentifier, const ServiceWorkerRegistrationKey&, bool result) final
    {
        if (auto completionHandler = m_unregisterJobs.take(jobIdentifier))
            completionHandler(result);
    }
    void startScriptFetchInClient(ServiceWorkerJobIdentifier jobIdentifier, const ServiceWorkerRegistrationKey& key, FetchOptions::Cache cachePolicy) final
    {
        toClient([jobIdentifier, key = ServiceWorkerRegistrationKey { key }, cachePolicy](ClientConnection& client) mutable {
            client.startScriptFetchForServer(jobIdentifier, WTF::move(key), cachePolicy);
        });
    }
    void updateRegistrationStateInClient(ServiceWorkerRegistrationIdentifier identifier, ServiceWorkerRegistrationState state, const std::optional<ServiceWorkerData>& data) final
    {
        toClient([identifier, state, data](ClientConnection& client) {
            client.updateRegistrationState(identifier, state, data);
        });
        post([identifier, state, data] {
            SWContextManager::singleton().updateRegistrationState(identifier, state, data);
        });
    }
    void updateWorkerStateInClient(ServiceWorkerIdentifier identifier, ServiceWorkerState state) final
    {
        toClient([identifier, state](ClientConnection& client) {
            client.updateWorkerState(identifier, state);
        });
        post([identifier, state] {
            SWContextManager::singleton().updateWorkerState(identifier, state);
        });
    }
    void fireUpdateFoundEvent(ServiceWorkerRegistrationIdentifier identifier) final
    {
        toClient([identifier](ClientConnection& client) {
            client.fireUpdateFoundEvent(identifier);
        });
        post([identifier] {
            SWContextManager::singleton().fireUpdateFoundEvent(identifier);
        });
    }
    void setRegistrationLastUpdateTime(ServiceWorkerRegistrationIdentifier identifier, WallTime time) final
    {
        toClient([identifier, time](ClientConnection& client) {
            client.setRegistrationLastUpdateTime(identifier, time);
        });
        post([identifier, time] {
            SWContextManager::singleton().setRegistrationLastUpdateTime(identifier, time);
        });
    }
    void setRegistrationUpdateViaCache(ServiceWorkerRegistrationIdentifier identifier, ServiceWorkerUpdateViaCache value) final
    {
        toClient([identifier, value](ClientConnection& client) {
            client.setRegistrationUpdateViaCache(identifier, value);
        });
        post([identifier, value] {
            SWContextManager::singleton().setRegistrationUpdateViaCache(identifier, value);
        });
    }
    void notifyClientsOfControllerChange(const HashSet<ScriptExecutionContextIdentifier>& contexts, const std::optional<ServiceWorkerData>& controller) final
    {
        toClient([contexts, controller = std::optional<ServiceWorkerData> { controller }](ClientConnection& client) mutable {
            client.notifyClientsOfControllerChange(contexts, WTF::move(controller));
        });
    }
    void postMessageToServiceWorkerClient(ScriptExecutionContextIdentifier destination, const MessageWithMessagePorts& message, ServiceWorkerIdentifier source, const SecurityOriginData& sourceOrigin, CompletionHandlerCallingScope&&) final
    {
        RefPtr server = this->server();
        if (!server)
            return;
        server->postMessageToServiceWorkerClient(destination, message, source, sourceOrigin, [this](auto destination, auto& message, auto& sourceData, auto& sourceOrigin) {
            sendMessageToClient(destination, message, ServiceWorkerData { sourceData }, sourceOrigin);
        });
    }
    void focusServiceWorkerClient(ScriptExecutionContextIdentifier, CompletionHandler<void(std::optional<ServiceWorkerClientData>&&)>&& callback) final { callback(std::nullopt); }
    void updateBackgroundFetchRegistration(const BackgroundFetchInformation&) final { }
    void contextConnectionCreated(SWServerToContextConnection&) final { }

    RefPtr<ClientConnection> m_client;
    HashMap<ScriptExecutionContextIdentifier, ClientOrigin> m_clientOrigins;
    HashMap<ServiceWorkerJobIdentifier, CompletionHandler<void(UnregisterJobResult&&)>> m_unregisterJobs;
};

// ---- ClientConnection's calls, on to the server ----

template<typename Function> void ClientConnection::toServer(Function&& function)
{
    post([server = m_server, function = std::forward<Function>(function)]() mutable {
        if (server)
            function(*server);
    });
}

void ClientConnection::scheduleJobInServer(const ServiceWorkerJobData& jobData)
{
    toServer([jobData = ServiceWorkerJobData { jobData }](ServerConnection& server) mutable {
        server.scheduleJobInServer(WTF::move(jobData));
    });
}

void ClientConnection::finishFetchingScriptInServer(const ServiceWorkerJobDataIdentifier& job, ServiceWorkerRegistrationKey&& key, WorkerFetchResult&& result)
{
    toServer([job, key = WTF::move(key), result = WTF::move(result)](ServerConnection& server) mutable {
        server.finishFetchingScriptInServer(job, WTF::move(key), WTF::move(result));
    });
}

void ClientConnection::addServiceWorkerRegistrationInServer(ServiceWorkerRegistrationIdentifier identifier)
{
    if (!m_registrations.add(identifier).isNewEntry)
        return;
    toServer([identifier](ServerConnection& server) {
        server.addServiceWorkerRegistrationInServer(identifier);
    });
}

void ClientConnection::removeServiceWorkerRegistrationInServer(ServiceWorkerRegistrationIdentifier identifier)
{
    if (!m_registrations.remove(identifier))
        return;
    toServer([identifier](ServerConnection& server) {
        server.removeServiceWorkerRegistrationInServer(identifier);
    });
}

void ClientConnection::registerServiceWorkerInServer(ServiceWorkerIdentifier identifier)
{
    if (!m_workers.add(identifier).isNewEntry)
        return;
    toServer([identifier](ServerConnection& server) {
        server.registerServiceWorkerInServer(identifier);
    });
}

void ClientConnection::unregisterServiceWorkerInServer(ServiceWorkerIdentifier identifier)
{
    if (!m_workers.remove(identifier))
        return;
    toServer([identifier](ServerConnection& server) {
        server.unregisterServiceWorkerInServer(identifier);
    });
}

void ClientConnection::scheduleUnregisterJobInServer(ServiceWorkerRegistrationIdentifier registration, ServiceWorkerOrClientIdentifier context, CompletionHandler<void(ExceptionOr<bool>&&)>&& completionHandler)
{
    toServer([registration, context, completionHandler = WTF::move(completionHandler)](ServerConnection& server) mutable {
        server.scheduleUnregisterJobInServer(ServiceWorkerJobIdentifier::generate(), registration, context, [completionHandler = WTF::move(completionHandler)](auto&& result) mutable {
            post([completionHandler = WTF::move(completionHandler), result = WTF::move(result)]() mutable {
                if (!result.has_value())
                    return completionHandler(result.error().toException());
                completionHandler(result.value());
            });
        });
    });
}

void ClientConnection::didResolveRegistrationPromise(const ServiceWorkerRegistrationKey& key)
{
    toServer([key](ServerConnection& server) {
        server.didResolveRegistrationPromise(key);
    });
}

void ClientConnection::postMessageToServiceWorker(ServiceWorkerIdentifier destination, MessageWithMessagePorts&& message, const ServiceWorkerOrClientIdentifier& source)
{
    toServer([destination, message = WTF::move(message), source](ServerConnection& server) mutable {
        server.postMessageToServiceWorker(destination, WTF::move(message), source);
    });
}

void ClientConnection::registerServiceWorkerClient(const ClientOrigin& clientOrigin, ServiceWorkerClientData&& data, const std::optional<ServiceWorkerRegistrationIdentifier>& registration, String&& userAgent)
{
    toServer([clientOrigin = ClientOrigin { clientOrigin }, data = WTF::move(data), registration, userAgent = WTF::move(userAgent)](ServerConnection& server) mutable {
        server.registerServiceWorkerClient(WTF::move(clientOrigin), WTF::move(data), registration, WTF::move(userAgent), SWServer::IsBeingCreatedClient::No);
    });
}

void ClientConnection::unregisterServiceWorkerClient(ScriptExecutionContextIdentifier identifier)
{
    toServer([identifier](ServerConnection& server) {
        server.unregisterServiceWorkerClient(identifier);
    });
}

void ClientConnection::matchRegistration(SecurityOriginData&& topOrigin, const URL& clientURL, RegistrationCallback&& callback)
{
    if (!m_server)
        return callback(std::nullopt);
    toServer([topOrigin = WTF::move(topOrigin), clientURL, callback = WTF::move(callback)](ServerConnection& server) mutable {
        server.doRegistrationMatching(topOrigin, clientURL, [callback = WTF::move(callback)](auto&& data) mutable {
            post([callback = WTF::move(callback), data = WTF::move(data)]() mutable {
                callback(WTF::move(data));
            });
        });
    });
}

void ClientConnection::getRegistrations(SecurityOriginData&& topOrigin, const URL& clientURL, GetRegistrationsCallback&& callback)
{
    if (!m_server)
        return callback({ });
    toServer([topOrigin = WTF::move(topOrigin), clientURL, callback = WTF::move(callback)](ServerConnection& server) mutable {
        RefPtr swServer = server.server();
        if (!swServer)
            return callback({ });
        swServer->getRegistrations(topOrigin, clientURL, [callback = WTF::move(callback)](auto&& registrations) mutable {
            post([callback = WTF::move(callback), registrations = WTF::move(registrations)]() mutable {
                callback(WTF::move(registrations));
            });
        });
    });
}

void ClientConnection::whenRegistrationReady(const SecurityOriginData& topOrigin, const URL& clientURL, WhenRegistrationReadyCallback&& callback)
{
    toServer([topOrigin, clientURL, callback = WTF::move(callback)](ServerConnection& server) mutable {
        server.whenRegistrationReady(topOrigin, clientURL, [callback = WTF::move(callback)](auto&& data) mutable {
            if (!data)
                return;
            post([callback = WTF::move(callback), data = WTF::move(*data)]() mutable {
                callback(WTF::move(data));
            });
        });
    });
}

void ClientConnection::storeRegistrationsOnDiskForTesting(CompletionHandler<void()>&& callback)
{
    if (!m_server)
        return callback();
    m_server->storeRegistrationsOnDisk(WTF::move(callback));
}

static ExceptionOr<void> toExceptionOrVoid(std::optional<ExceptionData>&& error)
{
    if (error)
        return error->toException();
    return { };
}

void ClientConnection::enableNavigationPreload(ServiceWorkerRegistrationIdentifier identifier, ExceptionOrVoidCallback&& callback)
{
    toServer([identifier, callback = WTF::move(callback)](ServerConnection& server) mutable {
        server.enableNavigationPreload(identifier, [callback = WTF::move(callback)](auto&& error) mutable {
            callback(toExceptionOrVoid(WTF::move(error)));
        });
    });
}

void ClientConnection::disableNavigationPreload(ServiceWorkerRegistrationIdentifier identifier, ExceptionOrVoidCallback&& callback)
{
    toServer([identifier, callback = WTF::move(callback)](ServerConnection& server) mutable {
        server.disableNavigationPreload(identifier, [callback = WTF::move(callback)](auto&& error) mutable {
            callback(toExceptionOrVoid(WTF::move(error)));
        });
    });
}

void ClientConnection::setNavigationPreloadHeaderValue(ServiceWorkerRegistrationIdentifier identifier, String&& value, ExceptionOrVoidCallback&& callback)
{
    toServer([identifier, value = WTF::move(value), callback = WTF::move(callback)](ServerConnection& server) mutable {
        server.setNavigationPreloadHeaderValue(identifier, WTF::move(value), [callback = WTF::move(callback)](auto&& error) mutable {
            callback(toExceptionOrVoid(WTF::move(error)));
        });
    });
}

void ClientConnection::getNavigationPreloadState(ServiceWorkerRegistrationIdentifier identifier, ExceptionOrNavigationPreloadStateCallback&& callback)
{
    toServer([identifier, callback = WTF::move(callback)](ServerConnection& server) mutable {
        auto state = server.navigationPreloadState(identifier);
        if (!state)
            return callback(Exception { ExceptionCode::InvalidStateError });
        callback(WTF::move(*state));
    });
}

auto ClientConnection::addRoutes(ServiceWorkerRegistrationIdentifier identifier, Vector<ServiceWorkerRoute>&& routes) -> Ref<AddRoutePromise>
{
    AddRoutePromise::Producer producer;
    Ref<AddRoutePromise> promise = producer;
    toServer([identifier, routes = WTF::move(routes), producer = WTF::move(producer)](ServerConnection& server) mutable {
        RefPtr swServer = server.server();
        if (!swServer)
            return producer.reject(ExceptionData { ExceptionCode::InvalidStateError, "No server"_s });
        swServer->addRoutes(identifier, WTF::move(routes), [producer = WTF::move(producer)](auto&& result) mutable {
            producer.settle(WTF::move(result));
        });
    });
    return promise;
}

// ---- The registration store: WebCore's SQLite database on a work queue ----

class RegistrationStore final : public SWRegistrationStore {
public:
    static Ref<RegistrationStore> create(SWServer& server, const String& directory) { return adoptRef(*new RegistrationStore(server, directory)); }

private:
    RegistrationStore(SWServer& server, const String& directory)
        : m_server(server)
        , m_queue(WorkQueue::create("ZeroG service worker registrations"_s))
        , m_directory(directory.isolatedCopy())
        , m_updateTimer(*this, &RegistrationStore::updateTimerFired)
    {
    }

    ~RegistrationStore()
    {
        m_queue->dispatch([database = WTF::move(m_database)] { });
    }

    // On the queue only.
    SWRegistrationDatabase* database()
    {
        if (!m_database)
            m_database = makeUnique<SWRegistrationDatabase>(m_directory);
        return m_database.get();
    }

    template<typename Result, typename Work> void onQueue(Work&& work, CompletionHandler<void(Result&&)>&& completionHandler)
    {
        m_queue->dispatch([protectedThis = Ref { *this }, work = std::forward<Work>(work), completionHandler = WTF::move(completionHandler)]() mutable {
            auto result = work(*protectedThis->database());
            RunLoop::mainSingleton().dispatch([result = crossThreadCopy(WTF::move(result)), completionHandler = WTF::move(completionHandler)]() mutable {
                completionHandler(WTF::move(result));
            });
        });
    }

    void clearAll(CompletionHandler<void()>&& completionHandler) final
    {
        m_updates.clear();
        m_updateTimer.stop();
        onQueue<bool>([](SWRegistrationDatabase& database) {
            database.deleteAllFiles();
            return true;
        }, [completionHandler = WTF::move(completionHandler)](bool) mutable {
            completionHandler();
        });
    }

    void flushChanges(CompletionHandler<void()>&& completionHandler) final
    {
        m_updateTimer.stop();
        updateToStorage(WTF::move(completionHandler));
    }

    void closeFiles(CompletionHandler<void()>&& completionHandler) final
    {
        m_queue->dispatch([protectedThis = Ref { *this }, completionHandler = WTF::move(completionHandler)]() mutable {
            protectedThis->m_database = nullptr;
            RunLoop::mainSingleton().dispatch(WTF::move(completionHandler));
        });
    }

    void importRegistrationsForOrigin(const SecurityOriginData& topOrigin, CompletionHandler<void(std::optional<Vector<ServiceWorkerContextData>>&&)>&& completionHandler) final
    {
        onQueue<std::optional<Vector<ServiceWorkerContextData>>>([topOrigin = topOrigin.isolatedCopy()](SWRegistrationDatabase& database) {
            return database.importRegistrations(topOrigin);
        }, WTF::move(completionHandler));
    }

    void importOriginList(CompletionHandler<void(std::optional<HashSet<ClientOrigin>>&&)>&& completionHandler) final
    {
        onQueue<std::optional<HashSet<ClientOrigin>>>([](SWRegistrationDatabase& database) {
            return database.importOrigins();
        }, WTF::move(completionHandler));
    }

    void updateRegistration(const ServiceWorkerContextData& data) final
    {
        m_updates.set(data.registration.key, data.copy());
        scheduleUpdate();
    }

    void removeRegistration(const ServiceWorkerRegistrationKey& key) final
    {
        m_updates.set(key, std::nullopt);
        scheduleUpdate();
    }

    void retrieveWorkerScripts(ServiceWorkerIdentifier identifier, const ServiceWorkerRegistrationKey& key, const URL& scriptURL, const Vector<URL>& importedScriptURLs, CompletionHandler<void(std::optional<ServiceWorkerScripts>&&)>&& completionHandler) final
    {
        onQueue<std::optional<ServiceWorkerScripts>>([identifier, key = crossThreadCopy(key), scriptURL = crossThreadCopy(scriptURL), importedScriptURLs = crossThreadCopy(importedScriptURLs)](SWRegistrationDatabase& database) {
            return database.retrieveWorkerScripts(identifier, key, scriptURL, importedScriptURLs);
        }, WTF::move(completionHandler));
    }

    void scheduleUpdate()
    {
        if (!m_updateTimer.isActive())
            m_updateTimer.startOneShot(0_s);
    }

    void updateTimerFired() { updateToStorage([] { }); }

    void updateToStorage(CompletionHandler<void()>&& completionHandler)
    {
        Vector<ServiceWorkerRegistrationKey> toDelete;
        Vector<ServiceWorkerContextData> toUpdate;
        for (auto& [key, registration] : m_updates) {
            if (!registration)
                toDelete.append(key);
            else
                toUpdate.append(WTF::move(*registration));
        }
        m_updates.clear();
        onQueue<std::optional<Vector<ServiceWorkerScripts>>>([toUpdate = crossThreadCopy(WTF::move(toUpdate)), toDelete = crossThreadCopy(WTF::move(toDelete))](SWRegistrationDatabase& database) {
            return database.updateRegistrations(toUpdate, toDelete);
        }, [weakThis = WeakPtr { *this }, completionHandler = WTF::move(completionHandler)](auto&& result) mutable {
            RefPtr protectedThis = weakThis.get();
            if (protectedThis && result) {
                if (RefPtr server = protectedThis->m_server.get()) {
                    for (auto& scripts : *result)
                        server->didSaveWorkerScriptsToDisk(scripts.identifier, WTF::move(scripts.mainScript), WTF::move(scripts.importedScripts));
                }
            }
            completionHandler();
        });
    }

    WeakPtr<SWServer> m_server;
    const Ref<WorkQueue> m_queue;
    const String m_directory;
    std::unique_ptr<SWRegistrationDatabase> m_database;
    HashMap<ServiceWorkerRegistrationKey, std::optional<ServiceWorkerContextData>> m_updates;
    Timer m_updateTimer;
};

class OriginStore final : public SWOriginStore {
    void importComplete() final { }
    void addToStore(const SecurityOriginData&) final { }
    void removeFromStore(const SecurityOriginData&) final { }
    void clearStore() final { }
};

class NoBackgroundFetchStore final : public BackgroundFetchStore {
public:
    static Ref<NoBackgroundFetchStore> create() { return adoptRef(*new NoBackgroundFetchStore); }

private:
    void initializeFetches(const ServiceWorkerRegistrationKey&, CompletionHandler<void()>&& callback) final { callback(); }
    void clearFetch(const ServiceWorkerRegistrationKey&, const String&, CompletionHandler<void()>&& callback) final { callback(); }
    void clearAllFetches(const ServiceWorkerRegistrationKey&, CompletionHandler<void()>&& callback) final { callback(); }
    void storeFetch(const ServiceWorkerRegistrationKey&, const String&, uint64_t, uint64_t, std::optional<size_t>, Vector<uint8_t>&&, CompletionHandler<void(StoreResult)>&& callback) final { callback(StoreResult::InternalError); }
    void storeFetchResponseBodyChunk(const ServiceWorkerRegistrationKey&, const String&, size_t, const SharedBuffer&, CompletionHandler<void(StoreResult)>&& callback) final { callback(StoreResult::InternalError); }
    void retrieveResponseBody(const ServiceWorkerRegistrationKey&, const String&, size_t, RetrieveRecordResponseBodyCallback&& callback) final { callback(RefPtr<SharedBuffer> { }); }
};

// ---- Pages' connections, by session ----

class Provider final : public ServiceWorkerProvider {
public:
    static Provider& singleton()
    {
        static NeverDestroyed<Provider> provider;
        return provider;
    }

    void add(PAL::SessionID sessionID, ClientConnection& connection)
    {
        static bool installed;
        if (!installed) {
            installed = true;
            ServiceWorkerProvider::setSharedProvider(*this);
        }
        m_connections.set(sessionID, connection);
    }
    void remove(PAL::SessionID sessionID) { m_connections.remove(sessionID); }

private:
    SWClientConnection& serviceWorkerConnection() final { return serviceWorkerConnectionForSession(std::nullopt); }
    SWClientConnection* existingServiceWorkerConnection() final { return existingServiceWorkerConnectionForSession(std::nullopt); }

    SWClientConnection& serviceWorkerConnectionForSession(std::optional<PAL::SessionID> sessionID) final
    {
        if (auto* connection = existingServiceWorkerConnectionForSession(sessionID))
            return *connection;
        // A session gone (or none given): a closed connection answers nothing.
        if (!m_closed) {
            m_closed = ClientConnection::create(ServerConnection::create(closedServer()));
            m_closed->close();
        }
        return *m_closed;
    }

    SWClientConnection* existingServiceWorkerConnectionForSession(std::optional<PAL::SessionID> sessionID) final
    {
        if (sessionID) {
            auto iterator = m_connections.find(*sessionID);
            if (iterator != m_connections.end())
                return iterator->value.ptr();
        }
        if (!sessionID && m_connections.size() == 1)
            return m_connections.begin()->value.ptr();
        return nullptr;
    }

    void terminateWorkerForTesting(ServiceWorkerIdentifier, CompletionHandler<void()>&& callback) final { callback(); }

    static SWServer& closedServer();

    HashMap<PAL::SessionID, Ref<ClientConnection>> m_connections;
    RefPtr<ClientConnection> m_closed;
};

// ---- A session's service workers ----

class Workers final : public ServiceWorkerSession, public SWServerDelegate, public CanMakeCheckedPtr<Workers> {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(Workers);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(Workers);
public:
    static Ref<Workers> create(Session& session, const String& directory) { return adoptRef(*new Workers(session, directory)); }

    ~Workers()
    {
        close();
    }

    uint32_t checkedPtrCount() const final { return CanMakeCheckedPtr::checkedPtrCount(); }
    uint32_t checkedPtrCountWithoutThreadCheck() const final { return CanMakeCheckedPtr::checkedPtrCountWithoutThreadCheck(); }
    void incrementCheckedPtrCount() const final { CanMakeCheckedPtr::incrementCheckedPtrCount(); }
    void decrementCheckedPtrCount() const final { CanMakeCheckedPtr::decrementCheckedPtrCount(); }
    void setDidBeginCheckedPtrDeletion() final { CanMakeCheckedPtr::setDidBeginCheckedPtrDeletion(); }

    Session* session() const { return m_session.get(); }
    SWServer* server() const { return m_server.get(); }
    ServerConnection* serverConnection() const { return m_serverConnection.get(); }
    const String& directory() const { return m_directory; }

    void close() final
    {
        if (!m_session)
            return;
        auto sessionID = m_session->sessionID();
        Provider::singleton().remove(sessionID);
        if (RefPtr client = std::exchange(m_clientConnection, nullptr))
            client->close();
        if (RefPtr connection = std::exchange(m_serverConnection, nullptr)) {
            connection->setClient(nullptr);
            if (RefPtr server = m_server)
                server->removeConnection(connection->identifier());
        }
        if (RefPtr server = std::exchange(m_server, nullptr)) {
            // Changes still waiting for the store's timer are written first
            // (its work queue keeps the store until they are).
            server->storeRegistrationsOnDisk([] { });
            server->close();
        }
        m_session = nullptr;
    }

    RefPtr<LoadTask> createLoadTask(ResourceLoader&, Function<RefPtr<LoadTask>()>&& fallback) final;

private:
    Workers(Session& session, const String& directory)
        : m_session(&session)
        , m_directory(directory)
    {
        ContextManagerConnection::ensure();
        auto databaseDirectory = directory;
        m_server = SWServer::create(*this, makeUniqueRef<OriginStore>(), false, WTF::move(databaseDirectory), session.sessionID(), false, true, std::nullopt, ServiceWorkerIsInspectable::No);
        m_serverConnection = ServerConnection::create(*m_server);
        m_server->addConnection(*m_serverConnection);
        m_clientConnection = ClientConnection::create(*m_serverConnection);
        m_serverConnection->setClient(m_clientConnection.get());
        Provider::singleton().add(session.sessionID(), *m_clientConnection);
    }

    // Updates fetch the script again through the session's loader; a failed
    // soft update keeps the worker that is there (WebKit2's
    // ServiceWorkerSoftUpdateLoader checks the bytes it gets).
    void softUpdate(ServiceWorkerJobData&&, bool, ResourceRequest&& request, CompletionHandler<void(WorkerFetchResult&&)>&& callback) final
    {
        callback(workerFetchError(ResourceError { errorDomainWebKitInternal, 0, request.url(), "Service worker updates are not fetched"_s, ResourceError::Type::Cancellation }));
    }

    void createContextConnection(const Site& site, std::optional<ProcessIdentifier>, std::optional<ScriptExecutionContextIdentifier> serviceWorkerPageIdentifier, CrossOriginEmbedderPolicyValue policy, CompletionHandler<void()>&& completionHandler) final
    {
        RefPtr server = m_server;
        if (!server)
            return completionHandler();
        if (!server->contextConnectionForRegistrableDomain(site.domain(), policy)) {
            auto connection = ServerToContextConnection::create(*this, *server, Site { site }, serviceWorkerPageIdentifier);
            connection->setCrossOriginEmbedderPolicyValue(policy);
            server->addContextConnection(connection.get());
            m_contextConnections.append(WTF::move(connection));
        }
        completionHandler();
    }

    void appBoundDomains(CompletionHandler<void(HashSet<RegistrableDomain>&&)>&& completionHandler) final { completionHandler({ }); }
    void addAllowedFirstPartyForCookies(ProcessIdentifier, std::optional<ProcessIdentifier>, RegistrableDomain&&) final { }
    void requestBackgroundFetchPermission(const ClientOrigin&, CompletionHandler<void(bool)>&& completionHandler) final { completionHandler(false); }
    RefPtr<BackgroundFetchRecordLoader> createBackgroundFetchRecordLoader(BackgroundFetchRecordLoaderClient&, const BackgroundFetchRequest&, size_t, const ClientOrigin&) final { return nullptr; }
    Ref<BackgroundFetchStore> createBackgroundFetchStore() final { return NoBackgroundFetchStore::create(); }
    RefPtr<SWRegistrationStore> createRegistrationStore(SWServer& server) final
    {
        if (m_directory.isEmpty())
            return nullptr;
        return RegistrationStore::create(server, m_directory);
    }

    RefPtr<Session> m_session;
    const String m_directory;
    RefPtr<SWServer> m_server;
    RefPtr<ServerConnection> m_serverConnection;
    RefPtr<ClientConnection> m_clientConnection;
    Vector<Ref<ServerToContextConnection>> m_contextConnections;
};

SWServer& Provider::closedServer()
{
    // Never used for anything: a closed connection still needs a server to
    // construct; it answers no call.
    struct NoDelegate final : public SWServerDelegate, public CanMakeCheckedPtr<NoDelegate> {
        WTF_DEPRECATED_MAKE_FAST_ALLOCATED(NoDelegate);
        WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(NoDelegate);
        uint32_t checkedPtrCount() const final { return CanMakeCheckedPtr::checkedPtrCount(); }
        uint32_t checkedPtrCountWithoutThreadCheck() const final { return CanMakeCheckedPtr::checkedPtrCountWithoutThreadCheck(); }
        void incrementCheckedPtrCount() const final { CanMakeCheckedPtr::incrementCheckedPtrCount(); }
        void decrementCheckedPtrCount() const final { CanMakeCheckedPtr::decrementCheckedPtrCount(); }
        void setDidBeginCheckedPtrDeletion() final { CanMakeCheckedPtr::setDidBeginCheckedPtrDeletion(); }
        void softUpdate(ServiceWorkerJobData&&, bool, ResourceRequest&&, CompletionHandler<void(WorkerFetchResult&&)>&& callback) final { callback(workerFetchError(ResourceError { ResourceError::Type::Cancellation })); }
        void createContextConnection(const Site&, std::optional<ProcessIdentifier>, std::optional<ScriptExecutionContextIdentifier>, CrossOriginEmbedderPolicyValue, CompletionHandler<void()>&& callback) final { callback(); }
        void appBoundDomains(CompletionHandler<void(HashSet<RegistrableDomain>&&)>&& callback) final { callback({ }); }
        void addAllowedFirstPartyForCookies(ProcessIdentifier, std::optional<ProcessIdentifier>, RegistrableDomain&&) final { }
        void requestBackgroundFetchPermission(const ClientOrigin&, CompletionHandler<void(bool)>&& callback) final { callback(false); }
        RefPtr<BackgroundFetchRecordLoader> createBackgroundFetchRecordLoader(BackgroundFetchRecordLoaderClient&, const BackgroundFetchRequest&, size_t, const ClientOrigin&) final { return nullptr; }
        Ref<BackgroundFetchStore> createBackgroundFetchStore() final { return NoBackgroundFetchStore::create(); }
        RefPtr<SWRegistrationStore> createRegistrationStore(SWServer&) final { return nullptr; }
    };
    static NeverDestroyed<NoDelegate> delegate;
    static NeverDestroyed<Ref<SWServer>> server { SWServer::create(delegate.get(), makeUniqueRef<OriginStore>(), false, { }, PAL::SessionID::defaultSessionID(), false, true, std::nullopt, ServiceWorkerIsInspectable::No) };
    return server.get();
}

// ---- Installing a worker: its page and thread ----

void ServerToContextConnection::installServiceWorkerContext(const ServiceWorkerContextData& contextData, const ServiceWorkerData& workerData, const String& userAgent, WorkerThreadMode workerThreadMode, OptionSet<AdvancedPrivacyProtections> advancedPrivacyProtections)
{
    RefPtr workers = m_workers.get();
    RefPtr<Session> session = workers ? workers->session() : nullptr;
    if (!session)
        return;
    post([session = session.releaseNonNull(), contextData = contextData.isolatedCopy(), workerData = workerData.isolatedCopy(), userAgent = userAgent.isNull() ? standardUserAgent() : userAgent, workerThreadMode, advancedPrivacyProtections]() mutable {
        auto configuration = pageConfigurationWithEmptyClients(PageIdentifier::generate(), session->sessionID());
        auto& mainFrameParameters = std::get<PageConfiguration::LocalMainFrameCreationParameters>(configuration.mainFrameCreationParameters);
        mainFrameParameters.effectiveSandboxFlags = { };
        mainFrameParameters.clientCreator = [session, userAgent](auto&, auto& frameLoader) mutable -> UniqueRef<LocalFrameLoaderClient> {
            return makeUniqueRefWithoutRefCountedCheck<ServiceWorkerFrameLoaderClient>(frameLoader, session.get(), userAgent);
        };
        configuration.cookieJar = createCookieJar(session.get());
        configuration.socketProvider = createSocketProvider();
        configuration.userContentProvider = session->userContentProvider();
        configuration.storageNamespaceProvider = session->storageNamespaceProvider();
        configuration.databaseProvider = session->databaseProvider();
        Ref cacheStorageProvider = session->cacheStorageProvider();
        configuration.cacheStorageProvider = cacheStorageProvider.copyRef();
        Ref page = Page::create(WTF::move(configuration));
        auto& settings = page->settings();
        settings.setServiceWorkersEnabled(true);
        settings.setCacheAPIEnabled(true);
        settings.setLocalStorageEnabled(true);
        page->setupForRemoteWorker(contextData.scriptURL, contextData.registration.key.topOrigin(), contextData.referrerPolicy, advancedPrivacyProtections, contextData.globalPrivacyControlEnabled);

        auto lastNavigationWasAppInitiated = contextData.lastNavigationWasAppInitiated;
        auto proxy = ServiceWorkerThreadProxy::create(WTF::move(page), WTF::move(contextData), WTF::move(workerData), WTF::move(userAgent), workerThreadMode, cacheStorageProvider.get(), nullptr);
        if (lastNavigationWasAppInitiated)
            proxy->setLastNavigationWasAppInitiated(*lastNavigationWasAppInitiated == LastNavigationWasAppInitiated::Yes);
        SWContextManager::singleton().registerServiceWorkerThreadForInstall(WTF::move(proxy));
    });
}

void ContextManagerConnection::postMessageToServiceWorkerClient(const ScriptExecutionContextIdentifier& destination, const MessageWithMessagePorts& message, ServiceWorkerIdentifier source, const SecurityOriginData& sourceOrigin)
{
    RefPtr worker = SWServerWorker::existingWorkerForIdentifier(source);
    RefPtr server = worker ? worker->server() : nullptr;
    RefPtr connection = server ? server->connection(destination.processIdentifier()) : nullptr;
    if (!connection)
        return;
    connection->postMessageToServiceWorkerClient(destination, message, source, sourceOrigin, { });
}

// ---- Fetches through a worker ----

class ServiceWorkerLoad;

HashMap<FetchIdentifier, WeakPtr<ServiceWorkerLoad>>& fetchesInProgress()
{
    static NeverDestroyed<HashMap<FetchIdentifier, WeakPtr<ServiceWorkerLoad>>> fetches;
    return fetches;
}

// The worker's side of a fetch: called on its thread, it hands each answer to
// the main thread, where the load continues (WebServiceWorkerFetchTaskClient).
class FetchClient final : public ServiceWorkerFetch::Client {
public:
    static Ref<FetchClient> create(FetchIdentifier fetch, ServiceWorkerIdentifier worker, SWServerConnectionIdentifier connection) { return adoptRef(*new FetchClient(fetch, worker, connection)); }

private:
    FetchClient(FetchIdentifier fetch, ServiceWorkerIdentifier worker, SWServerConnectionIdentifier connection)
        : m_fetch(fetch)
        , m_worker(worker)
        , m_connection(connection)
    {
    }

    template<typename Function> void toLoad(Function&&);
    void finished()
    {
        m_isDone = true;
        SWContextManager::singleton().removeFetch(m_worker, m_connection, m_fetch, false);
    }

    void didReceiveRedirection(const ResourceResponse&) final;
    void didReceiveResponse(ResourceResponse&&, const URL&) final;
    void didReceiveData(const SharedBuffer&) final;
    void didReceiveFormDataAndFinish(Ref<FormData>&&) final;
    void didFail(const ResourceError&) final;
    void didFinish(const NetworkLoadMetrics&) final;
    void didNotHandle() final;
    void setCancelledCallback(Function<void()>&& callback) final { m_cancelledCallback = WTF::move(callback); }
    // No navigation preload: the load goes as it would without the worker.
    void usePreload() final { didNotHandle(); }
    void contextIsStopping() final
    {
        if (m_isDone)
            return;
        if (!m_didSendResponse)
            return didNotHandle();
        didFail(ResourceError { errorDomainWebKitInternal, 0, { }, "Service Worker context closed"_s });
    }
    void doCancel() final
    {
        m_isDone = true;
        if (m_cancelledCallback)
            m_cancelledCallback();
    }

    const FetchIdentifier m_fetch;
    const ServiceWorkerIdentifier m_worker;
    const SWServerConnectionIdentifier m_connection;
    std::atomic<bool> m_isDone { false };
    std::atomic<bool> m_didSendResponse { false };
    Function<void()> m_cancelledCallback;
};

// A load a worker answers. Until it does, data and completion wait behind the
// page's response policy, as SchemeTask does; when it does not, the load goes
// the way it would have gone (`fallback`).
class BlobBodyClient;

class ServiceWorkerLoad final : public LoadTask, public CanMakeWeakPtr<ServiceWorkerLoad> {
public:
    // A subresource of a controlled client, or a navigation whose registration
    // is known.
    static Ref<ServiceWorkerLoad> create(Workers& workers, ResourceLoader& loader, ServiceWorkerRegistrationIdentifier registration, Function<RefPtr<LoadTask>()>&& fallback)
    {
        return adoptRef(*new ServiceWorkerLoad(workers, loader, registration, std::nullopt, WTF::move(fallback)));
    }
    // A navigation whose top origin's registrations are still on disk: matched
    // once they are read.
    static Ref<ServiceWorkerLoad> createMatching(Workers& workers, ResourceLoader& loader, const SecurityOriginData& topOrigin, Function<RefPtr<LoadTask>()>&& fallback)
    {
        return adoptRef(*new ServiceWorkerLoad(workers, loader, std::nullopt, topOrigin, WTF::move(fallback)));
    }

    ~ServiceWorkerLoad();

    void didReceiveRedirection(ResourceResponse&&);
    void didReceiveResponse(ResourceResponse&&);
    void didReceiveData(Ref<SharedBuffer>&&);
    void didFinish();
    void didFail(const ResourceError&);
    void didNotHandle();
    void loadBlob(const URL&);

private:
    ServiceWorkerLoad(Workers& workers, ResourceLoader& loader, std::optional<ServiceWorkerRegistrationIdentifier> registration, std::optional<SecurityOriginData> topOrigin, Function<RefPtr<LoadTask>()>&& fallback);

    void start() final;
    void stop() final;
    bool convertToDownload(Download&) final;

    void match();
    void startWorker();
    void startFetch();
    void runOrQueue(Function<void()>&&);
    void processQueue();

    WeakPtr<Workers> m_workers;
    LoadTarget m_target;
    ResourceRequest m_request;
    const ResourceLoaderOptions m_options;
    std::optional<ServiceWorkerRegistrationIdentifier> m_registration;
    std::optional<SecurityOriginData> m_topOrigin;
    std::optional<PageIdentifier> m_pageIdentifier;
    std::optional<FrameIdentifier> m_frameIdentifier;
    Function<RefPtr<LoadTask>()> m_fallback;
    const FetchIdentifier m_fetch { FetchIdentifier::generate() };
    std::optional<ServiceWorkerIdentifier> m_worker;
    RefPtr<LoadTask> m_fallbackTask;
    bool m_fetchStarted { false };
    bool m_waitingForResponsePolicy { false };
    Deque<Function<void()>> m_queue;
    RefPtr<FetchLoader> m_blobLoader;
    RefPtr<BlobBodyClient> m_blobClient;
};

template<typename Function> void FetchClient::toLoad(Function&& function)
{
    callOnMainThread([fetch = m_fetch, function = std::forward<Function>(function)]() mutable {
        if (RefPtr load = fetchesInProgress().get(fetch))
            function(*load);
    });
}

void FetchClient::didReceiveRedirection(const ResourceResponse& response)
{
    if (m_isDone)
        return;
    m_didSendResponse = true;
    finished();
    toLoad([data = response.crossThreadData()](ServiceWorkerLoad& load) mutable {
        load.didReceiveRedirection(ResourceResponse::fromCrossThreadData(WTF::move(data)));
    });
}

void FetchClient::didReceiveResponse(ResourceResponse&& response, const URL&)
{
    if (m_isDone)
        return;
    m_didSendResponse = true;
    toLoad([data = response.crossThreadData()](ServiceWorkerLoad& load) mutable {
        load.didReceiveResponse(ResourceResponse::fromCrossThreadData(WTF::move(data)));
    });
}

void FetchClient::didReceiveData(const SharedBuffer& buffer)
{
    if (m_isDone)
        return;
    toLoad([data = Vector<uint8_t> { buffer.span() }](ServiceWorkerLoad& load) mutable {
        load.didReceiveData(SharedBuffer::create(WTF::move(data)));
    });
}

void FetchClient::didReceiveFormDataAndFinish(Ref<FormData>&& formData)
{
    if (m_isDone)
        return;
    if (RefPtr buffer = formData->asSharedBuffer()) {
        didReceiveData(*buffer);
        didFinish({ });
        return;
    }
    auto blobURL = formData->asBlobURL();
    if (blobURL.isNull()) {
        didFail(ResourceError { errorDomainWebKitInternal, 0, { }, "The service worker's response body cannot be read"_s });
        return;
    }
    finished();
    toLoad([blobURL = WTF::move(blobURL).isolatedCopy()](ServiceWorkerLoad& load) {
        load.loadBlob(blobURL);
    });
}

void FetchClient::didFail(const ResourceError& error)
{
    if (m_isDone)
        return;
    finished();
    toLoad([error = error.isolatedCopy()](ServiceWorkerLoad& load) {
        load.didFail(error);
    });
}

void FetchClient::didFinish(const NetworkLoadMetrics&)
{
    if (m_isDone)
        return;
    finished();
    toLoad([](ServiceWorkerLoad& load) {
        load.didFinish();
    });
}

void FetchClient::didNotHandle()
{
    if (m_isDone)
        return;
    finished();
    toLoad([](ServiceWorkerLoad& load) {
        load.didNotHandle();
    });
}

ServiceWorkerLoad::ServiceWorkerLoad(Workers& workers, ResourceLoader& loader, std::optional<ServiceWorkerRegistrationIdentifier> registration, std::optional<SecurityOriginData> topOrigin, Function<RefPtr<LoadTask>()>&& fallback)
    : m_workers(workers)
    , m_target(loader)
    , m_request(loader.request())
    , m_options(loader.options())
    , m_registration(registration)
    , m_topOrigin(WTF::move(topOrigin))
    , m_fallback(WTF::move(fallback))
{
    if (RefPtr frame = loader.frame()) {
        m_frameIdentifier = frame->frameID();
        if (RefPtr page = frame->page())
            m_pageIdentifier = page->identifier();
    }
}

void ServiceWorkerLoad::start()
{
    if (!m_target.isActive())
        return;
    if (!m_topOrigin)
        return startWorker();
    RefPtr workers = m_workers.get();
    RefPtr server = workers ? workers->server() : nullptr;
    if (!server)
        return didNotHandle();
    server->importRegistrationsForOrigin(*m_topOrigin, [weakThis = WeakPtr { *this }] {
        if (RefPtr protectedThis = weakThis.get())
            protectedThis->match();
    });
}

void ServiceWorkerLoad::match()
{
    if (!m_target.isActive())
        return;
    RefPtr workers = m_workers.get();
    RefPtr server = workers ? workers->server() : nullptr;
    RefPtr connection = workers ? workers->serverConnection() : nullptr;
    RefPtr registration = server ? server->doRegistrationMatchingSync(*m_topOrigin, m_request.url()) : nullptr;
    if (!registration || !connection)
        return didNotHandle();
    m_registration = registration->identifier();
    connection->controlClient(*registration, m_request, m_options, m_pageIdentifier, m_frameIdentifier);
    startWorker();
}

void ServiceWorkerLoad::startWorker()
{
    RefPtr workers = m_workers.get();
    RefPtr server = workers ? workers->server() : nullptr;
    RefPtr registration = server ? server->getRegistration(*m_registration) : nullptr;
    RefPtr worker = registration ? registration->activeWorker() : nullptr;
    if (!worker || worker->hasTimedOutAnyFetchTasks())
        return didNotHandle();
    m_worker = worker->identifier();
    worker->whenActivated([weakThis = WeakPtr { *this }](bool success) {
        RefPtr protectedThis = weakThis.get();
        if (!protectedThis)
            return;
        RefPtr workers = protectedThis->m_workers.get();
        RefPtr server = workers ? workers->server() : nullptr;
        RefPtr worker = server && success ? server->workerByID(*protectedThis->m_worker) : nullptr;
        if (!worker)
            return protectedThis->didNotHandle();
        if (!worker->contextConnection())
            server->createContextConnection(worker->topSite(), worker->serviceWorkerPageIdentifier(), worker->crossOriginEmbedderPolicy().value);
        server->runServiceWorkerIfNecessary(worker->identifier(), [weakThis](auto* contextConnection) {
            RefPtr protectedThis = weakThis.get();
            if (!protectedThis)
                return;
            if (!contextConnection)
                return protectedThis->didNotHandle();
            // The worker's thread starts on the main run loop's next turns.
            post([weakThis] {
                if (RefPtr protectedThis = weakThis.get())
                    protectedThis->startFetch();
            });
        });
    });
}

void ServiceWorkerLoad::startFetch()
{
    if (!m_target.isActive())
        return;
    RefPtr workers = m_workers.get();
    RefPtr connection = workers ? workers->serverConnection() : nullptr;
    RefPtr proxy = SWContextManager::singleton().serviceWorkerThreadProxy(*m_worker);
    if (!connection || !proxy)
        return didNotHandle();

    String clientIdentifier;
    if (m_options.mode != FetchOptions::Mode::Navigate) {
        if (auto identifier = m_options.clientIdentifier)
            clientIdentifier = identifier->toString();
    }
    String resultingClientIdentifier;
    if (auto identifier = m_options.resultingClientIdentifier)
        resultingClientIdentifier = identifier->toString();

    fetchesInProgress().set(m_fetch, *this);
    m_fetchStarted = true;
    auto request = m_request;
    auto referrer = request.httpReferrer();
    proxy->startFetch(connection->identifier(), m_fetch, FetchClient::create(m_fetch, *m_worker, connection->identifier()), WTF::move(request), WTF::move(referrer), FetchOptions { m_options }, false, WTF::move(clientIdentifier), WTF::move(resultingClientIdentifier));
}

void ServiceWorkerLoad::stop()
{
    if (RefPtr fallback = std::exchange(m_fallbackTask, nullptr)) {
        fallback->stop();
        return;
    }
    if (!m_target.isActive())
        return;
    m_target.clear();
    m_queue.clear();
    m_blobLoader = nullptr;
    if (m_fetchStarted && fetchesInProgress().remove(m_fetch)) {
        if (RefPtr proxy = SWContextManager::singleton().serviceWorkerThreadProxy(*m_worker)) {
            if (RefPtr workers = m_workers.get(); workers && workers->serverConnection())
                proxy->cancelFetch(workers->serverConnection()->identifier(), m_fetch);
        }
    }
}

bool ServiceWorkerLoad::convertToDownload(Download& download)
{
    if (m_fallbackTask)
        return m_fallbackTask->convertToDownload(download);
    if (!m_target.isActive())
        return false;
    m_target.convertToDownload(download);
    m_waitingForResponsePolicy = false;
    processQueue();
    return true;
}

void ServiceWorkerLoad::runOrQueue(Function<void()>&& task)
{
    if (m_waitingForResponsePolicy) {
        m_queue.append(WTF::move(task));
        return;
    }
    task();
}

void ServiceWorkerLoad::processQueue()
{
    while (!m_waitingForResponsePolicy && !m_queue.isEmpty())
        m_queue.takeFirst()();
}

void ServiceWorkerLoad::didReceiveResponse(ResourceResponse&& response)
{
    response.setSource(ResourceResponse::Source::ServiceWorker);
    runOrQueue([this, protectedThis = Ref { *this }, response = WTF::move(response)] mutable {
        if (!m_target.isActive())
            return;
        m_waitingForResponsePolicy = true;
        m_target.didReceiveResponse(WTF::move(response), [this, protectedThis = Ref { *this }](bool accepted) {
            m_waitingForResponsePolicy = false;
            if (!accepted) {
                stop();
                return;
            }
            processQueue();
        });
    });
}

void ServiceWorkerLoad::didReceiveData(Ref<SharedBuffer>&& data)
{
    runOrQueue([this, protectedThis = Ref { *this }, data = WTF::move(data)] mutable {
        m_target.didReceiveData(WTF::move(data));
    });
}

void ServiceWorkerLoad::didFinish()
{
    fetchesInProgress().remove(m_fetch);
    runOrQueue([this, protectedThis = Ref { *this }] {
        m_target.didFinish(NetworkLoadMetrics { });
    });
}

void ServiceWorkerLoad::didFail(const ResourceError& error)
{
    fetchesInProgress().remove(m_fetch);
    runOrQueue([this, protectedThis = Ref { *this }, error] {
        m_target.didFail(error);
    });
}

// A redirect from the worker (Response.redirect()): the page decides, then the
// new request goes the way loads without a worker go.
void ServiceWorkerLoad::didReceiveRedirection(ResourceResponse&& response)
{
    fetchesInProgress().remove(m_fetch);
    runOrQueue([this, protectedThis = Ref { *this }, response = WTF::move(response)] mutable {
        if (!m_target.isActive())
            return;
        response.setSource(ResourceResponse::Source::ServiceWorker);
        auto newRequest = m_request.redirectedRequest(response, false, ResourceRequest::ShouldSetHash::Yes);
        m_waitingForResponsePolicy = true;
        m_target.willSendRequest(WTF::move(newRequest), response, [this, protectedThis = Ref { *this }](ResourceRequest&& request) {
            m_waitingForResponsePolicy = false;
            m_queue.clear();
            if (request.isNull() || !m_target.isActive())
                return;
            m_request = WTF::move(request);
            didNotHandle();
        });
    });
}

void ServiceWorkerLoad::didNotHandle()
{
    fetchesInProgress().remove(m_fetch);
    RefPtr loader = m_target.loader();
    if (!loader)
        return;
    m_target.clear();
    m_queue.clear();
    RefPtr task = m_fallback ? m_fallback() : nullptr;
    if (!task) {
        loader->didFail(cannotShowURLError(m_request.url()));
        return;
    }
    m_fallbackTask = task;
    task->start();
}

// A body the worker gave as a blob (new Response(blob)), read in the worker's
// page as WebKit2 reads it.
class BlobBodyClient final : public FetchLoaderClient, public RefCounted<BlobBodyClient> {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(BlobBodyClient);
public:
    explicit BlobBodyClient(ServiceWorkerLoad& load)
        : m_load(load)
    {
    }
    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

private:
    void didReceiveData(const SharedBuffer& buffer) final
    {
        if (RefPtr load = m_load.get())
            load->didReceiveData(SharedBuffer::create(buffer.span()));
    }
    void didSucceed(const NetworkLoadMetrics&) final
    {
        if (RefPtr load = m_load.get())
            load->didFinish();
    }
    void didFail(const ResourceError& error) final
    {
        if (RefPtr load = m_load.get())
            load->didFail(error);
    }

    WeakPtr<ServiceWorkerLoad> m_load;
};

void ServiceWorkerLoad::loadBlob(const URL& blobURL)
{
    RefPtr proxy = m_worker ? SWContextManager::singleton().serviceWorkerThreadProxy(*m_worker) : nullptr;
    if (!proxy)
        return didFail(ResourceError { errorDomainWebKitInternal, 0, blobURL, "The service worker is gone"_s });
    Ref client = adoptRef(*new BlobBodyClient(*this));
    m_blobLoader = proxy->createBlobLoader(client, blobURL);
    if (!m_blobLoader)
        return didFail(ResourceError { errorDomainWebKitInternal, 0, blobURL, "The service worker's response body cannot be read"_s });
    m_blobClient = WTF::move(client);
}

ServiceWorkerLoad::~ServiceWorkerLoad()
{
    fetchesInProgress().remove(m_fetch);
}

// ---- Which loads go to a worker (WebSWServerConnection::createFetchTask) ----

RefPtr<LoadTask> Workers::createLoadTask(ResourceLoader& loader, Function<RefPtr<LoadTask>()>&& fallback)
{
    RefPtr server = m_server;
    RefPtr connection = m_serverConnection;
    if (!server || !connection)
        return nullptr;
    auto& options = loader.options();
    auto& request = loader.request();
    if (options.serviceWorkersMode == ServiceWorkersMode::None)
        return nullptr;
    if (!server->canHandleScheme(request.url().protocol()))
        return nullptr;

    std::optional<ServiceWorkerRegistrationIdentifier> registrationIdentifier;
    if (options.resultingClientIdentifier) {
        // A navigation, or a worker's script: the registration whose scope
        // holds it controls the client it makes.
        RefPtr frame = loader.frame();
        if (!frame)
            return nullptr;
        RefPtr document = frame->isMainFrame() ? nullptr : frame->document();
        auto topOrigin = document ? document->topOrigin().data() : SecurityOriginData::fromURL(request.url());
        // Registrations saved by an earlier session are read first.
        if (!server->isImportCompletedForOrigin(topOrigin))
            return ServiceWorkerLoad::createMatching(*this, loader, topOrigin, WTF::move(fallback));
        RefPtr registration = server->doRegistrationMatchingSync(topOrigin, request.url());
        if (!registration)
            return nullptr;
        registrationIdentifier = registration->identifier();
        std::optional<PageIdentifier> pageIdentifier;
        if (RefPtr page = frame->page())
            pageIdentifier = page->identifier();
        connection->controlClient(*registration, request, options, pageIdentifier, frame->frameID());
    } else {
        if (!options.serviceWorkerRegistrationIdentifier || isPotentialNavigationOrSubresourceRequest(options.destination))
            return nullptr;
        registrationIdentifier = *options.serviceWorkerRegistrationIdentifier;
    }

    RefPtr registration = server->getRegistration(*registrationIdentifier);
    RefPtr worker = registration ? registration->activeWorker() : nullptr;
    if (!worker || worker->hasTimedOutAnyFetchTasks())
        return nullptr;
    // A router rule sending the request to the network skips the worker.
    auto routerSource = worker->getRouterSource(options, request);
    if (auto* source = routerSource ? std::get_if<RouterSourceEnum>(&*routerSource) : nullptr; source && *source == RouterSourceEnum::Network)
        return nullptr;
    return ServiceWorkerLoad::create(*this, loader, *registrationIdentifier, WTF::move(fallback));
}

} // namespace

Ref<ServiceWorkerSession> ServiceWorkerSession::create(Session& session, const String& directory)
{
    return Workers::create(session, directory);
}

Session* sessionForServiceWorkerFrameLoaderClient(const LocalFrameLoaderClient& client)
{
    return serviceWorkerFrameLoaderClients().get(&client);
}

} // namespace ZeroG
