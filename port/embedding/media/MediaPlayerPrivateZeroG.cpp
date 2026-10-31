#include "config.h"
#include "media/MediaPlayerPrivateZeroG.h"

#if ENABLE(VIDEO) && defined(ZEROG_MEDIA)

#include "media/AudioOutput.h"
#include <WebCore/ColorSpace.h>
#include <WebCore/ContentType.h>
#include <WebCore/GraphicsContext.h>
#include <WebCore/MediaPlayer.h>
#include <WebCore/NativeImage.h>
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/ResourceResponse.h>
#include <WebCore/SharedBuffer.h>
#include <WebCore/VideoPlaybackQualityMetrics.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#if ENABLE(WEB_AUDIO)
#include <WebCore/AudioBus.h>
#include <WebCore/AudioChannel.h>
#endif
#if USE(COORDINATED_GRAPHICS)
#include "CoordinatedPlatformLayerBufferProxy.h"
#include "CoordinatedPlatformLayerBufferSkiaImage.h"
#endif
#if ENABLE(MEDIA_SOURCE)
#include "media/MediaSourceZeroG.h"
#include <WebCore/MediaSourcePrivateClient.h>
#endif

namespace ZeroG::Media {

using namespace WebCore;

namespace {

// How far ahead the decode thread works: a quarter second of sound, a few
// frames of video.
constexpr double audioAheadSeconds = 0.25;
constexpr size_t framesAhead = 4;
constexpr Seconds tickInterval = 10_ms;

// The containers and codecs this engine plays (canPlayType, <source type>).
struct ContainerSupport {
    ASCIILiteral type;
    std::initializer_list<ASCIILiteral> codecs; // prefixes
};

const ContainerSupport containers[] = {
    { "video/webm"_s, { "vp8"_s, "vp9"_s, "vp09"_s, "av01"_s, "av1"_s, "opus"_s, "vorbis"_s } },
    { "audio/webm"_s, { "opus"_s, "vorbis"_s } },
    { "audio/ogg"_s, { "opus"_s, "vorbis"_s } },
    { "application/ogg"_s, { "opus"_s, "vorbis"_s } },
    { "audio/opus"_s, { "opus"_s } },
    { "audio/mpeg"_s, { "mp3"_s, "mp4a.40.34"_s, "mp4a.69"_s, "mp4a.6b"_s } },
    { "audio/mp3"_s, { "mp3"_s } },
    { "audio/flac"_s, { "flac"_s } },
    { "audio/x-flac"_s, { "flac"_s } },
    { "audio/wav"_s, { "1"_s } },
    { "audio/wave"_s, { "1"_s } },
    { "audio/x-wav"_s, { "1"_s } },
    { "audio/vnd.wave"_s, { "1"_s } },
};

// MP4 with the engine's own decoders (MP4File.cpp).
const ContainerSupport mp4Containers[] = {
    { "video/mp4"_s, { "vp09"_s, "av01"_s, "opus"_s } },
    { "audio/mp4"_s, { "opus"_s } },
};

#if OS(WINDOWS)
// And through Media Foundation, where the system has it: H.264 and AAC (and
// MP3 in MP4).
const ContainerSupport mediaFoundationContainers[] = {
    { "video/mp4"_s, { "vp09"_s, "av01"_s, "opus"_s, "avc1"_s, "avc3"_s, "mp4a.40"_s, "mp4a.69"_s, "mp4a.6b"_s } },
    { "audio/mp4"_s, { "opus"_s, "mp4a.40"_s, "mp4a.69"_s, "mp4a.6b"_s } },
    { "audio/x-m4a"_s, { "mp4a.40"_s } },
};
#endif

const ContainerSupport* findContainer(const String& type)
{
    for (auto& container : containers) {
        if (type == container.type)
            return &container;
    }
#if OS(WINDOWS)
    if (mediaFoundationAvailable()) {
        for (auto& container : mediaFoundationContainers) {
            if (type == container.type)
                return &container;
        }
    }
#endif
    for (auto& container : mp4Containers) {
        if (type == container.type)
            return &container;
    }
    return nullptr;
}

class MediaEngineFactory final : public MediaPlayerFactory {
public:
    MediaPlayerEnums::MediaEngineIdentifier identifier() const final { return MediaPlayerEnums::MediaEngineIdentifier::ZeroG; }

