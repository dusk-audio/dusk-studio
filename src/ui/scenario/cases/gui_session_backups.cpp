#include "../GuiScenarioSupport.h"
#include "../../LocalTimeText.h"
#include "../../SessionOpenAlerts.h"
#include "../../../session/SessionBackups.h"
#include "../../../session/SessionSerializer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace duskstudio::scenario::gui
{
namespace
{
namespace fs = std::filesystem;

std::vector<std::string> backupsIn (const fs::path& sessionDir)
{
    std::vector<std::string> names;
    std::error_code error;
    for (fs::directory_iterator it (sessionDir / "backups", error), end; ! error && it != end; it.increment (error))
        names.push_back (it->path().filename().u8string());
    std::sort (names.begin(), names.end());
    return names;
}

bool writeText (const fs::path& file, const std::string& text)
{
    std::error_code error;
    fs::create_directories (file.parent_path(), error);
    std::ofstream out (file, std::ios::binary);
    out << text;
    return static_cast<bool> (out);
}

bool absent (const fs::path& path)
{
    std::error_code error;
    return fs::symlink_status (path, error).type() == fs::file_type::not_found;
}

// A session of the folder with track 1's fader at faderDb, saved to target.
bool saveFixture (const fs::path& sessionDir, const fs::path& target, float faderDb,
                  const fs::path& audio = {})
{
    auto fixture = std::make_unique<Session>();
    applySessionDirectory (*fixture, sessionDir);
    fixture->track (0).strip.faderDb.store (faderDb);
    if (! audio.empty())
    {
        AudioRegion region;
        region.lengthInSamples = 4800;
        region.file = decltype (region.file) (audio.u8string().c_str());
        fixture->track (1).regions = { region };
    }
    std::error_code error;
    fs::create_directories (target.parent_path(), error);
    return SessionSerializer::save (*fixture, target);
}

bool faderIs (const fs::path& sessionJson, float faderDb)
{
    return std::abs (savedFaderOf (sessionJson) - faderDb) < 1.0e-4f;
}

std::string savedTimeOf (const fs::path& backup)
{
    const auto savedAt = sessionbackups::savedAtOf (backup);
    return savedAt ? localMinuteText (std::chrono::system_clock::to_time_t (*savedAt)) : std::string();
}

bool anyBackupHolds (const fs::path& sessionDir, const std::string& bytes)
{
    for (const auto& name : backupsIn (sessionDir))
        if (fileBytes (sessionDir / "backups" / name) == bytes) return true;
    return false;
}

std::string alertText (const char* title, const std::string& message)
{
    return std::string (title) + "\n" + message;
}

// Every case here opens what it made, so it registers the same way back.
std::optional<ScenarioResult> beginBackupCase (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    if (! ctx.engine().getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, originalDir, restore]
    {
        host.closeStartupDialog();
        drainModals (host);
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
    });
    reopenSavedSession (host, restore);
    return std::nullopt;
}

// Each Save keeps a copy of what it wrote in backups/, and the session.json it
// replaces when no backup holds it. An autosave keeps none. Save As takes
// backups/ along.
std::optional<ScenarioResult> runSavesKeepSessionBackups (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginBackupCase (host, ctx)) return early;
    auto& session = ctx.session();
    const auto folder = ctx.tempDir() / "Backed up";
    const auto sessionJson = folder / "session.json";
    if (! saveFixture (folder, sessionJson, -6.0f)) return ScenarioResult::fail ("could not write the session");
    const auto opened = fileBytes (sessionJson);
    reopenSavedSession (host, sessionJson);
    if (currentSessionDirectory (session) != folder) return ScenarioResult::fail ("could not open the session");
    if (! ctx.expect (backupsIn (folder).empty(), "opening the session wrote a backup")) return ctx.verdict();

    auto& fader = session.track (0).strip.faderDb;
    const auto kept = std::make_shared<std::vector<std::string>>();
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&fader] { fader.store (-3.0f); } });
    pushFileMenuSteps (host, ctx, *steps, "Save");
    steps->push_back ({ 500, [&host, &ctx, folder, sessionJson, opened, kept]
    {
        ctx.expect (host.statusMessage() == "Saved: Backed up", "Save reported '" + host.statusMessage() + "'");
        *kept = backupsIn (folder);
        if (! ctx.expect (kept->size() == 2, "the first Save left " + std::to_string (kept->size())
                                                 + " backups, not the session.json it replaced and the one it wrote"))
            return;
        ctx.expect (fileBytes (folder / "backups" / kept->front()) == opened,
                    "the first backup is not the session.json the Save replaced");
        ctx.expect (fileBytes (folder / "backups" / kept->back()) == fileBytes (sessionJson),
                    "the newest backup is not the session.json the Save wrote");
    } });
    pushFileMenuSteps (host, ctx, *steps, "Save");
    steps->push_back ({ 500, [&ctx, &fader, folder, kept]
    {
        ctx.expect (backupsIn (folder) == *kept, "a Save with nothing changed kept another backup");
        fader.store (-1.0f);
    } });
    pushFileMenuSteps (host, ctx, *steps, "Save");
    steps->push_back ({ 500, [&host, &ctx, &fader, folder, sessionJson, kept]
    {
        const auto names = backupsIn (folder);
        if (! ctx.expect (names.size() == 3 && std::equal (kept->begin(), kept->end(), names.begin()),
                          "a Save after a change left " + std::to_string (names.size())
                              + " backups, not the two before it and one more"))
            return;
        ctx.expect (fileBytes (folder / "backups" / names.back()) == fileBytes (sessionJson),
                    "the newest backup is not the session.json the second Save wrote");
        ctx.expect (faderIs (folder / "backups" / names.back(), -1.0f), "the newest backup does not hold the change");
        *kept = names;
        fader.store (-2.0f);
        host.autosaveTick();
        ctx.expect (! absent (folder / "session.json.autosave"), "the autosave tick wrote no autosave");
        ctx.expect (backupsIn (folder) == names, "an autosave kept a backup");
    } });

    const auto copy = ctx.tempDir() / "Backed up copy";
    pushSaveAsSteps (host, ctx, *steps, copy);
    steps->push_back ({ 500, [&ctx, &session, folder, copy, kept]
    {
        ctx.expect (currentSessionDirectory (session) == copy,
                    "Save As left the session at '" + currentSessionDirectory (session).string() + "'");
        const auto copied = backupsIn (copy);
        if (! ctx.expect (copied.size() == kept->size() + 1 && std::equal (kept->begin(), kept->end(), copied.begin()),
                          "Save As left " + std::to_string (copied.size()) + " backups in the new folder, not the "
                              + std::to_string (kept->size()) + " it took along and the one it wrote"))
            return;
        for (const auto& name : *kept)
            ctx.expect (fileBytes (copy / "backups" / name) == fileBytes (folder / "backups" / name),
                        "Save As changed the backup " + name);
        ctx.expect (fileBytes (copy / "backups" / copied.back()) == fileBytes (copy / "session.json"),
                    "the new folder's newest backup is not its session.json");
        ctx.expect (backupsIn (folder) == *kept, "Save As changed the old folder's backups");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar savesKeepSessionBackups { Scenario {
    "gui.saves_keep_session_backups", { "gui", "session", "autosave" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runSavesKeepSessionBackups (host, ctx); }
} };

// A session.json that does not read raises an alert that offers the newest
// backup. Opening it saves it as session.json and keeps the damaged file in
// backups/; a backup that does not read either offers the next older one.
std::optional<ScenarioResult> runDamagedSessionOpensBackup (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginBackupCase (host, ctx)) return early;
    auto& session = ctx.session();
    const auto before = currentSessionDirectory (session);
    const auto folder = ctx.tempDir() / "Damaged";
    const auto sessionJson = folder / "session.json";
    const auto good = folder / "backups" / "20261101-090000Z_session.json";
    const auto broken = folder / "backups" / "20261102-090000Z_session.json";
    if (! saveFixture (folder, good, -9.0f) || ! writeText (broken, "{ also damaged")
        || ! writeText (sessionJson, "{ damaged"))
        return ScenarioResult::fail ("could not write the damaged session");
    std::error_code error;
    fs::create_directories (folder / "midi" / "tracks", error);

    openalert::WontOpen report;
    report.file = sessionJson.u8string();
    report.problem = openalert::Problem::Damaged;
    report.backup = true;
    report.backupTime = savedTimeOf (broken);
    report.keepsFile = true;
    report.midiFolder = (folder / "midi").u8string();
    ctx.expect (! host.openSession (sessionJson), "the damaged session.json reported a load");
    ctx.expect (currentSessionDirectory (session) == before, "the damaged session.json switched the session");
    ctx.expect (host.modalText() == alertText (openalert::kWontOpenTitle, openalert::wontOpenMessage (report)),
                "the alert read '" + host.modalText() + "'");
    ctx.expect (! host.clickModalButton (openalert::kOpenAutosave), "the alert offered an autosave there is not");

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx]
    {
        ctx.expect (host.clickModalButton (openalert::kOpenNewestBackup), "the alert did not offer the newest backup");
    } });
    steps->push_back ({ 0, [&host, &ctx, broken, good, report]
    {
        auto older = report;
        older.file = broken.u8string();
        older.backupTime = savedTimeOf (good);
        older.backupIsOlder = true;
        ctx.expect (host.modalText() == alertText (openalert::kWontOpenTitle, openalert::wontOpenMessage (older)),
                    "the damaged backup's alert read '" + host.modalText() + "'");
        ctx.expect (host.clickModalButton (openalert::kOpenNewestBackup), "the alert did not offer the older backup");
    },
    [&host, broken] { return host.modalText().find (broken.u8string()) != std::string::npos; },
    "opening the damaged backup raised no alert naming it" });
    steps->push_back ({ 0, [&host, &ctx, &session, folder, sessionJson, good, broken]
    {
        ctx.expect (currentSessionDirectory (session) == folder, "the older backup did not open in the session folder");
        ctx.expect (host.statusMessage() == "Saved: Damaged",
                    "opening the backup reported '" + host.statusMessage() + "' rather than saving it");
        ctx.expect (faderIs (sessionJson, -9.0f), "session.json does not hold the backup that opened");
        ctx.expect (anyBackupHolds (folder, "{ damaged"), "the session.json that would not open is not in backups/");
        ctx.expect (fileBytes (broken) == "{ also damaged" && faderIs (good, -9.0f), "opening a backup changed a backup");
        const auto names = backupsIn (folder);
        ctx.expect (! names.empty() && fileBytes (folder / "backups" / names.back()) == fileBytes (sessionJson),
                    "the newest backup is not the session.json the open saved");
        ctx.expect (absent (folder / "backups" / "session.json"), "the open saved a session.json into backups/");
    },
    [&host, &session, folder] { return host.modalStackEmpty() && currentSessionDirectory (session) == folder; },
    "the older backup did not open" });

    // Load saved session in the recovery prompt, on a damaged session.json,
    // raises the alert, which offers the autosave too.
    const auto autosave = folder / "session.json.autosave";
    steps->push_back ({ 100, [&host, &ctx, sessionJson, autosave]
    {
        if (! ctx.expect (writeText (sessionJson, "{ damaged again")
                              && saveFixture (sessionJson.parent_path(), autosave, -7.0f),
                          "could not damage the session again"))
            return;
        host.openSession (sessionJson);
        ctx.expect (host.modalText().rfind ("Recover from autosave?", 0) == 0,
                    "a damaged session.json with an autosave raised '" + host.modalText() + "', not the recovery prompt");
        ctx.expect (host.answerRecovery (GuiHost::Recovery::LoadSaved), "the recovery prompt did not take Load saved session");
    } });
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (host.modalText().find ("\n    The autosave, from ") != std::string::npos,
                    "the alert did not offer the autosave: '" + host.modalText() + "'");
        ctx.expect (host.modalText().find ("\n    The newest backup, from ") != std::string::npos,
                    "the alert did not offer the newest backup: '" + host.modalText() + "'");
        ctx.expect (host.clickModalButton (openalert::kOpenAutosave), "the alert did not offer Open Autosave");
    },
    [&host] { return host.modalText().rfind (openalert::kWontOpenTitle, 0) == 0; },
    "Load saved session on a damaged session.json raised no alert" });
    steps->push_back ({ 0, [&ctx, folder, autosave]
    {
        ctx.expect (absent (autosave), "the autosave is still there once it was saved as session.json");
        ctx.expect (anyBackupHolds (folder, "{ damaged again"), "the session.json that would not open is not in backups/");
    },
    [&host, sessionJson] { return host.modalStackEmpty() && faderIs (sessionJson, -7.0f); },
    "Open Autosave did not save the autosave as session.json" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar damagedSessionOpensBackup { Scenario {
    "gui.damaged_session_opens_backup", { "gui", "session" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runDamagedSessionOpensBackup (host, ctx); }
} };

// File > Open of a backup opens the session it belongs to, from its own
// folder, so its audio is found, and saves it as session.json at once.
std::optional<ScenarioResult> runOpenBackupFromFileMenu (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginBackupCase (host, ctx)) return early;
    auto& session = ctx.session();
    const auto folder = ctx.tempDir() / "Opened backup";
    const auto sessionJson = folder / "session.json";
    const auto audio = folder / "audio" / "Ramp.wav";
    const auto backup = folder / "backups" / "20261101-090000Z_session.json";
    std::error_code error;
    fs::create_directories (audio.parent_path(), error);
    if (! writeRampFixture (audio, 48000.0, 4800) || ! saveFixture (folder, sessionJson, -4.0f, audio)
        || ! saveFixture (folder, backup, -8.0f, audio))
        return ScenarioResult::fail ("could not write the session and its backup");
    const auto replaced = fileBytes (sessionJson);

    auto steps = std::make_shared<std::vector<Step>>();
    pushFileMenuSteps (host, ctx, *steps, "Open...");
    steps->push_back ({ 0, [&host, &ctx, backup]
    {
        if (! ctx.expect (host.focusFileName(), "the Open browser has no name field")) return;
        typeReplacing (host, backup.string());
        ctx.expect (host.clickModalButton ("Open"), "the Open browser has no Open button");
    },
    [&host] { return host.modalText().rfind ("Open session.json", 0) == 0; },
    "File > Open... did not bring up the Open browser" });
    steps->push_back ({ 0, [&host, &ctx, &session, folder, sessionJson, audio, replaced]
    {
        ctx.expect (host.statusMessage() == "Saved: Opened backup",
                    "opening the backup reported '" + host.statusMessage() + "' rather than saving it");
        ctx.expect (faderIs (sessionJson, -8.0f), "session.json does not hold the backup that opened");
        const auto& regions = session.track (1).regions;
        ctx.expect (regions.size() == 1 && regions.front().file.getFullPathName().toStdString() == audio.u8string(),
                    "the backup's audio did not resolve against the session folder");
        ctx.expect (session.missingAudioFilesAfterLoad.empty(), "the backup opened with its audio missing");
        ctx.expect (anyBackupHolds (folder, replaced), "the session.json the open replaced is not in backups/");
        const auto names = backupsIn (folder);
        ctx.expect (! names.empty() && fileBytes (folder / "backups" / names.back()) == fileBytes (sessionJson),
                    "the newest backup is not the session.json the open saved");
        for (const char* stray : { "session.json", "audio", "backups", "midi", "notepad.md" })
            ctx.expect (absent (folder / "backups" / stray),
                        std::string ("opening a backup made ") + stray + " in backups/");
    },
    [&host, &session, folder] { return host.modalStackEmpty() && currentSessionDirectory (session) == folder; },
    "the backup did not open in its session folder" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar openBackupFromFileMenu { Scenario {
    "gui.open_backup_from_file_menu", { "gui", "session" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runOpenBackupFromFileMenu (host, ctx); }
} };

// A session from a newer Dusk Studio says so and offers nothing: a backup or
// the autosave would roll it back, and the next save would replace it.
std::optional<ScenarioResult> runNewerSessionAlert (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginBackupCase (host, ctx)) return early;
    auto& session = ctx.session();
    const auto before = currentSessionDirectory (session);
    const auto folder = ctx.tempDir() / "Future";
    const auto sessionJson = folder / "session.json";
    const auto future = std::string (R"({"version":9999,"tracks":[{"name":"From the future"}]})");
    if (! writeText (sessionJson, future)
        || ! saveFixture (folder, folder / "backups" / "20261101-090000Z_session.json", -5.0f))
        return ScenarioResult::fail ("could not write the session");
    Session probe;
    SessionLoadError expected;
    SessionSerializer::load (probe, sessionJson, &expected);
    if (expected.failure != SessionLoadFailure::NewerVersion)
        return ScenarioResult::fail ("the fixture does not read as a newer format");
    const auto backupsBefore = backupsIn (folder);

    ctx.expect (! host.openSession (sessionJson), "the newer session reported a load");
    ctx.expect (host.modalText() == alertText (openalert::kNewerVersionTitle,
                                               openalert::newerVersionMessage (sessionJson.u8string(), 9999,
                                                                               expected.supportedVersion)),
                "the alert read '" + host.modalText() + "'");
    ctx.expect (! host.clickModalButton (openalert::kOpenNewestBackup), "the newer session's alert offered a backup");

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx] { ctx.expect (host.clickModalButton ("OK"), "the alert did not offer OK"); } });
    steps->push_back ({ 300, [&host, &ctx, &session, before, folder, sessionJson, future, backupsBefore]
    {
        ctx.expect (host.modalStackEmpty(), "OK left a modal open");
        ctx.expect (currentSessionDirectory (session) == before, "the newer session switched the session");
        ctx.expect (fileBytes (sessionJson) == future, "the newer session's session.json changed");
        ctx.expect (backupsIn (folder) == backupsBefore, "the newer session's backups changed");
        ctx.expect (host.statusMessage().rfind ("Session format 9999 is newer than supported format", 0) == 0,
                    "the status read '" + host.statusMessage() + "'");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar newerSessionAlert { Scenario {
    "gui.newer_session_alert", { "gui", "session" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runNewerSessionAlert (host, ctx); }
} };

// A recent session whose session.json has gone offers its newest backup, and
// the startup dialog stays away once that opens.
std::optional<ScenarioResult> runStartupRecentOpensBackup (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires the native UI");
   #else
    if (host.startupDialogOpen()) return ScenarioResult::skip ("requires no startup dialog");
    if (auto early = beginBackupCase (host, ctx)) return early;
    auto& session = ctx.session();
    const auto folder = ctx.tempDir() / "Lost session";
    const auto backup = folder / "backups" / "20261101-090000Z_session.json";
    if (! saveFixture (folder, backup, -5.0f)) return ScenarioResult::fail ("could not write the backup");

    openalert::WontOpen report;
    report.file = (folder / "session.json").u8string();
    report.problem = openalert::Problem::Missing;
    report.backup = true;
    report.backupTime = savedTimeOf (backup);
    if (! ctx.expect (host.openStartupDialog ({ folder }, true), "the startup dialog did not open")) return ctx.verdict();

    const auto click = [&host, &ctx] (const std::string& control)
    { ctx.expect (host.clickStartupControl (control), "the startup dialog did not show " + control); };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 400, [click] { click ("row:0"); } });
    steps->push_back ({ 100, [click] { click ("row:0"); } });
    steps->push_back ({ 0, [&host, &ctx, report]
    {
        ctx.expect (! host.startupDialogOpen(), "the startup dialog stayed up with the alert");
        ctx.expect (host.modalText() == alertText (openalert::kWontOpenTitle, openalert::wontOpenMessage (report)),
                    "the alert read '" + host.modalText() + "'");
        ctx.expect (host.clickModalButton (openalert::kOpenNewestBackup), "the alert did not offer the backup");
    },
    [&host] { return host.modalText().rfind (openalert::kWontOpenTitle, 0) == 0; },
    "the recent session with no session.json raised no alert" });
    steps->push_back ({ 300, [&host, &ctx, &session, folder, backup]
    {
        ctx.expect (! host.startupDialogOpen(), "the startup dialog came back after the backup opened");
        ctx.expect (faderIs (folder / "session.json", -5.0f), "the backup was not saved as session.json");
        ctx.expect (faderIs (backup, -5.0f), "opening the backup changed it");
        const auto names = backupsIn (folder);
        ctx.expect (! names.empty() && fileBytes (folder / "backups" / names.back()) == fileBytes (folder / "session.json"),
                    "the newest backup is not the session.json the open saved");
        ctx.expect (host.statusMessage() == "Saved: Lost session", "the open reported '" + host.statusMessage() + "'");
    },
    [&host, &session, folder] { return host.modalStackEmpty() && currentSessionDirectory (session) == folder; },
    "the backup did not open" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar startupRecentOpensBackup { Scenario {
    "gui.startup_recent_opens_backup", { "gui", "startup", "session" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runStartupRecentOpensBackup (host, ctx); }
} };
} // namespace
} // namespace duskstudio::scenario::gui
