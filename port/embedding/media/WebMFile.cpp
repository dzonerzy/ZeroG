// WebM (Matroska) files: demuxed with libwebm's mkvparser, cluster by cluster
// as the bytes arrive; VP8 and VP9 video decoded with libvpx, AV1 with dav1d;
// Opus audio with libopus, Vorbis with libvorbis. A seek goes through the
// file's cues when it has them, else back to the last keyframe before the
// target, and decodes forward from there.

#include "config.h"
#include "media/MediaFile.h"

#if ENABLE(VIDEO) && defined(ZEROG_MEDIA)

#include "media/Decoders.h"
#include <deque>
#include <wtf/StdLibExtras.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <dav1d/dav1d.h>
#include <mkvparser/mkvparser.h>
#include <opus/opus.h>
#include <vorbis/codec.h>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace ZeroG::Media {

namespace {

constexpr long long nanosecondsPerSecond = 1000000000;

MediaTime fromNanoseconds(long long nanoseconds)
{
    return MediaTime(nanoseconds, nanosecondsPerSecond);
}

// mkvparser's reader over the arriving bytes. Its reads wait for them, so the
// length reported available is the whole file's when known.
class Reader final : public mkvparser::IMkvReader {
public:
    explicit Reader(MediaBytes& bytes)
        : m_bytes(bytes)
    {
    }

    int Read(long long position, long length, unsigned char* buffer) final
    {
        if (position < 0 || length < 0)
            return -1;
        if (!length)
            return 0;
        return m_bytes.read(position, { buffer, static_cast<size_t>(length) }) == static_cast<size_t>(length) ? 0 : -1;
    }

    int Length(long long* total, long long* available) final
    {
        auto length = m_bytes.length();
        if (total)
            *total = length ? static_cast<long long>(*length) : -1;
        if (available)
            *available = length ? static_cast<long long>(*length) : static_cast<long long>(m_bytes.available());
        return 0;
    }

private:
    MediaBytes& m_bytes;
};

class VPXDecoder final : public VideoDecoder {
public:
    static std::unique_ptr<VPXDecoder> create(bool vp9)
    {
        auto decoder = std::unique_ptr<VPXDecoder>(new VPXDecoder);
        vpx_codec_dec_cfg_t configuration { };
        configuration.threads = 4;
        if (vpx_codec_dec_init(&decoder->m_context, vp9 ? vpx_codec_vp9_dx() : vpx_codec_vp8_dx(), &configuration, 0))
            return nullptr;
        decoder->m_initialized = true;
        return decoder;
    }

    ~VPXDecoder()
    {
        if (m_initialized)
            vpx_codec_destroy(&m_context);
    }

    void decode(const Packet& packet, Vector<DecodedFrame>& out) final
    {
        if (vpx_codec_decode(&m_context, packet.data.span().data(), packet.data.size(), nullptr, 0))
            return;
        vpx_codec_iter_t iterator = nullptr;
        while (auto* image = vpx_codec_get_frame(&m_context, &iterator)) {
            YUVPlanes planes { image->planes[0], image->planes[1], image->planes[2], image->stride[0], image->stride[1], image->stride[2],
                static_cast<int>(image->d_w), static_cast<int>(image->d_h) };
            switch (image->fmt) {
            case VPX_IMG_FMT_I420:
                planes.subsampling = YUVPlanes::Subsampling::I420;
                break;
            case VPX_IMG_FMT_I422:
                planes.subsampling = YUVPlanes::Subsampling::I422;
                break;
            case VPX_IMG_FMT_I444:
                planes.subsampling = YUVPlanes::Subsampling::I444;
                break;
            default:
                continue; // high bit depth needs libvpx built for it
            }
            planes.matrix = image->cs == VPX_CS_BT_709 ? ColorMatrix::BT709 : image->cs == VPX_CS_BT_2020 ? ColorMatrix::BT2020 : ColorMatrix::BT601;
            planes.fullRange = image->range == VPX_CR_FULL_RANGE;
            if (auto converted = convertToImage(planes))
                out.append({ fromNanoseconds(packet.time), WTF::move(converted) });
        }
    }

