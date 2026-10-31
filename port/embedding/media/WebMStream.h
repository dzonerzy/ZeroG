#pragma once

// A WebM byte stream as Media Source Extensions append it (the "WebM Byte
// Stream Format"): initialization segments (the EBML header, a Segment's Info
// and Tracks) and media segments (Clusters), in chunks of any size. Parsed
// incrementally with libwebm's webm_parser: what a chunk completes is reported,
// the rest waits for the next one. Main thread.

#include <memory>
#include <span>
#include <wtf/MediaTime.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace webm {
class WebmParser;
}

namespace ZeroG::Media {

struct StreamTrack {
    enum class Kind : uint8_t { Audio, Video };
    uint64_t number { 0 };
    Kind kind { Kind::Video };
    String codecID; // Matroska's ("V_VP9", "A_OPUS", ...)
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
    MediaTime time;
    MediaTime duration;
    bool key { false };
    Vector<uint8_t> data;
};

class WebMStreamParser {
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

    explicit WebMStreamParser(Client&);
    ~WebMStreamParser();

    // Parses what the bytes complete; false when they are not WebM.
    bool append(std::span<const uint8_t>);
    // Forgets a partial element (MSE's "reset parser state"); the next bytes
    // start an initialization or a media segment.
    void reset();

    class Callback;
    class Reader;

private:
    Client& m_client;
    std::unique_ptr<webm::WebmParser> m_parser;
    std::unique_ptr<Callback> m_callback;
    std::unique_ptr<Reader> m_reader;
};

} // namespace ZeroG::Media
