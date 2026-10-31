// MP4 through Windows' Media Foundation (ZeroG.md, "Media"): H.264 and AAC, the
// formats whose patents keep their decoders out of the binary, decoded by the
// OS's own codecs. The player's bytes reach Media Foundation as an
// IMFByteStream (reads wait for the bytes, as for the other formats); a source
// reader demuxes and decodes them, video to NV12 (converted with libyuv like the
// other formats), audio to float samples.

#include "config.h"
#include "media/MediaFile.h"

#if ENABLE(VIDEO) && defined(ZEROG_MEDIA) && OS(WINDOWS)

#include <atomic>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <wtf/Lock.h>
#include <wtf/Vector.h>

namespace ZeroG::Media {

using Microsoft::WRL::ComPtr;

namespace {

constexpr int64_t hundredNanosecondsPerSecond = 10'000'000;

MediaTime fromHundredNanoseconds(LONGLONG time)
{
    return MediaTime(time, hundredNanosecondsPerSecond);
}

LONGLONG toHundredNanoseconds(const MediaTime& time)
{
    return static_cast<LONGLONG>(time.toDouble() * hundredNanosecondsPerSecond);
}

// The count of bytes one asynchronous read gave, carried by its IMFAsyncResult.
class ReadResult final : public IUnknown {
public:
    explicit ReadResult(ULONG count)
        : m_count(count)
    {
    }
    ULONG count() const { return m_count; }