    void flush() final { }

private:
    VPXDecoder() = default;
    vpx_codec_ctx_t m_context { };
    bool m_initialized { false };
};

class Dav1dDecoder final : public VideoDecoder {
public:
    static std::unique_ptr<Dav1dDecoder> create()
    {
        auto decoder = std::unique_ptr<Dav1dDecoder>(new Dav1dDecoder);
        Dav1dSettings settings;
        dav1d_default_settings(&settings);
        settings.n_threads = 4;
        settings.max_frame_delay = 1; // a frame out for each frame in
        if (dav1d_open(&decoder->m_context, &settings) < 0)
            return nullptr;
        return decoder;
    }

    ~Dav1dDecoder()
    {
        if (m_context)
            dav1d_close(&m_context);
    }

    void decode(const Packet& packet, Vector<DecodedFrame>& out) final
    {
        Dav1dData data { };
        auto* buffer = dav1d_data_create(&data, packet.data.size());
        if (!buffer)
            return;
        memcpy(buffer, packet.data.span().data(), packet.data.size());
        data.m.timestamp = packet.time;
        while (data.sz) {
            int result = dav1d_send_data(m_context, &data);
            if (result < 0 && result != DAV1D_ERR(EAGAIN)) {
                dav1d_data_unref(&data);
                return;
            }
            drain(out);
            if (result == DAV1D_ERR(EAGAIN))
                continue;
        }
        drain(out);
    }

    void flush() final
    {
        dav1d_flush(m_context);
    }

private:
    Dav1dDecoder() = default;

    void drain(Vector<DecodedFrame>& out)
    {
        Dav1dPicture picture { };
        while (!dav1d_get_picture(m_context, &picture)) {
            YUVPlanes planes { static_cast<const uint8_t*>(picture.data[0]), static_cast<const uint8_t*>(picture.data[1]), static_cast<const uint8_t*>(picture.data[2]),
                static_cast<int>(picture.stride[0]), static_cast<int>(picture.stride[1]), static_cast<int>(picture.stride[1]), picture.p.w, picture.p.h };
            planes.subsampling = picture.p.layout == DAV1D_PIXEL_LAYOUT_I444 ? YUVPlanes::Subsampling::I444 : picture.p.layout == DAV1D_PIXEL_LAYOUT_I422 ? YUVPlanes::Subsampling::I422 : YUVPlanes::Subsampling::I420;
            planes.highBitDepth = picture.p.bpc > 8;
            if (picture.seq_hdr) {
                auto matrix = picture.seq_hdr->mtrx;
                planes.matrix = matrix == DAV1D_MC_BT709 ? ColorMatrix::BT709 : (matrix == DAV1D_MC_BT2020_NCL || matrix == DAV1D_MC_BT2020_CL) ? ColorMatrix::BT2020 : ColorMatrix::BT601;
                planes.fullRange = picture.seq_hdr->color_range;
            }
            bool supported = picture.p.layout != DAV1D_PIXEL_LAYOUT_I400 && (!planes.highBitDepth || planes.subsampling == YUVPlanes::Subsampling::I420);
            if (supported) {
                if (auto converted = convertToImage(planes))
                    out.append({ fromNanoseconds(picture.m.timestamp), WTF::move(converted) });
            }
            dav1d_picture_unref(&picture);
        }
    }

    Dav1dContext* m_context { nullptr };
};

class OpusAudioDecoder final : public AudioDecoder {
public:
    // The OpusHead header (the track's codec private data).
    static std::unique_ptr<OpusAudioDecoder> create(std::span<const uint8_t> head, unsigned& channels)
    {
        if (head.size() < 19 || memcmp(head.data(), "OpusHead", 8))
            return nullptr;
        channels = std::clamp<unsigned>(head[9], 1, 2);
        auto decoder = std::unique_ptr<OpusAudioDecoder>(new OpusAudioDecoder);
        int error = 0;
        decoder->m_decoder = opus_decoder_create(48000, channels, &error);
        if (error != OPUS_OK || !decoder->m_decoder)
            return nullptr;
        decoder->m_channels = channels;
        decoder->m_preSkip = head[10] | (head[11] << 8);
        decoder->m_skip = decoder->m_preSkip;
        return decoder;
    }

    ~OpusAudioDecoder()
    {
        if (m_decoder)
            opus_decoder_destroy(m_decoder);
    }

