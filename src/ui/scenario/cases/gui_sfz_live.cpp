#include "../GuiScenarioSupport.h"
#include "../../../engine/sfz/SfzLibraryService.h"
#include "../../../foundation/AppConfigDir.h"
#include "../../../session/SessionSerializer.h"
#if DUSKSTUDIO_HAS_MULTISAMPLE
 #include "../../../engine/multisample/DuskMultisampleProcessor.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace duskstudio::scenario::gui
{
namespace
{
#if DUSKSTUDIO_HAS_MULTISAMPLE && DUSKSTUDIO_HAS_NATIVE_UI
namespace fs = std::filesystem;
using Json = dusk::json::Json;
using namespace sfz;
constexpr int kNetworkBoundMs = 3600000;

std::string setting (const char* name)
{
   #if defined(_WIN32)
    char* value = nullptr;
    std::size_t bytes = 0;
    const auto error = ::_dupenv_s (&value, &bytes, name);
    const std::unique_ptr<char, decltype (&std::free)> owned (value, &std::free);
    return error == 0 && value != nullptr ? value : "";
   #else
    const char* value = std::getenv (name);
    return value != nullptr ? value : "";
   #endif
}

bool require (ScenarioContext& ctx, bool condition, const std::string& message)
{
    if (ctx.expect (condition, message)) return true;
    ctx.complete (ctx.verdict());
    return false;
}

class LiveTransport final : public HttpTransport
{
public:
    explicit LiveTransport (AudioEngine& owner) : engine (owner) {}

    OperationResult perform (const HttpRequest& request, const HttpHeadersCallback& headers,
                             const HttpBodyCallback& body, const OperationControl& control) override
    {
        Json record { { "url", request.url }, { "responses", Json::array() } };
        for (const auto& header : request.headers)
        {
            const auto name = dusk::text::toLowerCase (header.first);
            if (name == "range" || name == "if-range") record[name] = header.second;
        }
        int responseCode = 0;
        auto result = curl.perform (request, [&] (const HttpResponse& response)
        {
            responseCode = response.statusCode;
            record["responses"].push_back (responseCode);
            return headers (response);
        }, [&] (std::string_view bytes)
        {
            if (! body (bytes)) return false;
            // Archive requests have no total timeout; metadata and artwork do.
            if (request.timeoutSeconds == 0 && (responseCode == 200 || responseCode == 206) && ! bytes.empty())
            {
                const std::lock_guard<std::mutex> lock (mutex);
                if (pauseArmed)
                {
                    pauseArmed = false;
                    pausedBytes = bytes.size();
                    engine.setAssetRenderActive (true);
                    holding = true;
                }
            }
            return true;
        }, control);
        record["status"] = static_cast<int> (result.status);
        record["error"] = result.error;
        const std::lock_guard<std::mutex> lock (mutex);
        requests.push_back (std::move (record));
        return result;
    }

    void armPause()
    {
        const std::lock_guard<std::mutex> lock (mutex);
        pauseArmed = true;
    }

    void releasePause()
    {
        const std::lock_guard<std::mutex> lock (mutex);
        pauseArmed = false;
        if (holding) { engine.setAssetRenderActive (false); holding = false; }
    }

    Json trace()
    {
        const std::lock_guard<std::mutex> lock (mutex);
        return { { "requests", requests }, { "pause_body_bytes", pausedBytes } };
    }

private:
    AudioEngine& engine;
    CurlTransport curl;
    std::mutex mutex;
    Json requests = Json::array();
    std::size_t pausedBytes = 0;
    bool pauseArmed = false, holding = false;
};

struct LiveState
{
    fs::path profile, restore, originalDirectory;
    std::string source, packId, query, program;
    std::shared_ptr<LiveTransport> transport;
    std::optional<InstalledPack> installed;
    std::optional<ManagedSfzLocator> locator;
    Json evidence;
    std::uint64_t generation = 0;
    std::size_t onlineIndex = 0, programIndex = 0;
    int note = 60;
    float audioPeakDb = -100.0f, masterPeakDb = -100.0f;
    bool restored = false;

    bool write (const std::string& phase)
    {
        evidence["phase"] = phase;
        evidence["http"] = transport->trace();
        std::ofstream output (dusk::fs::nativeIOPath (profile / "sfz-live-result.json"), std::ios::trunc);
        output << evidence.dump (2) << '\n';
        output.close();
        return ! output.fail();
    }
};

void click (std::vector<Step>& steps, GuiHost& host, ScenarioContext& ctx,
            std::function<std::string()> name)
{
    steps.push_back ({ 0, [name] { std::fprintf (stderr, "[Live SFZ] control=%s\n", name().c_str()); } });
    steps.push_back ({ 0, [] {}, [&host, name]
    { return host.sfzLibraryPointer (kStripIndex, name(), 0.5f, false); }, "Live SFZ control was not visible", {}, 15000 });
    steps.push_back ({ 0, [] {}, [&host, name]
    { return host.sfzLibraryPointer (kStripIndex, name(), 0.5f, true); }, "Live SFZ control could not be pressed" });
    steps.push_back ({ 0, [&host, &ctx, name]
    { require (ctx, host.sfzLibraryPointer (kStripIndex, name(), 0.5f, false), "Live SFZ control release failed: " + name()); } });
}

void click (std::vector<Step>& steps, GuiHost& host, ScenarioContext& ctx, const char* name)
{
    click (steps, host, ctx, [name] { return std::string (name); });
}

bool completePpm (const fs::path& path)
{
    std::ifstream input (dusk::fs::nativeIOPath (path), std::ios::binary);
    char signature[3] {};
    input.read (signature, 3);
    if (! input || signature[0] != 'P' || signature[1] != '6' || signature[2] != '\n') return false;
    std::uintmax_t width = 0, height = 0, maximum = 0;
    char delimiter = 0;
    if (! (input >> width >> height >> maximum) || ! input.get (delimiter)
        || delimiter != '\n' || width == 0 || height == 0 || maximum != 255) return false;
    const auto offset = input.tellg();
    std::error_code error;
    const auto bytes = fs::file_size (path, error);
    if (error || offset < 0 || bytes < static_cast<std::uintmax_t> (offset)) return false;
    const auto payload = bytes - static_cast<std::uintmax_t> (offset);
    return width <= payload / 3 / height && width * height * 3 == payload;
}

void captureLibrary (std::vector<Step>& steps, GuiHost& host, ScenarioContext& ctx, const fs::path& path,
                     std::function<std::string()> readyControl)
{
    steps.push_back ({ 0, [&host, &ctx, path]
    {
        std::error_code error;
        const bool exists = fs::exists (path, error);
        if (require (ctx, ! error && ! exists, "SFZ capture target already exists or cannot be inspected"))
            host.captureSfzLibrary (kStripIndex, path);
    }, [&host, readyControl]
    { return host.sfzLibraryPointer (kStripIndex, readyControl(), 0.5f, false); }, "SFZ view did not settle before capture" });
    steps.push_back ({ 0, [] {}, [path] { return completePpm (path); }, "Complete SFZ view capture was not written" });
}

void finished (std::vector<Step>& steps, ScenarioContext& ctx, const std::shared_ptr<LiveState>& state,
               LibraryState expected, const char* phase)
{
    steps.push_back ({ 0, [&ctx, state, expected, phase]
    {
        const auto snapshot = ctx.engine().getSfzLibrary().snapshot();
        if (! require (ctx, snapshot.state == expected, std::string (phase) + ": " + snapshot.error)) return;
        require (ctx, state->write (phase), "Cannot retain live SFZ evidence");
    }, [&ctx, state]
    {
        const auto snapshot = ctx.engine().getSfzLibrary().snapshot();
        return ! snapshot.busy && snapshot.generation > state->generation;
    }, std::string ("Live SFZ operation timed out: ") + phase, {}, kNetworkBoundMs });
}

std::optional<ScenarioResult> runLive (GuiHost& host, ScenarioContext& ctx)
{
    if (setting ("DUSKSTUDIO_LIVE_SFZ_CONSENT") != "public-https-download")
        return ScenarioResult::skip ("requires explicit live SFZ download consent");
    auto state = std::make_shared<LiveState>();
    state->source = setting ("DUSKSTUDIO_LIVE_SFZ_PROVIDER");
    state->packId = setting ("DUSKSTUDIO_LIVE_SFZ_PACK_ID");
    state->query = setting ("DUSKSTUDIO_LIVE_SFZ_QUERY");
    state->program = setting ("DUSKSTUDIO_LIVE_SFZ_PROGRAM");
    const auto profile = fs::u8path (setting ("DUSKSTUDIO_CONFIG_DIR"));
    const auto token = setting ("DUSKSTUDIO_LIVE_SFZ_TOKEN");
    const auto note = setting ("DUSKSTUDIO_LIVE_SFZ_NOTE");
    const auto sources = providerSources();
    if (std::none_of (sources.begin(), sources.end(), [&state] (const auto& source) { return source.id == state->source; })
        || state->packId.empty() || state->query.empty() || state->program.empty()
        || note.empty() || note.size() > 3 || ! std::all_of (note.begin(), note.end(), [] (char c) { return c >= '0' && c <= '9'; }))
        return ScenarioResult::fail ("Specify a supported provider, pack ID, nonempty query, exact program path and MIDI note");
    state->note = std::stoi (note);
    std::error_code error;
    state->profile = fs::weakly_canonical (profile, error);
    if (error || ! profile.is_absolute() || token.size() < 16 || token.size() > 128 || state->note > 127
        || fileBytes (state->profile / ".dusk-sfz-live-owned") != token + "\n"
        || fs::weakly_canonical (dusk::fs::appConfigDir(), error) != state->profile || error
        || fs::weakly_canonical (dusk::fs::appDataDir(), error) != state->profile || error
        || fs::exists (state->profile / "sfz-live-result.json"))
        return ScenarioResult::fail ("Live SFZ requires a fresh token-marked absolute private profile");
    auto& engine = ctx.engine();
    auto& service = engine.getSfzLibrary();
    if (service.snapshot().busy)
    {
        ctx.waitUntil ([&service] { return ! service.snapshot().busy; }, 15000, [&host, &ctx]
        {
            const auto result = runLive (host, ctx);
            if (result) ctx.complete (*result);
        }, "Initial SFZ library scan did not settle");
        return std::nullopt;
    }
    const auto initial = service.snapshot();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty() || host.renderRunning()
        || ! engine.isAudioCallbackRegistered() || engine.getVirtualKeyboardInputIndex() < 0
        || engine.hasPendingMultisampleLoads() || ! engine.managedSfzInUse().empty()
        || initial.busy || initial.state == LibraryState::Paused || ! initial.installed.empty()
        || ! initial.locations.error.empty() || ! initial.locations.previous.empty()
        || fs::weakly_canonical (initial.locations.active.root, error) != state->profile / "sfz-library" || error)
        return ScenarioResult::fail ("Live SFZ requires an idle empty private library, live audio and no pending instrument");
    for (const auto* child : { "packs", ".downloads" })
    {
        const auto directory = initial.locations.active.root / child;
        if (fs::exists (directory, error) && (! fs::is_empty (directory, error) || error))
            return ScenarioResult::fail ("Live SFZ refuses preexisting pack or download contents");
        if (error) return ScenarioResult::fail ("Cannot inspect the private SFZ library");
    }
    for (int index = 0; index < Session::kNumTracks; ++index)
    {
        const auto& track = ctx.session().track (index);
        if (! track.regions.empty() || ! track.midiRegions.current().empty() || ! track.takes.empty()
            || track.pluginDescriptor || ! track.pluginLegacyDescriptionXml.isEmpty() || ! track.nativeClapPath.isEmpty()
            || ! track.nativeLv2Path.isEmpty() || ! track.nativeVst3Path.isEmpty() || ! track.nativeAuIdentifier.isEmpty()
            || ! track.nativeMultisamplePath.isEmpty() || track.nativeMultisampleLocator || ! track.builtinUnitId.empty()
            || track.frozen.load())
            return ScenarioResult::fail ("Live SFZ refuses a populated session");
    }
    state->restore = ctx.tempDir() / "before-live-sfz.json";
    state->originalDirectory = currentSessionDirectory (ctx.session());
    if (! SessionSerializer::save (ctx.session(), state->restore)) return ScenarioResult::fail ("Cannot preserve the empty session");
    state->transport = std::make_shared<LiveTransport> (engine);
    if (! service.setTransportForTest (state->transport)) return ScenarioResult::fail ("Cannot trace the idle production HTTPS transport");
    state->evidence = { { "source", state->source }, { "pack", state->packId }, { "query", state->query },
        { "program", state->program }, { "note", state->note }, { "complete", false },
        { "transport", "Production CurlTransport; real public HTTPS; no fixture responses" },
        { "library_root", initial.locations.active.root.u8string() } };
    keepStage (host, ctx);
    ctx.cleanup ([&host, &ctx, state]
    {
        state->transport->releasePause();
        auto& owner = ctx.engine();
        auto& library = owner.getSfzLibrary();
        library.cancel();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (2);
        while (library.snapshot().busy && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        const std::uint8_t off[] { 0x80, static_cast<std::uint8_t> (state->note), 0 };
        owner.postVirtualKeyboardMidi (off, 3);
        host.closeSfzLibrary (kStripIndex);
        if (auto* strip = host.strip (kStripIndex)) strip->closeEditor();
        if (! state->restored)
        {
            reopenSavedSession (host, state->restore);
            applySessionDirectory (ctx.session(), state->originalDirectory);
        }
        if (! library.snapshot().busy) library.setTransportForTest ({});
        if (! state->evidence.value ("complete", false))
        {
            state->evidence["failure"] = ctx.verdict().message;
            state->write ("incomplete");
        }
    });
    if (! require (ctx, state->write ("started"), "Cannot create live SFZ evidence")) return std::nullopt;
    host.switchToStage (GuiHost::Stage::Mixing);
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&host, &ctx] { require (ctx, host.openSfzLibrary (kStripIndex), "SFZ Manager did not open"); } });
    click (*steps, host, ctx, "online-tab");
    click (*steps, host, ctx, "enabled-source");
    click (*steps, host, ctx, [state] { return "source-" + state->source; });
    click (*steps, host, ctx, "query");
    steps->push_back ({ 0, [&host, &ctx, state]
    {
        if (! require (ctx, host.sfzLibraryInput (kStripIndex, "focus-out"), "SFZ search did not accept shell focus")) return;
        for (const char character : state->query)
            if (! require (ctx, host.pressKey (character == ' ' ? "Space" : std::string (1, character), character),
                           "SFZ search did not receive text")) return;
    } });
    steps->push_back ({ 0, [&ctx, state] { state->generation = ctx.engine().getSfzLibrary().snapshot().generation; } });
    click (*steps, host, ctx, "browse");
    finished (*steps, ctx, state, LibraryState::Complete, "browsed");
    steps->push_back ({ 0, [&ctx, state]
    {
        const auto snapshot = ctx.engine().getSfzLibrary().snapshot();
        const auto found = std::find_if (snapshot.page.items.begin(), snapshot.page.items.end(), [&state] (const auto& item)
        { return item.sourceId == state->source && item.packId == state->packId; });
        if (! require (ctx, ! snapshot.cachedResponse && found != snapshot.page.items.end(), "Fresh GUI query did not contain the exact requested pack")) return;
        state->onlineIndex = static_cast<std::size_t> (std::distance (snapshot.page.items.begin(), found));
        state->generation = snapshot.generation;
        state->evidence["browse_items"] = snapshot.page.items.size();
    } });
    captureLibrary (*steps, host, ctx, state->profile / "sfz-live-browse.ppm",
                    [state] { return "online-item-" + std::to_string (state->onlineIndex); });
    click (*steps, host, ctx, [state] { return "online-item-" + std::to_string (state->onlineIndex); });
    finished (*steps, ctx, state, LibraryState::Complete, "resolved");
    steps->push_back ({ 0, [&ctx, state]
    {
        const auto snapshot = ctx.engine().getSfzLibrary().snapshot();
        if (! require (ctx, snapshot.pack && snapshot.pack->sourceId == state->source && snapshot.pack->packId == state->packId,
                       "GUI resolution changed the requested pack")) return;
        const auto& pack = *snapshot.pack;
        const auto& license = pack.license.id;
        const std::array<const char*, 11> allowed { "CC0-1.0", "CC-BY-1.0", "CC-BY-2.0", "CC-BY-2.5", "CC-BY-3.0", "CC-BY-4.0",
            "CC-BY-SA-1.0", "CC-BY-SA-2.0", "CC-BY-SA-2.5", "CC-BY-SA-3.0", "CC-BY-SA-4.0" };
        if (! require (ctx, std::find (allowed.begin(), allowed.end(), license) != allowed.end(),
                       "GUI resolution did not admit an allowed Creative Commons license")) return;
        state->evidence["license"] = { { "id", license }, { "source", pack.license.sourceUrl }, { "sha256", pack.license.sha256 } };
        state->evidence["release"] = pack.releaseId;
        state->evidence["download_url"] = pack.downloadUrl;
        state->generation = snapshot.generation;
    } });
    captureLibrary (*steps, host, ctx, state->profile / "sfz-live-license.ppm", [] { return "enabled-download"; });
    steps->push_back ({ 0, [state] { state->transport->armPause(); } });
    click (*steps, host, ctx, "enabled-download");
    finished (*steps, ctx, state, LibraryState::Paused, "paused");
    steps->push_back ({ 0, [&ctx, state]
    {
        const auto snapshot = ctx.engine().getSfzLibrary().snapshot();
        if (! require (ctx, snapshot.installed.empty() && snapshot.progress.completedBytes > 0, "Live archive did not pause after receiving bytes")) return;
        Json parts = Json::array();
        std::error_code scanError;
        const auto downloads = snapshot.locations.active.root / ".downloads";
        for (const auto& entry : fs::directory_iterator (dusk::fs::nativeIOPath (downloads), scanError))
        {
            if (entry.path().extension() != ".part") continue;
            const auto bytes = entry.file_size (scanError);
            if (! require (ctx, ! scanError && bytes > 0
                           && inspectDownloadPart (entry.path(), 2ULL * 1024 * 1024 * 1024) == DownloadPartState::Reusable,
                           "Paused live archive did not preserve a reusable nonempty partial")) return;
            parts.push_back ({ { "path", entry.path().u8string() }, { "bytes", bytes } });
        }
        if (! require (ctx, ! scanError && parts.size() == 1, "Paused live download did not retain exactly one reusable partial")) return;
        state->evidence["paused_partial"] = parts[0];
        state->evidence["paused_bytes"] = snapshot.progress.completedBytes;
        state->generation = snapshot.generation;
    } });
    captureLibrary (*steps, host, ctx, state->profile / "sfz-live-paused.ppm", [] { return "resume"; });
    steps->push_back ({ 0, [state] { state->transport->releasePause(); } });
    click (*steps, host, ctx, "resume");
    finished (*steps, ctx, state, LibraryState::Complete, "installed");
    steps->push_back ({ 0, [&ctx, state]
    {
        const auto installed = ctx.engine().getSfzLibrary().snapshot().installed;
        if (! require (ctx, installed.size() == 1 && installed[0].provider.sourceId == state->source
                       && installed[0].provider.packId == state->packId, "GUI download did not publish exactly the requested pack")) return;
        state->installed = installed[0];
        const auto& pack = *state->installed;
        const auto program = std::find_if (pack.programs.begin(), pack.programs.end(), [&state] (const auto& entry)
        { return entry.relativePath == state->program; });
        if (! require (ctx, program != pack.programs.end(), "Requested SFZ was not a declared installed program")) return;
        const auto raw = std::find_if (pack.instruments.begin(), pack.instruments.end(), [&program] (const auto& entry) { return entry.id == program->id; });
        if (! require (ctx, raw != pack.instruments.end(), "Declared program did not retain its raw identity")) return;
        state->programIndex = static_cast<std::size_t> (std::distance (pack.instruments.begin(), raw));
        state->locator = pack.locatorFor (*program);
        state->evidence["locator"] = locatorToJson (*state->locator);
        state->evidence["installed_tree_sha256"] = pack.installedTreeSha256;
        state->evidence["installed_programs"] = pack.programs.size();
    } });
    click (*steps, host, ctx, "installed-tab");
    click (*steps, host, ctx, "installed-item-0");
    steps->push_back ({ 0, [&ctx, state] { state->generation = ctx.engine().getSfzLibrary().snapshot().generation; } });
    click (*steps, host, ctx, "enabled-verify");
    finished (*steps, ctx, state, LibraryState::Complete, "verified");
    steps->push_back ({ 0, [] {}, [&host]
    { return host.sfzLibraryPointer (kStripIndex, "enabled-verify", 0.5f, false); }, "Installed SFZ view did not settle after verification" });
    click (*steps, host, ctx, "instrument");
    click (*steps, host, ctx, [state] { return "instrument-choice-" + std::to_string (state->programIndex); });
    captureLibrary (*steps, host, ctx, state->profile / "sfz-live-installed.ppm", [] { return "enabled-load"; });
    click (*steps, host, ctx, "load");
    steps->push_back ({ 0, [&ctx, state]
    {
        const auto& track = ctx.session().track (kStripIndex);
        if (! require (ctx, track.nativeMultisampleLocator == state->locator, "GUI Load did not preserve the exact portable locator")) return;
        state->evidence["runtime_loaded"] = true;
    }, [&ctx]
    {
        const auto* instance = ctx.engine().getChannelStrip (kStripIndex).getNativeMultisampleSlot().getInstance();
        return ! ctx.engine().hasPendingMultisampleLoads() && instance && instance->hasLoadedRuntime();
    }, "GUI Load did not prime the installed sample runtime", {}, 120000 });
    steps->push_back ({ 0, [&host, &ctx, state]
    {
        host.closeSfzLibrary (kStripIndex);
        if (auto* strip = host.strip (kStripIndex)) strip->closeEditor();
        auto& track = ctx.session().track (kStripIndex);
        track.midiInputIndex.store (ctx.engine().getVirtualKeyboardInputIndex());
        track.inputMonitor.store (true);
        track.meterOutLDb.store (-100.0f);
        track.meterOutRDb.store (-100.0f);
        ctx.session().master().meterPostMasterLDb.store (-100.0f);
        ctx.session().master().meterPostMasterRDb.store (-100.0f);
        const std::uint8_t on[] { 0x90, static_cast<std::uint8_t> (state->note), 100 };
        ctx.engine().postVirtualKeyboardMidi (on, 3);
    } });
    steps->push_back ({ 0, [&ctx, state]
    {
        state->evidence["audio_peak_db"] = state->audioPeakDb;
        state->evidence["master_peak_db"] = state->masterPeakDb;
        const std::uint8_t off[] { 0x80, static_cast<std::uint8_t> (state->note), 0 };
        ctx.engine().postVirtualKeyboardMidi (off, 3);
    }, [&ctx, state]
    {
        const auto& track = ctx.session().track (kStripIndex);
        const auto& master = ctx.session().master();
        state->audioPeakDb = std::max (track.meterOutLDb.load(), track.meterOutRDb.load());
        state->masterPeakDb = std::max (master.meterPostMasterLDb.load(), master.meterPostMasterRDb.load());
        return std::isfinite (state->audioPeakDb) && state->audioPeakDb > -90.0f
            && std::isfinite (state->masterPeakDb) && state->masterPeakDb > -90.0f;
    }, "Loaded live instrument produced no finite nonzero audio", {}, 10000 });
    steps->push_back ({ 0, [&host, &ctx, state]
    {
        reopenSavedSession (host, state->restore);
        applySessionDirectory (ctx.session(), state->originalDirectory);
        state->restored = true;
    } });
    steps->push_back ({ 0, [&ctx, state]
    {
        if (! require (ctx, ! ctx.session().track (kStripIndex).nativeMultisampleLocator, "Empty session restoration retained the test instrument")) return;
        state->evidence["complete"] = true;
        state->evidence["captures"] = { "sfz-live-browse.ppm", "sfz-live-license.ppm", "sfz-live-paused.ppm", "sfz-live-installed.ppm" };
        require (ctx, state->write ("complete"), "Cannot retain completed live SFZ evidence");
    }, [&ctx] { return ! ctx.engine().hasPendingMultisampleLoads(); }, "Empty session restoration did not finish" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}
#else
std::optional<ScenarioResult> runLive (GuiHost&, ScenarioContext&)
{
    return ScenarioResult::skip ("native SFZ GUI support is not built");
}
#endif

const ScenarioRegistrar liveRepository { Scenario {
    "gui.sfz_live_repository_acceptance", { "helper", "live", "sfz" }, Needs::Engine | Needs::Gui,
    {}, {}, 7200000, runLive
} };
} // namespace
} // namespace duskstudio::scenario::gui
