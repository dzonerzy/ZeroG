#pragma once

// ZeroG's media engine for WebCore (docs/design/build-system-static-linking.md, "Media"; standard, dev and debug
// profiles): <audio> and <video> for WebM (VP8, VP9, AV1 with Opus or Vorbis),
// Ogg (Opus, Vorbis), MP3, FLAC and WAV. The page's loader brings the bytes; a
// decode thread per player demuxes and decodes them (MediaFile) ahead of the
// clock, its sound goes out through SDL3 (AudioOutput) and its frames wait in
// a queue. On the engine thread the clock (the audio's position, or the
// monotonic clock for a silent video) picks the frame showing, which paint()
// draws.

#include <WebCore/MediaPlayerPrivate.h>

#if ENABLE(VIDEO) && defined(ZEROG_MEDIA)

#include "media/MediaFile.h"
#include <WebCore/PlatformMediaResourceLoader.h>
#if ENABLE(WEB_AUDIO)
#include <WebCore/AudioSourceProvider.h>
#include <WebCore/AudioSourceProviderClient.h>
#endif
#include <deque>
#include <wtf/Condition.h>
#include <wtf/Lock.h>
#include <wtf/MonotonicTime.h>
#include <wtf/RunLoop.h>
#include <wtf/ThreadSafeWeakPtr.h>
#include <wtf/Threading.h>

namespace ZeroG::Media {

class AudioOutput;
class MediaPlayerPrivateZeroG;
class MediaSourcePrivateZeroG;

// Registers the engine with WebCore (Embedding.cpp's initializeProcess).
void registerMediaEngine();

// What the decode thread and the engine thread share for one player.
class Pipeline : public ThreadSafeRefCounted<Pipeline> {
public:
    static Ref<Pipeline> create(MediaPlayerPrivateZeroG& player) { return adoptRef(*new Pipeline(player)); }
    ~Pipeline();

    MediaBytes& bytes() { return m_bytes.get(); }
    // After the "opened" event: the sound's output, if there is one.
    AudioOutput* output() const { return m_output.get(); }

    // Engine thread, before start(): what the decode thread reads, instead of
    // a file from bytes() (a media source's stream).
    void setFileOpener(Function<std::unique_ptr<MediaFile>()>&& opener) { m_openFile = WTF::move(opener); }
    void start();
    // Frames from now on stay YUV for the compositor's GPU to convert (a
    // composited player), or become RGBA on the decode thread.
    void setFramesStayYUV(bool stayYUV) { m_framesStayYUV = stayYUV; }
    void stop(); // waits for the thread
    void requestSeek(const MediaTime&, uint64_t identifier);
    // The frame showing at `time` (frames before it are dropped); null when
    // no frame is new. `paused`: the first frame queued shows even if early
    // (after a seek, before playback).
    sk_sp<SkImage> takeFrame(const MediaTime& time, bool paused);
    // getVideoPlaybackQuality(): frames shown, and those the clock went past.
    uint32_t shownFrames() const { return m_shownFrames; }
    uint32_t droppedFrames() const { return m_droppedFrames; }
    // The audio's time once its output has played `played` frames since the
    // last seek.
    MediaTime audioTime(uint64_t played) const;
    bool ended() const;

private:
    explicit Pipeline(MediaPlayerPrivateZeroG&);
    void run();
    void post(Function<void(MediaPlayerPrivateZeroG&)>&&);

    ThreadSafeWeakPtr<MediaPlayerPrivateZeroG> m_player;
    const Ref<MediaBytes> m_bytes;
    Function<std::unique_ptr<MediaFile>()> m_openFile;
    std::atomic<bool> m_framesStayYUV { false };
    std::atomic<uint32_t> m_shownFrames { 0 };
    std::atomic<uint32_t> m_droppedFrames { 0 };
    RefPtr<Thread> m_thread;
    std::unique_ptr<AudioOutput> m_output; // created on the decode thread before "opened"