    void decode(const Packet& packet, Vector<float>& out) final
    {
        std::array<float, 5760 * 2> pcm;
        int frames = opus_decode_float(m_decoder, packet.data.span().data(), packet.data.size(), pcm.data(), 5760, 0);
        if (frames <= 0)
            return;
        // The encoder's lookahead at the start of the stream.
        int skip = std::min<int>(m_skip, frames);
        m_skip -= skip;
        out.append(std::span<const float> { pcm }.subspan(skip * m_channels, (frames - skip) * m_channels));
    }

    void flush() final
    {
        opus_decoder_ctl(m_decoder, OPUS_RESET_STATE);
        // After a seek the decoder needs about 80 ms to converge; what comes
        // first is not worth discarding for an application's media.
        m_skip = 0;
    }

private:
    OpusAudioDecoder() = default;
    OpusDecoder* m_decoder { nullptr };
    unsigned m_channels { 0 };
    int m_preSkip { 0 };
    int m_skip { 0 };
};

class VorbisAudioDecoder final : public AudioDecoder {
public:
    // The three Xiph-laced headers (the track's codec private data).
    static std::unique_ptr<VorbisAudioDecoder> create(std::span<const uint8_t> headers, unsigned& channels, unsigned& sampleRate)
    {
        if (headers.size() < 3 || headers[0] != 2)
            return nullptr;
        size_t offset = 1;
        size_t sizes[3] { };
        for (int i = 0; i < 2; ++i) {
            while (offset < headers.size() && headers[offset] == 255) {
                sizes[i] += 255;
                ++offset;
            }
            if (offset >= headers.size())
                return nullptr;
            sizes[i] += headers[offset++];
        }
        if (offset + sizes[0] + sizes[1] > headers.size())
            return nullptr;
        sizes[2] = headers.size() - offset - sizes[0] - sizes[1];
        auto decoder = std::unique_ptr<VorbisAudioDecoder>(new VorbisAudioDecoder);
        vorbis_info_init(&decoder->m_info);
        vorbis_comment_init(&decoder->m_comment);
        for (int i = 0; i < 3; ++i) {
            ogg_packet header { };
            header.packet = const_cast<unsigned char*>(headers.subspan(offset).data());
            header.bytes = sizes[i];
            header.b_o_s = !i;
            header.packetno = i;
            if (vorbis_synthesis_headerin(&decoder->m_info, &decoder->m_comment, &header))
                return nullptr;
            offset += sizes[i];
        }
        if (vorbis_synthesis_init(&decoder->m_state, &decoder->m_info) || vorbis_block_init(&decoder->m_state, &decoder->m_block))
            return nullptr;
        decoder->m_ready = true;
        channels = std::clamp(decoder->m_info.channels, 1, 2);
        sampleRate = decoder->m_info.rate;
        decoder->m_channels = channels;
        return decoder;
    }

    ~VorbisAudioDecoder()
    {
        if (m_ready) {
            vorbis_block_clear(&m_block);
            vorbis_dsp_clear(&m_state);
        }
        vorbis_comment_clear(&m_comment);
        vorbis_info_clear(&m_info);
    }

    void decode(const Packet& packet, Vector<float>& out) final
    {
        ogg_packet data { };
        data.packet = const_cast<unsigned char*>(packet.data.span().data());
        data.bytes = packet.data.size();
        data.packetno = ++m_packetNumber;
        if (vorbis_synthesis(&m_block, &data) || vorbis_synthesis_blockin(&m_state, &m_block))
            return;
        float** pcm = nullptr;
        int frames;
        while ((frames = vorbis_synthesis_pcmout(&m_state, &pcm)) > 0) {
            for (int frame = 0; frame < frames; ++frame) {
                for (unsigned channel = 0; channel < m_channels; ++channel)
                    out.append(pcm[channel][frame]);
            }
            vorbis_synthesis_read(&m_state, frames);
        }
    }

    void flush() final
    {
        vorbis_synthesis_restart(&m_state);
    }

private:
    VorbisAudioDecoder() = default;
    vorbis_info m_info { };
    vorbis_comment m_comment { };
    vorbis_dsp_state m_state { };
    vorbis_block m_block { };
    bool m_ready { false };
    unsigned m_channels { 0 };
    int64_t m_packetNumber { 3 };
};

class WebMFile final : public MediaFile {
public:
    static std::unique_ptr<WebMFile> create(MediaBytes& bytes, String& error)
    {
        auto file = std::unique_ptr<WebMFile>(new WebMFile(bytes));
        if (!file->parse(error))
            return nullptr;
        return file;
    }

