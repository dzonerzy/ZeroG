#include "config.h"
#include "media/MediaSourceZeroG.h"

#if ENABLE(MEDIA_SOURCE) && defined(ZEROG_MEDIA)

#include "media/Decoders.h"
#include "media/MediaPlayerPrivateZeroG.h"
#include <WebCore/AudioTrackPrivate.h>
#include <WebCore/ContentType.h>
#include <WebCore/MediaDescription.h>
#include <WebCore/MediaPlayer.h>
#include <WebCore/MediaSourcePrivateClient.h>
#include <WebCore/SourceBufferPrivateClient.h>
#include <WebCore/VideoTrackPrivate.h>
#include <wtf/MainThread.h>

namespace ZeroG::Media {

using namespace WebCore;

namespace {

constexpr int64_t nanosecondsPerSecond = 1'000'000'000;

// How many frames wait for the decoder: about a second of each.
constexpr size_t maximumQueued(SampleQueues::Kind kind)
{
    return kind == SampleQueues::Kind::Video ? 30 : 60;
}

class AudioTrackPrivateZeroG final : public AudioTrackPrivate {
public:
    static Ref<AudioTrackPrivateZeroG> create(TrackID id) { return adoptRef(*new AudioTrackPrivateZeroG(id)); }
    TrackID id() const final { return m_id; }

private:
    explicit AudioTrackPrivateZeroG(TrackID id)
        : m_id(id)
    {
    }
    TrackID m_id;
};

class VideoTrackPrivateZeroG final : public VideoTrackPrivate {
public:
    static Ref<VideoTrackPrivateZeroG> create(TrackID id) { return adoptRef(*new VideoTrackPrivateZeroG(id)); }
    TrackID id() const final { return m_id; }

private:
    explicit VideoTrackPrivateZeroG(TrackID id)
        : m_id(id)
    {
    }
    TrackID m_id;
};

class MediaDescriptionZeroG final : public MediaDescription {
public:
    static Ref<MediaDescriptionZeroG> create(String&& codec, StreamTrack::Kind kind) { return adoptRef(*new MediaDescriptionZeroG(WTF::move(codec), kind)); }
    bool isVideo() const final { return m_kind == StreamTrack::Kind::Video; }
    bool isAudio() const final { return m_kind == StreamTrack::Kind::Audio; }
    bool isText() const final { return false; }

private:
    MediaDescriptionZeroG(String&& codec, StreamTrack::Kind kind)
        : MediaDescription(WTF::move(codec))
        , m_kind(kind)
    {
    }
    StreamTrack::Kind m_kind;
};

// The codec as MSE names it ("codecs" in a content type).
String codecName(const String& codecID)
{
    if (codecID == "V_VP8"_s)
        return "vp8"_s;
    if (codecID == "V_VP9"_s)
        return "vp09"_s;
    if (codecID == "V_AV1"_s)
        return "av01"_s;
    if (codecID == "A_OPUS"_s)
        return "opus"_s;
    if (codecID == "A_VORBIS"_s)
        return "vorbis"_s;
    return codecID.isolatedCopy();
}

long long toNanoseconds(const MediaTime& time)
{
    return static_cast<long long>(time.toDouble() * nanosecondsPerSecond);
}

// The decode thread's file: the queues' frames through WebM's decoders.
class MediaSourceFile final : public MediaFile {
public:
    explicit MediaSourceFile(SampleQueues& queues)
        : m_queues(queues)
    {
        if (auto format = queues.format(SampleQueues::Kind::Audio)) {
            unsigned channels = format->channels;
            unsigned sampleRate = format->sampleRate;
            m_audioDecoder = createAudioDecoder(format->codecID, format->codecPrivate.span(), channels, sampleRate);
            if (m_audioDecoder) {
                m_info.hasAudio = true;
                m_info.channels = channels;
                m_info.sampleRate = sampleRate;
            }
        }
        if (auto format = queues.format(SampleQueues::Kind::Video)) {
            m_videoDecoder = createVideoDecoder(format->codecID);
            if (m_videoDecoder) {
                m_info.hasVideo = true;
                m_info.videoSize = format->size;
            }
        }
    }

