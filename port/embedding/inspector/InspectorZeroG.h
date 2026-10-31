#pragma once

// The Web Inspector (docs/design/build-system-static-linking.md, "Build profiles": dev and debug only, where
// ZEROG_INSPECTOR is defined). Each view's page gets a backend client. The
// frontend (WebInspectorUI's resources, compiled in) runs in a second view the
// embedder creates and shows (ViewClient::createInspectorView), in a session of
// its own serving the resources under inspector://. It talks to a
// LegacyWebPageInspectorController, as WebKitLegacy's frontend does, with one
// target: the page. Frames are targets of their own only with Site Isolation
// (WebKit2 creates them only then), which a single-process engine does not
// have; the page's agents cover its frames. With ENABLE(REMOTE_INSPECTOR) every view is also
// a debuggable for the remote inspector's socket server, so pages can be
// inspected from another machine's WebKit frontend.

#include <cstdint>
#include <memory>
#include <wtf/Forward.h>

namespace WebCore {
class GraphicsContext;
class InspectorBackendClient;
}

namespace ZeroG {

class Session;
class View;

class ViewInspector {
public:
    // Whether this build has the inspector.
    static bool isAvailable();
    // For the view's page configuration; null without the inspector.
    static std::unique_ptr<WebCore::InspectorBackendClient> createBackendClient(View&);
    // Once the page exists; null without the inspector.
    static std::unique_ptr<ViewInspector> create(View&);
    virtual ~ViewInspector() = default;

    // The view is going away: its backend client stops reaching it.
    static void detachBackendClient(WebCore::InspectorBackendClient&);

    // Opens the frontend, or brings it to the front. False if the embedder shows
    // none (no createInspectorView).
    virtual bool show() = 0;
    virtual void close() = 0;
    // The frontend's view while it is open (the embedder owns it).
    virtual View* frontendView() const = 0;

    // The overlay of the node being inspected, over the page's contents.
    virtual void paintHighlight(WebCore::GraphicsContext&) = 0;

    // The page's title or URL changed (the remote inspector's listing shows them).
    virtual void pageChanged() = 0;
};

// The session the frontend's views belong to: inspector:// serves its resources.
Session& inspectorSession();

// The remote inspector's socket server on `address`:`port` (dev and debug, with
// ENABLE(REMOTE_INSPECTOR)). False if it is not available or cannot listen.
bool startRemoteInspectorServer(const String& address, uint16_t port);

} // namespace ZeroG
