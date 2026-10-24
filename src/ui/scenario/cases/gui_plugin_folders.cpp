#include "../GuiScenarioSupport.h"
#include "../../../engine/PluginSearchPaths.h"

#include <cstdlib>

namespace duskstudio::scenario::gui
{
namespace
{
std::optional<ScenarioResult> runPluginFolders (GuiHost& host, ScenarioContext& ctx, bool scan = false)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    (void) scan;
    return ScenarioResult::skip ("requires native settings");
   #else
   #if defined(_WIN32)
    std::size_t scanVariableSize = 0;
    ::getenv_s (&scanVariableSize, nullptr, 0, "DUSKSTUDIO_SETTINGS_SCAN_TEST");
    const bool isolatedScan = scanVariableSize > 0;
   #else
    const bool isolatedScan = std::getenv ("DUSKSTUDIO_SETTINGS_SCAN_TEST") != nullptr;
   #endif
    if (scan && ! isolatedScan)
        return ScenarioResult::skip ("requires an isolated profile with DUSKSTUDIO_SETTINGS_SCAN_TEST set");
    if (! ctx.engine().getTransport().isStopped() || ! host.modalStackEmpty() || host.audioSettingsOpen())
        return ScenarioResult::skip ("requires stopped transport and no open dialog");
    namespace fs = std::filesystem;
    const auto settings = pluginpaths::settingsFile();
    const bool existed = fs::exists (settings);
    const auto saved = dusk::fs::loadFileAsString (settings);
    ctx.cleanup ([&host, settings, existed, saved]
    {
        drainModals (host);
        host.closeAudioSettings();
        if (existed) dusk::fs::writeStringToFile (settings, saved);
        else { std::error_code error; fs::remove (settings, error); }
    });
    std::string settingsError;
    if (! pluginpaths::save ({}, settingsError)) return ScenarioResult::fail (settingsError);
    const auto folder = fs::canonical (ctx.tempDir()) / "Custom plug-in library";
    if (! fs::create_directory (folder)) return ScenarioResult::fail ("could not create the custom folder fixture");
    auto captures = ctx.tempDir();
    fs::path captureFolder;
   #if defined(_WIN32)
    std::size_t captureVariableSize = 0;
    ::_wgetenv_s (&captureVariableSize, nullptr, 0, L"DUSKSTUDIO_SETTINGS_CAPTURE_DIR");
    std::wstring location (captureVariableSize, L'\0');
    if (captureVariableSize > 0
        && ::_wgetenv_s (&captureVariableSize, location.data(), location.size(), L"DUSKSTUDIO_SETTINGS_CAPTURE_DIR") == 0)
    {
        location.resize (captureVariableSize - 1);
        captureFolder = location;
    }
   #else
    if (const auto* location = std::getenv ("DUSKSTUDIO_SETTINGS_CAPTURE_DIR")) captureFolder = fs::u8path (location);
   #endif
    if (! captureFolder.empty())
    {
        captures = captureFolder;
        std::error_code captureError;
        fs::create_directories (captures, captureError);
        if (captureError) return ScenarioResult::fail ("could not create the settings capture folder");
    }
    auto steps = std::make_shared<std::vector<Step>>();
    const auto click = [&] (std::string control)
    {
        steps->push_back ({ 0, [&host, &ctx, control]
        { ctx.expect (host.clickAudioSettingsControl (control), "Settings control could not be clicked: " + control); },
        [&host, control] { return host.pointerAudioSettings (control, 0.5f, false); },
        "Settings control did not appear: " + control, {}, kSettleBoundMs });
    };
    const auto capture = [&] (const std::string& name)
    {
        const auto path = captures / (name + ".ppm");
        steps->push_back ({ 0, [&host, &ctx, path]
        {
            std::error_code error;
            fs::remove (path, error);
            if (ctx.expect (! error, "Settings could not remove its previous capture"))
                ctx.expect (host.captureAudioSettings (path), "Settings could not queue its native frame");
        } });
        steps->push_back ({ 0, [] {}, [path] { std::error_code error; return fs::is_regular_file (path, error); },
                            "Settings native frame was not saved", {}, 15000 });
    };
    const auto chooseFolder = [&]
    {
        click ("plugin-folder-add");
        steps->push_back ({ 0, [&host, &ctx, folder]
        {
            ctx.expect (host.clickFileBrowserControl (true), "plug-in folder browser has no path field");
            typeReplacing (host, folder.u8string());
            ctx.expect (host.pressPeerKey ("Return"), "plug-in folder browser rejected its path");
        }, [&host] { return ! host.fileBrowserFolder().empty() && ! host.fileBrowserScanning(); },
        "plug-in folder browser did not open", {}, kSettleBoundMs });
        steps->push_back ({ 0, [&host, &ctx]
        { ctx.expect (host.clickModalButton ("Open"), "plug-in folder browser had no Open button"); },
        [&host, folder]
        {
            std::error_code sameError;
            return ! host.fileBrowserScanning() && fs::equivalent (host.fileBrowserFolder(), folder, sameError) && ! sameError;
        }, "plug-in folder browser did not reach the selected folder", {}, kSettleBoundMs });
        steps->push_back ({ 0, [&ctx, folder]
        { ctx.expect (pluginpaths::load() == pluginpaths::Roots { folder }, "the selected custom folder was not saved exactly once"); },
        [&host] { return host.audioSettingsOpen() && host.modalStackEmpty(); },
        "Settings did not return after folder selection", {}, kSettleBoundMs });
    };
    steps->push_back ({ 0, [&host, &ctx] { ctx.expect (host.openAudioSettings(), "Settings did not open"); } });
    for (const auto* tab : { "audio", "control-surface", "midi-bindings", "midi-sync", "general", "advanced", "sfz-library", "plugins" })
    {
        click (std::string ("tab-") + tab);
        capture (std::string ("settings-") + tab);
    }
    chooseFolder();
    chooseFolder();
    capture ("settings-plugin-folder-added");
    click ("plugin-folder-add");
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Cancel"), "plug-in folder picker could not be cancelled"); },
    [&host] { return ! host.fileBrowserFolder().empty() && ! host.fileBrowserScanning(); },
    "plug-in folder picker did not reopen", {}, kSettleBoundMs });
    steps->push_back ({ 0, [&ctx, folder]
    { ctx.expect (pluginpaths::load() == pluginpaths::Roots { folder }, "Cancel changed the custom folder list"); },
    [&host] { return host.audioSettingsOpen() && host.modalStackEmpty(); }, "Cancel did not return to Settings", {}, kSettleBoundMs });
    steps->push_back ({ 0, [&host] { host.closeAudioSettings(); } });
    steps->push_back ({ 0, [&host, &ctx] { ctx.expect (host.openAudioSettings(), "Settings did not reopen"); },
    [&host] { return ! host.audioSettingsOpen(); }, "Settings did not finish closing", {}, kSettleBoundMs });
    steps->push_back ({ 0, [&ctx, folder]
    { ctx.expect (pluginpaths::load() == pluginpaths::Roots { folder }, "reopening Settings lost its custom folder"); },
    [&host] { return host.pointerAudioSettings ("plugin-folder-remove", 0.5f, false); },
    "Settings did not remember the Plug-ins page", {}, kSettleBoundMs });
    click ("plugin-folder-remove");
    steps->push_back ({ 0, [&ctx, folder]
    {
        ctx.expect (pluginpaths::load().empty(), "Remove left a custom search folder configured");
        ctx.expect (fs::is_directory (folder), "Remove deleted the plug-in folder");
    } });
    capture ("settings-plugin-folder-removed");
    if (scan)
    {
        click ("plugin-scan");
        steps->push_back ({ 0, [] {}, [&host]
        {
            if (host.audioSettingsOpen() || host.modalStackEmpty()) return false;
            const auto text = host.modalText();
            return text.find ("Plugin scan complete") != std::string::npos
                || text.find ("Plugin scan cancelled") != std::string::npos || host.clickModalButton ("Cancel");
        }, "Settings did not hand off to a cancellable scanner", {}, kSettleBoundMs });
        steps->push_back ({ 0, [&host, &ctx]
        { ctx.expect (host.clickModalButton ("OK"), "the Settings scan notice could not be dismissed"); },
        [&host]
        {
            const auto text = host.modalText();
            return text.find ("Plugin scan complete") != std::string::npos || text.find ("Plugin scan cancelled") != std::string::npos;
        }, "the Settings scan did not finish or cancel", {}, 30000 });
        steps->push_back ({ 0, [] {}, [&host]
        { return host.audioSettingsOpen() && host.pointerAudioSettings ("plugin-scan", 0.5f, false) && host.modalStackEmpty(); },
        "Settings did not return to Plug-ins after scanning", {}, kSettleBoundMs });
    }
    steps->push_back ({ 0, [&host] { host.closeAudioSettings(); } });
    steps->push_back ({ 0, [] {}, [&host] { return ! host.audioSettingsOpen(); },
    "Settings did not close at the end of the folder workflow", {}, kSettleBoundMs });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