    size_t readAudio(float* out, size_t frames, MediaTime& time) final
    {
        if (!m_audioDecoder)
            return 0;
        while (m_audioOffset >= m_audio.size()) {
            if (!decodeAudio())
                return 0;
        }
        size_t channels = m_info.channels;
        size_t count = std::min(frames, (m_audio.size() - m_audioOffset) / channels);
        time = m_audioTime;
        memcpy(out, m_audio.span().subspan(m_audioOffset).data(), count * channels * sizeof(float));
        m_audioOffset += count * channels;
        m_audioTime = m_audioTime + MediaTime(count, m_info.sampleRate);
        return count;
    }

    std::optional<DecodedFrame> readVideo() final
    {
        if (!m_videoDecoder)
            return std::nullopt;
        while (m_frames.empty()) {
            bool flushed = false;
            auto sample = m_queues->take(SampleQueues::Kind::Video, flushed);
            if (flushed)
                m_videoDecoder->flush();
            if (!sample)
                return std::nullopt;
            Vector<DecodedFrame> decoded;
            m_videoDecoder->decode(packet(*sample), decoded);
            // Frames before a seek's target are decoded for the ones after
            // them, not shown.
            if (sample->isNonDisplaying())
                continue;
            for (auto& frame : decoded)
                m_frames.push_back(WTF::move(frame));
        }
        auto frame = WTF::move(m_frames.front());
        m_frames.pop_front();
        return frame;
    }

    bool audioEnded() const final { return m_queues->ended(SampleQueues::Kind::Audio) && m_audioOffset >= m_audio.size(); }
    bool videoEnded() const final { return m_queues->ended(SampleQueues::Kind::Video) && m_frames.empty(); }

    bool seek(const MediaTime& time) final
    {
        // WebCore enqueues the frames from the keyframe before `time`.
        m_audio.clear();
        m_audioOffset = 0;
        m_audioTime = time;
        m_audioSeekTarget = time;
        m_frames.clear();
        m_queues->endSeek();
        return true;
    }

private:
    static Packet packet(const MediaSampleZeroG& sample)
    {
        return { Vector<uint8_t> { sample.data() }, toNanoseconds(sample.presentationTime()), sample.isSync() };
    }

    bool decodeAudio()
    {
        bool flushed = false;
        auto sample = m_queues->take(SampleQueues::Kind::Audio, flushed);
        if (flushed)
            m_audioDecoder->flush();
        if (!sample)
            return false;
        m_audio.clear();
        m_audioOffset = 0;
        m_audioDecoder->decode(packet(*sample), m_audio);
        m_audioTime = sample->presentationTime();
        if (m_audioSeekTarget.isValid() && !m_audio.isEmpty()) {
            // The samples before the seek's target are dropped.
            size_t channels = m_info.channels;
            if (m_audioTime < m_audioSeekTarget) {
                size_t skip = static_cast<size_t>((m_audioSeekTarget - m_audioTime).toDouble() * m_info.sampleRate) * channels;
                m_audioOffset = std::min(skip, m_audio.size());
                m_audioTime = m_audioTime + MediaTime(m_audioOffset / channels, m_info.sampleRate);
            }
            if (m_audioOffset < m_audio.size())
                m_audioSeekTarget = MediaTime::invalidTime();
        }
        return true;
    }

    const Ref<SampleQueues> m_queues;
    std::unique_ptr<AudioDecoder> m_audioDecoder;
    std::unique_ptr<VideoDecoder> m_videoDecoder;
    Vector<float> m_audio;
    size_t m_audioOffset { 0 };
    MediaTime m_audioTime { MediaTime::zeroTime() };
    MediaTime m_audioSeekTarget { MediaTime::invalidTime() };
    std::deque<DecodedFrame> m_frames;
};

class SourceBufferPrivateZeroG final : public SourceBufferPrivate, private WebMStreamParser::Client {
public:
    static Ref<SourceBufferPrivateZeroG> create(MediaSourcePrivateZeroG& mediaSource) { return adoptRef(*new SourceBufferPrivateZeroG(mediaSource)); }

    constexpr MediaPlatformType platformType() const final { return MediaPlatformType::ZeroG; }

#if !RELEASE_LOG_DISABLED
    const Logger& sourceBufferLogger() const final { return m_logger; }
    uint64_t sourceBufferLogIdentifier() final { return m_logIdentifier; }
    const Logger& logger() const final { return m_logger; }
    ASCIILiteral logClassName() const final { return "SourceBufferPrivateZeroG"_s; }
    uint64_t logIdentifier() const final { return m_logIdentifier; }
    WTFLogChannel& logChannel() const final;
#endif

private:
    explicit SourceBufferPrivateZeroG(MediaSourcePrivateZeroG& mediaSource)
        : SourceBufferPrivate(mediaSource)
        , m_parser(*this)
        , m_queues(mediaSource.queues())
#if !RELEASE_LOG_DISABLED
        , m_logger(mediaSource.logger())
        , m_logIdentifier(mediaSource.logIdentifier())
#endif
    {
    }

