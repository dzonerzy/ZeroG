// Audio-only files: MP3, FLAC and WAV with dr_libs, Ogg Opus with opusfile, Ogg
// Vorbis with vorbisfile. Each reads the arriving bytes through a MediaReader,
// so playback starts before the file has downloaded.

#include "config.h"
#include "media/MediaFile.h"

#if ENABLE(VIDEO) && defined(ZEROG_MEDIA)

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#define DR_MP3_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#define DR_WAV_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#define DR_FLAC_NO_STDIO
#define DR_WAV_NO_STDIO
#include <dr_flac.h>
#include <dr_mp3.h>
#include <dr_wav.h>
#include <opus/opusfile.h>
#include <vorbis/vorbisfile.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace ZeroG::Media {

namespace {

constexpr unsigned maximumChannels = 8;

MediaReader::Origin origin(int whence)
{
    return whence == SEEK_CUR ? MediaReader::Origin::Current : whence == SEEK_END ? MediaReader::Origin::End : MediaReader::Origin::Start;
}

class AudioFile final : public MediaFile {
public:
    enum class Format : uint8_t { MP3, FLAC, WAV, OggOpus, OggVorbis };

    static std::unique_ptr<AudioFile> create(MediaBytes& bytes, Format format, String& error)
    {
        auto file = std::unique_ptr<AudioFile>(new AudioFile(bytes, format));
        if (!file->openDecoder(error))
            return nullptr;
        return file;
    }

    ~AudioFile()
    {
        switch (m_format) {
        case Format::MP3:
            if (m_mp3Open)
                drmp3_uninit(&m_mp3);
            break;
        case Format::FLAC:
            if (m_flac)
                drflac_close(m_flac);
            break;
        case Format::WAV:
            if (m_wavOpen)
                drwav_uninit(&m_wav);
            break;
        case Format::OggOpus:
            if (m_opus)
                op_free(m_opus);
            break;
        case Format::OggVorbis:
            if (m_vorbisOpen)
                ov_clear(&m_vorbis);
            break;
        }
    }

    size_t readAudio(float* out, size_t frames, MediaTime& time) final
    {
        time = MediaTime(m_position, m_info.sampleRate);
        size_t read = 0;
        switch (m_format) {
        case Format::MP3:
            read = drmp3_read_pcm_frames_f32(&m_mp3, frames, out);
            break;
        case Format::FLAC:
            read = drflac_read_pcm_frames_f32(m_flac, frames, out);
            break;
        case Format::WAV:
            read = drwav_read_pcm_frames_f32(&m_wav, frames, out);
            break;
        case Format::OggOpus: {
            // op_read_float stops at a packet's end: loop until `frames`.
            while (read < frames) {
                int count = op_read_float(m_opus, out + read * m_info.channels, (frames - read) * m_info.channels, nullptr);
                if (count <= 0)
                    break;
                read += count;
            }
            break;
        }
        case Format::OggVorbis:
            while (read < frames) {
                float** pcm = nullptr;
                int section = 0;
                long count = ov_read_float(&m_vorbis, &pcm, frames - read, &section);
                if (count <= 0)
                    break;
                for (long frame = 0; frame < count; ++frame) {
                    for (unsigned channel = 0; channel < m_info.channels; ++channel)
                        out[(read + frame) * m_info.channels + channel] = pcm[channel][frame];
                }
                read += count;
            }
            break;
        }
        m_position += read;
        return read;
    }

    bool seek(const MediaTime& time) final
    {
        uint64_t frame = std::max(0.0, time.toDouble()) * m_info.sampleRate;
        bool done = false;
        switch (m_format) {
        case Format::MP3:
            done = drmp3_seek_to_pcm_frame(&m_mp3, frame);
            break;
        case Format::FLAC:
            done = drflac_seek_to_pcm_frame(m_flac, frame);
            break;
        case Format::WAV:
            done = drwav_seek_to_pcm_frame(&m_wav, frame);
            break;
        case Format::OggOpus:
            done = !op_pcm_seek(m_opus, frame);
            break;
        case Format::OggVorbis:
            done = !ov_pcm_seek(&m_vorbis, frame);
            break;
        }
        if (done)
            m_position = frame;
        return done;
    }

private:
    AudioFile(MediaBytes& bytes, Format format)
        : m_reader(bytes)
        , m_format(format)
    {
    }

    // dr_libs' callbacks, over the reader.
    static size_t readBytes(void* reader, void* buffer, size_t count)
    {
        return static_cast<MediaReader*>(reader)->read({ static_cast<uint8_t*>(buffer), count });
    }
    template<typename Origin>
    static unsigned seekBytes(void* reader, int offset, Origin whence)
    {
        auto origin = static_cast<int>(whence) == 1 ? MediaReader::Origin::Current : static_cast<int>(whence) == 2 ? MediaReader::Origin::End : MediaReader::Origin::Start;
        return static_cast<MediaReader*>(reader)->seek(offset, origin);
    }
    template<typename Cursor>
    static unsigned tellBytes(void* reader, Cursor* cursor)
    {
        *cursor = static_cast<MediaReader*>(reader)->position();
        return true;
    }

