#pragma once

// WebKitLegacy's StorageTracker keeps one process-wide index of the origins that
// have local storage, at one path, to list and delete them. ZeroG has a data
// directory per context and no API for managing origins, so the storage areas
// (copied from WebKitLegacy, with the calls below) get an inactive tracker: what
// an uninitialized upstream tracker does. The databases themselves are the same.

#include <wtf/Seconds.h>
#include <wtf/text/WTFString.h>

namespace WebKit {

class StorageTracker {
public:
    static StorageTracker& tracker()
    {
        static StorageTracker tracker;
        return tracker;
    }

    void setOriginDetails(const String&, const String&) { }
    void cancelDeletingOrigin(const String&) { }
    void deleteOriginWithIdentifier(const String&) { }
    bool isActive() const { return false; }
    // As upstream: a database is closed after five idle minutes.
    Seconds storageDatabaseIdleInterval() const { return 300_s; }
};

} // namespace WebKit
