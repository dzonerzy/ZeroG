#pragma once

// Service workers (docs/design/networking-storage-resources.md, "Service workers"; standard, dev and debug profiles).
// WebKit2 splits them over three processes: the page's connection
// (WebSWClientConnection) talks to an SWServer in the network process, which
// runs each worker in a web process of its site (WebSWContextManagerConnection)
// and sends it the fetches of the pages it controls (ServiceWorkerFetchTask).
// Here every piece is in the one process: per session an SWServer with one
// connection each way, the workers on WebCore's service worker threads, each in
// a page of its own, and the loader asking the session before going to curl or
// a scheme handler. Calls between the sides go through the main run loop, as
// messages would, so each side sees the other's answers later, never inside
// its own call. Registrations persist in the session's directory
// (ServiceWorkers/), in memory for ephemeral sessions.

#include <wtf/Forward.h>
#include <wtf/RefCounted.h>

namespace WebCore {
class LocalFrameLoaderClient;
class ResourceLoader;
}

namespace ZeroG {

class LoadTask;
class Session;

class ServiceWorkerSession : public RefCounted<ServiceWorkerSession> {
public:
    // `directory` empty: registrations in memory only.
    static Ref<ServiceWorkerSession> create(Session&, const String& directory);
    virtual ~ServiceWorkerSession() = default;

    // The session's owner is gone: workers stop, the server closes.
    virtual void close() = 0;

    // A load a service worker answers: a navigation or worker script in a
    // registration's scope, or a subresource of a client the worker controls.
    // Null when none does; when the worker does not answer (no respondWith),
    // the task makes `fallback`, the load it would have been.
    virtual RefPtr<LoadTask> createLoadTask(WebCore::ResourceLoader&, Function<RefPtr<LoadTask>()>&& fallback) = 0;

protected:
    ServiceWorkerSession() = default;
};

// The session of a service worker's page (its loads, cookies and storage), or
// null for any other frame loader client.
Session* sessionForServiceWorkerFrameLoaderClient(const WebCore::LocalFrameLoaderClient&);

} // namespace ZeroG