    ~WebMFile()
    {
        delete m_segment;
    }

    size_t readAudio(float* out, size_t frames, MediaTime& time) final
    {
        if (!m_audioDecoder)
            return 0;
        size_t channels = m_info.channels;
        while (m_samples.size() < frames * channels) {
            auto packet = nextPacket(m_audioQueue, m_audioTrack);
            if (!packet)
                break;
            // Time runs from the first packet after a seek, by the samples
            // decoded since.
            if (!m_audioTime.isValid())
                m_audioTime = fromNanoseconds(packet->time);
            m_audioDecoder->decode(*packet, m_samples);
            trimBeforeSeekTarget();
        }
        size_t count = std::min(frames, m_samples.size() / channels);
        if (!count)
            return 0;
        time = m_audioTime.isValid() ? m_audioTime : MediaTime::zeroTime();
        memcpy(out, m_samples.span().data(), count * channels * sizeof(float));
        m_samples.removeAt(0, count * channels);
        m_audioTime = time + MediaTime(count, m_info.sampleRate);
        return count;
    }

    std::optional<DecodedFrame> readVideo() final
    {
        if (!m_videoDecoder)
            return std::nullopt;
        while (m_frames.empty()) {
            auto packet = nextPacket(m_videoQueue, m_videoTrack);
            if (!packet)
                return std::nullopt;
            Vector<DecodedFrame> decoded;
            m_videoDecoder->decode(*packet, decoded);
            for (auto& frame : decoded) {
                // After a seek: only the frame showing at the target and on.
                if (m_videoSeekTarget.isValid()) {
                    if (frame.time < m_videoSeekTarget) {
                        m_pendingSeekFrame = WTF::move(frame);
                        continue;
                    }
                    if (m_pendingSeekFrame && frame.time > m_videoSeekTarget)
                        m_frames.push_back(*std::exchange(m_pendingSeekFrame, std::nullopt));
                    m_videoSeekTarget = MediaTime::invalidTime();
                    m_pendingSeekFrame = std::nullopt;
                }
                m_frames.push_back(WTF::move(frame));
            }
        }
        auto frame = WTF::move(m_frames.front());
        m_frames.pop_front();
        return frame;
    }

    bool seek(const MediaTime& time) final
    {
        long long target = std::max<long long>(0, time.toDouble() * nanosecondsPerSecond);
        auto* cluster = clusterForSeek(target);
        if (!cluster)
            return false;
        m_cluster = cluster;
        m_entry = nullptr;
        m_clusterDone = false;
        m_audioQueue.clear();
        m_videoQueue.clear();
        m_frames.clear();
        m_samples.clear();
        m_pendingSeekFrame = std::nullopt;
        if (m_videoDecoder)
            m_videoDecoder->flush();
        if (m_audioDecoder)
            m_audioDecoder->flush();
        m_audioTime = MediaTime::invalidTime();
        m_audioSeekTarget = time;
        m_videoSeekTarget = time;
        return true;
    }

private:
    explicit WebMFile(MediaBytes& bytes)
        : m_reader(bytes)
    {
    }

