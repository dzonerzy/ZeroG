#pragma once

// A fragmented MP4 byte stream as Media Source Extensions append it (the "ISO
// BMFF Byte Stream Format"): an initialization segment (ftyp, moov with its
// tracks' sample descriptions and mvex) and media segments (moof with the
// samples' times, sizes and flags, then the mdat holding them). Whole boxes are
// parsed as they complete; the rest waits for the next append. Codecs: H.264
// and AAC (decoded on Windows), VP9, AV1 and Opus (everywhere). Main thread.

#include "media/StreamParser.h"

namespace ZeroG::Media {

class MP4StreamParser final : public StreamParser {
public:
    explicit MP4StreamParser(Client&);

    bool append(std::span<const uint8_t>) final;
    void reset() final;

    struct Track {
        uint32_t id { 0 };
        uint32_t timescale { 0 };
        uint32_t defaultDuration { 0 };
        uint32_t defaultSize { 0 };
        uint32_t defaultFlags { 0 };
        int64_t nextDecodeTime { 0 }; // where the last fragment ended
        // The edit list's start, taken off every time as browsers do (encoders
        // shift B-frames' times and audio's priming this way).
        int64_t timeOffset { 0 };
        StreamTrack info;
    };

private:
    bool parseMovie(std::span<const uint8_t>);
    bool parseFragment(std::span<const uint8_t> fromMovieFragment, size_t movieFragmentSize);
    Track* track(uint32_t id);

    Client& m_client;
    Vector<uint8_t> m_buffer; // bytes not parsed yet
    uint64_t m_bufferPosition { 0 }; // the stream offset of m_buffer's first byte
    Vector<Track> m_tracks;
};

} // namespace ZeroG::Media
