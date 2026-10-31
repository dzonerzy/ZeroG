#include "config.h"
#include "media/WebMStream.h"

#if ENABLE(MEDIA_SOURCE) && defined(ZEROG_MEDIA)

#include <wtf/HashMap.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <opus/opus.h>
#include <webm/callback.h>
#include <webm/reader.h>
#include <webm/webm_parser.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace ZeroG::Media {

namespace {

constexpr int64_t nanosecondsPerSecond = 1'000'000'000;

} // namespace

// The appended bytes not parsed yet. Reads past them would block: the parser
// stops there and resumes with the next append.
class WebMStreamParser::Reader final : public webm::Reader {
public:
    void append(std::span<const uint8_t> bytes)
    {
        if (m_offset) {
            m_data.removeAt(0, m_offset);
            m_offset = 0;
        }
        m_data.append(bytes);
    }

    void clear()
    {
        m_data.clear();
        m_offset = 0;
    }

    bool hasData() const { return m_offset < m_data.size(); }

    webm::Status Read(std::size_t count, std::uint8_t* buffer, std::uint64_t* read) final
    {
        size_t available = std::min<size_t>(count, m_data.size() - m_offset);
        *read = available;
        if (!available)
            return webm::Status(webm::Status::kWouldBlock);
        memcpy(buffer, m_data.span().subspan(m_offset).data(), available);
        m_offset += available;
        m_position += available;
        return webm::Status(available == count ? webm::Status::kOkCompleted : webm::Status::kOkPartial);
    }

    webm::Status Skip(std::uint64_t count, std::uint64_t* skipped) final
    {
        size_t available = static_cast<size_t>(std::min<uint64_t>(count, m_data.size() - m_offset));
        *skipped = available;
        if (!available)
            return webm::Status(webm::Status::kWouldBlock);
        m_offset += available;
        m_position += available;
        return webm::Status(available == count ? webm::Status::kOkCompleted : webm::Status::kOkPartial);
    }

    std::uint64_t Position() const final { return m_position; }

private:
    Vector<uint8_t> m_data;
    size_t m_offset { 0 };
    uint64_t m_position { 0 };
};

class WebMStreamParser::Callback final : public webm::Callback {
public:
    explicit Callback(WebMStreamParser::Client& client)
        : m_client(client)
    {
    }

    // After each append: an initialization segment whose Tracks ended, and
    // the frames held back for their duration.
    void didFeed(uint64_t position)
    {
        if (m_tracksEnd && position >= m_tracksEnd)
            reportInitializationSegment();
        for (auto& held : m_held.values())
            emit(WTF::move(held), true);
        m_held.clear();
    }

    // A reset: a partial frame or block is dropped.
    void reset()
    {
        m_frameInProgress = false;
        m_frame.clear();
        m_inBlockGroup = false;
        m_groupFrames.clear();
        for (auto& held : m_held.values())
            emit(WTF::move(held), true);
        m_held.clear();
    }

    webm::Status OnSegmentBegin(const webm::ElementMetadata&, webm::Action* action) final
    {
        *action = webm::Action::kRead;
        m_tracks.clear();
        m_duration = MediaTime::invalidTime();
        m_timecodeScale = 1'000'000;
        m_tracksEnd = 0;
        m_tracksPending = false;
        return webm::Status(webm::Status::kOkCompleted);
    }

    webm::Status OnElementBegin(const webm::ElementMetadata& metadata, webm::Action* action) final
    {
        *action = webm::Action::kRead;
        if (metadata.id == webm::Id::kTracks) {
            m_tracks.clear();
            bool sized = metadata.size != webm::kUnknownElementSize && metadata.position != webm::kUnknownElementPosition && metadata.header_size != webm::kUnknownHeaderSize;
            m_tracksEnd = sized ? metadata.position + metadata.header_size + metadata.size : 0;
            m_tracksPending = true;
        } else if (m_tracksPending && metadata.id != webm::Id::kTrackEntry && isSegmentChild(metadata.id))
            reportInitializationSegment();
        // Not needed to play: skipped rather than parsed and kept.
        if (metadata.id == webm::Id::kCues || metadata.id == webm::Id::kTags || metadata.id == webm::Id::kChapters)
            *action = webm::Action::kSkip;
        return webm::Status(webm::Status::kOkCompleted);
    }

    webm::Status OnInfo(const webm::ElementMetadata&, const webm::Info& info) final
    {
        m_timecodeScale = info.timecode_scale.value() ? info.timecode_scale.value() : 1'000'000;
        if (info.duration.is_present() && info.duration.value() > 0)
            m_duration = MediaTime::createWithDouble(info.duration.value() * m_timecodeScale / nanosecondsPerSecond);
        return webm::Status(webm::Status::kOkCompleted);
    }