    mutable Lock m_lock;
    Condition m_condition;
    bool m_stopping WTF_GUARDED_BY_LOCK(m_lock) { false };
    std::optional<std::pair<MediaTime, uint64_t>> m_seekRequest WTF_GUARDED_BY_LOCK(m_lock);
    std::deque<DecodedFrame> m_frames WTF_GUARDED_BY_LOCK(m_lock);
    MediaTime m_audioBase WTF_GUARDED_BY_LOCK(m_lock) { MediaTime::zeroTime() };
    unsigned m_sampleRate WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    bool m_audioDone WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_videoDone WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_hasAudio WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_hasVideo WTF_GUARDED_BY_LOCK(m_lock) { false };
};

#if ENABLE(WEB_AUDIO)
// A MediaElementAudioSourceNode's input: while it is attached, the player's
// sound goes into the audio graph instead of the device.
class AudioProvider final : public WebCore::AudioSourceProvider {
public:
    // Web Audio's rendering thread.
    void provideInput(WebCore::AudioBus&, size_t framesToProcess) final;
    // Engine thread.
    void setClient(WeakPtr<WebCore::AudioSourceProviderClient>&&) final;
    // The current load's sound (null between loads), engine thread.
    void setOutput(AudioOutput*);

private:
    WeakPtr<WebCore::AudioSourceProviderClient> m_client;
    Lock m_lock;
    AudioOutput* m_output WTF_GUARDED_BY_LOCK(m_lock) { nullptr };
    Vector<float> m_interleaved; // the rendering thread's
};
#endif

class MediaPlayerPrivateZeroG final : public WebCore::MediaPlayerPrivateInterface, public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<MediaPlayerPrivateZeroG, WTF::DestructionThread::Main> {
public:
    static Ref<MediaPlayerPrivateZeroG> create(WebCore::MediaPlayer& player) { return adoptRef(*new MediaPlayerPrivateZeroG(player)); }
    ~MediaPlayerPrivateZeroG();

    void ref() const final { ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr::ref(); }
    void deref() const final { ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr::deref(); }

    constexpr WebCore::MediaPlayerType mediaPlayerType() const final { return WebCore::MediaPlayerType::ZeroG; }
    String engineDescription() const final { return "ZeroG"_s; }

    void load(const String&) final;
#if ENABLE(MEDIA_SOURCE)
    void load(const URL&, const LoadOptions&, WebCore::MediaSourcePrivateClient&) final;
#endif
#if ENABLE(MEDIA_STREAM)
    void load(WebCore::MediaStreamPrivate&) final;
#endif
    void cancelLoad() final;

    void play() final;
    void pause() final;
    bool paused() const final { return m_paused; }

    WebCore::FloatSize naturalSize() const final;
    bool hasVideo() const final { return m_info.hasVideo; }
    bool hasAudio() const final { return m_info.hasAudio; }
    void setPageIsVisible(bool) final { }

    MediaTime duration() const final;
    MediaTime currentTime() const final;
    Ref<WebCore::MediaTimePromise> seekToTarget(const WebCore::SeekTarget&) final;
    void setRateDouble(double) final;
    double rate() const final { return m_rate; }
    void setVolumeDouble(double) final;
    void setMuted(bool) final;

    WebCore::MediaPlayer::NetworkState networkState() const final { return m_networkState; }
    WebCore::MediaPlayer::ReadyState readyState() const final;
    const WebCore::PlatformTimeRanges& buffered() const final;
    MediaTime maxTimeSeekable() const final { return duration(); }
    bool didLoadingProgress() const final;
    unsigned long long totalBytes() const final;
    bool didPassCORSAccessCheck() const final;