// The browser opens at the last custom folder. A non-ASCII name must reach it
// intact, or the browser falls back to the nearest parent that exists.
std::optional<ScenarioResult> runPluginFolderUnicodeStart (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires native settings");
   #else
    if (! ctx.engine().getTransport().isStopped() || ! host.modalStackEmpty() || host.audioSettingsOpen())
        return ScenarioResult::skip ("requires stopped transport and no open dialog");
    namespace fs = std::filesystem;
    const auto settings = pluginpaths::settingsFile();
    const bool existed = fs::exists (settings);
    const auto saved = dusk::fs::loadFileAsString (settings);
    ctx.cleanup ([&host, settings, existed, saved]
    {
        drainModals (host);
        host.closeAudioSettings();
        if (existed) dusk::fs::writeStringToFile (settings, saved);
        else { std::error_code error; fs::remove (settings, error); }
    });
    const auto folder = fs::canonical (ctx.tempDir()) / fs::u8path ("Jos\xc3\xa9 \xe9\x9f\xb3\xe8\x89\xb2");
    if (! fs::create_directory (folder)) return ScenarioResult::fail ("could not create the Unicode folder fixture");
    std::string settingsError;
    if (! pluginpaths::save ({ folder }, settingsError)) return ScenarioResult::fail (settingsError);
    auto steps = std::make_shared<std::vector<Step>>();
    const auto click = [&] (std::string control)
    {
        steps->push_back ({ 0, [&host, &ctx, control]
        { ctx.expect (host.clickAudioSettingsControl (control), "Settings control could not be clicked: " + control); },
        [&host, control] { return host.pointerAudioSettings (control, 0.5f, false); },
        "Settings control did not appear: " + control, {}, kSettleBoundMs });
    };
    steps->push_back ({ 0, [&host, &ctx] { ctx.expect (host.openAudioSettings(), "Settings did not open"); } });
    click ("tab-plugins");
    click ("plugin-folder-add");
    steps->push_back ({ 0, [&host, &ctx, folder]
    {
        std::error_code sameError;
        ctx.expect (fs::equivalent (host.fileBrowserFolder(), folder, sameError) && ! sameError,
                    "the plug-in folder browser opened at " + host.fileBrowserFolder().u8string()
                    + " instead of the stored folder");
        ctx.expect (host.clickModalButton ("Cancel"), "plug-in folder picker could not be cancelled");
    }, [&host] { return ! host.fileBrowserFolder().empty() && ! host.fileBrowserScanning(); },
    "plug-in folder browser did not open", {}, kSettleBoundMs });
    steps->push_back ({ 0, [&ctx, folder]
    { ctx.expect (pluginpaths::stored() == pluginpaths::Roots { folder }, "Cancel changed the custom folder list"); },
    [&host] { return host.audioSettingsOpen() && host.modalStackEmpty(); }, "Cancel did not return to Settings", {}, kSettleBoundMs });
    steps->push_back ({ 0, [&host] { host.closeAudioSettings(); } });
    steps->push_back ({ 0, [] {}, [&host] { return ! host.audioSettingsOpen(); },
    "Settings did not close at the end of the Unicode folder check", {}, kSettleBoundMs });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar pluginFolderUnicodeStart { Scenario {
    "gui.settings_plugin_folder_unicode_start", { "gui", "settings", "plugins" }, Needs::Engine | Needs::Gui,
    {}, {}, 60000, [] (GuiHost& host, ScenarioContext& ctx) { return runPluginFolderUnicodeStart (host, ctx); }
} };
const ScenarioRegistrar pluginFolders { Scenario {
    "gui.settings_plugin_folders", { "gui", "settings", "plugins" }, Needs::Engine | Needs::Gui,
    {}, {}, 90000, [] (GuiHost& host, ScenarioContext& ctx) { return runPluginFolders (host, ctx); }
} };
const ScenarioRegistrar pluginScan {
    Scenario { "gui.settings_plugin_scan", { "gui", "settings", "plugins" }, Needs::Engine | Needs::Gui,
    {}, {}, 120000, [] (GuiHost& host, ScenarioContext& ctx) { return runPluginFolders (host, ctx, true); }
} };
} // namespace
} // namespace duskstudio::scenario::gui
