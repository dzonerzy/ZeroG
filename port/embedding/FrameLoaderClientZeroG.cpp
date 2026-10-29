/*
 * Copyright (C) 2006 Eric Seidel (eric@webkit.org)
 * Copyright (C) 2008-2025 Apple Inc. All rights reserved.
 * Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2012 Samsung Electronics. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// Derived from EmptyFrameLoaderClient (Source/WebCore/loader/EmptyFrameLoaderClient.h
// and EmptyClients.cpp) at the pinned WebKit commit, with the loading behavior of
// WebKit's frame loader clients: loaded data is committed to the document, each
// committed page gets a frame view, policy decisions go to the view's client, and
// titles, load completion and new window objects are reported (docs/PROVENANCE.md).

#include "config.h"
#include "FrameLoaderClientZeroG.h"

#include "View.h"
#include <WebCore/Color.h>
#include <WebCore/DOMWrapperWorld.h>
#include <WebCore/DocumentLoader.h>
#include <WebCore/DocumentView.h>
#include <WebCore/FormState.h>
#include <WebCore/FrameLoader.h>
#include <WebCore/FrameNetworkingContext.h>
#include <WebCore/HTMLFrameOwnerElement.h>
#include <WebCore/HistoryItem.h>
#include <WebCore/HitTestResult.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/LocalFrameLoaderClient.h>
#include <WebCore/LocalFrameView.h>
#include <WebCore/MIMETypeRegistry.h>
#include <WebCore/NavigationAction.h>
#include <WebCore/Page.h>
#include <WebCore/PolicyChecker.h>
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/ResourceResponse.h>
#include <WebCore/ScriptController.h>
#include <WebCore/SharedBuffer.h>
#include <WebCore/StringWithDirection.h>
#include <WebCore/SubstituteData.h>
#include <WebCore/UserAgent.h>
#include <WebCore/Widget.h>

namespace ZeroG {

using namespace WebCore;

namespace {

class NetworkingContextZeroG final : public FrameNetworkingContext {
public:
    static Ref<NetworkingContextZeroG> create() { return adoptRef(*new NetworkingContextZeroG); }

private:
    NetworkingContextZeroG()
        : FrameNetworkingContext { nullptr }
    {
    }

    bool shouldClearReferrerOnHTTPSToHTTPRedirect() const { return true; }
    CookieStorageSession* storageSession() const final { return nullptr; }
};

class FrameLoaderClientZeroG final : public LocalFrameLoaderClient {
public:
    FrameLoaderClientZeroG(FrameLoader& frameLoader, View& view)
        : LocalFrameLoaderClient(frameLoader)
        , m_frameLoader(frameLoader)
        , m_view(&view)
    {
    }

    void detach() { m_view = nullptr; }

private:
    LocalFrame& frame() const { return m_frameLoader->frame(); }
    bool isMainFrame() const { return frame().isMainFrame(); }

    Ref<DocumentLoader> createDocumentLoader(ResourceRequest&& request, SubstituteData&& substituteData, ResourceRequest&& originalRequest) final
    {
        return DocumentLoader::create(WTF::move(request), WTF::move(substituteData), WTF::move(originalRequest));
    }

    Ref<DocumentLoader> createDocumentLoader(ResourceRequest&& request, SubstituteData&& substituteData) final
    {
        return DocumentLoader::create(WTF::move(request), WTF::move(substituteData), { });
    }

    bool hasWebView() const final { return true; }
    void makeRepresentation(DocumentLoader*) final { }
    void forceLayoutForNonHTML() final { }
    void setCopiesOnScroll() final { }
    void detachedFromParent2() final { }
    void detachedFromParent3() final { }
    void convertMainResourceLoadToDownload(DocumentLoader*, const ResourceRequest&, const ResourceResponse&) final { }
    void assignIdentifierToInitialRequest(ResourceLoaderIdentifier, DocumentLoader*, const ResourceRequest&) final { }
    bool shouldUseCredentialStorage(DocumentLoader*, ResourceLoaderIdentifier) final { return true; }
    void dispatchWillSendRequest(DocumentLoader*, ResourceLoaderIdentifier, ResourceRequest&, const ResourceResponse&) final { }
    void dispatchDidReceiveAuthenticationChallenge(DocumentLoader*, ResourceLoaderIdentifier, const AuthenticationChallenge&) final { }
#if USE(PROTECTION_SPACE_AUTH_CALLBACK)
    bool canAuthenticateAgainstProtectionSpace(DocumentLoader*, ResourceLoaderIdentifier, const ProtectionSpace&) final { return false; }
#endif
    void dispatchDidReceiveResponse(DocumentLoader*, ResourceLoaderIdentifier, const ResourceResponse&) final { }
    void dispatchDidReceiveContentLength(DocumentLoader*, ResourceLoaderIdentifier, int) final { }
    void dispatchDidFinishLoading(DocumentLoader*, ResourceLoaderIdentifier) final { }
    void dispatchDidFailLoading(DocumentLoader*, ResourceLoaderIdentifier, const ResourceError&) final { }
    bool dispatchDidLoadResourceFromMemoryCache(DocumentLoader*, const ResourceRequest&, const ResourceResponse&, int) final { return false; }

    void dispatchDidDispatchOnloadEvents() final { }
    void dispatchDidReceiveServerRedirectForProvisionalLoad() final { }
    void dispatchDidCancelClientRedirect() final { }
    void dispatchWillPerformClientRedirect(const URL&, double, WallTime, LockBackForwardList) final { }
    void dispatchDidChangeLocationWithinPage() final { }
    void dispatchDidPushStateWithinPage() final { }
    void dispatchDidReplaceStateWithinPage() final { }
    void dispatchDidPopStateWithinPage() final { }
    void dispatchWillClose() final { }
    void dispatchDidStartProvisionalLoad() final { }

    void dispatchDidReceiveTitle(const StringWithDirection& title) final
    {
        if (m_view && isMainFrame())
            m_view->client().titleChanged(title.string);
    }

    void dispatchDidCommitLoad(const std::optional<BackForwardCacheCommitData>&) final { }

    void dispatchDidFailProvisionalLoad(const ResourceError& error, WillContinueLoading, WillInternallyHandleFailure) final
    {
        if (m_view && isMainFrame())
            m_view->client().loadFinished(error.failingURL(), true);
    }

    void dispatchDidFailLoad(const ResourceError& error) final
    {
        if (m_view && isMainFrame())
            m_view->client().loadFinished(error.failingURL(), true);
    }

    void dispatchDidFinishDocumentLoad() final { }

    void dispatchDidFinishLoad() final
    {
        if (m_view && isMainFrame())
            m_view->client().loadFinished(frame().document() ? frame().document()->url() : URL { }, false);
    }

    void dispatchDidReachLayoutMilestone(OptionSet<LayoutMilestone>) final { }
    void dispatchDidReachVisuallyNonEmptyState() final { }

    LocalFrame* dispatchCreatePage(const NavigationAction&, NewFrameOpenerPolicy, const String&) final { return nullptr; }
    void dispatchShow() final { }

    // Responses: what WebCore can show is shown; there are no downloads yet.
    void dispatchDecidePolicyForResponse(const ResourceResponse& response, const ResourceRequest&, const String&, FramePolicyFunction&& function) final
    {
        auto mimeType = response.mimeType();
        function(mimeType.isEmpty() || MIMETypeRegistry::canShowMIMEType(mimeType) ? PolicyAction::Use : PolicyAction::Ignore);
    }

    // New windows (target=_blank, window.open): handed to the application.
    void dispatchDecidePolicyForNewWindowAction(const NavigationAction&, const ResourceRequest& request, FormState*, const String&, std::optional<HitTestResult>&&, FramePolicyFunction&& function) final
    {
        if (m_view)
            m_view->client().openExternal(request.url());
        function(PolicyAction::Ignore);
    }

    void dispatchDecidePolicyForNavigationAction(const NavigationAction&, const ResourceRequest& request, const ResourceResponse&, FormState*, const String&, std::optional<NavigationIdentifier>,
        std::optional<HitTestResult>&&, bool, NavigationUpgradeToHTTPSBehavior, SandboxFlags, PolicyDecisionMode, FramePolicyFunction&& function) final
    {
        bool allowed = !m_view || m_view->client().allowNavigation(request.url(), isMainFrame());
        function(allowed ? PolicyAction::Use : PolicyAction::Ignore);
    }

    void updateSandboxFlags(SandboxFlags) final { }
    void updateOpener(std::optional<FrameIdentifier>) final { }
    void setPrinting(bool, FloatSize, FloatSize, float, AdjustViewSize) final { }
    void cancelPolicyCheck() final { }
    void dispatchUnableToImplementPolicy(const ResourceError&) final { }
    void dispatchWillSendSubmitEvent(Ref<FormState>&&) final { }
    void dispatchWillSubmitForm(FormState&, URL&&, String&&, CompletionHandler<void()>&& completionHandler) final { completionHandler(); }
    void revertToProvisionalState(DocumentLoader*) final { }
    void setMainDocumentError(DocumentLoader*, const ResourceError&) final { }
    void setMainFrameDocumentReady(bool) final { }
    void startDownload(const ResourceRequest&, const String&, FromDownloadAttribute) final { }
    void willChangeTitle(DocumentLoader*) final { }
    void didChangeTitle(DocumentLoader*) final { }
    void willReplaceMultipartContent() final { }
    void didReplaceMultipartContent() final { }

    // Data of the main resource goes to the document.
    void committedLoad(DocumentLoader* loader, const SharedBuffer& data) final
    {
        loader->commitData(data);
    }

    void finishedLoading(DocumentLoader*) final { }
    void loadStorageAccessQuirksIfNeeded() final { }
    bool shouldFallBack(const ResourceError&) const final { return false; }
    bool canHandleRequest(const ResourceRequest&) const final { return true; }
    bool canShowMIMEType(const String& mimeType) const final { return MIMETypeRegistry::canShowMIMEType(mimeType); }
    bool canShowMIMETypeAsHTML(const String&) const final { return true; }
    bool representationExistsForURLScheme(StringView) const final { return false; }
    String generatedMIMETypeForURLScheme(StringView) const final { return emptyString(); }
    void frameLoadCompleted() final { }
    void restoreViewState() final { }
    void provisionalLoadStarted() final { }
    void didFinishLoad() final { }
    void prepareForDataSourceReplacement() final { }
    void updateCachedDocumentLoader(DocumentLoader&) final { }
    void setTitle(const StringWithDirection&, const URL&) final { }

    String userAgent(const URL& url) const final
    {
        auto userAgent = standardUserAgentForURL(url);
        return userAgent.isEmpty() ? standardUserAgent() : userAgent;
    }

    void savePlatformDataToCachedFrame(CachedFrame*) final { }
    void transitionToCommittedFromCachedFrame(CachedFrame*) final { }

    // Each committed page of the main frame gets a new frame view at the view's
    // size, as WebKit's clients create one.
    void transitionToCommittedForNewPage(InitializingIframe) final
    {
        if (!m_view)
            return;
        Ref frame = this->frame();
        bool transparent = frame->view() && frame->view()->isTransparent();
        std::optional<Color> backgroundColor;
        if (transparent)
            backgroundColor = Color::transparentBlack;
        IntSize size = frame->view() ? frame->view()->frameRect().size() : m_view->size();
        frame->createView(size, backgroundColor, { });
        if (transparent) {
            if (RefPtr view = frame->view())
                view->setTransparent(true);
        }
    }

    void didRestoreFromBackForwardCache() final { }
    void updateGlobalHistory() final { }
    void updateGlobalHistoryRedirectLinks() final { }
    ShouldGoToHistoryItem shouldGoToHistoryItem(HistoryItem&, IsSameDocumentNavigation) const final { return ShouldGoToHistoryItem::Yes; }
    bool supportsAsyncShouldGoToHistoryItem() const final { return false; }
    void shouldGoToHistoryItemAsync(HistoryItem&, CompletionHandler<void(ShouldGoToHistoryItem)>&& completionHandler) const final { completionHandler(ShouldGoToHistoryItem::Yes); }
    void dispatchGoToBackForwardItemAtIndex(int) final { }
    void dispatchEnqueueHistoryTraversalDelta(int) final { }
    void dispatchEnqueueHistoryTraversalToItem(BackForwardFrameItemIdentifier, CompletionHandler<void(bool)>&& completionHandler) final { completionHandler(false); }
    void saveViewStateToItem(HistoryItem&) final { }
    bool canCachePage() const final { return false; }

    RefPtr<LocalFrame> createFrame(const AtomString&, HTMLFrameOwnerElement&) final { return nullptr; }
    RefPtr<Widget> createPlugin(HTMLPlugInElement&, const URL&, const Vector<AtomString>&, const Vector<AtomString>&, const String&, bool) final { return nullptr; }
    ObjectContentType objectContentType(const URL&, const String&) final { return ObjectContentType::None; }
    AtomString overrideMediaType() const final { return nullAtom(); }
    void redirectDataToPlugin(Widget&) final { }

    void dispatchDidClearWindowObjectInWorld(DOMWrapperWorld& world) final
    {
        if (m_view && world.isNormal())
            m_view->client().windowObjectCleared(frame());
    }

    Ref<FrameNetworkingContext> createNetworkingContext() final { return NetworkingContextZeroG::create(); }
    bool isEmptyFrameLoaderClient() const final { return false; }
    void prefetchDNS(const String&) final { }

    void sendH2Ping(const URL& url, CompletionHandler<void(std::expected<Seconds, ResourceError>&&)>&& completionHandler) final
    {
        completionHandler(makeUnexpected(internalError(url)));
    }

    bool hasFrameSpecificStorageAccess() final { return false; }
    void revokeFrameSpecificStorageAccess() final { }
    void dispatchLoadEventToOwnerElementInAnotherProcess() final { }
    RefPtr<HistoryItem> createHistoryItemTree(bool, BackForwardItemIdentifier) const final { return nullptr; }

    WeakRef<FrameLoader> m_frameLoader;
    View* m_view;
};

} // namespace

UniqueRef<LocalFrameLoaderClient> createFrameLoaderClient(FrameLoader& frameLoader, View& view)
{
    return makeUniqueRefWithoutRefCountedCheck<FrameLoaderClientZeroG>(frameLoader, view);
}

void detachFrameLoaderClient(LocalFrameLoaderClient& client)
{
    static_cast<FrameLoaderClientZeroG&>(client).detach();
}

} // namespace ZeroG