    webm::Status OnTrackEntry(const webm::ElementMetadata&, const webm::TrackEntry& entry) final
    {
        StreamTrack track;
        track.number = entry.track_number.value();
        auto type = entry.track_type.value();
        if (type != webm::TrackType::kVideo && type != webm::TrackType::kAudio)
            return webm::Status(webm::Status::kOkCompleted);
        track.kind = type == webm::TrackType::kVideo ? StreamTrack::Kind::Video : StreamTrack::Kind::Audio;
        track.codecID = String::fromLatin1(entry.codec_id.value().c_str());
        auto& codecPrivate = entry.codec_private.value();
        track.codecPrivate = Vector<uint8_t>(std::span<const uint8_t> { codecPrivate.data(), codecPrivate.size() });
        if (entry.default_duration.is_present() && entry.default_duration.value())
            track.defaultDuration = MediaTime(entry.default_duration.value(), nanosecondsPerSecond);
        if (track.kind == StreamTrack::Kind::Video) {
            auto& video = entry.video.value();
            track.width = video.pixel_width.value();
            track.height = video.pixel_height.value();
            if (video.display_width.is_present() && video.display_height.is_present() && video.display_width.value() && video.display_height.value()) {
                track.width = video.display_width.value();
                track.height = video.display_height.value();
            }
        } else {
            auto& audio = entry.audio.value();
            track.channels = audio.channels.value();
            track.sampleRate = static_cast<unsigned>(audio.sampling_frequency.value());
        }
        m_tracks.append(WTF::move(track));
        return webm::Status(webm::Status::kOkCompleted);
    }

    webm::Status OnClusterBegin(const webm::ElementMetadata&, const webm::Cluster& cluster, webm::Action* action) final
    {
        *action = webm::Action::kRead;
        if (m_tracksPending)
            reportInitializationSegment();
        m_clusterTimecode = cluster.timecode.value();
        return webm::Status(webm::Status::kOkCompleted);
    }

    webm::Status OnSimpleBlockBegin(const webm::ElementMetadata&, const webm::SimpleBlock& block, webm::Action* action) final
    {
        *action = webm::Action::kRead;
        m_inBlockGroup = false;
        beginBlock(block, block.is_key_frame);
        return webm::Status(webm::Status::kOkCompleted);
    }

    webm::Status OnBlockGroupBegin(const webm::ElementMetadata&, webm::Action* action) final
    {
        *action = webm::Action::kRead;
        m_inBlockGroup = true;
        m_groupFrames.clear();
        return webm::Status(webm::Status::kOkCompleted);
    }

    webm::Status OnBlockBegin(const webm::ElementMetadata&, const webm::Block& block, webm::Action* action) final
    {
        *action = webm::Action::kRead;
        beginBlock(block, false); // a BlockGroup's keyframe flag is known at its end
        return webm::Status(webm::Status::kOkCompleted);
    }

    webm::Status OnBlockGroupEnd(const webm::ElementMetadata&, const webm::BlockGroup& group) final
    {
        bool key = group.references.empty();
        bool single = m_groupFrames.size() == 1;
        for (auto& frame : m_groupFrames) {
            frame.key = key;
            if (single && group.duration.is_present())
                frame.duration = ticksToTime(group.duration.value());
            deliver(WTF::move(frame));
        }
        m_groupFrames.clear();
        m_inBlockGroup = false;
        return webm::Status(webm::Status::kOkCompleted);
    }

    webm::Status OnFrame(const webm::FrameMetadata&, webm::Reader* reader, std::uint64_t* remaining) final
    {
        if (!m_frameInProgress) {
            m_frame.clear();
            m_frame.reserveInitialCapacity(*remaining);
            m_frameInProgress = true;
        }
        while (*remaining) {
            size_t chunk = static_cast<size_t>(std::min<uint64_t>(*remaining, 64 * 1024));
            size_t start = m_frame.size();
            m_frame.grow(start + chunk);
            uint64_t read = 0;
            auto status = reader->Read(chunk, m_frame.mutableSpan().subspan(start).data(), &read);
            m_frame.shrink(start + read);
            *remaining -= read;
            if (!status.ok())
                return status; // more bytes with the next append
        }
        m_frameInProgress = false;
        StreamFrame frame;
        frame.track = m_blockTrack;
        frame.time = ticksToTime(m_blockTicks);
        frame.duration = MediaTime::invalidTime();
        frame.key = m_blockKey;
        frame.data = std::exchange(m_frame, { });
        if (m_inBlockGroup)
            m_groupFrames.append(WTF::move(frame));
        else
            deliver(WTF::move(frame));
        return webm::Status(webm::Status::kOkCompleted);
    }

private:
    static bool isSegmentChild(webm::Id id)
    {
        switch (id) {
        case webm::Id::kSeekHead:
        case webm::Id::kInfo:
        case webm::Id::kCluster:
        case webm::Id::kCues:
        case webm::Id::kChapters:
        case webm::Id::kTags:
            return true;
        default:
            return false;
        }
    }

    MediaTime ticksToTime(int64_t ticks) const
    {
        return MediaTime(ticks * static_cast<int64_t>(m_timecodeScale), nanosecondsPerSecond);
    }

