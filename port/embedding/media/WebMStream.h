#pragma once

// A WebM byte stream as Media Source Extensions append it (the "WebM Byte
// Stream Format"): initialization segments (the EBML header, a Segment's Info
// and Tracks) and media segments (Clusters), in chunks of any size. Parsed
// incrementally with libwebm's webm_parser: what a chunk completes is reported,
// the rest waits for the next one. Main thread.

#include "media/StreamParser.h"
#include <memory>

namespace webm {
class WebmParser;
}

namespace ZeroG::Media {

class WebMStreamParser final : public StreamParser {
public:
    explicit WebMStreamParser(Client&);
    ~WebMStreamParser();

    bool append(std::span<const uint8_t>) final;
    void reset() final;

    class Callback;
    class Reader;

private:
    Client& m_client;
    std::unique_ptr<webm::WebmParser> m_parser;
    std::unique_ptr<Callback> m_callback;
    std::unique_ptr<Reader> m_reader;
};

} // namespace ZeroG::Media
