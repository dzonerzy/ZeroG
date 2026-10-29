#pragma once

namespace ZeroG {

// Installs WebCore's platform strategies once per process: loading and blobs go
// to the resource loader (Loader.h); pasteboard and media are still stubs.
void installPlatformStrategies();

} // namespace ZeroG
