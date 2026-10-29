#include "config.h"
#include "Download.h"

#include "Loader.h"
#include "View.h"
#include <wtf/FileSystem.h>
#include <wtf/text/MakeString.h>

namespace ZeroG {

using namespace WebCore;

Download::Download(View& view, const URL& url, const String& suggestedFilename)
    : m_view(&view)
    , m_url(url)
    , m_suggestedFilename(suggestedFilename)
{
    view.addDownload(*this);
}

Download::~Download() = default;

void Download::setTask(LoadTask* task)
{
    m_task = task;
}

bool Download::start(const ResourceResponse& response)
{
    if (m_started || m_finished)
        return false;
    m_started = true;
    m_mimeType = response.mimeType();
    m_expectedLength = response.expectedContentLength() > 0 ? response.expectedContentLength() : -1;
    // The download attribute's name, else Content-Disposition's, else the URL's.
    if (m_suggestedFilename.isEmpty())
        m_suggestedFilename = response.suggestedFilename();
    if (m_suggestedFilename.isEmpty())
        m_suggestedFilename = m_url.lastPathComponent().toString();
    if (m_suggestedFilename.isEmpty())
        m_suggestedFilename = "download"_s;

    if (m_view)
        m_view->client().downloadStarted(*this);
    if (m_destination.isEmpty()) {
        finish(cancelledError(m_url));
        return false;
    }
    m_file = FileSystem::openFile(m_destination, FileSystem::FileOpenMode::Truncate);
    if (!m_file) {
        finish(failedError(m_url, makeString("Cannot write "_s, m_destination)));
        return false;
    }
    return true;
}

void Download::didReceiveData(std::span<const uint8_t> data)
{
    if (m_finished || !m_file)
        return;
    if (m_file.write(data) != data.size()) {
        Ref protectedThis { *this };
        if (RefPtr task = std::exchange(m_task, nullptr))
            task->stop();
        finish(failedError(m_url, makeString("Cannot write "_s, m_destination)));
        return;
    }
    m_receivedLength += data.size();
    if (m_view)
        m_view->client().downloadProgress(*this);
}

void Download::didFinish()
{
    finish({ });
}

void Download::didFail(const ResourceError& error)
{
    finish(error.isNull() ? failedError(m_url, "The download failed"_s) : error);
}

void Download::cancel()
{
    if (m_finished)
        return;
    Ref protectedThis { *this };
    if (RefPtr task = std::exchange(m_task, nullptr))
        task->stop();
    finish(cancelledError(m_url));
}

void Download::detachFromView()
{
    Ref protectedThis { *this };
    m_view = nullptr;
    cancel();
}

void Download::finish(const ResourceError& error)
{
    if (m_finished)
        return;
    Ref protectedThis { *this };
    m_finished = true;
    m_task = nullptr;
    bool wasWriting = !!m_file;
    m_file = { };
    // Nothing half-written is left behind.
    if (!error.isNull() && wasWriting)
        FileSystem::deleteFile(m_destination);
    // A refused download (no destination) is not reported further.
    if (m_view) {
        if (!m_destination.isEmpty())
            m_view->client().downloadFinished(*this, error);
        m_view->removeDownload(*this);
    }
}

} // namespace ZeroG
