#pragma once

#if defined(__linux__) || defined(__APPLE__)

#include <cerrno>
#include <string>
#include <vector>

namespace duskstudio::test
{
// Faults for the calls an atomic write publishes and syncs with, on the thread
// that makes one, for as long as it lives: foundation_atomic_file.cpp wraps
// them (with --wrap on Linux, by interposition on macOS). An error left at 0
// is no fault. Folder syncs are wrapped on Linux only.
struct PublicationFaults
{
    PublicationFaults();
    ~PublicationFaults();
    PublicationFaults (const PublicationFaults&) = delete;
    PublicationFaults& operator= (const PublicationFaults&) = delete;

    // Answers to a no-replace rename, a hard link, an unlink and a folder sync.
    int renameError = 0, linkError = 0, unlinkError = 0, syncError = 0;
    // How many folder syncs fail with syncError before they succeed again;
    // negative for all of them.
    int syncFailures = -1;
    int links = 0, unlinks = 0, folderSyncs = 0;
    // A file of someone else's appears at the destination just before a link.
    bool raceTarget = false;
    // Every folder a sync was asked of, failed or not.
    std::vector<std::string> syncedFolders;
};
} // namespace duskstudio::test

#endif
