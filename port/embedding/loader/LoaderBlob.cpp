// Blobs (Blob, File, URL.createObjectURL, FileReader, fetch of blob: URLs) in
// the engine process. WebKit keeps blob data in its network process's
// BlobRegistryImpl and serves blob: URLs from it (NetworkDataTaskBlob, which
// this follows); here the registry is the process's own, and a load task reads
// a blob with WebCore's BlobResourceHandleBase (ranges, files, slices).

#include "config.h"
#include "Loader.h"

#include <WebCore/BlobData.h>
#include <WebCore/BlobDataFileReference.h>
#include <WebCore/BlobRegistry.h>
#include <WebCore/BlobRegistryImpl.h>
#include <WebCore/BlobResourceHandleBase.h>
#include <WebCore/Document.h>
#include <WebCore/HTTPHeaderNames.h>
#include <WebCore/HTTPParsers.h>
#include <WebCore/NetworkLoadMetrics.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/PolicyContainer.h>
#include <WebCore/ResourceLoader.h>
#include <WebCore/SecurityOrigin.h>
#include <WebCore/SecurityOriginData.h>
#include <wtf/FileSystem.h>
#include <wtf/NeverDestroyed.h>

namespace ZeroG {

using namespace WebCore;

static constexpr auto blobErrorDomain = "WebKitBlobResource"_s;

static BlobRegistryImpl& registry()
{
    static NeverDestroyed<BlobRegistryImpl> registry;
    return registry;
}

// Blob URLs are partitioned by the top document's origin, as in WebKit.
static std::optional<SecurityOriginData> topOrigin(LocalFrame* frame)
{
    if (!frame)
        return std::nullopt;
    RefPtr document = frame->document();
    if (!document)
        return std::nullopt;
    return document->topOrigin().data();
}

namespace {

class BlobRegistryZeroG final : public BlobRegistry {
    void registerInternalFileBlobURL(const URL& url, Ref<BlobDataFileReference>&& file, const String&, const String& contentType) final { registry().registerInternalFileBlobURL(url, WTF::move(file), contentType); }
    void registerInternalBlobURL(const URL& url, Vector<BlobPart>&& parts, const String& contentType) final { registry().registerInternalBlobURL(url, WTF::move(parts), contentType); }
    void registerBlobURL(const URL& url, const URL& sourceURL, const PolicyContainer& policyContainer, const std::optional<SecurityOriginData>& topOrigin) final { registry().registerBlobURL(url, sourceURL, policyContainer, topOrigin); }
    void registerInternalBlobURLOptionallyFileBacked(const URL& url, const URL& sourceURL, RefPtr<BlobDataFileReference>&& file, const String& contentType) final { registry().registerInternalBlobURLOptionallyFileBacked(url, sourceURL, WTF::move(file), contentType, { }); }
    void registerInternalBlobURLForSlice(const URL& url, const URL& sourceURL, long long start, long long end, const String& contentType) final { registry().registerInternalBlobURLForSlice(url, sourceURL, start, end, contentType); }
    void unregisterBlobURL(const URL& url, const std::optional<SecurityOriginData>& topOrigin) final { registry().unregisterBlobURL(url, topOrigin); }
    void registerBlobURLHandle(const URL& url, const std::optional<SecurityOriginData>& topOrigin) final { registry().registerBlobURLHandle(url, topOrigin); }
    void unregisterBlobURLHandle(const URL& url, const std::optional<SecurityOriginData>& topOrigin) final { registry().unregisterBlobURLHandle(url, topOrigin); }
    String blobType(const URL& url) final { return registry().blobType(url); }
    unsigned long long blobSize(const URL& url) final { return registry().blobSize(url); }
    void writeBlobsToTemporaryFilesForIndexedDB(const Vector<String>& urls, CompletionHandler<void(Vector<String>&&)>&& completionHandler) final { registry().writeBlobsToTemporaryFilesForIndexedDB(urls, WTF::move(completionHandler)); }
    BlobRegistryImpl* blobRegistryImpl() final { return &registry(); }
};

class BlobTask final : public LoadTask, public BlobResourceHandleBase {
public:
    static Ref<BlobTask> create(LoadTarget&& target, const ResourceRequest& request, RefPtr<BlobData>&& data)
    {
        return adoptRef(*new BlobTask(WTF::move(target), request, WTF::move(data)));
    }

    void ref() const final { LoadTask::ref(); }
    void deref() const final { LoadTask::deref(); }

private:
    BlobTask(LoadTarget&& target, const ResourceRequest& request, RefPtr<BlobData>&& data)
        : BlobResourceHandleBase(true, WTF::move(data))
        , m_target(WTF::move(target))
        , m_request(request)
    {
    }

    ~BlobTask()
    {
        clearStream();
    }

    // ---- LoadTask ----

    void start() final
    {
        if (!m_target.isActive())
            return;
        BlobResourceHandleBase::start();
    }

    void stop() final
    {
        m_target.clear();
        clearStream();
    }

