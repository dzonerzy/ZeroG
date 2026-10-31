#pragma once

// A byte stream as Media Source Extensions append it, in chunks of any size:
// initialization segments (the tracks) and media segments (coded frames).
// Formats: WebM (WebMStream.h) and fragmented MP4 (MP4Stream.h). Main thread.

#include <span>
#include <wtf/MediaTime.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace ZeroG::Media {

struct StreamTrack {
    enum class Kind : uint8_t { Audio, Video };
    uint64_t number { 0 };
    Kind kind { Kind::Video };
    // Matroska's codec IDs for every format ("V_VP9", "V_MPEG4/ISO/AVC",
    // "A_OPUS", "A_AAC", ...), which the decoders take (Decoders.h).
    String codecID;
    // The decoder's configuration: Matroska's CodecPrivate (avcC for H.264,
    // AudioSpecificConfig for AAC, OpusHead for Opus, Vorbis's headers).
    Vector<uint8_t> codecPrivate;
    MediaTime defaultDuration { MediaTime::invalidTime() };
    // Video.
    unsigned width { 0 };
    unsigned height { 0 };
    // Audio.
    unsigned channels { 0 };
    unsigned sampleRate { 0 };
};

struct StreamFrame {
    uint64_t track { 0 };
    MediaTime time; // presentation
    MediaTime decodeTime { MediaTime::invalidTime() }; // invalid: the same
    MediaTime duration;
    bool key { false };
    Vector<uint8_t> data;
};

class StreamParser {
public:
    class Client {
    public:
        virtual ~Client() = default;
        // An initialization segment: its tracks and the duration it gives
        // (invalid when it has none).
        virtual void didParseInitializationSegment(Vector<StreamTrack>&&, const MediaTime& duration) = 0;
        // A coded frame, in decode order per track.
        virtual void didParseFrame(StreamFrame&&) = 0;
    };

    virtual ~StreamParser() = default;
    // Parses what the bytes complete; false when they are damaged or not
    // this format.
    virtual bool append(std::span<const uint8_t>) = 0;
    // Forgets a partial element (MSE's "reset parser state"); the next bytes
    // start an initialization or a media segment.
    virtual void reset() = 0;
};

} // namespace ZeroG::Media
