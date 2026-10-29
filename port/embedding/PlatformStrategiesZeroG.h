#pragma once

namespace ZeroG {

// Installs WebCore's platform strategies once per process. Loading, pasteboard,
// media and blobs are stubs until the loader and network layer lands.
void installPlatformStrategies();

} // namespace ZeroG
