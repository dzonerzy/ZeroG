#pragma once

// Media Source Extensions for ZeroG's media engine (standard, dev and debug
// profiles), for WebM and fragmented MP4: WebCore's MediaSource and SourceBuffer
// keep the coded frames (SourceBufferPrivate's track buffers, buffered ranges,
// eviction, seeking); this side parses the appended bytes (WebMStream,
// MP4Stream) and plays what
// WebCore enqueues. The enqueued frames wait in SampleQueues, which the
// player's decode thread reads through a MediaFile, so playback, the clock and
// painting are the same as for a file.

#include <WebCore/MediaPlayerPrivate.h>

#if ENABLE(MEDIA_SOURCE) && defined(ZEROG_MEDIA)

#include "media/MediaFile.h"
#include "media/StreamParser.h"
#include <WebCore/FloatSize.h>
#include <WebCore/MediaSample.h>
#include <WebCore/MediaSourcePrivate.h>
#include <WebCore/SharedBuffer.h>
#include <WebCore/SourceBufferPrivate.h>
#include <array>
#include <deque>
#include <wtf/Function.h>
#include <wtf/Lock.h>

namespace ZeroG::Media {

class MediaPlayerPrivateZeroG;

// A coded frame from a WebM byte stream.
class MediaSampleZeroG final : public WebCore::MediaSample {
public:
    static Ref<MediaSampleZeroG> create(StreamFrame&&, WebCore::TrackID, const WebCore::FloatSize&);

    std::span<const uint8_t> data() const { return m_data->span(); }

    MediaTime presentationTime() const final { return m_presentationTime; }
    MediaTime decodeTime() const final { return m_decodeTime; }
    MediaTime duration() const final { return m_duration; }
    WebCore::TrackID trackID() const final { return m_trackID; }
    size_t sizeInBytes() const final { return m_data->size() + sizeof(*this); }
    WebCore::FloatSize presentationSize() const final { return m_size; }
    void offsetTimestampsBy(const MediaTime&) final;
    void setTimestamps(const MediaTime& presentationTime, const MediaTime& decodeTime) final;
    Ref<WebCore::MediaSample> createNonDisplayingCopy() const final;
    SampleFlags flags() const final { return m_flags; }
    WebCore::PlatformSample platformSample() const final;
    Type type() const final { return Type::None; }

private:
    MediaSampleZeroG(Ref<WebCore::SharedBuffer>&&, const MediaTime& time, const MediaTime& decodeTime, const MediaTime& duration, WebCore::TrackID, const WebCore::FloatSize&, SampleFlags);

    const Ref<WebCore::SharedBuffer> m_data;
    MediaTime m_presentationTime;
    MediaTime m_decodeTime;
    MediaTime m_duration;
    WebCore::TrackID m_trackID;
    WebCore::FloatSize m_size;
    SampleFlags m_flags;
};

// What plays: the frames enqueued for the first audio and the first video
// track, in decode order. Filled on the main thread, read on the decode
// thread.
class SampleQueues : public ThreadSafeRefCounted<SampleQueues> {
public:
    static Ref<SampleQueues> create() { return adoptRef(*new SampleQueues); }

    enum class Kind : uint8_t { Audio, Video };
    struct Format {
        String codecID;
        Vector<uint8_t> codecPrivate;
        unsigned channels { 0 };
        unsigned sampleRate { 0 };
        WebCore::IntSize size;
    };

    // Main thread.
    bool hasFormat(Kind) const;
    void setFormat(Kind, Format&&);
    void enqueue(Kind, Ref<MediaSampleZeroG>&&);
    void flush(Kind);
    void markEnded(Kind);
    bool hasRoom(Kind) const;
    // Called on the main thread once the decode thread made room.
    void notifyWhenRoom(Kind, Function<void()>&&);
    // A seek: what is queued goes, and nothing is taken until the decode
    // thread reaches the seek (endSeek()).
    void beginSeek();

    // Decode thread.
    std::optional<Format> format(Kind) const;
    // The next frame, if any; `flushed` when the queue was flushed since the
    // last one (the decoder starts over).
    RefPtr<MediaSampleZeroG> take(Kind, bool& flushed);
    bool ended(Kind) const; // marked ended, and empty
    void endSeek();

private:
    SampleQueues() = default;

    struct Queue {
        std::optional<Format> format;
        std::deque<Ref<MediaSampleZeroG>> samples;
        bool ended { false };
        bool flushed { false };
        Function<void()> whenRoom;
    };
    Queue& queue(Kind kind) WTF_REQUIRES_LOCK(m_lock) { return m_queues[static_cast<size_t>(kind)]; }
    const Queue& queue(Kind kind) const WTF_REQUIRES_LOCK(m_lock) { return m_queues[static_cast<size_t>(kind)]; }

    mutable Lock m_lock;
    std::array<Queue, 2> m_queues WTF_GUARDED_BY_LOCK(m_lock);
    bool m_seeking WTF_GUARDED_BY_LOCK(m_lock) { false };
};

// The decode thread's MediaFile over the queues.
std::unique_ptr<MediaFile> openMediaSourceFile(SampleQueues&);

// MediaSource.isTypeSupported() and addSourceBuffer(): WebM with VP8, VP9,
// AV1, Opus or Vorbis; MP4 with VP9, AV1 or Opus, and on Windows H.264 and AAC.
bool supportsMediaSourceType(const WebCore::ContentType&);

class MediaSourcePrivateZeroG final : public WebCore::MediaSourcePrivate {
public:
    static Ref<MediaSourcePrivateZeroG> create(MediaPlayerPrivateZeroG&, WebCore::MediaSourcePrivateClient&);
    ~MediaSourcePrivateZeroG();

    constexpr WebCore::MediaPlatformType platformType() const final { return WebCore::MediaPlatformType::ZeroG; }
    RefPtr<WebCore::MediaPlayerPrivateInterface> player() const final;
    void setPlayer(WebCore::MediaPlayerPrivateInterface*) final;

    SampleQueues& queues() const { return m_queues; }
#if !RELEASE_LOG_DISABLED
    const Logger& logger() const { return m_logger; }
    uint64_t logIdentifier() const { return m_logIdentifier; }
#endif

private:
    MediaSourcePrivateZeroG(MediaPlayerPrivateZeroG&, WebCore::MediaSourcePrivateClient&);

    AddStatus addSourceBuffer(const WebCore::ContentType&, const WebCore::MediaSourceConfiguration&, RefPtr<WebCore::SourceBufferPrivate>&) final;
    void durationChanged(const MediaTime&) final;
    void notifyActiveSourceBuffersChanged() final;

    ThreadSafeWeakPtr<MediaPlayerPrivateZeroG> m_player;
    const Ref<SampleQueues> m_queues;
#if !RELEASE_LOG_DISABLED
    const Ref<const Logger> m_logger;
    const uint64_t m_logIdentifier;
#endif
};

} // namespace ZeroG::Media

#endif // ENABLE(MEDIA_SOURCE) && defined(ZEROG_MEDIA)
