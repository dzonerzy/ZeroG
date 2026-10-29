#pragma once

// WebSockets without the network process (ZeroG.md, "Dev mode": the dev
// server's hot reload runs over one). WebKit splits a socket between the web
// process (WebSocketChannel: the page's side) and the network process
// (WebSocketTaskCurl: handshake and frames over a CurlStream); here one channel
// does both on the engine thread.

#include <WebCore/SocketProvider.h>

namespace ZeroG {

Ref<WebCore::SocketProvider> createSocketProvider();

} // namespace ZeroG
