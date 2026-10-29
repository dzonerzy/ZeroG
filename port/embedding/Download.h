#pragma once

// Downloads: a page's load that ends in a file instead of a document. A
// navigation whose response is an attachment or cannot be shown continues its
// transfer into the file (the load is converted); a <a download> link, or a
// data: URL, starts a new transfer. The view's client decides, when the
// response arrives, where the file goes; without a destination the download is
// refused. A download ends with its view.

#include <WebCore/ResourceError.h>
#include <WebCore/ResourceResponse.h>
#include <wtf/FileHandle.h>
#include <wtf/RefCounted.h>
#include <wtf/URL.h>
#include <wtf/text/WTFString.h>

namespace ZeroG {

class LoadTask;
class View;

class Download : public RefCounted<Download> {
public:
    static Ref<Download> create(View& view, const URL& url, const String& suggestedFilename = { })
    {
        return adoptRef(*new Download(view, url, suggestedFilename));
    }
    ~Download();

    const URL& url() const LIFETIME_BOUND { return m_url; }
    const String& suggestedFilename() const LIFETIME_BOUND { return m_suggestedFilename; }
    const String& mimeType() const LIFETIME_BOUND { return m_mimeType; }
    long long expectedLength() const { return m_expectedLength; } // -1: unknown
    uint64_t receivedLength() const { return m_receivedLength; }
    const String& destination() const LIFETIME_BOUND { return m_destination; }

    // During ViewClient::downloadStarted: where the file goes. Without it, the
    // download is refused.
    void setDestination(const String& path) { m_destination = path; }

    // The application or the view stops it; the partial file is removed.
    void cancel();

    // From the load: the response (false: refused, the load must stop), the
    // body, and the end.
    bool start(const WebCore::ResourceResponse&);
    void didReceiveData(std::span<const uint8_t>);
    void didFinish();
    void didFail(const WebCore::ResourceError&);

    void setTask(LoadTask*);
    void detachFromView();

private:
    Download(View&, const URL&, const String& suggestedFilename);
    void finish(const WebCore::ResourceError&);

    View* m_view;
    URL m_url;
    String m_suggestedFilename;
    String m_mimeType;
    long long m_expectedLength { -1 };
    uint64_t m_receivedLength { 0 };
    String m_destination;
    FileSystem::FileHandle m_file;
    RefPtr<LoadTask> m_task;
    bool m_started { false };
    bool m_finished { false };
};

} // namespace ZeroG
