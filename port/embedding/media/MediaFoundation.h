#pragma once

// What ZeroG's two uses of Windows' Media Foundation share: MP4 files through a
// source reader (MediaFoundationFile.cpp) and H.264 and AAC frames from Media
// Source Extensions through the decoders themselves (MediaFoundationDecoders.cpp).
// Decoded video comes as NV12.

#include "media/MediaFile.h"

#if OS(WINDOWS)

struct IMFMediaType;
struct IMFSample;

namespace ZeroG::Media {

// MFStartup, once for the process; false where Windows has no Media
// Foundation ("N" editions).
bool startMediaFoundation();

// Where the picture is in a decoder's NV12 buffers, and how to convert it.
struct NV12Layout {
    int x { 0 };
    int y { 0 };
    int width { 0 }; // the display aperture (the coded size is in whole macroblocks)
    int height { 0 };
    unsigned codedHeight { 0 };
    int stride { 0 };
    ColorMatrix matrix { ColorMatrix::BT601 };
    bool fullRange { false };
};
// From a video output type; false without a frame size.
bool readNV12Layout(IMFMediaType*, NV12Layout&);
sk_sp<SkImage> convertNV12Sample(IMFSample*, const NV12Layout&);

} // namespace ZeroG::Media

#endif // OS(WINDOWS)
