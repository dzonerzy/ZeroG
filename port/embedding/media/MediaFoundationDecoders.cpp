// H.264 and AAC frames (Media Source Extensions' MP4 segments) through Windows'
// own decoders: Media Foundation transforms fed one coded frame at a time, as
// the WebM decoders are. H.264 comes in MP4's form (length-prefixed NAL units
// and an avcC record) and goes to the decoder as an Annex B stream, the
// parameter sets before each keyframe; frames come out in presentation order,
// some held back for B-frames until a later frame or the end of the stream.
// Also the NV12 handling MP4 files share (MediaFoundation.h).

#include "config.h"
#include "media/MediaFoundation.h"

#if ENABLE(VIDEO) && defined(ZEROG_MEDIA) && OS(WINDOWS)

#include "media/Decoders.h"
#include <atomic>
#include <d3d11.h>
#include <d3d10.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <wrl/client.h>

namespace ZeroG::Media {

using Microsoft::WRL::ComPtr;

bool startMediaFoundation()
{
    // Once for the process; never shut down (the codecs stay loaded).
    static bool started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    return started;
}

static std::atomic<uint64_t> s_hardwareFrames;

IUnknown* mediaFoundationDeviceManager()
{
    static IMFDXGIDeviceManager* manager = [] () -> IMFDXGIDeviceManager* {
        if (!startMediaFoundation())
            return nullptr;
        ComPtr<ID3D11Device> device;
        static constexpr D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3 };
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, std::size(levels), D3D11_SDK_VERSION, &device, nullptr, nullptr)))
            return nullptr;
        // Decoders on several threads use it.
        ComPtr<ID3D10Multithread> multithread;
        if (SUCCEEDED(device.As(&multithread)))
            multithread->SetMultithreadProtected(TRUE);
        UINT token = 0;
        IMFDXGIDeviceManager* created = nullptr;
        if (FAILED(MFCreateDXGIDeviceManager(&token, &created)))
            return nullptr;
        if (FAILED(created->ResetDevice(device.Get(), token))) {
            created->Release();
            return nullptr;
        }
        return created; // kept for the process
    }();
    return manager;
}

uint64_t hardwareDecodedFrames()
{
    return s_hardwareFrames;
}

void noteDecodedSample(IMFSample* sample)
{
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFDXGIBuffer> surface;
    if (sample && SUCCEEDED(sample->GetBufferByIndex(0, &buffer)) && SUCCEEDED(buffer.As(&surface)))
        ++s_hardwareFrames;
}

bool readNV12Layout(IMFMediaType* type, NV12Layout& layout)
{
    UINT32 width = 0;
    UINT32 height = 0;
    if (FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height)) || !width || !height)
        return false;
    layout.codedHeight = height;
    // What shows: the display aperture (the coded size is in whole
    // macroblocks, 64x36 is coded 64x48).
    layout.x = layout.y = 0;
    layout.width = width;
    layout.height = height;
    MFVideoArea area { };
    if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr))
        || SUCCEEDED(type->GetBlob(MF_MT_GEOMETRIC_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr))) {
        if (area.Area.cx > 0 && area.Area.cy > 0) {
            layout.x = area.OffsetX.value;
            layout.y = area.OffsetY.value;
            layout.width = area.Area.cx;
            layout.height = area.Area.cy;
        }
    }
    layout.width &= ~1;
    layout.height &= ~1;
    layout.stride = static_cast<int>(MFGetAttributeUINT32(type, MF_MT_DEFAULT_STRIDE, width));
    switch (MFGetAttributeUINT32(type, MF_MT_YUV_MATRIX, MFVideoTransferMatrix_Unknown)) {
    case MFVideoTransferMatrix_BT709:
        layout.matrix = ColorMatrix::BT709;
        break;
    case MFVideoTransferMatrix_BT2020_10:
    case MFVideoTransferMatrix_BT2020_12:
        layout.matrix = ColorMatrix::BT2020;
        break;
    case MFVideoTransferMatrix_BT601:
    case MFVideoTransferMatrix_SMPTE240M:
        layout.matrix = ColorMatrix::BT601;
        break;
    default:
        // Unsaid: HD sizes are BT.709, as decoders and browsers assume.
        layout.matrix = height >= 720 ? ColorMatrix::BT709 : ColorMatrix::BT601;
        break;
    }
    layout.fullRange = MFGetAttributeUINT32(type, MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235) == MFNominalRange_0_255;
    return true;
}