    void beginBlock(const webm::Block& block, bool key)
    {
        m_blockTrack = block.track_number;
        m_blockTicks = static_cast<int64_t>(m_clusterTimecode) + block.timecode;
        m_blockKey = key;
    }

    void reportInitializationSegment()
    {
        if (!m_tracksPending)
            return;
        m_tracksPending = false;
        m_tracksEnd = 0;
        m_trackInfo.clear();
        for (auto& track : m_tracks)
            m_trackInfo.set(track.number, track);
        m_client.didParseInitializationSegment(Vector<StreamTrack> { m_tracks }, m_duration);
    }

    // Frames carry no duration: a frame is held until the next one of its
    // track (the difference is its duration) or the end of the append.
    void deliver(StreamFrame&& frame)
    {
        auto info = m_trackInfo.find(frame.track);
        if (info == m_trackInfo.end())
            return; // a track the initialization segment did not declare
        auto held = m_held.find(frame.track);
        if (held != m_held.end()) {
            auto& previous = held->value;
            if (!previous.duration.isValid()) {
                auto difference = frame.time - previous.time;
                previous.duration = difference > MediaTime::zeroTime() ? difference : estimatedDuration(previous);
            }
            // Laced frames share their block's time: each starts where the
            // one before ends.
            if (frame.time <= previous.time)
                frame.time = previous.time + previous.duration;
            emit(WTF::move(previous), false);
            held->value = WTF::move(frame);
            return;
        }
        m_held.add(frame.track, WTF::move(frame));
    }

    void emit(StreamFrame&& frame, bool estimate)
    {
        if (estimate && !frame.duration.isValid())
            frame.duration = estimatedDuration(frame);
        m_lastDuration.set(frame.track, frame.duration);
        m_client.didParseFrame(WTF::move(frame));
    }

    MediaTime estimatedDuration(const StreamFrame& frame) const
    {
        auto info = m_trackInfo.find(frame.track);
        if (info != m_trackInfo.end()) {
            // Opus packets say how many samples (at 48 kHz) they hold.
            if (info->value.codecID == "A_OPUS"_s && !frame.data.isEmpty()) {
                int samples = opus_packet_get_nb_samples(frame.data.span().data(), frame.data.size(), 48000);
                if (samples > 0)
                    return MediaTime(samples, 48000);
            }
            if (info->value.defaultDuration.isValid())
                return info->value.defaultDuration;
        }
        auto last = m_lastDuration.find(frame.track);
        if (last != m_lastDuration.end() && last->value.isValid())
            return last->value;
        bool video = info != m_trackInfo.end() && info->value.kind == StreamTrack::Kind::Video;
        return video ? MediaTime(1, 30) : MediaTime(20, 1000);
    }

    WebMStreamParser::Client& m_client;
    uint64_t m_timecodeScale { 1'000'000 }; // nanoseconds per tick
    MediaTime m_duration { MediaTime::invalidTime() };
    Vector<StreamTrack> m_tracks;
    HashMap<uint64_t, StreamTrack, WTF::IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_trackInfo;
    uint64_t m_tracksEnd { 0 };
    bool m_tracksPending { false };

    uint64_t m_clusterTimecode { 0 };
    uint64_t m_blockTrack { 0 };
    int64_t m_blockTicks { 0 };
    bool m_blockKey { false };
    bool m_inBlockGroup { false };
    Vector<StreamFrame> m_groupFrames;
    bool m_frameInProgress { false };
    Vector<uint8_t> m_frame;

    HashMap<uint64_t, StreamFrame, WTF::IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_held;
    HashMap<uint64_t, MediaTime, WTF::IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_lastDuration;
};

WebMStreamParser::WebMStreamParser(Client& client)
    : m_client(client)
    , m_parser(std::make_unique<webm::WebmParser>())
    , m_callback(std::make_unique<Callback>(client))
    , m_reader(std::make_unique<Reader>())
{
}

WebMStreamParser::~WebMStreamParser() = default;

bool WebMStreamParser::append(std::span<const uint8_t> bytes)
{
    m_reader->append(bytes);
    for (;;) {
        auto status = m_parser->Feed(m_callback.get(), m_reader.get());
        if (status.code == webm::Status::kWouldBlock)
            break;
        if (!status.completed_ok())
            return false;
        // A whole document (EBML header and Segment): another may follow.
        m_parser = std::make_unique<webm::WebmParser>();
        if (!m_reader->hasData())
            break;
    }
    m_callback->didFeed(m_reader->Position());
    return true;
}

void WebMStreamParser::reset()
{
    m_reader->clear();
    m_callback->reset();
    // The next bytes start an element at the Segment's level (a Cluster) or
    // a new document.
    m_parser->DidSeek();
}

} // namespace ZeroG::Media

#endif // ENABLE(MEDIA_SOURCE) && defined(ZEROG_MEDIA)