    STDMETHODIMP QueryInterface(REFIID iid, void** out) final
    {
        if (!out)
            return E_POINTER;
        if (iid != __uuidof(IUnknown)) {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        *out = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() final { return ++m_refCount; }
    STDMETHODIMP_(ULONG) Release() final
    {
        ULONG count = --m_refCount;
        if (!count)
            delete this;
        return count;
    }

private:
    std::atomic<ULONG> m_refCount { 1 };
    ULONG m_count;
};

// The player's bytes for Media Foundation: readable and seekable; reads wait
// for bytes still arriving. Asynchronous reads complete at once.
class ByteStream final : public IMFByteStream {
public:
    explicit ByteStream(MediaBytes& bytes)
        : m_reader(bytes)
    {
    }

    STDMETHODIMP QueryInterface(REFIID iid, void** out) final
    {
        if (!out)
            return E_POINTER;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IMFByteStream)) {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        *out = static_cast<IMFByteStream*>(this);
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() final { return ++m_refCount; }
    STDMETHODIMP_(ULONG) Release() final
    {
        ULONG count = --m_refCount;
        if (!count)
            delete this;
        return count;
    }

    STDMETHODIMP GetCapabilities(DWORD* capabilities) final
    {
        *capabilities = MFBYTESTREAM_IS_READABLE | MFBYTESTREAM_IS_SEEKABLE;
        return S_OK;
    }
    STDMETHODIMP GetLength(QWORD* length) final
    {
        auto known = m_reader.bytes().length();
        *length = known ? *known : static_cast<QWORD>(-1);
        return S_OK;
    }
    STDMETHODIMP SetLength(QWORD) final { return E_NOTIMPL; }
    STDMETHODIMP GetCurrentPosition(QWORD* position) final
    {
        Locker locker { m_lock };
        *position = m_reader.position();
        return S_OK;
    }
    STDMETHODIMP SetCurrentPosition(QWORD position) final
    {
        Locker locker { m_lock };
        return m_reader.seek(position, MediaReader::Origin::Start) ? S_OK : E_INVALIDARG;
    }
    STDMETHODIMP IsEndOfStream(BOOL* end) final
    {
        Locker locker { m_lock };
        auto length = m_reader.bytes().length();
        *end = length && m_reader.position() >= *length;
        return S_OK;
    }
    STDMETHODIMP Read(BYTE* buffer, ULONG size, ULONG* read) final
    {
        Locker locker { m_lock };
        *read = m_reader.read({ buffer, size });
        return S_OK;
    }
    STDMETHODIMP BeginRead(BYTE* buffer, ULONG size, IMFAsyncCallback* callback, IUnknown* state) final
    {
        ULONG read = 0;
        Read(buffer, size, &read);
        ComPtr<IUnknown> object;
        object.Attach(new ReadResult(read));
        ComPtr<IMFAsyncResult> result;
        HRESULT hr = MFCreateAsyncResult(object.Get(), callback, state, &result);
        if (FAILED(hr))
            return hr;
        return MFInvokeCallback(result.Get());
    }
    STDMETHODIMP EndRead(IMFAsyncResult* result, ULONG* read) final
    {
        ComPtr<IUnknown> object;
        HRESULT hr = result->GetObject(&object);
        if (FAILED(hr))
            return hr;
        *read = static_cast<ReadResult*>(object.Get())->count();
        return result->GetStatus();
    }
    STDMETHODIMP Write(const BYTE*, ULONG, ULONG*) final { return E_NOTIMPL; }
    STDMETHODIMP BeginWrite(const BYTE*, ULONG, IMFAsyncCallback*, IUnknown*) final { return E_NOTIMPL; }
    STDMETHODIMP EndWrite(IMFAsyncResult*, ULONG*) final { return E_NOTIMPL; }
    STDMETHODIMP Seek(MFBYTESTREAM_SEEK_ORIGIN origin, LONGLONG offset, DWORD, QWORD* position) final
    {
        Locker locker { m_lock };
        if (!m_reader.seek(offset, origin == msoBegin ? MediaReader::Origin::Start : MediaReader::Origin::Current))
            return E_INVALIDARG;
        if (position)
            *position = m_reader.position();
        return S_OK;
    }
    STDMETHODIMP Flush() final { return S_OK; }
    STDMETHODIMP Close() final { return S_OK; }

private:
    std::atomic<ULONG> m_refCount { 1 };
    Lock m_lock;
    MediaReader m_reader WTF_GUARDED_BY_LOCK(m_lock);
};

bool startMediaFoundation()
{
    // Once for the process; never shut down (the codecs stay loaded).
    static bool started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    return started;
}

class MediaFoundationFile final : public MediaFile {
public:
    ~MediaFoundationFile()
    {
        m_reader = nullptr;
        if (m_source)
            m_source->Shutdown();
        m_source = nullptr;
        if (m_comInitialized)
            CoUninitialize();
    }

    bool open(MediaBytes& bytes, std::span<const uint8_t> head, String& error)
    {
        m_comInitialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        if (!startMediaFoundation()) {
            error = "Media Foundation is not available"_s;
            return false;
        }
        ComPtr<IMFByteStream> stream;
        stream.Attach(new ByteStream(bytes));
        ComPtr<IMFSourceResolver> resolver;
        if (FAILED(MFCreateSourceResolver(&resolver))) {
            error = "Media Foundation is not available"_s;
            return false;
        }
        // The name picks the MPEG-4 source; the bytes decide in the end.
        bool audioOnly = head.size() >= 12 && (!memcmp(head.data() + 8, "M4A ", 4) || !memcmp(head.data() + 8, "M4B ", 4));
        MF_OBJECT_TYPE type = MF_OBJECT_INVALID;
        ComPtr<IUnknown> object;
        HRESULT hr = resolver->CreateObjectFromByteStream(stream.Get(), audioOnly ? L"media.m4a" : L"media.mp4",
            MF_RESOLUTION_MEDIASOURCE | MF_RESOLUTION_CONTENT_DOES_NOT_HAVE_TO_MATCH_EXTENSION_OR_MIME_TYPE, nullptr, &type, &object);
        if (FAILED(hr) || FAILED(object.As(&m_source))) {
            error = "the MP4 file could not be read"_s;
            return false;
        }
        ComPtr<IMFAttributes> attributes;
        MFCreateAttributes(&attributes, 1);
        // Color conversion inside the reader when the decoder's output is not NV12.
        attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        if (FAILED(MFCreateSourceReaderFromMediaSource(m_source.Get(), attributes.Get(), &m_reader))) {
            error = "the MP4 file could not be read"_s;
            return false;
        }

        ComPtr<IMFPresentationDescriptor> descriptor;
        if (SUCCEEDED(m_source->CreatePresentationDescriptor(&descriptor))) {
            UINT64 duration = 0;
            if (SUCCEEDED(descriptor->GetUINT64(MF_PD_DURATION, &duration)) && duration)
                m_info.duration = fromHundredNanoseconds(duration);
        }

        m_info.hasAudio = configureAudio();
        m_info.hasVideo = configureVideo();
        if (!m_info.hasAudio && !m_info.hasVideo) {
            error = "the MP4 file has no audio or video this system decodes"_s;
            return false;
        }
        return true;
    }

    size_t readAudio(float* out, size_t frames, MediaTime& time) final
    {
        if (!m_info.hasAudio)
            return 0;
        size_t channels = m_info.channels;
        while (m_audioOffset >= m_audio.size()) {
            if (!readAudioSample())
                return 0;
        }
        size_t available = (m_audio.size() - m_audioOffset) / channels;
        size_t count = std::min(frames, available);
        time = m_audioTime;
        memcpy(out, m_audio.span().subspan(m_audioOffset).data(), count * channels * sizeof(float));
        m_audioOffset += count * channels;
        m_audioTime = m_audioTime + MediaTime(count, m_info.sampleRate);
        return count;
    }

    std::optional<DecodedFrame> readVideo() final
    {
        if (!m_info.hasVideo)
            return std::nullopt;
        for (;;) {
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            if (FAILED(m_reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &timestamp, &sample)))
                return std::nullopt;
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
                readVideoFormat();
            if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR))
                return std::nullopt;
            if (!sample)
                continue; // a gap
            auto time = fromHundredNanoseconds(timestamp);
            if (m_videoSeekTarget.isValid()) {
                // After a seek the decoder starts at the keyframe before the
                // target: the frames that end before it are not shown.
                LONGLONG duration = 0;
                sample->GetSampleDuration(&duration);
                if (time + fromHundredNanoseconds(duration) <= m_videoSeekTarget)
                    continue;
                m_videoSeekTarget = MediaTime::invalidTime();
            }
            if (auto image = convert(sample.Get()))
                return DecodedFrame { time, WTF::move(image) };
        }
    }

