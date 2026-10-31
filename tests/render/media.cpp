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
    if (name)
      element.src = name;
    element.preload = 'auto';
    element.onloadeddata = () => { element.dataset.loaded = '1'; };
    element.onseeked = () => { element.dataset.seeked = String(element.currentTime); };
    element.onended = () => { element.dataset.ended = '1'; };
    element.onerror = () => { element.dataset.error = element.error ? String(element.error.code) : '-1'; };
    document.body.replaceChildren(element);
    return element;
  }
  // Media Source Extensions: a video whose source buffers are fed WebM files,
  // each appended in two pieces cut mid-element. `buffers`: [name, type].
  function playSource(buffers) {
    const element = addVideo(null);
    const state = element.dataset;
    const source = new MediaSource();
    source.addEventListener('sourceopen', async () => {
      try {
        const append = (buffer, part) => new Promise((resolve, reject) => {
          buffer.addEventListener('updateend', resolve, { once: true });
          buffer.addEventListener('error', () => reject('append error'), { once: true });
          buffer.appendBuffer(part);
        });
        const ranges = [];
        await Promise.all(buffers.map(async ([name, type]) => {
          const buffer = source.addSourceBuffer(type);
          const data = new Uint8Array(await (await fetch(name)).arrayBuffer());
          const middle = Math.floor(data.length / 2) + 7;
          await append(buffer, data.subarray(0, middle));
          await append(buffer, data.subarray(middle));
          const range = buffer.buffered;
          ranges.push(range.length + ':' + (range.length ? range.start(0).toFixed(2) + '-' + range.end(0).toFixed(2) : ''));
        }));
        state.buffered = ranges.join(' ');
        source.endOfStream();
      } catch (error) {
        state.sourceError = String(error);
      }
    });
    element.src = URL.createObjectURL(source);
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
        "magenta-cyan.mp4"_s, "tone-aac.m4a"_s, "magenta-cyan-frag.mp4"_s, "red-blue-frag.mp4"_s, "red-blue-vp9.mp4"_s,
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
#if OS(WINDOWS)
    // H.264 and AAC: the OS decodes them (Media Foundation).
    ok &= check(canPlay("video/mp4; codecs=\"avc1.42E01E, mp4a.40.2\""_s) == "probably"_s, "canPlayType H.264 and AAC in MP4 (Media Foundation)");
    ok &= check(canPlay("video/mp4; codecs=\"hvc1.1.6.L93.B0\""_s).isEmpty(), "canPlayType HEVC: no");
    static constexpr ASCIILiteral audioFiles[] = { "tone-opus.ogg"_s, "tone-vorbis.ogg"_s, "tone-vorbis.webm"_s, "tone.mp3"_s, "tone.flac"_s, "tone.wav"_s, "tone-aac.m4a"_s };
#else
    ok &= check(canPlay("video/mp4; codecs=\"avc1.42E01E\""_s).isEmpty(), "canPlayType H.264 in MP4: no (patented codecs are not in the engine; Windows decodes them)");
    static constexpr ASCIILiteral audioFiles[] = { "tone-opus.ogg"_s, "tone-vorbis.ogg"_s, "tone-vorbis.webm"_s, "tone.mp3"_s, "tone.flac"_s, "tone.wav"_s };
#endif

    // Every audio format, to its end.
    for (auto name : audioFiles) {
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

    // The native controls (WebKit's modern media controls, Adwaita's icons
    // compiled in): a paused video shows its play button, which plays it.
    {
        view->evaluateScript("window.video = addVideo('red-blue.webm'); video.controls = true; video.style.width = '320px'; video.style.height = '180px'; 0"_s);
        bool controlsLoaded = waitFor(10_s, [&] { return evaluate("video.dataset.loaded || video.dataset.error || ''"_s) != ""_s; });
        waitFor(1_s, [] { return false; });
        // The button: a dark disc over the red frame with a light icon in it.
        auto icon = pixel(160, 90);
        auto disc = pixel(136, 90);
        auto frame = pixel(60, 20);
        ok &= check(controlsLoaded && icon[0] > 200 && icon[1] > 200 && icon[2] > 200, "the play button's icon is drawn: (%d, %d, %d)", icon[0], icon[1], icon[2]);
        ok &= check(disc[0] < 200 && disc[0] > 60 && disc[1] < 100, "on its disc: (%d, %d, %d)", disc[0], disc[1], disc[2]);
        ok &= check(isColor(frame, { 255, 0, 0 }), "over the video's frame: (%d, %d, %d)", frame[0], frame[1], frame[2]);
        view->mouseMove({ 160, 90 }, ZeroG::View::MouseButton::None, { });
        view->mouseDown({ 160, 90 }, ZeroG::View::MouseButton::Left, 1, { });
        view->mouseUp({ 160, 90 }, ZeroG::View::MouseButton::Left, 1, { });
        ok &= check(waitFor(3_s, [&] { return evaluate("String(!video.paused && video.currentTime > 0.2)"_s) == "true"_s; }), "clicking it plays the video (at %s s, paused %s)",
            evaluate("String(video.currentTime)"_s).utf8().data(), evaluate("String(video.paused)"_s).utf8().data());
        // Hovered while playing: the controls bar (pause, times, seek bar,
        // volume) along the bottom.
        view->mouseMove({ 150, 150 }, ZeroG::View::MouseButton::None, { });
        waitFor(300_ms, [] { return false; });
        // The pause icon's two bars, light on the bar's gray, near its left end.
        int brightest = 0;
        for (int x = 20; x < 40; ++x)
            brightest = std::max(brightest, pixel(x, 158)[1]);
        ok &= check(brightest > 180, "hovered while playing, the controls bar shows its pause button (brightest %d)", brightest);
        evaluate("video.pause(); 0"_s);
    }

    // VP9 and Opus in MP4 files, everywhere: a regular one (its moov after the
    // mdat) and a fragmented one, red then blue.
    ok &= check(canPlay("video/mp4; codecs=\"vp09.00.10.08, opus\""_s) == "probably"_s, "canPlayType VP9 and Opus in MP4");
    for (auto name : { "red-blue-vp9.mp4"_s, "red-blue-frag.mp4"_s }) {
        std::printf("     %s\n", name.characters());
        view->evaluateScript(makeString("window.video = addVideo('"_s, name, "'); 0"_s));
        bool fileLoaded = waitFor(10_s, [&] { return evaluate("video.dataset.loaded || video.dataset.error || ''"_s) != ""_s; });
        ok &= check(fileLoaded && evaluate("String(video.dataset.loaded)"_s) == "1"_s, "it loads its first frame (error %s)", evaluate("String(video.dataset.error)"_s).utf8().data());
        // A fragmented file's moov does not say (no mehd): open-ended until its end.
        auto fileDuration = evaluate("String(video.duration)"_s);
        bool fragmented = name == "red-blue-frag.mp4"_s;
        ok &= check(fragmented ? fileDuration == "Infinity"_s : (fileDuration.toDouble() > 1.9 && fileDuration.toDouble() < 2.2), "duration %s", fileDuration.utf8().data());
        waitFor(500_ms, [] { return false; });
        auto fileRed = pixel(32, 24);
        ok &= check(isColor(fileRed, { 255, 0, 0 }), "the first frame is painted: (%d, %d, %d)", fileRed[0], fileRed[1], fileRed[2]);
        evaluate("video.currentTime = 1.5; 0"_s);
        ok &= check(waitFor(5_s, [&] { return evaluate("video.dataset.seeked || ''"_s) != ""_s; }), "a seek to 1.5 s completes (at %s)", evaluate("String(video.dataset.seeked)"_s).utf8().data());
        waitFor(500_ms, [] { return false; });
        auto fileBlue = pixel(32, 24);
        ok &= check(isColor(fileBlue, { 0, 0, 255 }), "the frame there is painted: (%d, %d, %d)", fileBlue[0], fileBlue[1], fileBlue[2]);
        evaluate("video.play(); 0"_s);
        ok &= check(waitFor(5_s, [&] { return evaluate("video.dataset.ended || ''"_s) == "1"_s; }), "it plays to its end (at %s s of %s)", evaluate("String(video.currentTime)"_s).utf8().data(), evaluate("String(video.duration)"_s).utf8().data());
    }

#if OS(WINDOWS)
    // H.264 and AAC in MP4: magenta for a second, then cyan; 64x36, coded in
    // whole macroblocks (64x48), so the decoder's frames are cropped.
    evaluate("window.video = addVideo('magenta-cyan.mp4'); 0"_s);
    bool mp4Loaded = waitFor(10_s, [&] { return evaluate("video.dataset.loaded || video.dataset.error || ''"_s) != ""_s; });
    ok &= check(mp4Loaded && evaluate("String(video.dataset.loaded)"_s) == "1"_s, "an MP4 video loads its first frame (error %s)", evaluate("String(video.dataset.error)"_s).utf8().data());
    auto mp4Size = evaluate("video.videoWidth + 'x' + video.videoHeight"_s);
    ok &= check(mp4Size == "64x36"_s, "its size is the visible one: %s", mp4Size.utf8().data());
    auto mp4Duration = evaluate("String(video.duration)"_s).toDouble();
    ok &= check(mp4Duration > 1.9 && mp4Duration < 2.2, "duration %.3f s", mp4Duration);
    waitFor(500_ms, [] { return false; });
    auto magenta = pixel(32, 24);
    ok &= check(isColor(magenta, { 255, 0, 255 }), "the first frame is painted: (%d, %d, %d)", magenta[0], magenta[1], magenta[2]);
    evaluate("video.currentTime = 1.5; 0"_s);
    ok &= check(waitFor(5_s, [&] { return evaluate("video.dataset.seeked || ''"_s) != ""_s; }), "a seek to 1.5 s completes (at %s)", evaluate("String(video.dataset.seeked)"_s).utf8().data());
    waitFor(500_ms, [] { return false; });
    auto cyan = pixel(32, 24);
    ok &= check(isColor(cyan, { 0, 255, 255 }), "the frame there is painted: (%d, %d, %d)", cyan[0], cyan[1], cyan[2]);
    evaluate("video.play(); 0"_s);
    ok &= check(waitFor(5_s, [&] { return evaluate("video.dataset.ended || ''"_s) == "1"_s; }), "it plays to its end (at %s s)", evaluate("String(video.currentTime)"_s).utf8().data());
#endif

#if ENABLE(MEDIA_SOURCE)
    // Media Source Extensions: WebM, and fragmented MP4.
    ok &= check(evaluate("String(MediaSource.isTypeSupported('video/webm; codecs=\"vp9, opus\"'))"_s) == "true"_s, "MediaSource.isTypeSupported VP9 and Opus in WebM");
    ok &= check(evaluate("String(MediaSource.isTypeSupported('audio/webm; codecs=\"vorbis\"'))"_s) == "true"_s, "MediaSource.isTypeSupported Vorbis in WebM");
    ok &= check(evaluate("String(MediaSource.isTypeSupported('video/mp4; codecs=\"vp09.00.10.08, opus\"'))"_s) == "true"_s, "MediaSource.isTypeSupported VP9 and Opus in MP4");
#if OS(WINDOWS)
    ok &= check(evaluate("String(MediaSource.isTypeSupported('video/mp4; codecs=\"avc1.4D401E, mp4a.40.2\"'))"_s) == "true"_s, "MediaSource.isTypeSupported H.264 and AAC in MP4 (Media Foundation)");
#else
    ok &= check(evaluate("String(MediaSource.isTypeSupported('video/mp4; codecs=\"avc1.4D401E\"'))"_s) == "false"_s, "MediaSource.isTypeSupported H.264: false (Windows decodes it)");
#endif
    ok &= check(evaluate("String(MediaSource.isTypeSupported('video/mp4; codecs=\"hvc1.1.6.L93.B0\"'))"_s) == "false"_s, "MediaSource.isTypeSupported HEVC: false");

    // A video through a source buffer, two colours a second each: its buffer,
    // frames, a seek and its end.
    auto checkSourceVideo = [&](ASCIILiteral name, ASCIILiteral type, ASCIILiteral size, std::array<int, 3> first, std::array<int, 3> second) {
        std::printf("     %s (%s)\n", name.characters(), type.characters());
        view->evaluateScript(makeString("window.video = playSource([['"_s, name, "', '"_s, type, "']]); 0"_s));
        bool sourceLoaded = waitFor(10_s, [&] { return evaluate("video.dataset.loaded || video.dataset.error || video.dataset.sourceError || ''"_s) != ""_s; });
        ok &= check(sourceLoaded && evaluate("String(video.dataset.loaded)"_s) == "1"_s, "a media source's video loads its first frame (error %s, %s)",
            evaluate("String(video.dataset.error)"_s).utf8().data(), evaluate("String(video.dataset.sourceError)"_s).utf8().data());
        waitFor(5_s, [&] { return evaluate("video.dataset.buffered || ''"_s) != ""_s; });
        auto bufferedRanges = evaluate("String(video.dataset.buffered)"_s);
        ok &= check(bufferedRanges.startsWith("1:0.0"_s) && (bufferedRanges.contains("-2.0"_s) || bufferedRanges.contains("-1.9"_s)), "its buffer holds the whole video, appended in two pieces: %s", bufferedRanges.utf8().data());
        auto sourceSize = evaluate("video.videoWidth + 'x' + video.videoHeight"_s);
        ok &= check(sourceSize == StringView { size }, "videoWidth and videoHeight: %s", sourceSize.utf8().data());
        waitFor(500_ms, [] { return false; });
        auto firstColor = pixel(32, 24);
        ok &= check(isColor(firstColor, first), "the first frame is painted: (%d, %d, %d)", firstColor[0], firstColor[1], firstColor[2]);
        evaluate("video.currentTime = 1.5; 0"_s);
        ok &= check(waitFor(5_s, [&] { return evaluate("video.dataset.seeked || ''"_s) != ""_s; }), "a seek to 1.5 s completes (at %s)", evaluate("String(video.dataset.seeked)"_s).utf8().data());
        waitFor(500_ms, [] { return false; });
        auto secondColor = pixel(32, 24);
        ok &= check(isColor(secondColor, second), "the frame there is painted: (%d, %d, %d)", secondColor[0], secondColor[1], secondColor[2]);
        evaluate("video.play(); 0"_s);
        ok &= check(waitFor(5_s, [&] { return evaluate("video.dataset.ended || ''"_s) == "1"_s; }), "it plays to its end (%s)",
            evaluate("`at ${video.currentTime} s of ${video.duration}, ready state ${video.readyState}`"_s).utf8().data());
    };
    checkSourceVideo("red-blue.webm"_s, "video/webm; codecs=\"vp9, opus\""_s, "64x48"_s, { 255, 0, 0 }, { 0, 0, 255 });
    checkSourceVideo("red-blue-frag.mp4"_s, "video/mp4; codecs=\"vp09.00.10.08, opus\""_s, "64x48"_s, { 255, 0, 0 }, { 0, 0, 255 });
#if OS(WINDOWS)
    // H.264 with B-frames (decode order is not presentation order) and AAC.
    checkSourceVideo("magenta-cyan-frag.mp4"_s, "video/mp4; codecs=\"avc1.4D401E, mp4a.40.2\""_s, "64x36"_s, { 255, 0, 255 }, { 0, 255, 255 });
#endif

    // Two source buffers: VP8 video from one file, Vorbis from another.
    evaluate("window.video = playSource([['green-vp8.webm', 'video/webm; codecs=\"vp8\"'], ['tone-vorbis.webm', 'audio/webm; codecs=\"vorbis\"']]); 0"_s);
    bool twoLoaded = waitFor(10_s, [&] { return evaluate("video.dataset.loaded || video.dataset.error || video.dataset.sourceError || ''"_s) != ""_s; });
    waitFor(5_s, [&] { return evaluate("video.dataset.buffered || ''"_s) != ""_s; });
    ok &= check(twoLoaded && evaluate("String(video.dataset.loaded)"_s) == "1"_s, "two source buffers, audio and video, load (buffered %s; error %s, %s)", evaluate("String(video.dataset.buffered)"_s).utf8().data(),
        evaluate("String(video.dataset.error)"_s).utf8().data(), evaluate("String(video.dataset.sourceError)"_s).utf8().data());
    waitFor(300_ms, [] { return false; });
    auto green = pixel(32, 24);
    ok &= check(isColor(green, { 0, 128, 0 }), "the VP8 frame is painted: (%d, %d, %d)", green[0], green[1], green[2]);
    evaluate("video.play(); 0"_s);
    ok &= check(waitFor(5_s, [&] { return evaluate("video.dataset.ended || ''"_s) == "1"_s; }), "they play to the end (at %s s)", evaluate("String(video.currentTime)"_s).utf8().data());
#endif
#else
    // The minimal build: no engine.
    ok &= check(evaluate("document.createElement('video').canPlayType('video/webm')"_s).isEmpty(), "minimal build: nothing can play");
    ok &= check(evaluate("typeof MediaSource"_s) == "undefined"_s, "and there is no MediaSource");
    evaluate("playAudio('tone.wav'); 0"_s);
    ok &= check(waitFor(5_s, [&] { return evaluate("String(audio['tone.wav'].error)"_s) != "undefined"_s; }), "an <audio> reports that it cannot play (error %s)",
        evaluate("String(audio['tone.wav'].error)"_s).utf8().data());
#endif

    view = nullptr;
    std::printf("%s\n", ok ? "PASS" : "FAILED");
    return ok ? 0 : 1;
}
