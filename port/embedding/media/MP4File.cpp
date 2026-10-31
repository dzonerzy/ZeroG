// MP4 files with the codecs the engine decodes itself: VP9 and AV1 video, Opus
// audio (everywhere; H.264 and AAC files go to Media Foundation on Windows and
// do not play elsewhere). The moov's sample tables (MP4Stream.h) say where each
// sample is; samples are read as they are decoded, waiting for bytes still
// arriving like the other formats. A fragmented file (its samples in moofs) is
// read through the MSE parser instead, fragment by fragment.

#include "config.h"
#include "media/MediaFile.h"

#if ENABLE(VIDEO) && defined(ZEROG_MEDIA)

#include "media/Decoders.h"
#include "media/MP4Stream.h"
#include <deque>

namespace ZeroG::Media {

namespace {

constexpr int64_t nanosecondsPerSecond = 1'000'000'000;

long long toNanoseconds(const MediaTime& time)
{
    return static_cast<long long>(time.toDouble() * nanosecondsPerSecond);
}

bool isSystemCodec(const String& codecID)
{
    return codecID == "V_MPEG4/ISO/AVC"_s || codecID == "A_AAC"_s;
}

class MP4File final : public MediaFile, private StreamParser::Client {
public:
    explicit MP4File(MediaBytes& bytes)
        : m_bytes(bytes)
    {
    }

    bool open(bool leaveSystemCodecs, String& error)
    {
        Vector<uint8_t> movieBox;
        if (!readMovie(movieBox)) {
            error = "the MP4 file has no moov"_s;
            return false;
        }
        MP4Movie movie;
        if (!parseMP4Movie(movieBox.span(), movie)) {
            error = "damaged MP4 file (moov)"_s;
            return false;
        }
        // Files with H.264 or AAC are Media Foundation's (on Windows).
        if (leaveSystemCodecs) {
            for (auto& table : movie.tracks) {
                if (isSystemCodec(table.track.info.codecID)) {
                    error = "an MP4 file for the system's decoders"_s;
                    return false;
                }
            }
        }
        for (auto& table : movie.tracks) {
            auto& info = table.track.info;
            if (info.kind == StreamTrack::Kind::Video && !m_videoDecoder) {
                m_videoDecoder = createVideoDecoder(info.codecID, info.codecPrivate.span());
                if (m_videoDecoder) {
                    adopt(m_video, table);
                    m_info.hasVideo = true;
                    m_info.videoSize = { static_cast<int>(info.width), static_cast<int>(info.height) };
                }
            } else if (info.kind == StreamTrack::Kind::Audio && !m_audioDecoder) {
                unsigned channels = info.channels;
                unsigned sampleRate = info.sampleRate;
                m_audioDecoder = createAudioDecoder(info.codecID, info.codecPrivate.span(), channels, sampleRate);
                if (m_audioDecoder) {
                    adopt(m_audio, table);
                    m_info.hasAudio = true;
                    m_info.channels = channels;
                    m_info.sampleRate = sampleRate;
                }
            }
        }
        if (!m_info.hasAudio && !m_info.hasVideo) {
            error = "the MP4 file has no VP9, AV1 or Opus track"_s;
            return false;
        }
        // No sample tables: a fragmented file, read through its moofs.
        m_fragmented = (m_info.hasVideo && m_video.samples.isEmpty()) || (m_info.hasAudio && m_audio.samples.isEmpty());
        if (m_fragmented) {
            m_video.samples.clear();
            m_audio.samples.clear();
            m_parser = std::make_unique<MP4StreamParser>(static_cast<StreamParser::Client&>(*this));
        }
        m_info.duration = movie.duration;
        if (!m_info.duration.isValid() && !m_fragmented) {
            for (auto* track : { &m_video, &m_audio }) {
                if (!track->samples.isEmpty())
                    m_info.duration = std::max(m_info.duration.isValid() ? m_info.duration : MediaTime::zeroTime(), track->samples.last().time + track->lastDuration);
            }
        }
        return true;
    }