sk_sp<SkImage> convertNV12Sample(IMFSample* sample, const NV12Layout& layout)
{
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
        return nullptr;
    BYTE* data = nullptr;
    LONG pitch = layout.stride;
    ComPtr<IMF2DBuffer> buffer2D;
    bool locked2D = SUCCEEDED(buffer.As(&buffer2D)) && SUCCEEDED(buffer2D->Lock2D(&data, &pitch));
    if (!locked2D && FAILED(buffer->Lock(&data, nullptr, nullptr)))
        return nullptr;
    // NV12: the Y plane, then interleaved U and V at half resolution.
    const uint8_t* y = data + layout.y * pitch + layout.x;
    const uint8_t* uv = data + layout.codedHeight * pitch + (layout.y / 2) * pitch + (layout.x & ~1);
    auto image = convertNV12ToImage(y, pitch, uv, pitch, layout.width, layout.height, layout.matrix, layout.fullRange);
    if (locked2D)
        buffer2D->Unlock2D();
    else
        buffer->Unlock();
    return image;
}

namespace {

constexpr int64_t hundredNanosecondsPerSecond = 10'000'000;

// The first synchronous decoder for an input type.
ComPtr<IMFTransform> createDecoder(const GUID& category, const GUID& majorType, const GUID& subtype)
{
    MFT_REGISTER_TYPE_INFO input { majorType, subtype };
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    if (FAILED(MFTEnumEx(category, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, &input, nullptr, &activates, &count)))
        return nullptr;
    ComPtr<IMFTransform> transform;
    for (UINT32 i = 0; i < count; ++i) {
        if (!transform)
            activates[i]->ActivateObject(IID_PPV_ARGS(&transform));
        activates[i]->Release();
    }
    CoTaskMemFree(activates);
    return transform;
}

ComPtr<IMFSample> createSample(std::span<const uint8_t> data, long long nanoseconds)
{
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateMemoryBuffer(data.size(), &buffer)))
        return nullptr;
    BYTE* bytes = nullptr;
    if (FAILED(buffer->Lock(&bytes, nullptr, nullptr)))
        return nullptr;
    memcpy(bytes, data.data(), data.size());
    buffer->Unlock();
    buffer->SetCurrentLength(data.size());
    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample)))
        return nullptr;
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(nanoseconds / 100);
    return sample;
}

// A decoder transform, with COM on the thread that uses it (the decode
// thread) and the output loop both kinds share.
class Transform {
public:
    ~Transform()
    {
        m_transform = nullptr;
        if (m_comInitialized)
            CoUninitialize();
    }

    bool start(const GUID& category, const GUID& majorType, const GUID& subtype)
    {
        m_comInitialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        if (!startMediaFoundation())
            return false;
        m_transform = createDecoder(category, majorType, subtype);
        return !!m_transform;
    }

    IMFTransform* get() const { return m_transform.Get(); }

    // The output type of `subtype` the decoder offers; false if none.
    bool setOutputType(const GUID& subtype)
    {
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> type;
            if (FAILED(m_transform->GetOutputAvailableType(0, i, &type)))
                return false;
            GUID offered { };
            if (SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &offered)) && offered == subtype && SUCCEEDED(m_transform->SetOutputType(0, type.Get(), 0)))
                return true;
        }
    }

    void beginStreaming()
    {
        m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }

    // Feeds a sample, taking out what the decoder holds when it is full.
    template<typename Output>
    void input(IMFSample* sample, const Output& output)
    {
        HRESULT result = m_transform->ProcessInput(0, sample, 0);
        if (result == MF_E_NOTACCEPTING) {
            drainOutput(output);
            result = m_transform->ProcessInput(0, sample, 0);
        }
        if (SUCCEEDED(result))
            drainOutput(output);
    }

    // Every output the decoder has ready; `output(sample)` for each, and
    // `output(nullptr)` when its format changed (the caller sets it again).
    template<typename Output>
    void drainOutput(const Output& output)
    {
        for (;;) {
            MFT_OUTPUT_STREAM_INFO info { };
            if (FAILED(m_transform->GetOutputStreamInfo(0, &info)))
                return;
            ComPtr<IMFSample> sample;
            if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
                ComPtr<IMFMediaBuffer> buffer;
                if (FAILED(MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, 4096), &buffer)) || FAILED(MFCreateSample(&sample)))
                    return;
                sample->AddBuffer(buffer.Get());
            }
            MFT_OUTPUT_DATA_BUFFER data { };
            data.pSample = sample.Get();
            DWORD status = 0;
            HRESULT result = m_transform->ProcessOutput(0, 1, &data, &status);
            if (data.pEvents)
                data.pEvents->Release();
            ComPtr<IMFSample> produced;
            if (data.pSample && data.pSample != sample.Get())
                produced.Attach(data.pSample); // the decoder's own sample
            else
                produced = sample;
            if (result == MF_E_TRANSFORM_STREAM_CHANGE) {
                output(nullptr);
                continue;
            }
            if (FAILED(result))
                return; // MF_E_TRANSFORM_NEED_MORE_INPUT: nothing more for now
            output(produced.Get());
        }
    }

    void drain()
    {
        m_transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    }

    void flush()
    {
        m_transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }

private:
    bool m_comInitialized { false };
    ComPtr<IMFTransform> m_transform;
};

class MediaFoundationH264Decoder final : public VideoDecoder {
public:
    static std::unique_ptr<MediaFoundationH264Decoder> create(std::span<const uint8_t> configuration)
    {
        auto decoder = std::unique_ptr<MediaFoundationH264Decoder>(new MediaFoundationH264Decoder);
        if (!decoder->parseConfiguration(configuration) || !decoder->m_transform.start(MFT_CATEGORY_VIDEO_DECODER, MFMediaType_Video, MFVideoFormat_H264))
            return nullptr;
        // Hardware decoding (DXVA) where the decoder and the GPU can: frames
        // come out as GPU surfaces, read back for the pipeline. The decoder
        // falls back to the CPU by itself for what the GPU does not take.
        ComPtr<IMFAttributes> attributes;
        if (auto* manager = mediaFoundationDeviceManager(); manager && SUCCEEDED(decoder->m_transform.get()->GetAttributes(&attributes))
            && MFGetAttributeUINT32(attributes.Get(), MF_SA_D3D11_AWARE, FALSE))
            decoder->m_transform.get()->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager));
        ComPtr<IMFMediaType> input;
        MFCreateMediaType(&input);
        input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_MixedInterlaceOrProgressive);
        if (FAILED(decoder->m_transform.get()->SetInputType(0, input.Get(), 0)) || !decoder->configureOutput())
            return nullptr;
        decoder->m_transform.beginStreaming();
        return decoder;
    }

    void decode(const Packet& packet, Vector<DecodedFrame>& out) final
    {
        // MP4's length-prefixed NAL units to an Annex B stream.
        Vector<uint8_t> stream;
        if (packet.key || !m_sentParameterSets) {
            stream.append(m_parameterSets.span());
            m_sentParameterSets = true;
        }
        auto data = packet.data.span();
        while (data.size() > m_lengthSize) {
            size_t length = 0;
            for (unsigned i = 0; i < m_lengthSize; ++i)
                length = (length << 8) | data[i];
            data = data.subspan(m_lengthSize);
            if (length > data.size())
                break;
            stream.append(std::span<const uint8_t> { startCode });
            stream.append(data.first(length));
            data = data.subspan(length);
        }
        if (stream.isEmpty())
            return;
        if (auto sample = createSample(stream.span(), packet.time))
            m_transform.input(sample.Get(), [&](IMFSample* frame) { output(frame, out); });
    }

    void drain(Vector<DecodedFrame>& out) final
    {
        m_transform.drain();
        m_transform.drainOutput([&](IMFSample* frame) { output(frame, out); });
    }

    void flush() final
    {
        m_transform.flush();
        m_sentParameterSets = false;
    }

private:
    MediaFoundationH264Decoder() = default;

    static constexpr uint8_t startCode[] = { 0, 0, 0, 1 };

    // avcC: the NAL length size and the SPS and PPS, kept in Annex B form.
    bool parseConfiguration(std::span<const uint8_t> configuration)
    {
        if (configuration.size() < 7 || configuration[0] != 1)
            return false;
        m_lengthSize = (configuration[4] & 3) + 1;
        size_t offset = 5;
        for (int list = 0; list < 2; ++list) {
            if (offset >= configuration.size())
                return false;
            unsigned count = list ? configuration[offset] : configuration[offset] & 0x1F;
            ++offset;
            for (unsigned i = 0; i < count; ++i) {
                if (offset + 2 > configuration.size())
                    return false;
                size_t length = (configuration[offset] << 8) | configuration[offset + 1];
                offset += 2;
                if (offset + length > configuration.size())
                    return false;
                m_parameterSets.append(std::span<const uint8_t> { startCode });
                m_parameterSets.append(configuration.subspan(offset, length));
                offset += length;
            }
        }
        return !m_parameterSets.isEmpty();
    }

    bool configureOutput()
    {
        if (!m_transform.setOutputType(MFVideoFormat_NV12))
            return false;
        ComPtr<IMFMediaType> type;
        if (FAILED(m_transform.get()->GetOutputCurrentType(0, &type)))
            return false;
        // Before the first frame the decoder may not know the size yet.
        readNV12Layout(type.Get(), m_layout);
        return true;
    }

    void output(IMFSample* frame, Vector<DecodedFrame>& out)
    {
        if (!frame) {
            configureOutput();
            return;
        }
        if (!m_layout.width)
            return;
        noteDecodedSample(frame);
        LONGLONG time = 0;
        frame->GetSampleTime(&time);
        if (auto image = convertNV12Sample(frame, m_layout))
            out.append({ MediaTime(time, hundredNanosecondsPerSecond), WTF::move(image) });
    }

    Transform m_transform;
    unsigned m_lengthSize { 4 };
    Vector<uint8_t> m_parameterSets;
    bool m_sentParameterSets { false };
    NV12Layout m_layout;
};

