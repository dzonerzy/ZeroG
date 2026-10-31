# Networking, storage and resources

The network stack is **the same on all platforms: curl with BoringSSL**. curl is already integrated in WebCore (the classes of `platform/network/curl`, including the SQLite cookie jar), links statically without problems and offers HTTP/2 and, later, HTTP/3.

## Stack

| Component | Role | Notes |
| --- | --- | --- |
| curl (multi interface) | HTTP/1.1, HTTP/2, connection pool | Runs on the engine's network thread |
| BoringSSL | TLS 1.3 | Fast, used by Chrome, suited to static linking; OpenSSL 3 as alternative |
| nghttp2 | HTTP/2 with multiplexing | — |
| ngtcp2 and nghttp3 | HTTP/3 over QUIC | Optional, version 2 |
| c-ares | Asynchronous DNS | Optional; curl's threaded resolver is enough for version 1 |
| zlib, brotli, zstd | Response decompression | — |

## Performance

In an application almost all resources come from `app://`, served from memory without touching the network. For external requests (APIs, remote images) what matters most is: connection reuse, HTTP/2 multiplexing, TLS session resumption and DNS cache. They are all curl features to configure, not to write.

## Certificates

BoringSSL does not use the system stores on its own. The engine uses the system root CAs: the bundle from the standard paths on Linux, the Windows store via curl (`CURLSSLOPT_NATIVE_CA`), the Keychain on macOS (to be done with the macOS port). The application adds its own CAs with `zg_runtime_add_ca_certificates` (PEM, also a bundle embedded in the binary for embedded environments); they also apply on Windows and to `wss://` (patch 0022). A rejected certificate gives a readable error ("certificate not trusted").

## Resource loader

The embedding layer implements loading: `app://` and the custom schemes respond directly on the main thread; `http` and `https` go to curl on the network thread, and the responses come back to the main thread in chunks, streaming.

## HTTP cache

WebKit's disk cache lives in WebKit2's network process, so it is not available. The engine has its own disk cache (`loader/HTTPCache`) next to WebCore's in-memory one: per session in the data directory (none in ephemeral contexts), with a configurable limit (`zg_context_config.http_cache_limit`), HTTP freshness and validation (max-age, ETag, Last-Modified, `Vary`), eviction of the least used.

## Cache API

The Cache API (`caches`, used by pages and workers) also lives in the network process in WebKit2. In the standard, dev and debug profiles the engine has its own storage (`storage/CacheStorageZeroG`) on the main thread: one file per origin in `<data_dir>/CacheStorage/`, rewritten in full at every change on a WorkQueue, no file in ephemeral contexts. Matching follows `DOMCacheEngine::queryCacheMatch` (with `Vary`), opaque responses respect the Cross-Origin-Resource-Policy, the quota is 512 MB per origin. In the minimal profile `caches` does not exist.

## Service workers

In WebKit2 service workers cross three processes: the page's connection (`WebSWClientConnection`) talks to an `SWServer` in the network process, which runs each worker in a web process of its site and sends it the fetches of the pages it controls. In the standard, dev and debug profiles the engine keeps everything in its single process (`serviceworkers/ServiceWorkersZeroG`): per session a WebCore `SWServer` with one connection per side, the workers on WebCore's threads each in its own page (with the session's storage, cookies and Cache API), and the loader that asks the session before going to curl or to a scheme handler. Calls between the sides go through the main run loop, like IPC messages: no side sees the other's responses inside its own call.

- Navigations and worker scripts in the scope of a registration follow it; the subresources of controlled clients go to the worker; whatever the worker does not respond to (no `respondWith`) proceeds as without a worker. Router rules that send to the network skip the worker.
- Registrations in the data directory (`ServiceWorkers/`, WebCore's SQLite database on a work queue), in memory for ephemeral contexts; the next session on the same directory finds them again from the first navigation.
- WebCore assumes one process per session; ZeroG has multiple sessions in the same process, so the service worker connection is looked up per session (patch 0034).
- Updates: a controlled navigation (like `update()` and the functional events) makes the server recheck the script, which it re-downloads with the session's network loader and installs the new version if the bytes have changed (WebKit2's soft update). For this the loader has a third kind of destination besides pages and downloads (`LoadClient`).
- Navigation preload: if the registration enables it, the navigation starts toward the network while the worker starts, with the `Service-Worker-Navigation-Preload` header; the worker sees it as `event.preloadResponse`, and if it uses it (or does not respond) the page receives that response, without a second request.
- Only http and https, as in WebKit. No push, notifications, background fetch nor `clients.openWindow`: an application has no other browser windows, and push and notifications would require a public API (the application delivering the messages and showing the notifications).
- In the minimal profile `navigator.serviceWorker` does not exist.

## Custom `app://` scheme

- The frontend's assets (HTML, JS, CSS, images, fonts) are embedded in the binary as a compressed archive, generated at compile time.
- The scheme responds from the archive with the correct MIME type and support for range requests for audio and video.
- The scheme must be registered as **secure** and with CORS enabled, otherwise APIs like `crypto.subtle` and cross-origin `fetch` do not work.
- The application can register additional schemes with a callback (`zg_context_register_scheme`), for example to serve files generated at runtime.

## Dev mode

In development the view loads the frontend's dev server directly (for example Vite on `http://localhost:5173`), and the switch to `app://` happens only in the release build. Hot reload goes through HTTP and **WebSocket**: without WebKit2's network process the WebSocket backend is provided by the embedding layer, on top of curl (`loader/WebSocketChannelZeroG`), with subprotocols, handshake cookies, text and binary messages and `wss://`. The dev server is trusted only in the dev profile (`dev_server_url`).

## Storage

- Cookies with WebCore's SQLite cookie jar; localStorage and IndexedDB with in-process implementations in the embedding layer, on the WebKitLegacy model.
- Per-application data directory in the OS's standard path (`$XDG_DATA_HOME`, `%LOCALAPPDATA%`, `~/Library/Application Support`).
- Optional ephemeral mode: no data written to disk.