    size_t readAudio(float* out, size_t frames, MediaTime& time) final
    {
        if (!m_audioDecoder)
            return 0;
        while (m_audioOffset >= m_audioBuffer.size()) {
            auto* sample = nextSample(m_audio);
            if (!sample)
                return 0;
            m_audioBuffer.clear();
            m_audioOffset = 0;
            m_audioDecoder->decode(packet(*sample), m_audioBuffer);
            m_audioTime = sample->time;
            if (m_audioSeekTarget.isValid() && !m_audioBuffer.isEmpty()) {
                // The samples before the seek's target are dropped.
                size_t channels = m_info.channels;
                if (m_audioTime < m_audioSeekTarget) {
                    size_t skip = static_cast<size_t>((m_audioSeekTarget - m_audioTime).toDouble() * m_info.sampleRate) * channels;
                    m_audioOffset = std::min(skip, m_audioBuffer.size());
                    m_audioTime = m_audioTime + MediaTime(m_audioOffset / channels, m_info.sampleRate);
                }
                if (m_audioOffset < m_audioBuffer.size())
                    m_audioSeekTarget = MediaTime::invalidTime();
            }
        }
        size_t channels = m_info.channels;
        size_t count = std::min(frames, (m_audioBuffer.size() - m_audioOffset) / channels);
        time = m_audioTime;
        memcpy(out, m_audioBuffer.span().subspan(m_audioOffset).data(), count * channels * sizeof(float));
        m_audioOffset += count * channels;
        m_audioTime = m_audioTime + MediaTime(count, m_info.sampleRate);
        return count;
    }

    std::optional<DecodedFrame> readVideo() final
    {
        if (!m_videoDecoder)
            return std::nullopt;
        while (m_frames.empty()) {
            Vector<DecodedFrame> decoded;
            if (auto* sample = nextSample(m_video))
                m_videoDecoder->decode(packet(*sample), decoded);
            else if (!m_videoDrained) {
                // The end: what a reordering decoder still holds, and the
                // frame showing at a seek's target if it was the last.
                m_videoDrained = true;
                m_videoDecoder->drain(decoded);
                for (auto& frame : decoded)
                    show(WTF::move(frame));
                if (auto held = std::exchange(m_heldFrame, std::nullopt))
                    m_frames.push_back(WTF::move(*held));
                continue;
            } else
                return std::nullopt;
            for (auto& frame : decoded)
                show(WTF::move(frame));
        }
        auto frame = WTF::move(m_frames.front());
        m_frames.pop_front();
        return frame;
    }

    bool seek(const MediaTime& time) final
    {
        // A fragmented file is read up to the target first.
        if (m_fragmented) {
            while (!m_streamEnded && !covers(time))
                feed();
        }
        // Video from the keyframe before the target, audio from the sample
        // holding it.
        m_video.next = startIndex(m_video, time, true);
        m_audio.next = startIndex(m_audio, time, false);
        if (m_videoDecoder)
            m_videoDecoder->flush();
        if (m_audioDecoder)
            m_audioDecoder->flush();
        m_frames.clear();
        m_heldFrame = std::nullopt;
        m_videoDrained = false;
        m_videoSeekTarget = time;
        m_audioBuffer.clear();
        m_audioOffset = 0;
        m_audioTime = time;
        m_audioSeekTarget = time;
        return true;
    }

private:
    struct Sample {
        MediaTime time; // presentation
        uint64_t offset { 0 };
        uint32_t size { 0 };
        bool key { false };
        Vector<uint8_t> data; // a fragmented file's, parsed with its fragment
    };
    struct Track {
        uint32_t id { 0 };
        Vector<Sample> samples; // decode order
        size_t next { 0 };
        MediaTime lastDuration { MediaTime::zeroTime() };
    };

    void adopt(Track& track, const MP4Movie::Table& table)
    {
        track.id = table.track.id;
        auto timescale = table.track.timescale;
        track.samples.reserveInitialCapacity(table.samples.size());
        for (auto& sample : table.samples)
            track.samples.append({ MediaTime(sample.presentationTime, timescale), sample.offset, sample.size, sample.key, { } });
        if (table.samples.size() >= 2)
            track.lastDuration = MediaTime(table.samples.last().decodeTime - table.samples[table.samples.size() - 2].decodeTime, timescale);
    }

    // The top-level boxes up to the moov (at the start or after the mdat).
    bool readMovie(Vector<uint8_t>& movie)
    {
        uint64_t position = 0;
        for (int boxes = 0; boxes < 64; ++boxes) {
            std::array<uint8_t, 16> header { };
            size_t length = m_bytes.read(position, header);
            if (length < 8)
                return false;
            uint64_t size = (uint64_t { header[0] } << 24) | (header[1] << 16) | (header[2] << 8) | header[3];
            bool isMovie = !memcmp(header.data() + 4, "moov", 4);
            size_t headerSize = 8;
            if (size == 1) {
                if (length < 16)
                    return false;
                size = 0;
                for (int i = 8; i < 16; ++i)
                    size = (size << 8) | header[i];
                headerSize = 16;
            }
            if (!size || size < headerSize)
                return false; // a box to the end of the file, or garbage
            if (isMovie) {
                if (size > 256 * 1024 * 1024)
                    return false;
                movie.resize(size - headerSize);
                return m_bytes.read(position + headerSize, movie.mutableSpan()) == movie.size();
            }
            position += size;
        }
        return false;
    }

