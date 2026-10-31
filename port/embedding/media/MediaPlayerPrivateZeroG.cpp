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
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>

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
    }

    MediaPlayer::SupportsType supportsTypeAndCodecs(const MediaEngineSupportParameters& parameters) const final
    {
        // Files only: no Media Source Extensions or media streams yet.
        if (parameters.platformType != PlatformMediaDecodingType::FileOrHLS)
            return MediaPlayer::SupportsType::IsNotSupported;
        auto type = parameters.type.containerType().convertToASCIILowercase();
        auto* container = std::find_if(std::begin(containers), std::end(containers), [&](auto& entry) {
            return type == entry.type;
        });
        if (container == std::end(containers))
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
        showing = WTF::move(m_frames.front());
        m_frames.pop_front();
    }
    if (!showing && paused && !m_frames.empty()) {
        // The first frame after a seek or a load, shown before playback.
        showing = WTF::move(m_frames.front());
        m_frames.pop_front();
    }
    if (showing)
        m_condition.notifyAll(); // room for the next frame
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
    auto file = MediaFile::open(m_bytes, error);
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
            else {
                m_output->finish();
                Locker locker { m_lock };
                m_audioDone = true;
            }
            worked = true;
            if (frames && !hasVideo && !std::exchange(firstData, true))
                post([](auto& player) { player.didDecodeFirstData(); });
        }
        if (hasVideo && !videoDone && queuedFrames < framesAhead) {
            auto frame = file->readVideo();
            {
                Locker locker { m_lock };
                if (frame)
                    m_frames.push_back(WTF::move(*frame));
                else
                    m_videoDone = true;
            }
            worked = true;
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
void MediaPlayerPrivateZeroG::load(const URL&, const LoadOptions&, MediaSourcePrivateClient&)
{
    setNetworkState(MediaPlayer::NetworkState::FormatError);
}
#endif

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
    if (auto pipeline = std::exchange(m_pipeline, nullptr))
        pipeline->stop();
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
}

FloatSize MediaPlayerPrivateZeroG::naturalSize() const
{
    return m_info.videoSize;
}

MediaTime MediaPlayerPrivateZeroG::duration() const
{
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
    if (!m_pipeline)
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

void MediaPlayerPrivateZeroG::didOpen(const MediaInfo& info)
{
    m_info = info;
    applyVolume();
    if (auto* output = m_pipeline ? m_pipeline->output() : nullptr) {
        output->setRate(m_rate);
        output->setPlaying(!m_paused);
    }
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
    if (auto image = m_pipeline->takeFrame(currentTime(), m_paused || !m_currentFrame)) {
        m_currentFrame = NativeImage::create(WTF::move(image));
        if (RefPtr player = m_player.get())
            player->repaint();
    }
}

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
    if (m_seekTarget.isValid() || !m_pipeline->ended())
        return;
    // The end: the clock stops there and the element sees it. It pauses the
    // player (ended), or seeks back to the start while it keeps playing (loop).
    m_position = currentTime();
    if (!m_info.duration.isValid()) {
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
