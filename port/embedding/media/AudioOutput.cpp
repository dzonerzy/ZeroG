#include "config.h"
#include "media/AudioOutput.h"

#if ENABLE(VIDEO) && defined(ZEROG_MEDIA)

#include <SDL3/SDL.h>
#include <wtf/MainThread.h>

namespace ZeroG::Media {

static std::atomic<bool> s_initialized;

bool AudioOutput::initialize()
{
    ASSERT(isMainThread());
    // Once for the process; SDL counts its subsystems' users.
    static bool initialized = SDL_InitSubSystem(SDL_INIT_AUDIO);
    s_initialized = initialized;
    return initialized;
}

std::unique_ptr<AudioOutput> AudioOutput::create(unsigned sampleRate, unsigned channels)
{
    if (!sampleRate || !channels || !s_initialized)
        return nullptr;
    SDL_AudioSpec spec { SDL_AUDIO_F32, static_cast<int>(channels), static_cast<int>(sampleRate) };
    auto* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream)
        return nullptr;
    return std::unique_ptr<AudioOutput>(new AudioOutput(stream, sampleRate, channels));
}

AudioOutput::AudioOutput(SDL_AudioStream* stream, unsigned sampleRate, unsigned channels)
    : m_stream(stream)
    , m_sampleRate(sampleRate)
    , m_channels(channels)
{
}

AudioOutput::~AudioOutput()
{
    SDL_DestroyAudioStream(m_stream);
}

void AudioOutput::write(const float* samples, size_t frames)
{
    if (SDL_PutAudioStreamData(m_stream, samples, frames * m_channels * sizeof(float)))
        m_writtenFrames += frames;
}

void AudioOutput::finish()
{
    SDL_FlushAudioStream(m_stream);
}

size_t AudioOutput::queuedFrames() const
{
    int queued = SDL_GetAudioStreamQueued(m_stream);
    return queued > 0 ? queued / (m_channels * sizeof(float)) : 0;
}

uint64_t AudioOutput::playedFrames() const
{
    uint64_t written = m_writtenFrames;
    size_t queued = queuedFrames();
    return written > queued ? written - queued : 0;
}

void AudioOutput::clear()
{
    SDL_ClearAudioStream(m_stream);
    m_writtenFrames = 0;
}

void AudioOutput::setPlaying(bool playing)
{
    if (playing)
        SDL_ResumeAudioStreamDevice(m_stream);
    else
        SDL_PauseAudioStreamDevice(m_stream);
}

void AudioOutput::setVolume(float volume)
{
    SDL_SetAudioStreamGain(m_stream, volume);
}

void AudioOutput::setRate(double rate)
{
    SDL_SetAudioStreamFrequencyRatio(m_stream, std::clamp(rate, 0.01, 100.0));
}

} // namespace ZeroG::Media

#endif
