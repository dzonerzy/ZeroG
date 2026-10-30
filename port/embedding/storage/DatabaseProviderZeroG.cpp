#include "config.h"
#include "DatabaseProviderZeroG.h"

#include "InProcessIDBServer.h"
#include <pal/SessionID.h>

namespace ZeroG {

DatabaseProviderZeroG::DatabaseProviderZeroG(const String& directory)
    : m_directory(directory.isolatedCopy())
{
}

DatabaseProviderZeroG::~DatabaseProviderZeroG() = default;

WebCore::IDBClient::IDBConnectionToServer& DatabaseProviderZeroG::idbConnectionToServerForSession(PAL::SessionID sessionID)
{
    // One server per provider: a session has one ID. An empty directory makes the
    // IDB server keep its databases in memory.
    if (!m_server)
        m_server = InProcessIDBServer::create(sessionID, m_directory);
    return m_server->connectionToServer();
}

} // namespace ZeroG