    bool seek(const MediaTime& time) final
    {
        PROPVARIANT position;
        PropVariantInit(&position);
        position.vt = VT_I8;
        position.hVal.QuadPart = toHundredNanoseconds(time);
        HRESULT hr = m_reader->SetCurrentPosition(GUID_NULL, position);
        PropVariantClear(&position);
        if (FAILED(hr))
            return false;
        m_audio.clear();
        m_audioOffset = 0;
        m_audioTime = time;
        m_audioSeekTarget = time;
        m_videoSeekTarget = time;
        return true;
    }

private:
    bool configureAudio()
    {
        ComPtr<IMFMediaType> type;
        MFCreateMediaType(&type);
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        if (FAILED(m_reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, type.Get())))
            return false;
        m_reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
        ComPtr<IMFMediaType> current;
        if (FAILED(m_reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &current)))
            return false;
        m_info.channels = MFGetAttributeUINT32(current.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0);
        m_info.sampleRate = MFGetAttributeUINT32(current.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
        return m_info.channels && m_info.sampleRate;
    }

    bool configureVideo()
    {
        ComPtr<IMFMediaType> type;
        MFCreateMediaType(&type);
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        if (FAILED(m_reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.Get())))
            return false;
        m_reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
        return readVideoFormat();
    }

    bool readVideoFormat()
    {
        ComPtr<IMFMediaType> current;
        if (FAILED(m_reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current)))
            return false;
        UINT32 width = 0;
        UINT32 height = 0;
        if (FAILED(MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &width, &height)) || !width || !height)
            return false;
        m_codedHeight = height;
        // What shows: the display aperture (the coded size is in whole
        // macroblocks, 64x36 is coded 64x48).
        m_visible = { 0, 0, static_cast<int>(width), static_cast<int>(height) };
        MFVideoArea area { };
        if (SUCCEEDED(current->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr))
            || SUCCEEDED(current->GetBlob(MF_MT_GEOMETRIC_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr))) {
            if (area.Area.cx > 0 && area.Area.cy > 0)
                m_visible = { area.OffsetX.value, area.OffsetY.value, static_cast<int>(area.Area.cx), static_cast<int>(area.Area.cy) };
        }
        m_visible.width &= ~1;
        m_visible.height &= ~1;
        m_info.videoSize = { m_visible.width, m_visible.height };
        m_stride = static_cast<int>(MFGetAttributeUINT32(current.Get(), MF_MT_DEFAULT_STRIDE, width));
        switch (MFGetAttributeUINT32(current.Get(), MF_MT_YUV_MATRIX, MFVideoTransferMatrix_Unknown)) {
        case MFVideoTransferMatrix_BT709:
            m_matrix = ColorMatrix::BT709;
            break;
        case MFVideoTransferMatrix_BT2020_10:
        case MFVideoTransferMatrix_BT2020_12:
            m_matrix = ColorMatrix::BT2020;
            break;
        case MFVideoTransferMatrix_BT601:
        case MFVideoTransferMatrix_SMPTE240M:
            m_matrix = ColorMatrix::BT601;
            break;
        default:
            // Unsaid: HD sizes are BT.709, as decoders and browsers assume.
            m_matrix = height >= 720 ? ColorMatrix::BT709 : ColorMatrix::BT601;
            break;
        }
        m_fullRange = MFGetAttributeUINT32(current.Get(), MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235) == MFNominalRange_0_255;
        return true;
    }

    bool readAudioSample()
    {
        for (;;) {
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            if (FAILED(m_reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &timestamp, &sample)))
                return false;
            if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR))
                return false;
            if (!sample)
                continue;
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
                return false;
            BYTE* data = nullptr;
            DWORD length = 0;
            if (FAILED(buffer->Lock(&data, nullptr, &length)))
                return false;
            std::span<const float> samples { reinterpret_cast<const float*>(data), length / sizeof(float) };
            auto time = fromHundredNanoseconds(timestamp);
            if (m_audioSeekTarget.isValid()) {
                // The samples before the seek's target are dropped.
                size_t skip = 0;
                if (time < m_audioSeekTarget)
                    skip = static_cast<size_t>(((m_audioSeekTarget - time).toDouble()) * m_info.sampleRate) * m_info.channels;
                if (skip >= samples.size()) {
                    buffer->Unlock();
                    continue;
                }
                samples = samples.subspan(skip);
                time = time + MediaTime(skip / m_info.channels, m_info.sampleRate);
                m_audioSeekTarget = MediaTime::invalidTime();
            }
            m_audio.clear();
            m_audio.append(samples);
            buffer->Unlock();
            m_audioOffset = 0;
            m_audioTime = time;
            return true;
        }
    }

    sk_sp<SkImage> convert(IMFSample* sample)
    {
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
            return nullptr;
        BYTE* data = nullptr;
        LONG pitch = m_stride;
        ComPtr<IMF2DBuffer> buffer2D;
        bool locked2D = SUCCEEDED(buffer.As(&buffer2D)) && SUCCEEDED(buffer2D->Lock2D(&data, &pitch));
        if (!locked2D && FAILED(buffer->Lock(&data, nullptr, nullptr)))
            return nullptr;
        // NV12: the Y plane, then interleaved U and V at half resolution.
        const uint8_t* y = data + m_visible.y * pitch + m_visible.x;
        const uint8_t* uv = data + m_codedHeight * pitch + (m_visible.y / 2) * pitch + (m_visible.x & ~1);
        auto image = convertNV12ToImage(y, pitch, uv, pitch, m_visible.width, m_visible.height, m_matrix, m_fullRange);
        if (locked2D)
            buffer2D->Unlock2D();
        else
            buffer->Unlock();
        return image;
    }

    struct Rect {
        int x { 0 };
        int y { 0 };
        int width { 0 };
        int height { 0 };
    };

    bool m_comInitialized { false };
    ComPtr<IMFMediaSource> m_source;
    ComPtr<IMFSourceReader> m_reader;

    Vector<float> m_audio;
    size_t m_audioOffset { 0 };
    MediaTime m_audioTime { MediaTime::zeroTime() };
    MediaTime m_audioSeekTarget { MediaTime::invalidTime() };
    MediaTime m_videoSeekTarget { MediaTime::invalidTime() };

    Rect m_visible;
    UINT32 m_codedHeight { 0 };
    int m_stride { 0 };
    ColorMatrix m_matrix { ColorMatrix::BT601 };
    bool m_fullRange { false };
};

} // namespace

bool mediaFoundationAvailable()
{
    return startMediaFoundation();
}

std::unique_ptr<MediaFile> openMediaFoundation(MediaBytes& bytes, std::span<const uint8_t> head, String& error)
{
    auto file = std::make_unique<MediaFoundationFile>();
    if (!file->open(bytes, head, error))
        return nullptr;
    return file;
}

} // namespace ZeroG::Media

#endif // ENABLE(VIDEO) && defined(ZEROG_MEDIA) && OS(WINDOWS)
