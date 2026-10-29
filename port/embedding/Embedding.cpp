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
#include <WebCore/PageConfiguration.h>
#include <WebCore/ProcessWarming.h>
#include <WebCore/Settings.h>
#include <WebCore/WebCoreJITOperations.h>
#include <mutex>
#include <pal/SessionID.h>
#include <wtf/MainThread.h>
#include <wtf/text/WTFString.h>

namespace ZeroG {

using namespace WebCore;

void initializeProcess()
{
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        JSC::initialize();
        WTF::initializeMainThread();
        installPlatformStrategies();
        populateJITOperations();
        ProcessWarming::prewarmGlobally();
    });
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

void loadHTML(Page& page, const String& html)
{
    RefPtr frame = page.localMainFrame();
    RELEASE_ASSERT(frame);
    RefPtr activeDocumentLoader = frame->loader().activeDocumentLoader();
    RELEASE_ASSERT(activeDocumentLoader);

    auto& writer = activeDocumentLoader->writer();
    writer.setMIMEType("text/html"_s);
    writer.begin();
    writer.insertDataSynchronously(html);
    writer.end();

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

RefPtr<ImageBuffer> paint(Page& page, float deviceScaleFactor)
{
    RefPtr frame = page.localMainFrame();
    RELEASE_ASSERT(frame);
    RefPtr view = frame->view();
    RELEASE_ASSERT(view);

    view->updateLayoutAndStyleIfNeededRecursive();

    IntRect rect { { }, view->frameRect().size() };
    RefPtr buffer = ImageBuffer::create(rect.size(), RenderingMode::Unaccelerated, RenderingPurpose::Unspecified, deviceScaleFactor, ColorSpace::SRGB(), ImageBufferFormat { PixelFormat::BGRA8 });
    if (!buffer)
        return nullptr;

    view->paint(buffer->context(), rect);
    return buffer;
}

Vector<uint8_t> encodePNG(ImageBuffer& buffer)
{
    return encodeData(RefPtr { &buffer }, "image/png"_s);
}

} // namespace ZeroG
