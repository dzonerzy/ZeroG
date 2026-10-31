// Web Audio (standard, dev and debug builds), from a page on app://: what the
// graph renders, checked on its samples. An OfflineAudioContext's 440 Hz
// oscillator through a 0.5 gain has that amplitude and frequency; an
// AnalyserNode (the FFT) finds a 1 kHz tone at 1 kHz; a PannerNode in HRTF mode
// (the HRTF responses, the convolver's FFT) renders sound; decodeAudioData
// decodes every format <audio> plays; a live AudioContext's clock runs on the
// sound device. The minimal build has no Web Audio.
//
// Usage: webaudio   (on Linux in Docker, SDL_AUDIODRIVER=dummy gives the live
//        context a device)

#include "config.h"
#include "Embedding.h"
#include "View.h"
#include "loader/Loader.h"
#include "loader/MemoryFS.h"

#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>
#include <wtf/FileSystem.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

using namespace WebCore;

namespace {

#define check(condition, ...) [&] {                    \
        bool result = (condition);                     \
        std::printf("%s ", result ? "ok  " : "FAIL");  \
        std::printf(__VA_ARGS__);                      \
        std::printf("\n");                             \
        return result;                                 \
    }()

constexpr auto page = R"HTML(<!DOCTYPE html>
<html><body><script>
  window.results = {};
  const run = (name, promise) => promise.then(value => results[name] = value, error => results[name] = 'error: ' + error);

  // 0.1 s of a 440 Hz sine at gain 0.5: its RMS and frequency (zero crossings).
  window.oscillatorTest = () => {
    const context = new OfflineAudioContext(1, 4800, 48000);
    const oscillator = new OscillatorNode(context, { frequency: 440 });
    const gain = new GainNode(context, { gain: 0.5 });
    oscillator.connect(gain).connect(context.destination);
    oscillator.start();
    run('oscillator', context.startRendering().then(buffer => {
      const data = buffer.getChannelData(0);
      let sum = 0, peak = 0, crossings = 0;
      for (let i = 0; i < data.length; ++i) {
        sum += data[i] * data[i];
        peak = Math.max(peak, Math.abs(data[i]));
        if (i && (data[i - 1] < 0) != (data[i] < 0))
          ++crossings;
      }
      return { rms: Math.sqrt(sum / data.length), peak, frequency: crossings / 2 / buffer.duration };
    }));
  };

  // A 1 kHz tone through an AnalyserNode, read halfway through the rendering.
  window.analyserTest = () => {
    const context = new OfflineAudioContext(1, 48000, 48000);
    const oscillator = new OscillatorNode(context, { frequency: 1000 });
    const analyser = new AnalyserNode(context, { fftSize: 2048, smoothingTimeConstant: 0 });
    oscillator.connect(analyser).connect(context.destination);
    oscillator.start();
    run('analyser', new Promise(resolve => {
      context.suspend(0.5).then(() => {
        const data = new Float32Array(analyser.frequencyBinCount);
        analyser.getFloatFrequencyData(data);
        let peak = 0;
        for (let i = 1; i < data.length; ++i) {
          if (data[i] > data[peak])
            peak = i;
        }
        resolve({ frequency: peak * context.sampleRate / analyser.fftSize, level: data[peak] });
        context.resume();
      });
      context.startRendering();
    }));
  };

  // A PannerNode in HRTF mode: the impulse responses load and the convolution
  // renders.
  window.pannerTest = () => {
    const context = new OfflineAudioContext(2, 48000, 48000);
    const oscillator = new OscillatorNode(context, { frequency: 440 });
    const panner = new PannerNode(context, { panningModel: 'HRTF', positionX: 1, positionY: 0, positionZ: 0 });
    oscillator.connect(panner).connect(context.destination);
    oscillator.start();
    run('panner', context.startRendering().then(buffer => {
      const peak = channel => buffer.getChannelData(channel).slice(24000).reduce((a, b) => Math.max(a, Math.abs(b)), 0);
      return { left: peak(0), right: peak(1) };
    }));
  };

  window.decodeTest = name => run('decode ' + name, fetch(name).then(response => response.arrayBuffer())
    .then(data => new OfflineAudioContext(1, 1, 48000).decodeAudioData(data))
    .then(buffer => ({ duration: buffer.duration, channels: buffer.numberOfChannels, rate: buffer.sampleRate })));

  window.liveTest = () => {
    const context = new AudioContext();
    window.liveContext = context;
    const oscillator = new OscillatorNode(context, { frequency: 440 });
    const gain = new GainNode(context, { gain: 0.05 });
    oscillator.connect(gain).connect(context.destination);
    oscillator.start();
    run('live', context.resume().then(() => new Promise(resolve => setTimeout(() => resolve({ state: context.state, time: context.currentTime }), 700))));
  };

  // An <audio>'s sound in the graph (MediaElementAudioSourceNode): the
  // analyser hears its 440 Hz tone.
  window.elementTest = () => {
    const context = window.liveContext;
    const audio = new Audio('tone.wav');
    audio.loop = true;
    const analyser = new AnalyserNode(context, { fftSize: 2048, smoothingTimeConstant: 0 });
    context.createMediaElementSource(audio).connect(analyser).connect(context.destination);
    run('element', audio.play().then(() => new Promise(resolve => setTimeout(() => {
      const data = new Float32Array(analyser.fftSize);
      analyser.getFloatTimeDomainData(data);
      const spectrum = new Float32Array(analyser.frequencyBinCount);
      analyser.getFloatFrequencyData(spectrum);
      let peak = 1;
      for (let i = 1; i < spectrum.length; ++i) {
        if (spectrum[i] > spectrum[peak])
          peak = i;
      }
      audio.pause();
      resolve({ peak: data.reduce((a, b) => Math.max(a, Math.abs(b)), 0), frequency: peak * context.sampleRate / analyser.fftSize, time: audio.currentTime });
    }, 600))));
  };
