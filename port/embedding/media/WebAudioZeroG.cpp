// Web Audio for ZeroG (standard, dev and debug profiles): the pieces WebCore
// leaves to each port.
// - AudioDestination: an SDL3 audio stream on the default device whose callback
//   pulls the rendered graph (AudioDestinationResampler converts from the
//   context's sample rate to the device's, as on Cocoa).
// - createBusFromInMemoryAudioFile (decodeAudioData): the media engine's
//   decoders (MediaFile), so the same formats as <audio>.
// - AudioBus::loadPlatformResource: PannerNode's HRTF impulse responses
//   (WebCore's Composite.wav), compiled in.
// The FFT is WebCore/platform/audio/zerog/FFTFrameZeroG.cpp.

#include "config.h"

#if ENABLE(WEB_AUDIO) && defined(ZEROG_MEDIA)

#include "AudioFileReader.h" // not among WebCore's exported headers
#include "media/AudioOutput.h"
#include "media/MediaFile.h"
#include <SDL3/SDL.h>
#include <WebCore/AudioBus.h>
#include <WebCore/AudioChannel.h>
#include <WebCore/AudioDestinationResampler.h>
#include <wtf/MainThread.h>

extern "C" {
extern const unsigned char zerog_hrtf_composite[];
extern const unsigned long zerog_hrtf_composite_size;
}

namespace WebCore {

using namespace ZeroG::Media;

namespace {

constexpr float fallbackSampleRate = 48000;

class AudioDestinationZeroG final : public AudioDestinationResampler, public ThreadSafeRefCounted<AudioDestinationZeroG, WTF::DestructionThread::Main> {
public:
    static Ref<AudioDestinationZeroG> create(const CreationOptions& options) { return adoptRef(*new AudioDestinationZeroG(options)); }

    void ref() const final { ThreadSafeRefCounted::ref(); }
    void deref() const final { ThreadSafeRefCounted::deref(); }

    ~AudioDestinationZeroG()
    {
        if (m_stream)
            SDL_DestroyAudioStream(m_stream);
    }

private:
    explicit AudioDestinationZeroG(const CreationOptions& options)
        : AudioDestinationResampler(options, hardwareSampleRate())
        , m_channels(options.numberOfOutputChannels)
    {
    }

    void startRendering(CompletionHandler<void(bool)>&& completionHandler) final
    {
        ASSERT(isMainThread());
        if (!m_stream && AudioOutput::initialize()) {
            SDL_AudioSpec spec { SDL_AUDIO_F32, static_cast<int>(m_channels), static_cast<int>(hardwareSampleRate()) };
            m_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, this);
        }
        bool success = m_stream && SDL_ResumeAudioStreamDevice(m_stream);
        if (success)
            setIsPlaying(true);
        callOnMainThread([completionHandler = WTF::move(completionHandler), success]() mutable {
            completionHandler(success);
        });
    }

    void stopRendering(CompletionHandler<void(bool)>&& completionHandler) final
    {
        ASSERT(isMainThread());
        bool success = !m_stream || SDL_PauseAudioStreamDevice(m_stream);
        if (success)
            setIsPlaying(false);
        callOnMainThread([completionHandler = WTF::move(completionHandler), success]() mutable {
            completionHandler(success);
        });
    }

    // SDL's audio thread: the device wants `additional` more bytes.
    static void SDLCALL feed(void* userData, SDL_AudioStream* stream, int additional, int)
    {
        auto& destination = *static_cast<AudioDestinationZeroG*>(userData);
        size_t frameBytes = destination.m_channels * sizeof(float);
        size_t frames = (additional + frameBytes - 1) / frameBytes;
        while (frames) {
            size_t chunk = std::min<size_t>(frames, 512);
            destination.renderInto(chunk);
            SDL_PutAudioStreamData(stream, destination.m_interleaved.span().data(), chunk * frameBytes);
            frames -= chunk;
        }
    }