    // StreamParser::Client: a fragmented file's frames.
    void didParseInitializationSegment(Vector<StreamTrack>&&, const MediaTime&) final { }
    void didParseFrame(StreamFrame&& frame) final
    {
        for (auto* track : { &m_video, &m_audio }) {
            if (track->id == frame.track && (track == &m_video ? m_info.hasVideo : m_info.hasAudio)) {
                track->samples.append({ frame.time, 0, 0, frame.key, WTF::move(frame.data) });
                track->lastDuration = frame.duration;
                return;
            }
        }
    }

    void feed()
    {
        std::array<uint8_t, 256 * 1024> chunk;
        size_t length = m_bytes.read(m_streamPosition, chunk);
        if (!length) {
            m_streamEnded = true;
            return;
        }
        m_streamPosition += length;
        if (!m_parser->append(std::span<const uint8_t> { chunk }.first(length)))
            m_streamEnded = true; // damaged: what was parsed plays
    }

    bool covers(const MediaTime& time) const
    {
        for (auto* track : { &m_video, &m_audio }) {
            bool present = track == &m_video ? m_info.hasVideo : m_info.hasAudio;
            if (present && (track->samples.isEmpty() || track->samples.last().time < time))
                return false;
        }
        return true;
    }

    const Sample* nextSample(Track& track)
    {
        while (track.next >= track.samples.size()) {
            if (!m_fragmented || m_streamEnded)
                return nullptr;
            feed();
        }
        auto& sample = track.samples[track.next++];
        if (!m_fragmented) {
            // Read from the file as it is decoded.
            m_sampleData.resize(sample.size);
            if (m_bytes.read(sample.offset, m_sampleData.mutableSpan()) != sample.size)
                return nullptr;
        }
        return &sample;
    }

    Packet packet(const Sample& sample) const
    {
        return { m_fragmented ? sample.data : m_sampleData, toNanoseconds(sample.time), sample.key };
    }

    static size_t startIndex(const Track& track, const MediaTime& time, bool keyframe)
    {
        size_t start = 0;
        for (size_t i = 0; i < track.samples.size(); ++i) {
            if ((!keyframe || track.samples[i].key) && track.samples[i].time <= time)
                start = i;
        }
        return start;
    }

    // After a seek, frames before the target are not shown, except the one
    // showing at it.
    void show(DecodedFrame&& frame)
    {
        if (m_videoSeekTarget.isValid()) {
            if (frame.time <= m_videoSeekTarget) {
                m_heldFrame = WTF::move(frame);
                return;
            }
            m_videoSeekTarget = MediaTime::invalidTime();
            if (auto held = std::exchange(m_heldFrame, std::nullopt))
                m_frames.push_back(WTF::move(*held));
        }
        m_frames.push_back(WTF::move(frame));
    }

    MediaBytes& m_bytes;
    Track m_video;
    Track m_audio;
    std::unique_ptr<VideoDecoder> m_videoDecoder;
    std::unique_ptr<AudioDecoder> m_audioDecoder;
    Vector<uint8_t> m_sampleData;

    bool m_fragmented { false };
    std::unique_ptr<MP4StreamParser> m_parser;
    uint64_t m_streamPosition { 0 };
    bool m_streamEnded { false };

    std::deque<DecodedFrame> m_frames;
    std::optional<DecodedFrame> m_heldFrame;
    bool m_videoDrained { false };
    MediaTime m_videoSeekTarget { MediaTime::invalidTime() };

    Vector<float> m_audioBuffer;
    size_t m_audioOffset { 0 };
    MediaTime m_audioTime { MediaTime::zeroTime() };
    MediaTime m_audioSeekTarget { MediaTime::invalidTime() };
};

} // namespace

std::unique_ptr<MediaFile> openMP4(MediaBytes& bytes, bool leaveSystemCodecs, String& error)
{
    auto file = std::make_unique<MP4File>(bytes);
    if (!file->open(leaveSystemCodecs, error))
        return nullptr;
    return file;
}

} // namespace ZeroG::Media

#endif // ENABLE(VIDEO) && defined(ZEROG_MEDIA)
