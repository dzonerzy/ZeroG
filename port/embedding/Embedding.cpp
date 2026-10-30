#include "config.h"
#include "Embedding.h"

#include "PlatformStrategiesZeroG.h"
#include <JavaScriptCore/InitializeThreading.h>
#include <JavaScriptCore/JSCJSValueInlines.h>
#include <JavaScriptCore/JSLock.h>
#include <WebCore/DOMWrapperWorld.h>
#include <WebCore/JSDOMGlobalObject.h>
#include <WebCore/ScriptController.h>
#include <WebCore/ColorSpace.h>
#include <WebCore/DeprecatedGlobalSettings.h>
#include <WebCore/Document.h>
#include <WebCore/DocumentLoader.h>
#include <WebCore/DocumentView.h>
#include <WebCore/DocumentWriter.h>
#include <WebCore/EmptyClients.h>
#include <WebCore/FrameLoader.h>
#include <WebCore/GraphicsContext.h>
#include <WebCore/ImageBuffer.h>
#include <WebCore/ImageUtilities.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/LocalFrameView.h>
#include <WebCore/Page.h>
#include <GLES2/gl2.h>
#include <WebCore/GLContext.h>
#include <WebCore/PlatformDisplay.h>
#include <WebCore/PlatformDisplayZeroG.h>
#include <WebCore/PageConfiguration.h>
#include <WebCore/ProcessWarming.h>
#include <WebCore/Settings.h>
#include <WebCore/WebCoreJITOperations.h>
#include <mutex>
#include <pal/SessionID.h>
#include <wtf/MainThread.h>
#include <JavaScriptCore/Options.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

namespace ZeroG {

using namespace WebCore;

static Lock s_wakeUpLock;
static Vector<Function<void()>>& wakeUpObservers() WTF_REQUIRES_LOCK(s_wakeUpLock)
{
    static NeverDestroyed<Vector<Function<void()>>> observers;
    return observers;
}

static ProcessOptions s_processOptions;

const ProcessOptions& processOptions()
{
    return s_processOptions;
}

void initializeProcess(const ProcessOptions& options)
{
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [&] {
        s_processOptions = options;
        JSC::Options::initialize([&] {
            if (!options.jit)
                JSC::Options::useJIT() = false;
            if (options.gcThreads)
                JSC::Options::numberOfGCMarkers() = options.gcThreads;
            if (options.jitThreads) {
                JSC::Options::numberOfBaselineCompilerThreads() = options.jitThreads;
                JSC::Options::numberOfDFGCompilerThreads() = options.jitThreads;
                JSC::Options::numberOfFTLCompilerThreads() = options.jitThreads;
            }
            if (options.heapLimit)
                JSC::Options::gcMaxHeapSize() = static_cast<unsigned>(std::min<size_t>(options.heapLimit, std::numeric_limits<unsigned>::max()));
#if ASAN_ENABLED && OS(WINDOWS)
            // AddressSanitizer maps its shadow memory on access violations, and
            // WebAssembly's fault handler takes them first and crashes the process:
            // WebAssembly checks its bounds explicitly instead.
            JSC::Options::useWasmFaultSignalHandler() = false;
#endif
        });
        JSC::initialize();
        WTF::initializeMainThread();
        installPlatformStrategies();
        // A page's own clipboard and drag data (DataTransfer.setData with any
        // type, navigator.clipboard), kept as WebKit's ports keep it.
        DeprecatedGlobalSettings::setCustomPasteboardDataEnabled(true);
        populateJITOperations();
        ProcessWarming::prewarmGlobally();
        RunLoop::setWakeUpCallback([] {
            Locker locker { s_wakeUpLock };
            for (auto& observer : wakeUpObservers())
                observer();
        });
    });
}

void addWakeUpObserver(Function<void()>&& observer)
{
    Locker locker { s_wakeUpLock };
    wakeUpObservers().append(WTF::move(observer));
}

void runPendingWork()
{
    RunLoop::cycle();
}

static Lock s_logLock;

static Function<void(LogLevel, const String&)>& logHandler() WTF_REQUIRES_LOCK(s_logLock)
{
    static NeverDestroyed<Function<void(LogLevel, const String&)>> handler;
    return handler;
}

void setLogHandler(Function<void(LogLevel, const String&)>&& handler)
{
    Locker locker { s_logLock };
    logHandler() = WTF::move(handler);
}

void log(LogLevel level, const String& message)
{
    Locker locker { s_logLock };
    if (auto& handler = logHandler()) {
        handler(level, message);
        return;
    }
    static constexpr ASCIILiteral names[] = { "debug"_s, "info"_s, "warning"_s, "error"_s };
    fprintf(stderr, "ZeroG %s: %s\n", names[static_cast<int>(level)].characters(), message.utf8().data());
}