    Ref<MediaPromise> appendInternal(Ref<SharedBuffer>&& data) final
    {
        if (!m_parser.append(data->span()))
            return MediaPromise::createAndReject(PlatformMediaError::ParsingError);
        return MediaPromise::createAndResolve();
    }

    void resetParserStateInternal() final
    {
        m_parser.reset();
    }

    // WebMStreamParser::Client
    void didParseInitializationSegment(Vector<StreamTrack>&& tracks, const MediaTime& duration) final
    {
        SourceBufferPrivateClient::InitializationSegment segment;
        segment.duration = duration;
        for (auto& track : tracks) {
            auto id = static_cast<TrackID>(track.number);
            auto kind = track.kind == StreamTrack::Kind::Video ? SampleQueues::Kind::Video : SampleQueues::Kind::Audio;
            // The first track of each kind plays (or this buffer's again,
            // after a new initialization segment).
            bool playing = false;
            auto existing = m_tracks.find(id);
            if (existing != m_tracks.end() && existing->value.playing)
                playing = true;
            else if (!m_queues->hasFormat(kind))
                playing = true;
            if (playing) {
                SampleQueues::Format format;
                format.codecID = track.codecID.isolatedCopy();
                format.codecPrivate = track.codecPrivate;
                format.channels = track.channels;
                format.sampleRate = track.sampleRate;
                format.size = { static_cast<int>(track.width), static_cast<int>(track.height) };
                m_queues->setFormat(kind, WTF::move(format));
            }
            m_tracks.set(id, Track { kind, { static_cast<float>(track.width), static_cast<float>(track.height) }, playing });
            auto description = MediaDescriptionZeroG::create(codecName(track.codecID), track.kind);
            if (track.kind == StreamTrack::Kind::Video)
                segment.videoTracks.append({ WTF::move(description), VideoTrackPrivateZeroG::create(id) });
            else
                segment.audioTracks.append({ WTF::move(description), AudioTrackPrivateZeroG::create(id) });
        }
        SourceBufferPrivate::didReceiveInitializationSegment(WTF::move(segment));
    }

    void didParseFrame(StreamFrame&& frame) final
    {
        auto id = static_cast<TrackID>(frame.track);
        auto track = m_tracks.find(id);
        if (track == m_tracks.end())
            return;
        didReceiveSample(MediaSampleZeroG::create(WTF::move(frame), id, track->value.size));
    }

    // Playback: WebCore's track buffers give the frames to decode.
    std::optional<SampleQueues::Kind> playingKind(TrackID id) const
    {
        auto track = m_tracks.find(id);
        if (track == m_tracks.end() || !track->value.playing)
            return std::nullopt;
        return track->value.kind;
    }

    void flush(TrackID id) final
    {
        if (auto kind = playingKind(id))
            m_queues->flush(*kind);
    }

    void enqueueSample(Ref<MediaSample>&& sample, TrackID id) final
    {
        if (auto kind = playingKind(id))
            m_queues->enqueue(*kind, static_cast<MediaSampleZeroG&>(sample.get()));
    }

    void allSamplesInTrackEnqueued(TrackID id) final
    {
        if (auto kind = playingKind(id))
            m_queues->markEnded(*kind);
    }

    bool isReadyForMoreSamples(TrackID id) final
    {
        auto kind = playingKind(id);
        return !kind || m_queues->hasRoom(*kind);
    }

    void notifyClientWhenReadyForMoreSamples(TrackID id) final
    {
        auto kind = playingKind(id);
        if (!kind)
            return;
        m_queues->notifyWhenRoom(*kind, [weakThis = ThreadSafeWeakPtr { *this }, id] {
            if (RefPtr protectedThis = weakThis.get())
                protectedThis->provideMediaData(id);
        });
    }

    struct Track {
        SampleQueues::Kind kind;
        FloatSize size;
        bool playing { false };
    };

