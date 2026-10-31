#pragma once

// The decoders of WebM's codecs (WebMFile.cpp), shared by WebM files and Media
// Source Extensions' WebM byte streams: VP8 and VP9 (libvpx), AV1 (dav1d), Opus
// (libopus), Vorbis (libvorbis). Decode thread.

#include "media/MediaFile.h"
#include <wtf/Vector.h>
#include <wtf/text/StringView.h>

namespace ZeroG::Media {

struct Packet {
    Vector<uint8_t> data;
    long long time { 0 }; // nanoseconds
    bool key { false };
};

class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;
    // Decodes a packet; the frames it completed, in order.
    virtual void decode(const Packet&, Vector<DecodedFrame>& out) = 0;
    virtual void flush() = 0;
};

class AudioDecoder {
public:
    virtual ~AudioDecoder() = default;
    // Appends the packet's interleaved float samples.
    virtual void decode(const Packet&, Vector<float>& out) = 0;
    virtual void flush() = 0;
};

// A decoder for a Matroska codec ID ("V_VP8", "V_VP9", "V_AV1"); null for
// other codecs.
std::unique_ptr<VideoDecoder> createVideoDecoder(StringView codecID);
// "A_OPUS" (the codec private data is OpusHead) or "A_VORBIS" (the three
// Xiph-laced headers); null for other codecs or damaged headers. Gives the
// decoded audio's channels and rate (`sampleRate` is the track's on input).
std::unique_ptr<AudioDecoder> createAudioDecoder(StringView codecID, std::span<const uint8_t> codecPrivate, unsigned& channels, unsigned& sampleRate);

} // namespace ZeroG::Media
