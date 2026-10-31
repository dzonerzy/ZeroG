#pragma once

// The Cache API (window.caches and workers' caches; standard, dev and debug
// profiles) in process: WebKit2 keeps it in the network process
// (NetworkProcess/storage/CacheStorage*), WebKitLegacy only has WebCore's
// dummy. One engine per session: each origin's caches persist in a file under
// the session's directory (CacheStorage/), or stay in memory for an ephemeral
// session. Main thread (workers reach it through WebCore's
// WorkerCacheStorageConnection).

#include <WebCore/CacheStorageProvider.h>

namespace ZeroG {

// A session's cache storage, for its pages' configurations.
Ref<WebCore::CacheStorageProvider> createCacheStorageProvider(const String& directory);

} // namespace ZeroG
