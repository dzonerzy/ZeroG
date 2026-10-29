// Prefix header for ZeroG's own sources built against WebKit internals, the same
// set of includes TestWebKitAPI's config.h uses for its WebCore tests.
#pragma once

#if defined(HAVE_CONFIG_H) && HAVE_CONFIG_H && defined(BUILDING_WITH_CMAKE)
#include "cmakeconfig.h"
#endif

#include <wtf/Platform.h>

#include <JavaScriptCore/JSExportMacros.h>
#include <WebCore/PlatformExportMacros.h>
#include <pal/ExportMacros.h>

// WebCore's own config.h provides these to every WebCore header.
#ifdef __cplusplus
#include <wtf/FastMalloc.h>
#include <wtf/TZoneMalloc.h>
#endif