    Ref<MediaPlayerPrivateInterface> createMediaEnginePlayer(MediaPlayer& player) const final
    {
        return MediaPlayerPrivateZeroG::create(player);
    }

    void getSupportedTypes(HashSet<String>& types) const final
    {
        for (auto& container : containers)
            types.add(container.type);
#if OS(WINDOWS)
        if (mediaFoundationAvailable()) {
            for (auto& container : mediaFoundationContainers)
                types.add(container.type);
        }
#endif
        for (auto& container : mp4Containers)
            types.add(container.type);
    }

    MediaPlayer::SupportsType supportsTypeAndCodecs(const MediaEngineSupportParameters& parameters) const final
    {
#if ENABLE(MEDIA_SOURCE)
        // Media Source Extensions: WebM.
        if (parameters.platformType == PlatformMediaDecodingType::MediaSource) {
            if (!supportsMediaSourceType(parameters.type))
                return MediaPlayer::SupportsType::IsNotSupported;
            return parameters.type.codecs().isEmpty() ? MediaPlayer::SupportsType::MayBeSupported : MediaPlayer::SupportsType::IsSupported;
        }
#endif
        // Files (no media streams).
        if (parameters.platformType != PlatformMediaDecodingType::FileOrHLS)
            return MediaPlayer::SupportsType::IsNotSupported;
        auto* container = findContainer(parameters.type.containerType().convertToASCIILowercase());
        if (!container)
            return MediaPlayer::SupportsType::IsNotSupported;
        auto codecs = parameters.type.codecs();
        if (codecs.isEmpty())
            return MediaPlayer::SupportsType::MayBeSupported;
        for (auto& codec : codecs) {
            auto lowercase = codec.convertToASCIILowercase();
            bool known = std::any_of(container->codecs.begin(), container->codecs.end(), [&](ASCIILiteral prefix) {
                return lowercase.startsWith(prefix);
            });
            if (!known)
                return MediaPlayer::SupportsType::IsNotSupported;
        }
        return MediaPlayer::SupportsType::IsSupported;
    }
};

// The resource's bytes into the pipeline's buffer.
class ResourceClient final : public PlatformMediaResourceClient {
public:
    static Ref<ResourceClient> create(MediaPlayerPrivateZeroG& player, MediaBytes& bytes) { return adoptRef(*new ResourceClient(player, bytes)); }

    void responseReceived(PlatformMediaResource&, const ResourceResponse& response, CompletionHandler<void(ShouldContinuePolicyCheck)>&& completionHandler) final
    {
        if (response.httpStatusCode() >= 400) {
            completionHandler(ShouldContinuePolicyCheck::No);
            finish(true);
            return;
        }
        long long length = response.expectedContentLength();
        m_bytes->setExpectedLength(length > 0 ? std::optional<uint64_t>(length) : std::nullopt);
        completionHandler(ShouldContinuePolicyCheck::Yes);
    }

    void dataReceived(PlatformMediaResource&, const SharedBuffer& buffer) final
    {
        m_bytes->append(buffer.span());
    }

    void accessControlCheckFailed(PlatformMediaResource&, const ResourceError&) final { finish(true); }
    void loadFailed(PlatformMediaResource&, const ResourceError&) final { finish(true); }
    void loadFinished(PlatformMediaResource&, const NetworkLoadMetrics&) final { finish(false); }

private:
    ResourceClient(MediaPlayerPrivateZeroG& player, MediaBytes& bytes)
        : m_player(player)
        , m_bytes(bytes)
    {
    }

    void finish(bool failed)
    {
        if (failed)
            m_bytes->fail();
        else
            m_bytes->finish();
        if (RefPtr player = m_player.get())
            player->didFinishLoading(failed);
    }

