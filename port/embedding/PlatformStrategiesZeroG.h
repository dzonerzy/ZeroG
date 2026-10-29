#pragma once

namespace ZeroG {

// Installs WebCore's platform strategies once per process: loading goes to the
// resource loader (Loader.h); pasteboard, media and blobs are still stubs.
void installPlatformStrategies();

} // namespace ZeroG