    void renderInto(size_t frames)
    {
        // The graph renders into planar buffers; the device takes interleaved.
        m_planar.resize(m_channels * frames);
        m_interleaved.resize(m_channels * frames);
        unsigned busChannels = std::min<unsigned>(m_channels, m_outputBus->numberOfChannels());
        for (unsigned channel = 0; channel < busChannels; ++channel)
            m_outputBus->setChannelMemory(channel, m_planar.mutableSpan().subspan(channel * frames, frames));
        auto framesToRender = pullRendered(frames);
        render(m_sampleTime, MonotonicTime::now(), framesToRender);
        m_sampleTime += frames;
        for (size_t frame = 0; frame < frames; ++frame) {
            for (unsigned channel = 0; channel < m_channels; ++channel)
                m_interleaved[frame * m_channels + channel] = channel < busChannels ? m_planar[channel * frames + frame] : 0;
        }
    }

    unsigned m_channels;
    SDL_AudioStream* m_stream { nullptr };
    // SDL's audio thread.
    Vector<float> m_planar;
    Vector<float> m_interleaved;
    double m_sampleTime { 0 };
};

} // namespace

Ref<AudioDestination> AudioDestination::create(const CreationOptions& options)
{
    return AudioDestinationZeroG::create(options);
}

float AudioDestination::hardwareSampleRate()
{
    // The default device's rate, once SDL's audio is up (on the main thread).
    if (!isMainThread() || !AudioOutput::initialize())
        return fallbackSampleRate;
    SDL_AudioSpec spec { };
    int sampleFrames = 0;
    if (!SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &sampleFrames) || spec.freq <= 0)
        return fallbackSampleRate;
    return spec.freq;
}

unsigned long AudioDestination::maxChannelCount()
{
    // Stereo destinations only (SDL mixes them down or up to the device).
    return 0;
}

RefPtr<AudioBus> createBusFromInMemoryAudioFile(std::span<const uint8_t> data, bool mixToMono, float sampleRate)
{
    // The whole file is there: a MediaBytes that is complete at once.
    auto bytes = MediaBytes::create();
    bytes->append(data);
    bytes->finish();
    String error;
    auto file = MediaFile::open(bytes, error);
    if (!file || !file->info().hasAudio)
        return nullptr;
    unsigned channels = file->info().channels;
    float fileRate = file->info().sampleRate;

    Vector<float> samples;
    std::array<float, 4096 * 8> chunk;
    size_t chunkFrames = chunk.size() / channels;
    for (;;) {
        MediaTime time;
        size_t frames = file->readAudio(chunk.data(), chunkFrames, time);
        if (!frames)
            break;
        samples.append(std::span<const float> { chunk }.first(frames * channels));
    }
    size_t length = samples.size() / channels;
    if (!length)
        return nullptr;

    Ref bus = AudioBus::create(channels, length);
    bus->setSampleRate(fileRate);
    for (unsigned channel = 0; channel < channels; ++channel) {
        auto destination = bus->channel(channel)->mutableSpan();
        for (size_t frame = 0; frame < length; ++frame)
            destination[frame] = samples[frame * channels + channel];
    }
    RefPtr<AudioBus> result = WTF::move(bus);
    if (mixToMono && channels > 1)
        result = AudioBus::createByMixingToMono(*result);
    if (sampleRate && sampleRate != fileRate)
        result = AudioBus::createBySampleRateConverting(*result, false, sampleRate);
    return result;
}

RefPtr<AudioBus> AudioBus::loadPlatformResource(StringView name, float sampleRate)
{
    // HRTFDatabase's one subject.
    if (name != "Composite"_s)
        return nullptr;
    return createBusFromInMemoryAudioFile({ zerog_hrtf_composite, static_cast<size_t>(zerog_hrtf_composite_size) }, false, sampleRate);
}

} // namespace WebCore

#endif // ENABLE(WEB_AUDIO) && defined(ZEROG_MEDIA)