    ThreadSafeWeakPtr<MediaPlayerPrivateZeroG> m_player;
    const Ref<MediaBytes> m_bytes;
};

} // namespace

void registerMediaEngine()
{
    MediaPlayerFactorySupport::callRegisterMediaEngine([](MediaEngineRegistrar registrar) {
        registrar(std::unique_ptr<MediaPlayerFactory>(new MediaEngineFactory));
    });
}

// ---- Pipeline ----

Pipeline::Pipeline(MediaPlayerPrivateZeroG& player)
    : m_player(player)
    , m_bytes(MediaBytes::create())
{
}

Pipeline::~Pipeline() = default;

void Pipeline::start()
{
    m_thread = Thread::create("ZeroG media"_s, [protectedThis = Ref { *this }] {
        protectedThis->run();
    });
}

void Pipeline::stop()
{
    {
        Locker locker { m_lock };
        m_stopping = true;
        m_condition.notifyAll();
    }
    m_bytes->abort();
    if (auto thread = std::exchange(m_thread, nullptr))
        thread->waitForCompletion();
    m_output = nullptr;
}

void Pipeline::requestSeek(const MediaTime& time, uint64_t identifier)
{
    Locker locker { m_lock };
    m_seekRequest = { time, identifier };
    m_condition.notifyAll();
}

sk_sp<SkImage> Pipeline::takeFrame(const MediaTime& time, bool paused)
{
    Locker locker { m_lock };
    std::optional<DecodedFrame> showing;
    while (!m_frames.empty() && m_frames.front().time <= time) {
        // A frame the clock went past before it showed is dropped.
        if (showing)
            ++m_droppedFrames;
        showing = WTF::move(m_frames.front());
        m_frames.pop_front();
    }
    if (!showing && paused && !m_frames.empty()) {
        // The first frame after a seek or a load, shown before playback.
        showing = WTF::move(m_frames.front());
        m_frames.pop_front();
    }
    if (showing) {
        ++m_shownFrames;
        m_condition.notifyAll(); // room for the next frame
    }
    return showing ? showing->image : nullptr;
}

MediaTime Pipeline::audioTime(uint64_t played) const
{
    Locker locker { m_lock };
    if (!m_sampleRate)
        return m_audioBase;
    return m_audioBase + MediaTime(played, m_sampleRate);
}

bool Pipeline::ended() const
{
    Locker locker { m_lock };
    bool audioEnded = !m_hasAudio || (m_audioDone && (!m_output || !m_output->queuedFrames()));
    bool videoEnded = !m_hasVideo || (m_videoDone && m_frames.empty());
    return audioEnded && videoEnded;
}

void Pipeline::post(Function<void(MediaPlayerPrivateZeroG&)>&& function)
{
    callOnMainThread([player = m_player, function = WTF::move(function)] {
        if (RefPtr protectedPlayer = player.get())
            function(*protectedPlayer);
    });
}

// The decode thread.
void Pipeline::run()
{
    String error;
    auto file = m_openFile ? m_openFile() : MediaFile::open(m_bytes, error);
    if (!file) {
        bool networkError = m_bytes->isFailed();
        if (!m_bytes->isAborted())
            post([error = error.isolatedCopy(), networkError](auto& player) { player.didFailToOpen(error, networkError); });
        return;
    }
    auto info = file->info();
    if (info.hasAudio)
        m_output = AudioOutput::create(info.sampleRate, info.channels);
    {
        Locker locker { m_lock };
        m_hasVideo = info.hasVideo;
        // Without an audio device the sound is not decoded: a silent video,
        // or a silent clock for an audio file, on the monotonic clock.
        m_hasAudio = info.hasAudio && m_output;
        m_sampleRate = info.sampleRate;
    }
    post([info](auto& player) { player.didOpen(info); });

    Vector<float> samples(4096 * std::max(info.channels, 1u));
    bool firstData = false;
    size_t audioAheadFrames = info.sampleRate * audioAheadSeconds;
    for (;;) {
        setDecodedFramesStayYUV(m_framesStayYUV);
        std::optional<std::pair<MediaTime, uint64_t>> seek;
        bool hasAudio, hasVideo, audioDone, videoDone;
        size_t queuedFrames;
        {
            Locker locker { m_lock };
            if (m_stopping)
                return;
            seek = std::exchange(m_seekRequest, std::nullopt);
            hasAudio = m_hasAudio;
            hasVideo = m_hasVideo;
            audioDone = m_audioDone;
            videoDone = m_videoDone;
            queuedFrames = m_frames.size();
        }
        if (seek) {
            file->seek(seek->first);
            if (m_output)
                m_output->clear();
            {
                Locker locker { m_lock };
                m_frames.clear();
                m_audioDone = m_videoDone = false;
                m_audioBase = seek->first;
            }
            post([identifier = seek->second](auto& player) { player.didSeek(identifier); });
            continue;
        }

        bool worked = false;
        if (hasAudio && !audioDone && m_output->queuedFrames() < audioAheadFrames) {
            MediaTime time;
            size_t frames = file->readAudio(samples.mutableSpan().data(), 4096, time);
            if (frames)
                m_output->write(samples.span().data(), frames);
            else if (file->audioEnded()) {
                m_output->finish();
                Locker locker { m_lock };
                m_audioDone = true;
            }
            // Nothing yet (a media source's stream) is not work done.
            worked |= frames || file->audioEnded();
            if (frames && !hasVideo && !std::exchange(firstData, true))
                post([](auto& player) { player.didDecodeFirstData(); });
        }
        if (hasVideo && !videoDone && queuedFrames < framesAhead) {
            auto frame = file->readVideo();
            bool videoEnded = !frame && file->videoEnded();
            {
                Locker locker { m_lock };
                if (frame)
                    m_frames.push_back(WTF::move(*frame));
                else if (videoEnded)
                    m_videoDone = true;
            }
            worked |= frame || videoEnded;
            if (frame && !std::exchange(firstData, true))
                post([](auto& player) { player.didDecodeFirstData(); });
        }
        if (!worked) {
            Locker locker { m_lock };
            if (!m_stopping && !m_seekRequest)
                m_condition.waitFor(m_lock, 10_ms);
        }
    }
}

#if ENABLE(WEB_AUDIO)

// ---- Web Audio's input from the player ----

void AudioProvider::setClient(WeakPtr<AudioSourceProviderClient>&& client)
{
    m_client = WTF::move(client);
    Locker locker { m_lock };
    if (!m_output)
        return;
    m_output->setTapped(!!m_client);
    if (RefPtr client = m_client.get()) {
        auto format = m_output->readFormat();
        client->setFormat(format.channels, format.sampleRate);
    }
}

void AudioProvider::setOutput(AudioOutput* output)
{
    Locker locker { m_lock };
    m_output = output;
    if (!m_output || !m_client)
        return;
    m_output->setTapped(true);
    if (RefPtr client = m_client.get()) {
        auto format = m_output->readFormat();
        client->setFormat(format.channels, format.sampleRate);
    }
}

void AudioProvider::provideInput(AudioBus& bus, size_t framesToProcess)
{
    // Never waits on the engine thread: silence while it changes the output.
    if (!m_lock.tryLock()) {
        bus.zero();
        return;
    }
    Locker locker { AdoptLock, m_lock };
    if (!m_output) {
        bus.zero();
        return;
    }
    unsigned channels = m_output->readFormat().channels;
    m_interleaved.resize(framesToProcess * channels);
    size_t frames = m_output->read(m_interleaved.mutableSpan().data(), framesToProcess);
    for (unsigned channel = 0; channel < bus.numberOfChannels(); ++channel) {
        auto destination = bus.channel(channel)->mutableSpan();
        for (size_t frame = 0; frame < framesToProcess; ++frame)
            destination[frame] = channel < channels && frame < frames ? m_interleaved[frame * channels + channel] : 0;
    }
}

#endif // ENABLE(WEB_AUDIO)

// ---- The player, on the engine thread ----

MediaPlayerPrivateZeroG::MediaPlayerPrivateZeroG(MediaPlayer& player)
    : m_player(player)
    , m_timer(RunLoop::mainSingleton(), "MediaPlayerPrivateZeroG::Timer"_s, this, &MediaPlayerPrivateZeroG::tick)
{
}

MediaPlayerPrivateZeroG::~MediaPlayerPrivateZeroG()
{
    cancelLoad();
}

void MediaPlayerPrivateZeroG::load(const String& url)
{
    cancelLoad();
    RefPtr player = m_player.get();
    if (!player)
        return;
    // Sound starts on this thread, the first time (the decode thread then opens
    // the player's stream).
    AudioOutput::initialize();
    m_pipeline = Pipeline::create(*this);
    m_resource = player->mediaResourceLoader()->requestResource(ResourceRequest { URL { url } }, PlatformMediaResourceLoader::LoadOption::DisallowCaching);
    if (!m_resource) {
        setNetworkState(MediaPlayer::NetworkState::NetworkError);
        return;
    }
    m_resource->setClient(ResourceClient::create(*this, m_pipeline->bytes()));
    setNetworkState(MediaPlayer::NetworkState::Loading);
    setReadyState(MediaPlayer::ReadyState::HaveNothing);
    m_pipeline->start();
}

#if ENABLE(MEDIA_SOURCE)
void MediaPlayerPrivateZeroG::load(const URL&, const LoadOptions&, MediaSourcePrivateClient& client)
{
    cancelLoad();
    RefPtr existing = client.mediaSourcePrivate();
    if (existing && existing->platformType() == MediaPlatformType::ZeroG) {
        // The element's media source, attached again.
        m_mediaSource = static_cast<MediaSourcePrivateZeroG*>(existing.get());
        m_mediaSource->setPlayer(this);
        client.reOpen();
    } else
        m_mediaSource = MediaSourcePrivateZeroG::create(*this, client);
}

void MediaPlayerPrivateZeroG::readyStateFromMediaSourceChanged()
{
    // Every source buffer has its tracks: the decode thread can start.
    if (m_mediaSource && !m_pipeline && m_mediaSource->mediaPlayerReadyState() >= MediaPlayer::ReadyState::HaveMetadata) {
        AudioOutput::initialize();
        m_pipeline = Pipeline::create(*this);
        m_pipeline->setFileOpener([queues = Ref { m_mediaSource->queues() }] {
            return openMediaSourceFile(queues);
        });
        m_pipeline->start();
    }
    if (RefPtr player = m_player.get())
        player->readyStateChanged();
}
#endif

MediaPlayer::ReadyState MediaPlayerPrivateZeroG::readyState() const
{
#if ENABLE(MEDIA_SOURCE)
    if (m_mediaSource)
        return m_mediaSource->mediaPlayerReadyState();
#endif
    return m_readyState;
}

#if ENABLE(MEDIA_STREAM)
void MediaPlayerPrivateZeroG::load(MediaStreamPrivate&)
{
    setNetworkState(MediaPlayer::NetworkState::FormatError);
}
#endif

void MediaPlayerPrivateZeroG::cancelLoad()
{
    m_timer.stop();
    if (auto resource = std::exchange(m_resource, nullptr)) {
        resource->shutdown();
        resource->setClient(nullptr);
    }
#if ENABLE(WEB_AUDIO)
    m_audioProvider.setOutput(nullptr);
#endif
    if (auto pipeline = std::exchange(m_pipeline, nullptr))
        pipeline->stop();
#if ENABLE(MEDIA_SOURCE)
    m_mediaSource = nullptr;
#endif
    if (auto producer = std::exchange(m_seekProducer, std::nullopt))
        producer->reject(PlatformMediaError::Cancelled);
}

void MediaPlayerPrivateZeroG::play()
{
    if (!m_paused)
        return;
    m_position = currentTime();
    m_positionAt = MonotonicTime::now();
    m_paused = false;
    m_ended = false;
    if (auto* output = m_pipeline ? m_pipeline->output() : nullptr)
        output->setPlaying(true);
    m_timer.startRepeating(tickInterval);
    // The element asked for it: no playbackStateChanged() (that is for
    // changes the player makes by itself).
}

void MediaPlayerPrivateZeroG::pause()
{
    if (m_paused)
        return;
    m_position = currentTime();
    m_paused = true;
    if (auto* output = m_pipeline ? m_pipeline->output() : nullptr)
        output->setPlaying(false);
    // Paused by the element for having reached the duration before the
    // player saw its end (a stream whose audio runs a little longer): the
    // end it is waiting for.
    if (!m_ended && !m_seekTarget.isValid() && atDuration(m_position)) {
        m_ended = true;
        callOnMainThread([weakThis = ThreadSafeWeakPtr { *this }] {
            RefPtr protectedThis = weakThis.get();
            if (!protectedThis || !protectedThis->m_ended)
                return;
            if (RefPtr player = protectedThis->m_player.get())
                player->timeChanged();
        });
    }
}

bool MediaPlayerPrivateZeroG::atDuration(const MediaTime& time) const
{
    auto end = duration();
    return end.isFinite() && end > MediaTime::zeroTime() && time >= end;
}

FloatSize MediaPlayerPrivateZeroG::naturalSize() const
{
    return m_info.videoSize;
}

MediaTime MediaPlayerPrivateZeroG::duration() const
{
#if ENABLE(MEDIA_SOURCE)
    if (m_mediaSource)
        return m_mediaSource->duration();
#endif
    if (m_info.duration.isValid())
        return m_info.duration;
    if (m_endTime.isValid())
        return m_endTime;
    // Unknown until the end: an open-ended stream.
    return m_readyState >= MediaPlayer::ReadyState::HaveMetadata ? MediaTime::positiveInfiniteTime() : MediaTime::zeroTime();
}

MediaTime MediaPlayerPrivateZeroG::monotonicTime() const
{
    return m_position + MediaTime::createWithDouble((MonotonicTime::now() - m_positionAt).seconds() * m_rate);
}

MediaTime MediaPlayerPrivateZeroG::currentTime() const
{
    if (m_seekTarget.isValid())
        return m_seekTarget;
    if (m_ended)
        return duration().isFinite() ? duration() : m_position;
    if (m_paused || !m_pipeline)
        return m_position;
    MediaTime time;
    if (auto* output = m_pipeline->output())
        time = m_pipeline->audioTime(output->playedFrames());
    else
        time = monotonicTime();
    auto end = duration();
    return end.isFinite() ? std::min(time, end) : time;
}

Ref<MediaTimePromise> MediaPlayerPrivateZeroG::seekToTarget(const SeekTarget& target)
{
    if (auto producer = std::exchange(m_seekProducer, std::nullopt))
        producer->reject(PlatformMediaError::Cancelled);
    auto time = std::max(target.time, MediaTime::zeroTime());
    if (auto end = duration(); end.isFinite())
        time = std::min(time, end);
#if ENABLE(MEDIA_SOURCE)
    if (m_mediaSource)
        return seekMediaSource(target, time);
#endif
    if (!m_pipeline || m_readyState < MediaPlayer::ReadyState::HaveMetadata) {
        m_position = time;
        return MediaTimePromise::createAndResolve(time);
    }
    m_seekTarget = time;
    m_ended = false;
    m_seekProducer.emplace();
    Ref promise = m_seekProducer->promise();
    m_pipeline->requestSeek(time, ++m_seekIdentifier);
    return promise;
}

#if ENABLE(MEDIA_SOURCE)
Ref<MediaTimePromise> MediaPlayerPrivateZeroG::seekMediaSource(const SeekTarget& target, const MediaTime& time)
{
    // The media source waits for the target to be buffered and gives the
    // time to seek to (a keyframe, within the target's tolerance); its frames
    // from there are enqueued again once the decode thread starts over.
    m_seekTarget = time;
    m_ended = false;
    m_seekProducer.emplace();
    Ref promise = m_seekProducer->promise();
    auto identifier = ++m_seekIdentifier;
    m_mediaSource->waitForTarget(target)->whenSettled(RunLoop::mainSingleton(), [weakThis = ThreadSafeWeakPtr { *this }, identifier](auto&& result) {
        RefPtr protectedThis = weakThis.get();
        if (!protectedThis || identifier != protectedThis->m_seekIdentifier || !protectedThis->m_mediaSource)
            return;
        if (!result) {
            protectedThis->m_seekTarget = MediaTime::invalidTime();
            if (auto producer = std::exchange(protectedThis->m_seekProducer, std::nullopt))
                producer->reject(result.error());
            return;
        }
        auto time = *result;
        protectedThis->m_seekTarget = time;
        if (!protectedThis->m_pipeline) {
            // No metadata yet: playback starts there.
            protectedThis->m_position = std::exchange(protectedThis->m_seekTarget, MediaTime::invalidTime());
            if (auto producer = std::exchange(protectedThis->m_seekProducer, std::nullopt))
                producer->resolve(time);
            return;
        }
        protectedThis->m_mediaSource->queues().beginSeek();
        protectedThis->m_pipeline->requestSeek(time, identifier);
        protectedThis->m_mediaSource->reenqueueMediaForTime(time);
    });
    return promise;
}
#endif

void MediaPlayerPrivateZeroG::didSeek(uint64_t identifier)
{
    if (identifier != m_seekIdentifier || !m_seekTarget.isValid())
        return;
    m_position = std::exchange(m_seekTarget, MediaTime::invalidTime());
    m_positionAt = MonotonicTime::now();
    // The frame at the target shows at once, even paused.
    m_currentFrame = nullptr;
    if (!m_timer.isActive())
        m_timer.startRepeating(tickInterval);
    if (auto producer = std::exchange(m_seekProducer, std::nullopt))
        producer->resolve(m_position);
    if (RefPtr player = m_player.get())
        player->timeChanged();
}

void MediaPlayerPrivateZeroG::setRateDouble(double rate)
{
    m_position = currentTime();
    m_positionAt = MonotonicTime::now();
    m_rate = rate;
    if (auto* output = m_pipeline ? m_pipeline->output() : nullptr)
        output->setRate(rate);
    if (RefPtr player = m_player.get())
        player->rateChanged();
}

void MediaPlayerPrivateZeroG::setVolumeDouble(double volume)
{
    m_volume = volume;
    applyVolume();
}

void MediaPlayerPrivateZeroG::setMuted(bool muted)
{
    m_muted = muted;
    applyVolume();
}

void MediaPlayerPrivateZeroG::applyVolume()
{
    if (auto* output = m_pipeline ? m_pipeline->output() : nullptr)
        output->setVolume(m_muted ? 0 : m_volume);
}

const PlatformTimeRanges& MediaPlayerPrivateZeroG::buffered() const
{
#if ENABLE(MEDIA_SOURCE)
    if (m_mediaSource) {
        m_buffered = m_mediaSource->buffered();
        return m_buffered;
    }
#endif
    m_buffered.clear();
    auto end = duration();
    if (!m_pipeline || !end.isFinite() || end <= MediaTime::zeroTime())
        return m_buffered;
    auto& bytes = m_pipeline->bytes();
    auto length = bytes.length();
    if (bytes.isComplete())
        m_buffered.add(MediaTime::zeroTime(), end);
    else if (length && *length)
        m_buffered.add(MediaTime::zeroTime(), MediaTime::createWithDouble(end.toDouble() * bytes.available() / *length));
    return m_buffered;
}

bool MediaPlayerPrivateZeroG::didLoadingProgress() const
{
    if (!m_pipeline || isMediaSource())
        return false;
    uint64_t loaded = m_pipeline->bytes().available();
    return std::exchange(m_lastLoadedBytes, loaded) != loaded;
}

unsigned long long MediaPlayerPrivateZeroG::totalBytes() const
{
    if (!m_pipeline)
        return 0;
    auto length = m_pipeline->bytes().length();
    return length.value_or(0);
}

bool MediaPlayerPrivateZeroG::didPassCORSAccessCheck() const
{
#if ENABLE(MEDIA_SOURCE)
    if (m_mediaSource)
        return true; // the page's own bytes
#endif
    return m_resource && m_resource->didPassAccessControlCheck();
}

void MediaPlayerPrivateZeroG::paint(GraphicsContext& context, const FloatRect& rect)
{
    if (context.paintingDisabled() || !m_currentFrame)
        return;
    context.drawNativeImage(*m_currentFrame, rect, FloatRect { { }, m_currentFrame->size() });
}

RefPtr<NativeImage> MediaPlayerPrivateZeroG::nativeImageForCurrentTime()
{
    return m_currentFrame;
}

ColorSpace MediaPlayerPrivateZeroG::colorSpace()
{
    return ColorSpace::SRGB();
}

std::optional<VideoPlaybackQualityMetrics> MediaPlayerPrivateZeroG::videoPlaybackQualityMetrics()
{
    // Over the current load: every frame that showed or was skipped.
    VideoPlaybackQualityMetrics metrics;
    if (m_pipeline) {
        metrics.droppedVideoFrames = m_pipeline->droppedFrames();
        metrics.totalVideoFrames = m_pipeline->shownFrames() + metrics.droppedVideoFrames;
#if USE(COORDINATED_GRAPHICS)
        metrics.displayCompositedVideoFrames = m_layerProxy ? m_pipeline->shownFrames() : 0;
#endif
    }
    return metrics;
}

void MediaPlayerPrivateZeroG::didOpen(const MediaInfo& info)
{
    m_info = info;
    applyVolume();
    if (auto* output = m_pipeline ? m_pipeline->output() : nullptr) {
        output->setRate(m_rate);
        output->setPlaying(!m_paused);
#if ENABLE(WEB_AUDIO)
        m_audioProvider.setOutput(output);
#endif
    }
    // A media source gives its ready state itself.
    if (!isMediaSource())
        setReadyState(MediaPlayer::ReadyState::HaveMetadata);
    if (RefPtr player = m_player.get()) {
        player->durationChanged();
        player->sizeChanged();
        player->characteristicChanged();
    }
    // Frames and the end are looked at from now on.
    m_timer.startRepeating(tickInterval);
}

void MediaPlayerPrivateZeroG::didFailToOpen(const String&, bool networkError)
{
    setNetworkState(networkError ? MediaPlayer::NetworkState::NetworkError : MediaPlayer::NetworkState::FormatError);
}

void MediaPlayerPrivateZeroG::didDecodeFirstData()
{
    updateFrame();
    if (!isMediaSource())
        setReadyState(MediaPlayer::ReadyState::HaveEnoughData);
}

void MediaPlayerPrivateZeroG::didFinishLoading(bool failed)
{
    if (failed && m_readyState < MediaPlayer::ReadyState::HaveMetadata) {
        setNetworkState(MediaPlayer::NetworkState::NetworkError);
        return;
    }
    if (!failed)
        setNetworkState(MediaPlayer::NetworkState::Loaded);
}

void MediaPlayerPrivateZeroG::setReadyState(MediaPlayer::ReadyState state)
{
    if (m_readyState == state)
        return;
    m_readyState = state;
    if (RefPtr player = m_player.get())
        player->readyStateChanged();
}

void MediaPlayerPrivateZeroG::setNetworkState(MediaPlayer::NetworkState state)
{
    if (m_networkState == state)
        return;
    m_networkState = state;
    if (RefPtr player = m_player.get())
        player->networkStateChanged();
}

void MediaPlayerPrivateZeroG::updateFrame()
{
    if (!m_pipeline || !m_info.hasVideo)
        return;
#if USE(COORDINATED_GRAPHICS)
    // A composited video's frames go to the GPU as they are decoded, YUV.
    m_pipeline->setFramesStayYUV(m_layerProxy && m_layerProxy->isValid());
#endif
    if (auto image = m_pipeline->takeFrame(currentTime(), m_paused || !m_currentFrame)) {
#if USE(COORDINATED_GRAPHICS)
        m_currentImage = image;
#endif
        m_currentFrame = NativeImage::create(WTF::move(image));
#if USE(COORDINATED_GRAPHICS)
        // A composited video's frame goes to its layer; the page is not repainted.
        if (pushToLayer(false))
            return;
#endif
        if (RefPtr player = m_player.get())
            player->repaint();
    }
}

#if USE(COORDINATED_GRAPHICS)
void MediaPlayerPrivateZeroG::setPlatformLayerBufferProxy(Ref<CoordinatedPlatformLayerBufferProxy>&& proxy)
{
    m_layerProxy = WTF::move(proxy);
    pushToLayer(true);
}

RefPtr<CoordinatedPlatformLayerBufferProxy> MediaPlayerPrivateZeroG::platformLayerBufferProxy() const
{
    return m_layerProxy;
}

bool MediaPlayerPrivateZeroG::pushToLayer(bool initial)
{
    RefPtr proxy = m_layerProxy;
    if (!proxy || !proxy->isValid() || !m_currentImage)
        return false;
    auto buffer = CoordinatedPlatformLayerBufferSkiaImage::create(m_currentImage, proxy->threadSafeGrContext());
    if (!buffer)
        return false;
    if (initial)
        proxy->setInitialDisplayBuffer(WTF::move(buffer));
    else
        proxy->setDisplayBuffer(WTF::move(buffer));
    return true;
}
#endif

void MediaPlayerPrivateZeroG::tick()
{
    if (!m_pipeline)
        return;
    updateFrame();
    if (m_paused) {
        // Nothing moves while paused once the first frame is up.
        if (!m_info.hasVideo || m_currentFrame)
            m_timer.stop();
        return;
    }
    if (m_seekTarget.isValid() || (!m_pipeline->ended() && !atDuration(currentTime())))
        return;
    // The end: the clock stops there and the element sees it. It pauses the
    // player (ended), or seeks back to the start while it keeps playing (loop).
    m_position = currentTime();
    if (!m_info.duration.isValid() && !isMediaSource()) {
        m_endTime = m_position;
        if (RefPtr player = m_player.get())
            player->durationChanged();
    }
    m_ended = true;
    m_timer.stop();
    if (RefPtr player = m_player.get())
        player->timeChanged();
}

} // namespace ZeroG::Media

#endif