    void paint(WebCore::GraphicsContext&, const WebCore::FloatRect&) final;
    RefPtr<WebCore::NativeImage> nativeImageForCurrentTime() final;
    WebCore::ColorSpace colorSpace() final;
    std::optional<WebCore::VideoPlaybackQualityMetrics> videoPlaybackQualityMetrics() final;
#if ENABLE(WEB_AUDIO)
    WebCore::AudioSourceProvider* audioSourceProvider() final { return &m_audioProvider; }
#endif
#if USE(COORDINATED_GRAPHICS)
    // Composited views: the video is a layer whose frames go to the
    // compositor as they are, with no repaint of the page.
    bool supportsAcceleratedRendering() const final { return true; }
    void setPlatformLayerBufferProxy(Ref<WebCore::CoordinatedPlatformLayerBufferProxy>&&) final;
    RefPtr<WebCore::CoordinatedPlatformLayerBufferProxy> platformLayerBufferProxy() const final;
#endif

#if ENABLE(MEDIA_SOURCE)
    // A media source's (MediaSourceZeroG.h).
    void readyStateFromMediaSourceChanged() final;
#endif
    RefPtr<WebCore::MediaPlayer> mediaPlayer() const { return m_player.get(); }

    // From the pipeline, on the engine thread.
    void didOpen(const MediaInfo&);
    void didFailToOpen(const String&, bool networkError);
    void didDecodeFirstData();
    void didSeek(uint64_t identifier);
    // From the resource's client.
    void didFinishLoading(bool failed);

private:
    explicit MediaPlayerPrivateZeroG(WebCore::MediaPlayer&);

    void tick();
    void updateFrame();
    // Whether a time is at (or past) a known duration.
    bool atDuration(const MediaTime&) const;
#if ENABLE(MEDIA_SOURCE)
    bool isMediaSource() const { return !!m_mediaSource; }
    Ref<WebCore::MediaTimePromise> seekMediaSource(const WebCore::SeekTarget&, const MediaTime&);
#else
    bool isMediaSource() const { return false; }
#endif
    void setReadyState(WebCore::MediaPlayer::ReadyState);
    void setNetworkState(WebCore::MediaPlayer::NetworkState);
    void applyVolume();
    // The playback clock without audio: from a time and the instant it held.
    MediaTime monotonicTime() const;

    ThreadSafeWeakPtr<WebCore::MediaPlayer> m_player;
    RefPtr<Pipeline> m_pipeline;
    RefPtr<WebCore::PlatformMediaResource> m_resource;
#if ENABLE(MEDIA_SOURCE)
    // Media Source Extensions: WebCore's MediaSource gives the ready state,
    // duration and buffered ranges; the pipeline starts once it has metadata.
    RefPtr<MediaSourcePrivateZeroG> m_mediaSource;
#endif
    RunLoop::Timer m_timer;

    MediaInfo m_info;
    WebCore::MediaPlayer::NetworkState m_networkState { WebCore::MediaPlayer::NetworkState::Empty };
    WebCore::MediaPlayer::ReadyState m_readyState { WebCore::MediaPlayer::ReadyState::HaveNothing };
    bool m_paused { true };
    bool m_ended { false };
    double m_rate { 1 };
    double m_volume { 1 };
    bool m_muted { false };
    // The clock: `m_position` while paused (and the base of the monotonic one).
    MediaTime m_position { MediaTime::zeroTime() };
    MonotonicTime m_positionAt;
    // The duration once the end is reached, when the file did not say.
    MediaTime m_endTime { MediaTime::invalidTime() };

    std::optional<WebCore::MediaTimePromise::Producer> m_seekProducer;
    MediaTime m_seekTarget { MediaTime::invalidTime() };
    uint64_t m_seekIdentifier { 0 };

    RefPtr<WebCore::NativeImage> m_currentFrame;
#if USE(COORDINATED_GRAPHICS)
    RefPtr<WebCore::CoordinatedPlatformLayerBufferProxy> m_layerProxy;
    sk_sp<SkImage> m_currentImage;
    bool pushToLayer(bool initial);
#endif
#if ENABLE(WEB_AUDIO)
    AudioProvider m_audioProvider;
#endif
    mutable uint64_t m_lastLoadedBytes { 0 };
    mutable WebCore::PlatformTimeRanges m_buffered;
};

} // namespace ZeroG::Media

#endif
