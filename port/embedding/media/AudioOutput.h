#pragma once

// A player's sound, through SDL3's audio (WASAPI on Windows; PipeWire,
// PulseAudio or ALSA on Linux, loaded at run time): one audio stream per player
// on the default device, which SDL mixes. Its position, the frames played so
// far, is the player's clock. Thread-safe (SDL's streams are).

#include <atomic>
#include <cstdint>
#include <memory>

struct SDL_AudioStream;

namespace ZeroG::Media {

class AudioOutput {
public:
    // Engine thread, before the first create(): SDL's audio subsystem (it
    // brings SDL's events with it, which start on the main thread). False
    // without a sound device or driver.
    static bool initialize();
    // Any thread, after initialize(). Null when there is no audio device (the
    // player then keeps time itself).
    static std::unique_ptr<AudioOutput> create(unsigned sampleRate, unsigned channels);
    ~AudioOutput();

    unsigned sampleRate() const { return m_sampleRate; }
    unsigned channels() const { return m_channels; }

    // Interleaved float frames, played after those queued.
    void write(const float*, size_t frames);
    // No more frames until the next clear(): what the resampler holds back
    // (it waits for more input) is played too.
    void finish();
    // Frames queued but not played yet.
    size_t queuedFrames() const;
    // Frames played since the last clear().
    uint64_t playedFrames() const;
    // Drops what is queued (a seek); the count starts again at 0.
    void clear();

    void setPlaying(bool);
    void setVolume(float);
    // Faster or slower playback (the pitch follows).
    void setRate(double);

private:
    AudioOutput(SDL_AudioStream*, unsigned sampleRate, unsigned channels);

    SDL_AudioStream* m_stream;
    unsigned m_sampleRate;
    unsigned m_channels;
    std::atomic<uint64_t> m_writtenFrames { 0 }; // by the decode thread, read for the clock
};

} // namespace ZeroG::Media