class MediaFoundationAACDecoder final : public AudioDecoder {
public:
    static std::unique_ptr<MediaFoundationAACDecoder> create(std::span<const uint8_t> config, unsigned& channels, unsigned& sampleRate)
    {
        if (config.empty() || !channels || !sampleRate)
            return nullptr;
        auto decoder = std::unique_ptr<MediaFoundationAACDecoder>(new MediaFoundationAACDecoder);
        if (!decoder->m_transform.start(MFT_CATEGORY_AUDIO_DECODER, MFMediaType_Audio, MFAudioFormat_AAC))
            return nullptr;
        ComPtr<IMFMediaType> input;
        MFCreateMediaType(&input);
        input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        input->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
        input->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        input->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0); // raw frames, as MP4 holds them
        // HEAACWAVEINFO's fields after WAVEFORMATEX (payload type 0, profile
        // level unknown), then the AudioSpecificConfig.
        Vector<uint8_t> userData { 0, 0, 0xFE, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        userData.append(config);
        input->SetBlob(MF_MT_USER_DATA, userData.span().data(), userData.size());
        if (FAILED(decoder->m_transform.get()->SetInputType(0, input.Get(), 0)))
            return nullptr;
        // Float if the decoder gives it, else 16-bit samples converted here.
        decoder->m_float = decoder->m_transform.setOutputType(MFAudioFormat_Float);
        if (!decoder->m_float && !decoder->m_transform.setOutputType(MFAudioFormat_PCM))
            return nullptr;
        ComPtr<IMFMediaType> output;
        if (FAILED(decoder->m_transform.get()->GetOutputCurrentType(0, &output)))
            return nullptr;
        channels = MFGetAttributeUINT32(output.Get(), MF_MT_AUDIO_NUM_CHANNELS, channels);
        sampleRate = MFGetAttributeUINT32(output.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
        decoder->m_transform.beginStreaming();
        return decoder;
    }

    void decode(const Packet& packet, Vector<float>& out) final
    {
        if (auto sample = createSample(packet.data.span(), packet.time))
            m_transform.input(sample.Get(), [&](IMFSample* frame) { output(frame, out); });
    }

    void flush() final
    {
        m_transform.flush();
    }

private:
    MediaFoundationAACDecoder() = default;

    void output(IMFSample* frame, Vector<float>& out)
    {
        if (!frame)
            return;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(frame->ConvertToContiguousBuffer(&buffer)))
            return;
        BYTE* data = nullptr;
        DWORD length = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &length)))
            return;
        if (m_float)
            out.append(std::span<const float> { reinterpret_cast<const float*>(data), length / sizeof(float) });
        else {
            std::span<const int16_t> samples { reinterpret_cast<const int16_t*>(data), length / sizeof(int16_t) };
            for (auto sample : samples)
                out.append(sample / 32768.0f);
        }
        buffer->Unlock();
    }

    Transform m_transform;
    bool m_float { false };
};

} // namespace

std::unique_ptr<VideoDecoder> createMediaFoundationH264Decoder(std::span<const uint8_t> configuration)
{
    return MediaFoundationH264Decoder::create(configuration);
}

std::unique_ptr<AudioDecoder> createMediaFoundationAACDecoder(std::span<const uint8_t> config, unsigned& channels, unsigned& sampleRate)
{
    return MediaFoundationAACDecoder::create(config, channels, sampleRate);
}

} // namespace ZeroG::Media

#endif // ENABLE(VIDEO) && defined(ZEROG_MEDIA) && OS(WINDOWS)