    bool parse(String& error)
    {
        long long position = 0;
        mkvparser::EBMLHeader header;
        if (header.Parse(&m_reader, position) < 0) {
            error = "not a WebM file (EBML header)"_s;
            return false;
        }
        if (mkvparser::Segment::CreateInstance(&m_reader, position, m_segment) || !m_segment || m_segment->ParseHeaders() < 0) {
            error = "damaged WebM file (segment)"_s;
            return false;
        }
        auto* tracks = m_segment->GetTracks();
        for (unsigned long i = 0; tracks && i < tracks->GetTracksCount(); ++i) {
            auto* track = tracks->GetTrackByIndex(i);
            if (!track || !track->GetCodecId())
                continue;
            StringView codec = StringView::fromLatin1(track->GetCodecId());
            size_t privateSize = 0;
            auto* privateData = track->GetCodecPrivate(privateSize);
            std::span<const uint8_t> codecPrivate { privateData, privateData ? privateSize : 0 };
            if (track->GetType() == mkvparser::Track::kVideo && !m_videoDecoder) {
                m_videoDecoder = createVideoDecoder(codec);
                if (m_videoDecoder) {
                    auto* video = static_cast<const mkvparser::VideoTrack*>(track);
                    m_videoTrack = track->GetNumber();
                    m_info.hasVideo = true;
                    long long displayWidth = video->GetDisplayWidth();
                    long long displayHeight = video->GetDisplayHeight();
                    m_info.videoSize = { static_cast<int>(displayWidth > 0 ? displayWidth : video->GetWidth()), static_cast<int>(displayHeight > 0 ? displayHeight : video->GetHeight()) };
                }
            } else if (track->GetType() == mkvparser::Track::kAudio && !m_audioDecoder) {
                auto* audio = static_cast<const mkvparser::AudioTrack*>(track);
                unsigned channels = 0;
                unsigned sampleRate = static_cast<unsigned>(audio->GetSamplingRate());
                m_audioDecoder = createAudioDecoder(codec, codecPrivate, channels, sampleRate);
                if (m_audioDecoder) {
                    m_audioTrack = track->GetNumber();
                    m_info.hasAudio = true;
                    m_info.channels = channels;
                    m_info.sampleRate = sampleRate;
                }
            }
        }
        if (!m_info.hasAudio && !m_info.hasVideo) {
            error = "the WebM file has no VP8, VP9, AV1, Opus or Vorbis track"_s;
            return false;
        }
        if (auto* information = m_segment->GetInfo(); information && information->GetDuration() >= 0)
            m_info.duration = fromNanoseconds(information->GetDuration());
        return true;
    }

    // The cluster after `cluster` (the first when null), loading the next one
    // from the file when none is loaded yet; null at the end.
    const mkvparser::Cluster* nextCluster(const mkvparser::Cluster* cluster)
    {
        for (;;) {
            auto* next = cluster ? m_segment->GetNext(cluster) : m_segment->GetFirst();
            if (next && !next->EOS())
                return next;
            long long position;
            long length;
            long status = m_segment->LoadCluster(position, length);
            if (status) // the end of the segment, or an error
                return nullptr;
        }
    }

    // The next block's frames go to their track's queue; false at the end.
    bool demuxBlock()
    {
        if (m_clusterDone)
            return false;
        for (;;) {
            if (!m_cluster) {
                m_cluster = nextCluster(nullptr);
                m_entry = nullptr;
                if (!m_cluster) {
                    m_clusterDone = true;
                    return false;
                }
            }
            const mkvparser::BlockEntry* entry = nullptr;
            long status = m_entry ? m_cluster->GetNext(m_entry, entry) : m_cluster->GetFirst(entry);
            if (status < 0 || !entry || entry->EOS()) {
                m_cluster = nextCluster(m_cluster);
                m_entry = nullptr;
                if (!m_cluster) {
                    m_clusterDone = true;
                    return false;
                }
                continue;
            }
            m_entry = entry;
            auto* block = entry->GetBlock();
            if (!block)
                continue;
            long long track = block->GetTrackNumber();
            auto* queue = track == m_videoTrack ? &m_videoQueue : track == m_audioTrack ? &m_audioQueue : nullptr;
            if (!queue)
                continue;
            long long time = block->GetTime(m_cluster);
            for (int i = 0; i < block->GetFrameCount(); ++i) {
                auto& frame = block->GetFrame(i);
                Packet packet;
                packet.data.grow(frame.len);
                if (frame.Read(&m_reader, packet.data.mutableSpan().data()))
                    continue;
                packet.time = time;
                packet.key = block->IsKey();
                queue->push_back(WTF::move(packet));
            }
            return true;
        }
    }

    std::optional<Packet> nextPacket(std::deque<Packet>& queue, long long)
    {
        while (queue.empty()) {
            if (!demuxBlock())
                return std::nullopt;
        }
        auto packet = WTF::move(queue.front());
        queue.pop_front();
        return packet;
    }