    bool convertToDownload(Download& download) final
    {
        if (!m_target.isActive() || m_completed)
            return false;
        m_target.convertToDownload(download);
        // The page's response policy will not complete for it any more.
        if (std::exchange(m_waitingForResponsePolicy, false))
            readBody();
        return true;
    }

    // ---- BlobResourceHandleBase ----

    const ResourceRequest& firstRequest() const final { return m_request; }
    bool erroredOrAborted() const final { return m_completed || !const_cast<LoadTarget&>(m_target).isActive(); }

    void clearStream() final
    {
        if (m_completed)
            return;
        m_completed = true;
        closeFileIfOpen();
        clearAsyncStream();
    }

    void didReceiveResponse(ResourceResponse&& response) final
    {
        if (!m_target.isActive())
            return;
        m_waitingForResponsePolicy = true;
        m_target.didReceiveResponse(WTF::move(response), [this, protectedThis = Ref { *this }](bool accepted) {
            if (!std::exchange(m_waitingForResponsePolicy, false))
                return;
            if (!accepted) {
                stop();
                return;
            }
            readBody();
        });
    }

    void readBody()
    {
        if (!m_target.isActive() || m_completed)
            return;
        static constexpr size_t bufferSize = 512 * 1024;
        resizeBuffer(bufferSize);
        readAsync();
    }

    bool didReceiveData(std::span<const uint8_t> data) final
    {
        if (!m_target.isActive())
            return false;
        m_target.didReceiveData(SharedBuffer::create(data));
        return true;
    }

    void didFinish() final
    {
        Ref protectedThis { *this };
        clearStream();
        m_target.didFinish(NetworkLoadMetrics { });
    }

    void didFail(Error error) final
    {
        Ref protectedThis { *this };
        clearStream();
        m_target.didFail(ResourceError { blobErrorDomain, static_cast<int>(error), m_request.url(), { } });
    }

    LoadTarget m_target;
    ResourceRequest m_request;
    bool m_completed { false };
    bool m_waitingForResponsePolicy { false };
};

} // anonymous namespace

BlobRegistry* createBlobRegistry()
{
    return new BlobRegistryZeroG;
}

Ref<LoadTask> createBlobTask(ResourceLoader& loader)
{
    return BlobTask::create(LoadTarget { loader }, loader.request(), registry().blobDataFromURL(loader.request().url(), topOrigin(loader.frame())));
}

Ref<LoadTask> createBlobTask(Download& download, const ResourceRequest& request, LocalFrame* frame)
{
    return BlobTask::create(LoadTarget { download }, request, registry().blobDataFromURL(request.url(), topOrigin(frame)));
}

// Synchronous loads (synchronous XMLHttpRequest, FileReaderSync): the whole
// blob, read in place.
bool loadBlobSynchronously(LocalFrame* frame, const ResourceRequest& request, ResourceError& error, ResourceResponse& response, Vector<uint8_t>& data)
{
    auto fail = [&](BlobResourceHandleBase::Error code, ASCIILiteral description) {
        error = ResourceError { blobErrorDomain, static_cast<int>(code), request.url(), description };
        return false;
    };
    if (!equalLettersIgnoringASCIICase(request.httpMethod(), "get"_s))
        return fail(BlobResourceHandleBase::Error::MethodNotAllowed, "Request method must be GET"_s);
    RefPtr blob = registry().blobDataFromURL(request.url(), topOrigin(frame));
    if (!blob)
        return fail(BlobResourceHandleBase::Error::NotFoundError, "Blob not found"_s);

    Vector<uint8_t> bytes;
    for (auto& item : blob->items()) {
        bool readable = item.switchOn([&](const DataSegment& segment) {
            auto span = segment.span();
            auto offset = std::min<uint64_t>(item.offset(), span.size());
            bytes.append(span.subspan(offset, std::min<uint64_t>(item.length(), span.size() - offset)));
            return true;
        }, [&](const BlobDataFileReference& file) {
            auto contents = FileSystem::readEntireFile(const_cast<BlobDataFileReference&>(file).path());
            if (!contents)
                return false;
            auto offset = std::min<uint64_t>(item.offset(), contents->size());
            auto length = item.length() == BlobDataItem::toEndOfFile ? contents->size() - offset : std::min<uint64_t>(item.length(), contents->size() - offset);
            bytes.append(contents->span().subspan(offset, length));
            return true;
        });
        if (!readable)
            return fail(BlobResourceHandleBase::Error::NotReadableError, "A file of the blob could not be read"_s);
    }

    response = ResourceResponse { URL { request.url() }, extractMIMETypeFromMediaType(blob->contentType()), static_cast<long long>(bytes.size()), extractCharsetFromMediaType(blob->contentType()).toString() };
    response.setHTTPStatusCode(200);
    response.setHTTPStatusText("OK"_s);
    response.setHTTPHeaderField(HTTPHeaderName::ContentType, blob->contentType());
    response.setHTTPHeaderField(HTTPHeaderName::ContentLength, String::number(bytes.size()));
    data = WTF::move(bytes);
    return true;
}

} // namespace ZeroG
