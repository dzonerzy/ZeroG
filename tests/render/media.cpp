// Media (ZeroG.md, "Media"): <audio> and <video> through ZeroG's engine, in the
// standard, dev and debug builds. Pages served from app:// (tests/media: tiny
// files made for the test) are played: canPlayType answers for the formats;
// each audio format (Opus and Vorbis in Ogg and WebM, MP3, FLAC, WAV) loads its
// duration and plays to `ended`; a VP9 and Opus video (red for a second, then
// blue) has its size and duration, its frame painted in the page, a seek
// showing the blue frame, and plays to its end; VP8 and AV1 videos show their
// frame. The minimal build has no engine: nothing can play there.
//
// Usage: media   (a sound device is not needed: without one, time runs on the
//        monotonic clock; in Docker SDL_AUDIODRIVER=dummy gives one)

#include "config.h"
#include "Embedding.h"
#include "View.h"
#include "loader/Loader.h"
#include "loader/MemoryFS.h"

#include <WebCore/ColorSpace.h>
#include <WebCore/ImageBuffer.h>
#include <WebCore/Page.h>
#include <WebCore/PixelBuffer.h>
#include <WebCore/PixelBufferFormat.h>
#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>
#include <wtf/FileSystem.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>
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
<html><head><style>
  html, body { margin: 0; background: white; }
  video { position: absolute; left: 0; top: 0; width: 64px; height: 48px; }
</style></head><body><script>
  window.audio = {};
  // An audio file's metadata and its playback to the end.
  function playAudio(name) {
    const result = audio[name] = { };
    const element = new Audio(name);
    element.onloadedmetadata = () => { result.duration = element.duration; element.play().catch(e => result.playError = String(e)); };
    element.onended = () => { result.ended = true; result.endTime = element.currentTime; };
    element.onerror = () => { result.error = element.error ? element.error.code : -1; };
    result.element = element;
  }
  function addVideo(name) {
    const element = document.createElement('video');
    element.src = name;
    element.preload = 'auto';
    element.onloadeddata = () => { element.dataset.loaded = '1'; };
    element.onseeked = () => { element.dataset.seeked = String(element.currentTime); };
    element.onended = () => { element.dataset.ended = '1'; };
    element.onerror = () => { element.dataset.error = element.error ? String(element.error.code) : '-1'; };
    document.body.replaceChildren(element);
    return element;
  }
</script></body></html>)HTML";

