#pragma once

#include <wtf/UniqueRef.h>

namespace WebCore {
class FrameLoader;
class LocalFrameLoaderClient;
}

namespace ZeroG {

class View;

// The frame loader client of a view's frames: commits loaded data, decides
// navigation and response policy, creates the frame view of each committed page,
// and reports titles, load completion and new window objects to the view.
UniqueRef<WebCore::LocalFrameLoaderClient> createFrameLoaderClient(WebCore::FrameLoader&, View&);

// The page can outlive its view while something still references it.
void detachFrameLoaderClient(WebCore::LocalFrameLoaderClient&);

} // namespace ZeroG
