#pragma once

// IndexedDB for a session (zg_context): an in-process IDB server (WebKitLegacy's
// InProcessIDBServer, on its own work queue) whose databases live under the
// session's directory, or in memory when it has none (an ephemeral context).

#include <WebCore/DatabaseProvider.h>
#include <wtf/text/WTFString.h>

class InProcessIDBServer;

namespace ZeroG {

class DatabaseProviderZeroG final : public WebCore::DatabaseProvider {
public:
    static Ref<DatabaseProviderZeroG> create(const String& directory) { return adoptRef(*new DatabaseProviderZeroG(directory)); }
    ~DatabaseProviderZeroG();

    WebCore::IDBClient::IDBConnectionToServer& idbConnectionToServerForSession(PAL::SessionID) final;

private:
    explicit DatabaseProviderZeroG(const String& directory);

    const String m_directory;
    RefPtr<InProcessIDBServer> m_server;
};

} // namespace ZeroG