Ref<Page> createPage(const PageOptions& options)
{
    initializeProcess();

    auto configuration = pageConfigurationWithEmptyClients(std::nullopt, PAL::SessionID::defaultSessionID());
    // The empty clients are made for SVG images, whose frames are fully sandboxed;
    // a ZeroG page is the application's own UI.
    std::get<PageConfiguration::LocalMainFrameCreationParameters>(configuration.mainFrameCreationParameters).effectiveSandboxFlags = { };
    Ref page = Page::create(WTF::move(configuration));

    auto& settings = page->settings();
    settings.setScriptEnabled(options.scriptEnabled);
    // CPU painting into an ImageBuffer until the GPU compositor exists.
    settings.setAcceleratedCompositingEnabled(false);

    RefPtr frame = page->localMainFrame();
    RELEASE_ASSERT(frame);
    frame->setView(LocalFrameView::create(*frame, options.viewportSize));
    frame->init();

    return page;
}

void loadHTML(Page& page, const String& html, const URL& baseURL)
{
    RefPtr frame = page.localMainFrame();
    RELEASE_ASSERT(frame);
    RefPtr activeDocumentLoader = frame->loader().activeDocumentLoader();
    RELEASE_ASSERT(activeDocumentLoader);

    auto& writer = activeDocumentLoader->writer();
    writer.setMIMEType("text/html"_s);
    writer.begin(baseURL);
    writer.insertDataSynchronously(html);
    writer.end();

    // The document is the application's, as a committed load's would be (as
    // FrameLoader::initForSynthesizedDocument does): otherwise the frame keeps
    // showing its initial empty document, and every same-origin replacement
    // reuses the previous page's window object and its globals.
    auto& stateMachine = frame->loader().stateMachine();
    if (stateMachine.stateForDebugging() == FrameLoaderStateMachine::DisplayingInitialEmptyDocument)
        stateMachine.advanceTo(FrameLoaderStateMachine::DisplayingInitialEmptyDocumentPostCommit);
    if (stateMachine.stateForDebugging() == FrameLoaderStateMachine::DisplayingInitialEmptyDocumentPostCommit)
        stateMachine.advanceTo(FrameLoaderStateMachine::CommittedFirstRealLoad);

    if (RefPtr document = frame->document())
        document->updateLayoutIgnorePendingStylesheets();
}

String evaluateScript(Page& page, const String& script)
{
    RefPtr frame = page.localMainFrame();
    RELEASE_ASSERT(frame);
    auto& controller = frame->script();
    auto result = controller.executeScriptIgnoringException(script, JSC::SourceTaintedOrigin::Untainted);
    if (!result)
        return { };
    auto* globalObject = controller.globalObject(mainThreadNormalWorldSingleton());
    JSC::JSLockHolder lock(globalObject);
    return result.toWTFString(globalObject);
}

bool initializeGPU(void* hostDisplay)
{
    ASSERT(isMainThread());
    if (auto* display = PlatformDisplay::sharedDisplayIfExists())
        return (!hostDisplay || display->eglDisplay() == hostDisplay) && display->skiaGLContext();
    auto display = PlatformDisplayZeroG::create(hostDisplay);
    if (!display) {
        log(LogLevel::Warning, makeString("No GPU display, rendering on the CPU: "_s, String::fromLatin1(GLContext::lastErrorString())));
        return false;
    }
    PlatformDisplay::setSharedDisplay(WTF::move(display));
    // Skia's GL context for this thread, created now so a failure shows here.
    if (!PlatformDisplay::sharedDisplay().skiaGLContext()) {
        log(LogLevel::Warning, makeString("No GPU context for Skia, rendering on the CPU: "_s, String::fromLatin1(GLContext::lastErrorString())));
        return false;
    }
    return true;
}

String gpuRenderer()
{
    auto* display = PlatformDisplay::sharedDisplayIfExists();
    auto* context = display ? display->skiaGLContext() : nullptr;
    if (!context || !context->makeContextCurrent())
        return { };
    return String::fromLatin1(reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
}

RefPtr<ImageBuffer> paint(Page& page, Rendering rendering, float deviceScaleFactor)
{
    RefPtr frame = page.localMainFrame();
    RELEASE_ASSERT(frame);
    RefPtr view = frame->view();
    RELEASE_ASSERT(view);

    view->updateLayoutAndStyleIfNeededRecursive();

    IntRect rect { { }, view->frameRect().size() };
    auto mode = rendering == Rendering::GPU ? RenderingMode::Accelerated : RenderingMode::Unaccelerated;
    RefPtr buffer = ImageBuffer::create(rect.size(), mode, RenderingPurpose::Unspecified, deviceScaleFactor, ColorSpace::SRGB(), ImageBufferFormat { PixelFormat::BGRA8 });
    if (!buffer)
        return nullptr;

    // Composited layers too, painted into the one buffer (a GPU view's page has
    // them; the CPU reference must show the same thing).
    auto behavior = view->paintBehavior();
    view->setPaintBehavior(behavior | PaintBehavior::FlattenCompositingLayers);
    view->paint(buffer->context(), rect);
    view->setPaintBehavior(behavior);
    return buffer;
}

Vector<uint8_t> encodePNG(ImageBuffer& buffer)
{
    return encodeData(RefPtr { &buffer }, "image/png"_s);
}

} // namespace ZeroG