</script></body></html>)HTML";

Vector<uint8_t> readFixture(const String& name)
{
    auto contents = FileSystem::readEntireFile(FileSystem::pathByAppendingComponent(String::fromUTF8(ZEROG_TEST_MEDIA), name));
    return contents ? WTF::move(*contents) : Vector<uint8_t> { };
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool ok = true;
    ZeroG::initializeProcess();

    static constexpr ASCIILiteral fixtures[] = { "tone-opus.ogg"_s, "tone-vorbis.ogg"_s, "tone-vorbis.webm"_s, "tone.mp3"_s, "tone.flac"_s, "tone.wav"_s };
    auto fs = ZeroG::MemoryFS::create();
    fs->addFile("/index.html"_s, std::span { byteCast<uint8_t>(page), strlen(page) });
    for (auto name : fixtures)
        fs->addFile(makeString('/', name), readFixture(name).span());
    auto session = ZeroG::Session::create();
    session->registerScheme("app"_s, fs.copyRef());
    ZeroG::View::Configuration configuration;
    configuration.size = { 200, 100 };
    configuration.session = session.copyRef();
    auto view = ZeroG::View::create(WTF::move(configuration));

    auto frame = [&] {
        ZeroG::runPendingWork();
        view->render(MonotonicTime::now());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    };
    auto evaluate = [&](const String& script) {
        return view->evaluateScript(script);
    };
    auto waitFor = [&](Seconds timeout, const std::function<bool()>& condition) {
        auto deadline = MonotonicTime::now() + timeout;
        while (!condition()) {
            if (MonotonicTime::now() > deadline)
                return false;
            frame();
        }
        return true;
    };
    // A result of the page's tests, as JSON, once settled.
    auto result = [&](const String& name) {
        auto key = makeString("results['"_s, name, "']"_s);
        waitFor(20_s, [&] { return evaluate(makeString("String("_s, key, " !== undefined)"_s)) == "true"_s; });
        return evaluate(makeString("JSON.stringify("_s, key, ")"_s));
    };
    auto number = [&](const String& name, ASCIILiteral field) {
        return evaluate(makeString("String(results['"_s, name, "']."_s, field, ")"_s)).toDouble();
    };

    view->loadURL(URL { "app://localhost/index.html"_s });
    ok &= check(waitFor(10_s, [&] { return evaluate("typeof oscillatorTest + document.readyState"_s) == "functioncomplete"_s; }), "the page loads");

#if ENABLE(WEB_AUDIO)
    evaluate("oscillatorTest(); 0"_s);
    auto oscillator = result("oscillator"_s);
    double rms = number("oscillator"_s, "rms"_s);
    double peak = number("oscillator"_s, "peak"_s);
    double frequency = number("oscillator"_s, "frequency"_s);
    ok &= check(std::abs(rms - 0.5 / std::sqrt(2.0)) < 0.02 && peak < 0.51, "OfflineAudioContext: a 440 Hz sine at gain 0.5 (RMS %.3f, peak %.3f) %s", rms, peak, oscillator.utf8().data());
    ok &= check(std::abs(frequency - 440) < 15, "its frequency from the samples: %.0f Hz", frequency);

    evaluate("analyserTest(); 0"_s);
    auto analyser = result("analyser"_s);
    double analysed = number("analyser"_s, "frequency"_s);
    ok &= check(std::abs(analysed - 1000) < 2 * 48000.0 / 2048, "AnalyserNode (the FFT) finds a 1 kHz tone at %.0f Hz %s", analysed, analyser.utf8().data());

    evaluate("pannerTest(); 0"_s);
    auto panner = result("panner"_s);
    double left = number("panner"_s, "left"_s);
    double right = number("panner"_s, "right"_s);
    ok &= check(right > 0.05 && right > left, "PannerNode in HRTF mode renders, louder to the right (left %.3f, right %.3f) %s", left, right, panner.utf8().data());

    for (auto name : fixtures) {
        auto key = makeString("decode "_s, name);
        evaluate(makeString("decodeTest('"_s, name, "'); 0"_s));
        auto decoded = result(key);
        double duration = number(key, "duration"_s);
        ok &= check(duration > 0.9 && duration < 1.1, "decodeAudioData %s: %s", name.characters(), decoded.utf8().data());
    }

    evaluate("liveTest(); 0"_s);
    auto live = result("live"_s);
    double liveTime = number("live"_s, "time"_s);
    ok &= check(liveTime > 0.3, "a live AudioContext's clock runs on the sound device: %s", live.utf8().data());

    evaluate("elementTest(); 0"_s);
    auto element = result("element"_s);
    double elementPeak = number("element"_s, "peak"_s);
    double elementFrequency = number("element"_s, "frequency"_s);
    ok &= check(elementPeak > 0.05 && std::abs(elementFrequency - 440) < 2 * 48000.0 / 2048, "an <audio> in the graph (MediaElementAudioSourceNode): its tone reaches an AnalyserNode %s", element.utf8().data());
    evaluate("liveContext.close(); 0"_s);
    waitFor(300_ms, [] { return false; });
#else
    ok &= check(evaluate("typeof AudioContext"_s) == "undefined"_s, "minimal build: no Web Audio");
#endif

    view = nullptr;
    std::printf("%s\n", ok ? "PASS" : "FAILED");
    return ok ? 0 : 1;
}