Vector<uint8_t> readFixture(const String& name)
{
    auto path = FileSystem::pathByAppendingComponent(String::fromUTF8(ZEROG_TEST_MEDIA), name);
    auto contents = FileSystem::readEntireFile(path);
    return contents ? WTF::move(*contents) : Vector<uint8_t> { };
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool ok = true;
    ZeroG::initializeProcess();

    static constexpr ASCIILiteral fixtures[] = {
        "red-blue.webm"_s, "green-vp8.webm"_s, "yellow-av1.webm"_s, "tone-vorbis.webm"_s,
        "tone-opus.ogg"_s, "tone-vorbis.ogg"_s, "tone.mp3"_s, "tone.flac"_s, "tone.wav"_s,
    };
    auto fs = ZeroG::MemoryFS::create();
    fs->addFile("/index.html"_s, std::span { byteCast<uint8_t>(page), strlen(page) });
    for (auto name : fixtures) {
        auto bytes = readFixture(name);
        ok &= check(!bytes.isEmpty(), "fixture %s (%zu bytes)", name.characters(), bytes.size());
        fs->addFile(makeString('/', name), bytes.span());
    }
    auto session = ZeroG::Session::create();
    session->registerScheme("app"_s, fs.copyRef());

    ZeroG::View::Configuration configuration;
    configuration.size = { 320, 240 };
    configuration.session = session.copyRef();
    auto view = ZeroG::View::create(WTF::move(configuration));

    auto frameTime = MonotonicTime::now();
    auto frame = [&] {
        ZeroG::runPendingWork();
        frameTime = MonotonicTime::now();
        view->render(frameTime);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    };
    auto evaluate = [&](ASCIILiteral script) {
        return view->evaluateScript(String { script });
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
    // The pixel at (x, y) of a CPU render of the page.
    auto pixel = [&](int x, int y) -> std::array<int, 3> {
        RefPtr image = ZeroG::paint(view->page(), ZeroG::Rendering::CPU);
        RefPtr pixels = image ? image->getPixelBuffer({ AlphaPremultiplication::Unpremultiplied, PixelFormat::RGBA8, ColorSpace::SRGB() }, { { x, y }, { 1, 1 } }) : nullptr;
        if (!pixels)
            return { -1, -1, -1 };
        auto bytes = pixels->bytes();
        return { bytes[0], bytes[1], bytes[2] };
    };
    auto isColor = [](std::array<int, 3> actual, std::array<int, 3> expected) {
        for (int i = 0; i < 3; ++i) {
            if (std::abs(actual[i] - expected[i]) > 48)
                return false;
        }
        return true;
    };

    view->loadURL(URL { "app://localhost/index.html"_s });
    // The page's own functions (the initial about:blank is complete at once).
    ok &= check(waitFor(10_s, [&] { return evaluate("typeof addVideo + document.readyState"_s) == "functioncomplete"_s; }), "the page loads");

#if defined(ZEROG_MEDIA)
    // What can play.
    auto canPlay = [&](ASCIILiteral type) {
        return view->evaluateScript(makeString("document.createElement('video').canPlayType('"_s, type, "')"_s));
    };
    ok &= check(canPlay("video/webm; codecs=\"vp9, opus\""_s) == "probably"_s, "canPlayType VP9 and Opus in WebM: %s", canPlay("video/webm; codecs=\"vp9, opus\""_s).utf8().data());
    ok &= check(canPlay("video/webm; codecs=\"av01.0.05M.08\""_s) == "probably"_s, "canPlayType AV1 in WebM");
    ok &= check(canPlay("audio/ogg; codecs=vorbis"_s) == "probably"_s, "canPlayType Vorbis in Ogg");
    ok &= check(canPlay("audio/mpeg"_s) == "maybe"_s, "canPlayType MP3: maybe");
    ok &= check(canPlay("audio/flac"_s) == "maybe"_s && canPlay("audio/wav"_s) == "maybe"_s, "canPlayType FLAC and WAV: maybe");
    ok &= check(canPlay("video/mp4; codecs=\"avc1.42E01E\""_s).isEmpty(), "canPlayType H.264 in MP4: no (patented codecs are not in the engine)");

    // Every audio format, to its end.
    for (auto name : { "tone-opus.ogg"_s, "tone-vorbis.ogg"_s, "tone-vorbis.webm"_s, "tone.mp3"_s, "tone.flac"_s, "tone.wav"_s }) {
        view->evaluateScript(makeString("playAudio('"_s, name, "'); 0"_s));
        auto field = [&](ASCIILiteral key) {
            return view->evaluateScript(makeString("String(audio['"_s, name, "']."_s, key, ")"_s));
        };
        bool ended = waitFor(10_s, [&] { return field("ended"_s) == "true"_s || field("error"_s) != "undefined"_s; });
        auto duration = field("duration"_s).toDouble();
        auto endTime = field("endTime"_s).toDouble();
        ok &= check(duration > 0.9 && duration < 1.2, "%s: duration %.3f s", name.characters(), duration);
        ok &= check(ended && field("ended"_s) == "true"_s && endTime > 0.9, "%s: plays to its end (at %.3f s; error %s, play %s)", name.characters(), endTime,
            field("error"_s).utf8().data(), field("playError"_s).utf8().data());
    }

    // A VP9 and Opus video: red for a second, then blue.
    evaluate("window.video = addVideo('red-blue.webm'); 0"_s);
    bool loaded = waitFor(10_s, [&] { return evaluate("video.dataset.loaded || video.dataset.error || ''"_s) != ""_s; });
    ok &= check(loaded && evaluate("String(video.dataset.loaded)"_s) == "1"_s, "the video loads its first frame (error %s)", evaluate("String(video.dataset.error)"_s).utf8().data());
    auto size = evaluate("video.videoWidth + 'x' + video.videoHeight"_s);
    ok &= check(size == "64x48"_s, "videoWidth and videoHeight: %s", size.utf8().data());
    auto videoDuration = evaluate("String(video.duration)"_s).toDouble();
    ok &= check(videoDuration > 1.9 && videoDuration < 2.2, "duration %.3f s", videoDuration);
    waitFor(500_ms, [] { return false; });
    auto red = pixel(32, 24);
    ok &= check(isColor(red, { 255, 0, 0 }), "the first frame is painted: (%d, %d, %d)", red[0], red[1], red[2]);

    evaluate("video.currentTime = 1.5; 0"_s);
    ok &= check(waitFor(5_s, [&] { return evaluate("video.dataset.seeked || ''"_s) != ""_s; }), "a seek to 1.5 s completes (at %s)", evaluate("String(video.dataset.seeked)"_s).utf8().data());
    waitFor(500_ms, [] { return false; });
    auto blue = pixel(32, 24);
    ok &= check(isColor(blue, { 0, 0, 255 }), "the frame there is painted: (%d, %d, %d)", blue[0], blue[1], blue[2]);

    evaluate("video.play(); 0"_s);
    ok &= check(waitFor(5_s, [&] { return evaluate("video.dataset.ended || ''"_s) == "1"_s; }), "it plays to its end (at %s s)", evaluate("String(video.currentTime)"_s).utf8().data());

    // VP8 and AV1.
    for (auto [name, expected] : { std::pair { "green-vp8.webm"_s, std::array<int, 3> { 0, 128, 0 } }, std::pair { "yellow-av1.webm"_s, std::array<int, 3> { 255, 255, 0 } } }) {
        view->evaluateScript(makeString("window.video = addVideo('"_s, name, "'); 0"_s));
        bool videoLoaded = waitFor(10_s, [&] { return evaluate("video.dataset.loaded || video.dataset.error || ''"_s) != ""_s; });
        waitFor(300_ms, [] { return false; });
        auto color = pixel(32, 24);
        ok &= check(videoLoaded && isColor(color, expected), "%s: its frame is painted (%d, %d, %d)", name.characters(), color[0], color[1], color[2]);
    }
#else
    // The minimal build: no engine.
    ok &= check(evaluate("document.createElement('video').canPlayType('video/webm')"_s).isEmpty(), "minimal build: nothing can play");
    evaluate("playAudio('tone.wav'); 0"_s);
    ok &= check(waitFor(5_s, [&] { return evaluate("String(audio['tone.wav'].error)"_s) != "undefined"_s; }), "an <audio> reports that it cannot play (error %s)",
        evaluate("String(audio['tone.wav'].error)"_s).utf8().data());
#endif

    view = nullptr;
    std::printf("%s\n", ok ? "PASS" : "FAILED");
    return ok ? 0 : 1;
}
