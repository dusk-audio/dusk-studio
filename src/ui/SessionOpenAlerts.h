#pragma once

#include <string>

// The alerts a session that will not open raises, built here so the manual can
// quote them and a test can pin them.
namespace duskstudio::openalert
{
inline constexpr const char* kWontOpenTitle     = "Session won't open";
inline constexpr const char* kOpenAutosave      = "Open Autosave";
inline constexpr const char* kOpenNewestBackup  = "Open Newest Backup";
inline constexpr const char* kOpenOlderBackup   = "Open Older Backup";
inline constexpr const char* kNewerVersionTitle = "Session needs a newer Dusk Studio";

enum class Problem
{
    Missing,
    Unreadable,
    Damaged,
    NotMigrated,
};

struct WontOpen
{
    std::string file;
    Problem     problem = Problem::Damaged;
    bool        autosave = false;           // the autosave is offered
    std::string autosaveTime;               // empty when its time is unknown
    bool        backup = false;             // a backup is offered
    std::string backupTime;                 // empty when its time is unknown
    bool        backupIsOlder = false;      // a backup has failed, and the one offered is older than it
    bool        discardsAutosave = false;   // opening the backup removes the session's autosave
    bool        keepsFile = false;          // opening an offer keeps the file in backups/
    std::string midiFolder;                 // empty: the session has no midi folder
};

inline std::string wontOpenMessage (const WontOpen& report)
{
    std::string text = "Dusk Studio could not open this session file:\n\n    " + report.file + "\n\n";
    switch (report.problem)
    {
        case Problem::Missing:     text += "The file is not there."; break;
        case Problem::Unreadable:  text += "The file could not be read. Check that you have permission to read it."; break;
        case Problem::Damaged:     text += "The file is damaged and does not read as a session."; break;
        case Problem::NotMigrated: text += "The file is from an older Dusk Studio and could not be brought up to date."; break;
    }

    const auto from = [] (const std::string& time) { return time.empty() ? time : ", from " + time; };
    if (! report.autosave && ! report.backup)
        text += "\n\nThere is no autosave or backup to open instead.";
    else
    {
        text += "\n\nYou can open one of these instead:\n";
        if (report.autosave)
            text += "\n    The autosave" + from (report.autosaveTime);
        if (report.backup)
            text += std::string (report.backupIsOlder ? "\n    The next older backup" : "\n    The newest backup")
                  + from (report.backupTime);
        text += "\n\nOpening one saves it as session.json at once.";
        if (report.backup && report.discardsAutosave) text += " Opening the backup discards the autosave.";
        if (report.keepsFile) text += " The file that would not open is kept in backups/.";
    }

    if (! report.midiFolder.empty())
        text += "\n\nThe session's MIDI is also kept as Standard MIDI Files in:\n\n    " + report.midiFolder;
    return text;
}

inline const char* backupButton (const WontOpen& report)
{
    return report.backupIsOlder ? kOpenOlderBackup : kOpenNewestBackup;
}

inline std::string newerVersionMessage (const std::string& file, int fileVersion, int supportedVersion)
{
    return "This session was saved by a newer version of Dusk Studio:\n\n    " + file + "\n\n"
           "It is in session format " + std::to_string (fileVersion) + ", and this version reads formats up to "
           + std::to_string (supportedVersion) + ". Upgrade Dusk Studio to open it. Nothing was changed.";
}
} // namespace duskstudio::openalert