    bool openDecoder(String& error)
    {
        unsigned channels = 0;
        unsigned sampleRate = 0;
        uint64_t totalFrames = 0;
        switch (m_format) {
        case Format::MP3:
            m_mp3Open = drmp3_init(&m_mp3, readBytes, seekBytes<drmp3_seek_origin>, tellBytes<drmp3_int64>, nullptr, &m_reader, nullptr);
            if (!m_mp3Open)
                break;
            channels = m_mp3.channels;
            sampleRate = m_mp3.sampleRate;
            // An MP3 knows its length only from its frames: counted once the
            // file has arrived (dr_mp3 reads it all for this).
            if (m_reader.bytes().isComplete()) {
                totalFrames = drmp3_get_pcm_frame_count(&m_mp3);
                drmp3_seek_to_pcm_frame(&m_mp3, 0);
            }
            break;
        case Format::FLAC:
            m_flac = drflac_open(readBytes, seekBytes<drflac_seek_origin>, tellBytes<drflac_int64>, &m_reader, nullptr);
            if (!m_flac)
                break;
            channels = m_flac->channels;
            sampleRate = m_flac->sampleRate;
            totalFrames = m_flac->totalPCMFrameCount;
            break;
        case Format::WAV:
            m_wavOpen = drwav_init(&m_wav, readBytes, seekBytes<drwav_seek_origin>, tellBytes<drwav_int64>, &m_reader, nullptr);
            if (!m_wavOpen)
                break;
            channels = m_wav.channels;
            sampleRate = m_wav.sampleRate;
            totalFrames = m_wav.totalPCMFrameCount;
            break;
        case Format::OggOpus: {
            OpusFileCallbacks callbacks {
                [](void* reader, unsigned char* buffer, int count) -> int {
                    return static_cast<MediaReader*>(reader)->read({ buffer, static_cast<size_t>(count) });
                },
                [](void* reader, opus_int64 offset, int whence) -> int {
                    return static_cast<MediaReader*>(reader)->seek(offset, origin(whence)) ? 0 : -1;
                },
                [](void* reader) -> opus_int64 {
                    return static_cast<MediaReader*>(reader)->position();
                },
                nullptr,
            };
            int result = 0;
            m_opus = op_open_callbacks(&m_reader, &callbacks, nullptr, 0, &result);
            if (!m_opus)
                break;
            channels = op_channel_count(m_opus, -1);
            sampleRate = 48000; // opusfile always decodes at 48 kHz
            if (auto total = op_pcm_total(m_opus, -1); total > 0)
                totalFrames = total;
            break;
        }
        case Format::OggVorbis: {
            ov_callbacks callbacks {
                [](void* buffer, size_t size, size_t count, void* reader) -> size_t {
                    return static_cast<MediaReader*>(reader)->read({ static_cast<uint8_t*>(buffer), size * count }) / std::max<size_t>(size, 1);
                },
                [](void* reader, ogg_int64_t offset, int whence) -> int {
                    return static_cast<MediaReader*>(reader)->seek(offset, origin(whence)) ? 0 : -1;
                },
                nullptr,
                [](void* reader) -> long {
                    return static_cast<MediaReader*>(reader)->position();
                },
            };
            m_vorbisOpen = !ov_open_callbacks(&m_reader, &m_vorbis, nullptr, 0, callbacks);
            if (!m_vorbisOpen)
                break;
            auto* information = ov_info(&m_vorbis, -1);
            channels = information ? information->channels : 0;
            sampleRate = information ? information->rate : 0;
            if (auto total = ov_pcm_total(&m_vorbis, -1); total > 0)
                totalFrames = total;
            break;
        }
        }
        if (!channels || channels > maximumChannels || !sampleRate) {
            error = "damaged or unsupported audio file"_s;
            return false;
        }
        m_info.hasAudio = true;
        m_info.channels = channels;
        m_info.sampleRate = sampleRate;
        if (totalFrames)
            m_info.duration = MediaTime(totalFrames, sampleRate);
        return true;
    }

    MediaReader m_reader;
    Format m_format;
    uint64_t m_position { 0 }; // frames
    drmp3 m_mp3 { };
    bool m_mp3Open { false };
    drflac* m_flac { nullptr };
    drwav m_wav { };
    bool m_wavOpen { false };
    OggOpusFile* m_opus { nullptr };
    OggVorbis_File m_vorbis { };
    bool m_vorbisOpen { false };
};

bool contains(std::span<const uint8_t> haystack, std::span<const uint8_t> needle)
{
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
}

} // namespace

std::unique_ptr<MediaFile> openAudioFile(MediaBytes& bytes, std::span<const uint8_t> head, String& error)
{
    auto starts = [&](const char* magic) {
        size_t length = strlen(magic);
        return head.size() >= length && !memcmp(head.data(), magic, length);
    };
    std::optional<AudioFile::Format> format;
    if (starts("OggS")) {
        static constexpr uint8_t opusHead[] = { 'O', 'p', 'u', 's', 'H', 'e', 'a', 'd' };
        static constexpr uint8_t vorbisHead[] = { 0x01, 'v', 'o', 'r', 'b', 'i', 's' };
        if (contains(head, opusHead))
            format = AudioFile::Format::OggOpus;
        else if (contains(head, vorbisHead))
            format = AudioFile::Format::OggVorbis;
    } else if (starts("fLaC"))
        format = AudioFile::Format::FLAC;
    else if ((starts("RIFF") || starts("RF64")) && head.size() >= 12 && !memcmp(head.data() + 8, "WAVE", 4))
        format = AudioFile::Format::WAV;
    else if (starts("ID3") || (head.size() >= 2 && head[0] == 0xFF && (head[1] & 0xE0) == 0xE0))
        format = AudioFile::Format::MP3;
    if (!format) {
        error = "not a WebM, Ogg, MP3, FLAC or WAV file"_s;
        return nullptr;
    }
    return AudioFile::create(bytes, *format, error);
}

} // namespace ZeroG::Media

#endif
