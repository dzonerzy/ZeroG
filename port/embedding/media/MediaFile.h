#pragma once

// A media file the player decodes (ZeroG.md, "Media"): what it holds, its
// audio as float samples and its video as RGBA images, from a position.
// Decode thread only. The formats are the royalty-free ones every browser
// plays: WebM (VP8, VP9, AV1 video; Opus, Vorbis audio), Ogg (Opus, Vorbis),
// MP3, FLAC and WAV; on Windows also MP4 (H.264, AAC), decoded by the OS.

#include "media/MediaBytes.h"
#include <WebCore/IntSize.h>
#include <memory>
#include <optional>
#include <wtf/MediaTime.h>
#include <wtf/text/WTFString.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkImage.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace ZeroG::Media {

struct MediaInfo {
    MediaTime duration { MediaTime::invalidTime() }; // invalid: not known
    bool hasAudio { false };
    bool hasVideo { false };
    WebCore::IntSize videoSize;
    unsigned sampleRate { 0 };
    unsigned channels { 0 };
};

struct DecodedFrame {
    MediaTime time;
    sk_sp<SkImage> image; // RGBA, opaque
};

class MediaFile {
public:
    virtual ~MediaFile() = default;

    // Recognizes the format from its first bytes and reads its headers
    // (waiting for them to arrive); null with `error` when it is none of the
    // formats or is damaged.
    static std::unique_ptr<MediaFile> open(MediaBytes&, String& error);

    const MediaInfo& info() const { return m_info; }

    // Up to `frames` frames of interleaved float samples (info().channels per
    // frame, at info().sampleRate) into `out`; `time` is the first one's. 0 at
    // the end of the audio.
    virtual size_t readAudio(float* out, size_t frames, MediaTime& time) = 0;
    // The next video frame; nullopt at the end of the video.
    virtual std::optional<DecodedFrame> readVideo() { return std::nullopt; }
    // After a read that gave nothing: whether that is the end, or only that
    // nothing has arrived yet (Media Source Extensions' streams).
    virtual bool audioEnded() const { return true; }
    virtual bool videoEnded() const { return true; }
    // Positions the audio at `time`, and the video at the frame showing then.
    virtual bool seek(const MediaTime&) = 0;

protected:
    MediaInfo m_info;
};

// The formats' detection and opening, one function each (WebMFile.cpp,
// AudioFile.cpp).
std::unique_ptr<MediaFile> openWebM(MediaBytes&, String& error);
std::unique_ptr<MediaFile> openAudioFile(MediaBytes&, std::span<const uint8_t> head, String& error);
#if OS(WINDOWS)
// MP4 (H.264, AAC) through the OS's Media Foundation (MediaFoundationFile.cpp),
// when the system has it (Windows "N" editions do not).
bool mediaFoundationAvailable();
std::unique_ptr<MediaFile> openMediaFoundation(MediaBytes&, std::span<const uint8_t> head, String& error);
#endif

// YUV planes to an RGBA image, with the stream's matrix and range.
enum class ColorMatrix : uint8_t { BT601, BT709, BT2020 };
struct YUVPlanes {
    const uint8_t* y;
    const uint8_t* u;
    const uint8_t* v;
    int yStride;
    int uStride;
    int vStride;
    int width;
    int height;
    enum class Subsampling : uint8_t { I420, I422, I444 } subsampling { Subsampling::I420 };
    ColorMatrix matrix { ColorMatrix::BT601 };
    bool fullRange { false };
    bool highBitDepth { false }; // 10 bits in 16-bit samples (I420 only)
};
sk_sp<SkImage> convertToImage(const YUVPlanes&);
// NV12 (a Y plane, then U and V interleaved at half resolution), what hardware
// and OS decoders give.
sk_sp<SkImage> convertNV12ToImage(const uint8_t* y, int yStride, const uint8_t* uv, int uvStride, int width, int height, ColorMatrix, bool fullRange);

} // namespace ZeroG::Media