    WebMStreamParser m_parser;
    const Ref<SampleQueues> m_queues;
    HashMap<TrackID, Track, WTF::IntHash<TrackID>, WTF::UnsignedWithZeroKeyHashTraits<TrackID>> m_tracks;
#if !RELEASE_LOG_DISABLED
    const Ref<const Logger> m_logger;
    const uint64_t m_logIdentifier;
#endif
};

#if !RELEASE_LOG_DISABLED
WTFLogChannel& SourceBufferPrivateZeroG::logChannel() const
{
    return LogMediaSource;
}
#endif

} // namespace

// ---- Samples ----

Ref<MediaSampleZeroG> MediaSampleZeroG::create(StreamFrame&& frame, TrackID trackID, const FloatSize& size)
{
    auto flags = frame.key ? IsSync : None;
    return adoptRef(*new MediaSampleZeroG(SharedBuffer::create(WTF::move(frame.data)), frame.time, frame.duration, trackID, size, flags));
}

MediaSampleZeroG::MediaSampleZeroG(Ref<SharedBuffer>&& data, const MediaTime& time, const MediaTime& duration, TrackID trackID, const FloatSize& size, SampleFlags flags)
    : m_data(WTF::move(data))
    , m_presentationTime(time)
    , m_decodeTime(time) // no reordering in WebM's codecs
    , m_duration(duration)
    , m_trackID(trackID)
    , m_size(size)
    , m_flags(flags)
{
}

void MediaSampleZeroG::offsetTimestampsBy(const MediaTime& offset)
{
    m_presentationTime += offset;
    m_decodeTime += offset;
}

void MediaSampleZeroG::setTimestamps(const MediaTime& presentationTime, const MediaTime& decodeTime)
{
    m_presentationTime = presentationTime;
    m_decodeTime = decodeTime;
}

Ref<MediaSample> MediaSampleZeroG::createNonDisplayingCopy() const
{
    auto copy = adoptRef(*new MediaSampleZeroG(m_data.copyRef(), m_presentationTime, m_duration, m_trackID, m_size, static_cast<SampleFlags>(m_flags | IsNonDisplaying)));
    copy->m_decodeTime = m_decodeTime;
    return copy;
}

PlatformSample MediaSampleZeroG::platformSample() const
{
    // No platform object: the player reads data().
    return PlatformSample { static_cast<const MockSampleBox*>(nullptr) };
}

// ---- Queues ----

bool SampleQueues::hasFormat(Kind kind) const
{
    Locker locker { m_lock };
    return !!queue(kind).format;
}

void SampleQueues::setFormat(Kind kind, Format&& format)
{
    Locker locker { m_lock };
    queue(kind).format = WTF::move(format);
}

std::optional<SampleQueues::Format> SampleQueues::format(Kind kind) const
{
    Locker locker { m_lock };
    auto& format = queue(kind).format;
    if (!format)
        return std::nullopt;
    return Format { format->codecID.isolatedCopy(), format->codecPrivate, format->channels, format->sampleRate, format->size };
}

void SampleQueues::enqueue(Kind kind, Ref<MediaSampleZeroG>&& sample)
{
    Locker locker { m_lock };
    queue(kind).samples.push_back(WTF::move(sample));
}

void SampleQueues::flush(Kind kind)
{
    Locker locker { m_lock };
    auto& entry = queue(kind);
    entry.samples.clear();
    entry.ended = false;
    entry.flushed = true;
}

void SampleQueues::markEnded(Kind kind)
{
    Locker locker { m_lock };
    queue(kind).ended = true;
}

bool SampleQueues::hasRoom(Kind kind) const
{
    Locker locker { m_lock };
    return queue(kind).samples.size() < maximumQueued(kind);
}

void SampleQueues::notifyWhenRoom(Kind kind, Function<void()>&& function)
{
    Locker locker { m_lock };
    queue(kind).whenRoom = WTF::move(function);
}

void SampleQueues::beginSeek()
{
    Locker locker { m_lock };
    m_seeking = true;
    for (auto& entry : m_queues) {
        entry.samples.clear();
        entry.ended = false;
        entry.flushed = true;
    }
}

void SampleQueues::endSeek()
{
    Locker locker { m_lock };
    m_seeking = false;
}

RefPtr<MediaSampleZeroG> SampleQueues::take(Kind kind, bool& flushed)
{
    Function<void()> whenRoom;
    RefPtr<MediaSampleZeroG> sample;
    {
        Locker locker { m_lock };
        auto& entry = queue(kind);
        if (m_seeking)
            return nullptr;
        flushed = std::exchange(entry.flushed, false);
        if (entry.samples.empty())
            return nullptr;
        sample = WTF::move(entry.samples.front());
        entry.samples.pop_front();
        if (entry.whenRoom && entry.samples.size() < maximumQueued(kind) / 2)
            whenRoom = std::exchange(entry.whenRoom, nullptr);
    }
    if (whenRoom)
        callOnMainThread(WTF::move(whenRoom));
    return sample;
}

bool SampleQueues::ended(Kind kind) const
{
    Locker locker { m_lock };
    auto& entry = queue(kind);
    return !m_seeking && entry.ended && entry.samples.empty();
}

std::unique_ptr<MediaFile> openMediaSourceFile(SampleQueues& queues)
{
    return std::make_unique<MediaSourceFile>(queues);
}

// ---- The media source ----

Ref<MediaSourcePrivateZeroG> MediaSourcePrivateZeroG::create(MediaPlayerPrivateZeroG& player, MediaSourcePrivateClient& client)
{
    Ref mediaSource = adoptRef(*new MediaSourcePrivateZeroG(player, client));
    client.setPrivateAndOpen(mediaSource.copyRef());
    return mediaSource;
}

MediaSourcePrivateZeroG::MediaSourcePrivateZeroG(MediaPlayerPrivateZeroG& player, MediaSourcePrivateClient& client)
    : MediaSourcePrivate(client)
    , m_player(player)
    , m_queues(SampleQueues::create())
#if !RELEASE_LOG_DISABLED
    , m_logger(player.mediaPlayer()->mediaPlayerLogger())
    , m_logIdentifier(player.mediaPlayer()->mediaPlayerLogIdentifier())
#endif
{
}

MediaSourcePrivateZeroG::~MediaSourcePrivateZeroG() = default;

RefPtr<MediaPlayerPrivateInterface> MediaSourcePrivateZeroG::player() const
{
    return m_player.get();
}

void MediaSourcePrivateZeroG::setPlayer(MediaPlayerPrivateInterface* player)
{
    m_player = static_cast<MediaPlayerPrivateZeroG*>(player);
}

MediaSourcePrivate::AddStatus MediaSourcePrivateZeroG::addSourceBuffer(const ContentType& type, const MediaSourceConfiguration&, RefPtr<SourceBufferPrivate>& sourceBuffer)
{
    if (!supportsMediaSourceType(type))
        return AddStatus::NotSupported;
    {
        Locker locker { m_lock };
        m_sourceBuffers.append(SourceBufferPrivateZeroG::create(*this));
        sourceBuffer = m_sourceBuffers.last();
    }
    sourceBuffer->setMediaSourceDuration(duration());
    return AddStatus::Ok;
}

void MediaSourcePrivateZeroG::durationChanged(const MediaTime& duration)
{
    MediaSourcePrivate::durationChanged(duration);
    ensureOnMainThread([weakPlayer = m_player] {
        if (RefPtr player = weakPlayer.get()) {
            if (RefPtr mediaPlayer = player->mediaPlayer())
                mediaPlayer->durationChanged();
        }
    });
}

void MediaSourcePrivateZeroG::notifyActiveSourceBuffersChanged()
{
    if (RefPtr player = m_player.get()) {
        if (RefPtr mediaPlayer = player->mediaPlayer())
            mediaPlayer->activeSourceBuffersChanged();
    }
}

bool supportsMediaSourceType(const ContentType& type)
{
    // WebM, with the codecs WebM files play.
    auto container = type.containerType().convertToASCIILowercase();
    if (container != "video/webm"_s && container != "audio/webm"_s)
        return false;
    static constexpr ASCIILiteral codecs[] = { "vp8"_s, "vp9"_s, "vp09"_s, "av01"_s, "opus"_s, "vorbis"_s };
    for (auto& codec : type.codecs()) {
        auto lowercase = codec.convertToASCIILowercase();
        bool known = std::any_of(std::begin(codecs), std::end(codecs), [&](ASCIILiteral prefix) {
            return lowercase.startsWith(prefix);
        });
        if (!known)
            return false;
    }
    return true;
}

} // namespace ZeroG::Media

#endif // ENABLE(MEDIA_SOURCE) && defined(ZEROG_MEDIA)
