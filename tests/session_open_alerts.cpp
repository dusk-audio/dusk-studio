#include <catch2/catch_test_macros.hpp>

#include "ui/LocalTimeText.h"
#include "ui/SessionOpenAlerts.h"

#include <cctype>
#include <string>

using namespace duskstudio;

TEST_CASE ("The alert for a session that won't open names the file, the reason and each offer", "[session][alerts]")
{
    openalert::WontOpen report;
    report.file = "/music/Song/session.json";
    report.problem = openalert::Problem::Damaged;
    report.autosave = true;
    report.autosaveTime = "2026-11-08 14:30";
    report.backup = true;
    report.backupTime = "2026-11-08 14:25";
    report.keepsFile = true;
    report.midiFolder = "/music/Song/midi";
    CHECK (openalert::wontOpenMessage (report)
           == "Dusk Studio could not open this session file:\n\n    /music/Song/session.json\n\n"
              "The file is damaged and does not read as a session.\n\n"
              "You can open one of these instead:\n\n"
              "    The autosave, from 2026-11-08 14:30\n"
              "    The newest backup, from 2026-11-08 14:25\n\n"
              "Opening one saves it as session.json at once. The file that would not open is kept in backups/.\n\n"
              "The session's MIDI is also kept as Standard MIDI Files in:\n\n    /music/Song/midi");

    SECTION ("a backup that would not open offers the next older one")
    {
        report.file = "/music/Song/backups/20261108-142500Z_session.json";
        report.autosave = false;
        report.autosaveTime.clear();
        report.backupTime = "2026-11-07 09:15";
        report.backupIsOlder = true;
        report.midiFolder.clear();
        CHECK (openalert::wontOpenMessage (report)
               == "Dusk Studio could not open this session file:\n\n"
                  "    /music/Song/backups/20261108-142500Z_session.json\n\n"
                  "The file is damaged and does not read as a session.\n\n"
                  "You can open one of these instead:\n\n"
                  "    The next older backup, from 2026-11-07 09:15\n\n"
                  "Opening one saves it as session.json at once. The file that would not open is kept in backups/.");
    }
}

TEST_CASE ("The alert for a session that won't open gives each reason", "[session][alerts]")
{
    openalert::WontOpen report;
    report.file = "/music/Song/session.json";
    report.backup = true;
    report.backupTime = "2026-11-08 14:25";
    const auto reasonOf = [&report] (openalert::Problem problem)
    {
        report.problem = problem;
        const auto text = openalert::wontOpenMessage (report);
        const auto start = text.find ("\n\n", text.find ("session.json")) + 2;
        return text.substr (start, text.find ("\n\n", start) - start);
    };
    CHECK (reasonOf (openalert::Problem::Missing) == "The file is not there.");
    CHECK (reasonOf (openalert::Problem::Unreadable)
           == "The file could not be read. Check that you have permission to read it.");
    CHECK (reasonOf (openalert::Problem::Damaged) == "The file is damaged and does not read as a session.");
    CHECK (reasonOf (openalert::Problem::NotMigrated)
           == "The file is from an older Dusk Studio and could not be brought up to date.");

    report.problem = openalert::Problem::Missing;
    CHECK (openalert::wontOpenMessage (report).find ("kept in backups/") == std::string::npos);
}

TEST_CASE ("The alert for a session that won't open says when there is nothing to offer", "[session][alerts]")
{
    openalert::WontOpen report;
    report.file = "/music/Song/session.json";
    report.problem = openalert::Problem::Missing;
    report.midiFolder = "/music/Song/midi";
    CHECK (openalert::wontOpenMessage (report)
           == "Dusk Studio could not open this session file:\n\n    /music/Song/session.json\n\n"
              "The file is not there.\n\n"
              "There is no autosave or backup to open instead.\n\n"
              "The session's MIDI is also kept as Standard MIDI Files in:\n\n    /music/Song/midi");
}

TEST_CASE ("The alert for a session that won't open leaves out a time it does not know, and warns of the autosave",
           "[session][alerts]")
{
    openalert::WontOpen report;
    report.file = "/music/Song/session.json.autosave";
    report.problem = openalert::Problem::Damaged;
    report.backup = true;
    report.discardsAutosave = true;
    CHECK (openalert::wontOpenMessage (report)
           == "Dusk Studio could not open this session file:\n\n    /music/Song/session.json.autosave\n\n"
              "The file is damaged and does not read as a session.\n\n"
              "You can open one of these instead:\n\n"
              "    The newest backup\n\n"
              "Opening one saves it as session.json at once. Opening the backup discards the autosave.");

    report.backup = false;
    report.autosave = true;
    CHECK (openalert::wontOpenMessage (report).find ("discards the autosave") == std::string::npos);
    CHECK (openalert::wontOpenMessage (report).find ("    The autosave\n\n") != std::string::npos);
}

TEST_CASE ("The alert for a session from a newer Dusk Studio names both formats", "[session][alerts]")
{
    CHECK (std::string (openalert::kNewerVersionTitle) == "Session needs a newer Dusk Studio");
    CHECK (openalert::newerVersionMessage ("/music/Song/session.json", 12, 11)
           == "This session was saved by a newer version of Dusk Studio:\n\n    /music/Song/session.json\n\n"
              "It is in session format 12, and this version reads formats up to 11. "
              "Upgrade Dusk Studio to open it. Nothing was changed.");
}

TEST_CASE ("A time in an alert reads as the local date and minute", "[session][alerts]")
{
    const auto text = localMinuteText (1794148205);
    REQUIRE (text.size() == 16);
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        CAPTURE (text, i);
        if (i == 4 || i == 7) CHECK (text[i] == '-');
        else if (i == 10) CHECK (text[i] == ' ');
        else if (i == 13) CHECK (text[i] == ':');
        else CHECK (std::isdigit (static_cast<unsigned char> (text[i])));
    }
}
