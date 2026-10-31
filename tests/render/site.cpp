// A real site in a view, to see what breaks (not part of the suite: it needs the
// network). Loads the URL, prints the page's console messages and, every
// second, the state of its <video> elements; at the end saves a CPU render.
//
// Usage: site <url> [seconds=20] [out.png=site.png] [--run <script>] [--gpu]
//   --run: evaluated once the page has loaded (to click a button, say)
//   --gpu: a composited GPU view instead of a CPU one

#include "config.h"
#include "Embedding.h"
#include "View.h"
#include "graphics/Surface.h"
#include "loader/Loader.h"

#include <WebCore/ImageBuffer.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

using namespace WebCore;

namespace {

class Client final : public ZeroG::ViewClient {
public:
    void consoleMessage(JSC::MessageSource, JSC::MessageLevel level, const String& message, unsigned line, unsigned, const String& url, const String&) final
    {
        static const char* levels[] = { "log", "warning", "error", "debug", "info" };
        auto index = static_cast<unsigned>(level);
        std::printf("console %s: %s (%s:%u)\n", index < 5 ? levels[index] : "?", message.left(400).utf8().data(), url.right(80).utf8().data(), line);
    }
    void titleChanged(const String& title) final { std::printf("title: %s\n", title.utf8().data()); }
    void loadFinished(const URL& url, const ResourceError& error) final
    {
        auto description = error.isNull() ? String() : makeString(" error: "_s, error.localizedDescription());
        std::printf("load finished: %s%s\n", url.string().left(200).utf8().data(), description.utf8().data());
        loaded = true;
    }
    bool loaded { false };
};

constexpr auto report = R"JS(JSON.stringify({
  url: location.href.slice(0, 120),
  videos: [...document.querySelectorAll('video')].map(v => ({
    src: (v.currentSrc || v.src).slice(0, 60),
    time: +v.currentTime.toFixed(2), duration: v.duration, readyState: v.readyState, networkState: v.networkState,
    paused: v.paused, ended: v.ended, error: v.error && (v.error.code + ' ' + v.error.message),
    size: v.videoWidth + 'x' + v.videoHeight, buffered: [...Array(v.buffered.length).keys()].map(i => v.buffered.start(i).toFixed(1) + '-' + v.buffered.end(i).toFixed(1)).join(' ')
  }))
}))JS";

} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::printf("usage: site <url> [seconds] [out.png] [--run <script>]\n");
        return 2;
    }
    String url = String::fromUTF8(argv[1]);
    int seconds = 20;
    std::string out = "site.png";
    String script;
    bool gpu = false;
    int positional = 0;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--run") && i + 1 < argc)
            script = String::fromUTF8(argv[++i]);
        else if (!std::strcmp(argv[i], "--gpu"))
            gpu = true;
        else if (!positional++)
            seconds = std::atoi(argv[i]);
        else
            out = argv[i];
    }

    ZeroG::initializeProcess();
    Client client;
    ZeroG::View::Configuration configuration;
    configuration.size = { 1280, 720 };
    // --gpu: a composited view (layers, video frames as YUV, GPU canvases).
    if (gpu && ZeroG::initializeGPU())
        configuration.surface = ZeroG::createGPUSurface(3);
    auto view = ZeroG::View::create(WTF::move(configuration));
    std::printf("view: %s\n", view->compositor() ? "GPU, composited" : "CPU");
    view->setClient(&client);
    view->loadURL(URL { url });

    auto start = MonotonicTime::now();
    auto lastReport = start;
    bool ran = false;
    while (MonotonicTime::now() - start < Seconds(seconds)) {
        ZeroG::runPendingWork();
        view->render(MonotonicTime::now());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (client.loaded && !ran && !script.isEmpty()) {
            ran = true;
            std::printf("run: %s\n", view->evaluateScript(script).left(300).utf8().data());
        }
        if (MonotonicTime::now() - lastReport >= 1_s) {
            lastReport = MonotonicTime::now();
            std::printf("[%4.1f s] %s\n", (lastReport - start).seconds(), view->evaluateScript(String::fromLatin1(report)).utf8().data());
        }
    }

    if (RefPtr image = ZeroG::paint(view->page(), ZeroG::Rendering::CPU)) {
        auto png = ZeroG::encodePNG(*image);
        if (FILE* file = std::fopen(out.c_str(), "wb")) {
            std::fwrite(png.span().data(), 1, png.size(), file);
            std::fclose(file);
            std::printf("wrote %s\n", out.c_str());
        }
    }
    view = nullptr;
    return 0;
}