    // After a seek, the audio decoded before its target is dropped (the buffer
    // then holds only what was decoded since the seek, from m_audioTime).
    void trimBeforeSeekTarget()
    {
        if (!m_audioSeekTarget.isValid() || !m_audioTime.isValid())
            return;
        size_t channels = m_info.channels;
        size_t frames = m_samples.size() / channels;
        auto end = m_audioTime + MediaTime(frames, m_info.sampleRate);
        if (end <= m_audioSeekTarget) {
            m_samples.clear();
            m_audioTime = end;
            return;
        }
        if (m_audioTime < m_audioSeekTarget) {
            size_t drop = std::min<size_t>((m_audioSeekTarget - m_audioTime).toDouble() * m_info.sampleRate + 0.5, frames);
            m_samples.removeAt(0, drop * channels);
            m_audioTime = m_audioTime + MediaTime(drop, m_info.sampleRate);
        }
        m_audioSeekTarget = MediaTime::invalidTime();
    }

    // The cluster to decode from for a seek to `target` (nanoseconds): the
    // cues' when the file has them, else the last cluster starting before the
    // target whose first video block is a keyframe.
    const mkvparser::Cluster* clusterForSeek(long long target)
    {
        auto* cues = m_segment->GetCues();
        long long cueTrack = m_videoTrack >= 0 ? m_videoTrack : m_audioTrack;
        if (cues) {
            while (!cues->DoneParsing())
                cues->LoadCuePoint();
            const mkvparser::CuePoint* cuePoint = nullptr;
            const mkvparser::CuePoint::TrackPosition* position = nullptr;
            auto* track = m_segment->GetTracks()->GetTrackByNumber(cueTrack);
            if (track && cues->Find(target, track, cuePoint, position) && position) {
                if (auto* cluster = m_segment->FindOrPreloadCluster(position->m_pos); cluster && !cluster->EOS())
                    return cluster;
            }
        }
        const mkvparser::Cluster* best = nullptr;
        for (auto* cluster = nextCluster(nullptr); cluster; cluster = nextCluster(cluster)) {
            if (cluster->GetTime() > target)
                break;
            if (m_videoTrack < 0 || startsWithKeyframe(*cluster))
                best = cluster;
        }
        return best ? best : nextCluster(nullptr);
    }

    bool startsWithKeyframe(const mkvparser::Cluster& cluster)
    {
        const mkvparser::BlockEntry* entry = nullptr;
        for (long status = cluster.GetFirst(entry); status >= 0 && entry && !entry->EOS(); status = cluster.GetNext(entry, entry)) {
            auto* block = entry->GetBlock();
            if (block && block->GetTrackNumber() == m_videoTrack)
                return block->IsKey();
        }
        return false;
    }

    Reader m_reader;
    mkvparser::Segment* m_segment { nullptr };
    long long m_videoTrack { -1 };
    long long m_audioTrack { -1 };
    std::unique_ptr<VideoDecoder> m_videoDecoder;
    std::unique_ptr<AudioDecoder> m_audioDecoder;

    const mkvparser::Cluster* m_cluster { nullptr };
    const mkvparser::BlockEntry* m_entry { nullptr };
    bool m_clusterDone { false };
    std::deque<Packet> m_audioQueue;
    std::deque<Packet> m_videoQueue;
    std::deque<DecodedFrame> m_frames;
    Vector<float> m_samples;
    MediaTime m_audioTime { MediaTime::invalidTime() };
    MediaTime m_audioSeekTarget { MediaTime::invalidTime() };
    MediaTime m_videoSeekTarget { MediaTime::invalidTime() };
    std::optional<DecodedFrame> m_pendingSeekFrame;
};

} // namespace

std::unique_ptr<MediaFile> openWebM(MediaBytes& bytes, String& error)
{
    return WebMFile::create(bytes, error);
}

std::unique_ptr<VideoDecoder> createVideoDecoder(StringView codecID)
{
    if (codecID == "V_VP8"_s || codecID == "V_VP9"_s)
        return VPXDecoder::create(codecID == "V_VP9"_s);
    if (codecID == "V_AV1"_s)
        return Dav1dDecoder::create();
    return nullptr;
}

std::unique_ptr<AudioDecoder> createAudioDecoder(StringView codecID, std::span<const uint8_t> codecPrivate, unsigned& channels, unsigned& sampleRate)
{
    if (codecID == "A_OPUS"_s) {
        sampleRate = 48000; // Opus decodes at 48 kHz whatever the track says
        return OpusAudioDecoder::create(codecPrivate, channels);
    }
    if (codecID == "A_VORBIS"_s)
        return VorbisAudioDecoder::create(codecPrivate, channels, sampleRate);
    return nullptr;
}

} // namespace ZeroG::Media

#endif
