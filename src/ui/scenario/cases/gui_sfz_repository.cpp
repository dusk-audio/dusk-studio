#include "../GuiScenarioSupport.h"
#include "../../../engine/sfz/SfzLibraryService.h"
#include "../../../foundation/Base64.h"
#include "../../../foundation/Fs.h"
#include "../../../foundation/Json.h"
#include "../../../session/RegionEditActions.h"
#include "../../../session/SessionSerializer.h"
#if DUSKSTUDIO_HAS_NATIVE_UI
 #include "../../imgui/SfzArtwork.h"
#endif
#if DUSKSTUDIO_HAS_MULTISAMPLE
 #include "../../../engine/multisample/DuskMultisampleProcessor.h"
#endif

#include <archive.h>
#include <archive_entry.h>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace duskstudio::scenario::gui
{
namespace
{
namespace fs = std::filesystem;
using Json = dusk::json::Json;
using namespace sfz;
constexpr const char* kCc0 = "Creative Commons CC0 1.0 Universal\nhttps://creativecommons.org/publicdomain/zero/1.0/\n";
constexpr const char* kCatalogUrl = "https://raw.githubusercontent.com/sfzinstruments/sfzinstruments.github.io/master/data/sfz/instruments.yml";
constexpr const char* kDemoPage = "https://sfzinstruments.github.io/woodwinds/dusk_gui/";

bool sameFile (const std::string& actual, const fs::path& expected)
{
    std::error_code error;
    return ! actual.empty() && fs::equivalent (fs::u8path (actual), expected, error) && ! error;
}

std::string hash (std::string_view bytes)
{
    if (sodium_init() < 0) return {};
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest {};
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex {};
    crypto_hash_sha256 (digest.data(), reinterpret_cast<const unsigned char*> (bytes.data()), bytes.size());
    sodium_bin2hex (hex.data(), hex.size(), digest.data(), digest.size());
    return hex.data();
}

std::string base64 (std::string_view bytes)
{
    std::string result (sodium_base64_encoded_len (bytes.size(), sodium_base64_VARIANT_ORIGINAL), '\0');
    if (sodium_bin2base64 (result.data(), result.size(), reinterpret_cast<const unsigned char*> (bytes.data()),
                           bytes.size(), sodium_base64_VARIANT_ORIGINAL) == nullptr) return {};
    result.resize (result.find ('\0'));
    return result;
}

std::string waveBytes()
{
    constexpr std::uint32_t frames = 8192;
    std::string bytes;
    const auto little = [&bytes] (std::uint32_t value, unsigned width)
    {
        for (unsigned i = 0; i < width; ++i) bytes += static_cast<char> ((value >> (8 * i)) & 255);
    };
    bytes += "RIFF"; little (36 + frames * 2, 4); bytes += "WAVEfmt "; little (16, 4);
    little (1, 2); little (1, 2); little (48000, 4); little (96000, 4);
    little (2, 2); little (16, 2); bytes += "data"; little (frames * 2, 4);
    for (std::uint32_t i = 0; i < frames; ++i)
        little (static_cast<std::uint16_t> ((i % 64 < 32) ? 6000 : -6000), 2);
    return bytes;
}

std::string zipBytes (const std::string& root)
{
    std::string bytes (128 * 1024, '\0');
    std::size_t written = 0;
    const std::unique_ptr<struct archive, decltype (&archive_write_free)> writer (archive_write_new(), archive_write_free);
    if (! writer || archive_write_set_format_zip (writer.get()) != ARCHIVE_OK
        || archive_write_set_format_option (writer.get(), "zip", "compression", "store") != ARCHIVE_OK
        || archive_write_set_format_option (writer.get(), "zip", "hdrcharset", "UTF-8") != ARCHIVE_OK
        || archive_write_open_memory (writer.get(), bytes.data(), bytes.size(), &written) != ARCHIVE_OK) return {};
    const std::vector<std::pair<std::string, std::string>> members {
        { "LICENSE", kCc0 },
        { "Instruments/One.sfz", "<region> sample=../Samples/tone.wav key=60\n" },
        { "Instruments/Two.sfz", "<region> sample=../Samples/tone.wav key=61\n" },
        { "Mappings/One.sfz", "<global> volume=-3\n" },
        { "Fixture.bank.xml", "<?xml version=\"1.0\"?><Key/><AriaBank>"
            "<AriaProgram name=\"One\"><AriaElement path=\"Instruments/One.sfz\"/></AriaProgram>"
            "<AriaProgram name=\"Two\"><AriaElement path=\"Instruments/Two.sfz\"/></AriaProgram></AriaBank>" },
        { "Samples/tone.wav", waveBytes() }
    };
    for (const auto& member : members)
    {
        const std::unique_ptr<struct archive_entry, decltype (&archive_entry_free)> entry (archive_entry_new(), archive_entry_free);
        if (! entry) return {};
        const auto path = root + "/" + member.first;
        archive_entry_set_pathname_utf8 (entry.get(), path.c_str());
        archive_entry_set_filetype (entry.get(), AE_IFREG);
        archive_entry_set_perm (entry.get(), 0644);
        archive_entry_set_size (entry.get(), static_cast<la_int64_t> (member.second.size()));
        if (archive_write_header (writer.get(), entry.get()) != ARCHIVE_OK
            || archive_write_data (writer.get(), member.second.data(), member.second.size())
                 != static_cast<la_ssize_t> (member.second.size())) return {};
    }
    if (archive_write_close (writer.get()) != ARCHIVE_OK) return {};
    bytes.resize (written);
    return bytes;
}

class RepositoryTransport final : public HttpTransport
{
public:
    explicit RepositoryTransport (const std::string& seed)
    {
        const auto digest = hash (seed);
        if (digest.empty()) return;
        name = "DuskGui-" + digest.substr (0, 12);
        packId = std::to_string (std::stoull (digest.substr (0, 10), nullptr, 16) + 1);
        commit = digest.substr (0, 40);
        tree = hash ("tree-" + seed).substr (0, 40);
        api = "https://api.github.com/repos/sfzinstruments/" + name;
        source = "https://github.com/sfzinstruments/" + name;
        archiveUrl = source + "/archive/" + commit + ".zip";
        archive = zipBytes (name + "-" + commit);
    }

    OperationResult perform (const HttpRequest& request, const HttpHeadersCallback& headers,
                             const HttpBodyCallback& body, const OperationControl& control) override
    {
        #if DUSKSTUDIO_HAS_NATIVE_UI
        if (request.url == artworkUrl())
        {
            artworkRequests.fetch_add (1, std::memory_order_relaxed);
            const auto pixels = imgui::instrumentArtwork (InstrumentCategory::Piano);
            if (control.status() != OperationStatus::Complete) return { control.status(), {} };
            if (! headers ({ 200, request.url, { { "content-type", "image/png" } } }) || ! body (pixels))
                return { OperationStatus::Failed, "Fixture image rejected" };
            return { OperationStatus::Complete, {} };
        }
#endif
        requests.fetch_add (1, std::memory_order_relaxed);
        const auto stopped = [&]
        {
            const auto status = control.status();
            return OperationResult { status == OperationStatus::Complete ? OperationStatus::Failed : status,
                                     "Fixture transfer stopped" };
        };
        if (control.status() != OperationStatus::Complete) return stopped();
        if (networkFailure.load (std::memory_order_acquire))
        {
            if (! headers ({ 429, request.url, { { "retry-after", "25" }, { "x-ratelimit-reset", "1700000000" },
                                               { "content-type", "application/json" } } })) return stopped();
            if (! body (R"({"message":"Fixture repository rate limit"})")) return stopped();
            return { OperationStatus::Complete, {} };
        }
        if (request.url == kCatalogUrl || request.url == kDemoPage)
        {
            const bool page = request.url == kDemoPage;
            const auto bytes = page ? std::string ("<audio src=\"/assets/audio/woodwinds/dusk_gui.mp3\" controls></audio><img src=\"/assets/img/woodwinds/") + name + ".png\">"
                : "- name: \"Woodwinds\"\n  page: \"woodwinds\"\n  instruments:\n  - name: \"Fixture\"\n"
                  "    page: \"dusk_gui\"\n    url: \"" + source + "\"\n";
            if (! headers ({ 200, request.url, { { "content-type", page ? "text/html" : "text/plain" } } })) return stopped();
            if (! body (bytes)) return stopped();
            return { OperationStatus::Complete, {} };
        }
        if (request.url == archiveUrl)
        {
            std::size_t offset = 0;
            int status = 200;
            HttpHeaders response { { "etag", "\"" + commit + "\"" }, { "content-type", "application/zip" } };
            if (const auto found = request.headers.find ("range"); found != request.headers.end())
            {
                const auto validator = request.headers.find ("if-range");
                if (validator == request.headers.end() || validator->second != response["etag"])
                    return { OperationStatus::Failed, "Fixture resume did not preserve its entity validator" };
                const auto prefix = std::string ("bytes=");
                if (found->second.compare (0, prefix.size(), prefix) != 0) return { OperationStatus::Failed, "Invalid fixture range" };
                try { offset = static_cast<std::size_t> (std::stoull (found->second.substr (prefix.size()))); }
                catch (...) { return { OperationStatus::Failed, "Invalid fixture offset" }; }
                if (offset >= archive.size()) return { OperationStatus::Failed, "Fixture range is past the archive" };
                status = 206;
                response["content-range"] = "bytes " + std::to_string (offset) + "-" + std::to_string (archive.size() - 1)
                    + "/" + std::to_string (archive.size());
                rangedRequests.fetch_add (1, std::memory_order_relaxed);
            }
            response["content-length"] = std::to_string (archive.size() - offset);
            if (! headers ({ status, request.url, response })) return stopped();
            archiveEntered.store (true, std::memory_order_release);
            const auto first = std::min<std::size_t> (256, archive.size() - offset);
            if (! body (std::string_view (archive).substr (offset, first))) return stopped();
            offset += first;
            // The worker never needs a GUI callback to leave this barrier.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (10);
            while (holdArchive.load (std::memory_order_acquire))
            {
                if (control.status() != OperationStatus::Complete) return stopped();
                if (std::chrono::steady_clock::now() >= deadline)
                    return { OperationStatus::Failed, "Fixture download barrier expired" };
                std::unique_lock<std::mutex> lock (barrierMutex);
                barrier.wait_for (lock, std::chrono::milliseconds (5));
            }
            for (; offset < archive.size(); offset += 4096)
            {
                if (control.status() != OperationStatus::Complete) return stopped();
                if (! body (std::string_view (archive).substr (offset, 4096))) return stopped();
            }
            return { OperationStatus::Complete, {} };
        }
        Json reply;
        const Json repo { { "id", std::stoull (packId) }, { "name", name }, { "description", "Two CC0 piano SFZ fixture instruments" },
                          { "stargazers_count", 7 },
                          { "owner", { { "login", "sfzinstruments" } } }, { "private", false }, { "disabled", false },
                          { "license", { { "spdx_id", "CC0-1.0" } } } };
        if (request.url == "https://api.github.com/orgs/sfzinstruments/repos?type=public&sort=full_name&per_page=50&page=1") reply = Json::array ({ repo });
        else if (request.url == "https://api.github.com/users/freepats/repos?type=owner&sort=full_name&per_page=50&page=1"
                 || request.url == "https://api.github.com/users/sgossner/repos?type=owner&sort=full_name&per_page=50&page=1")
        {
            auto extra = repo;
            const bool freepats = request.url.find ("/freepats/") != std::string::npos;
            extra["owner"]["login"] = freepats ? "freepats" : "sgossner";
            extra["name"] = freepats ? "FreePats-Piano" : "VCSL";
            reply = Json::array ({ extra });
        }
        else if (request.url == "https://musical-artifacts.com/artifacts.json?formats=sfz&page=1&q="
                 || request.url == "https://musical-artifacts.com/artifacts.json?formats=sfz&page=1&q=Piano")
        {
            pianoSearched.store (request.url.find ("q=Piano") != std::string::npos, std::memory_order_release);
            reply = Json::array ({ { { "id", std::stoull (packId) }, { "name", "Community Piano" },
                                    { "author", "Fixture author" }, { "license", "cc-zero" },
                                    { "formats", Json::array ({ "sfz" }) } } });
        }
        else if (request.url == api) reply = repo;
        else if (request.url == api + "/commits/HEAD")
            reply = { { "sha", commit }, { "commit", { { "tree", { { "sha", tree } } } } } };
        else if (request.url == api + "/git/trees/" + tree + "?recursive=1")
            reply = { { "truncated", false }, { "tree", Json::array ({
                { { "path", "LICENSE" }, { "type", "blob" }, { "mode", "100644" } },
                { { "path", "Instruments/One.sfz" }, { "type", "blob" }, { "mode", "100644" } },
                { { "path", "Instruments/Two.sfz" }, { "type", "blob" }, { "mode", "100644" } },
                { { "path", "Samples/tone.wav" }, { "type", "blob" }, { "mode", "100644" } }
            }) } };
        else if (request.url == api + "/license?ref=" + commit)
            reply = { { "path", "LICENSE" }, { "encoding", "base64" }, { "content", base64 (kCc0) },
                      { "license", { { "spdx_id", "CC0-1.0" } } } };
        else return { OperationStatus::Failed, "Unexpected fixture URL: " + request.url };
        if (! headers ({ 200, request.url, { { "content-type", "application/json" } } })) return stopped();
        const auto bytes = reply.dump();
        if (! body (bytes)) return stopped();
        return { OperationStatus::Complete, {} };
    }

    std::string artworkUrl() const { return "https://raw.githubusercontent.com/sfzinstruments/sfzinstruments.github.io/master/assets/img/woodwinds/" + name + ".png"; }

    void release()
    {
        holdArchive.store (false, std::memory_order_release);
        barrier.notify_all();
    }

    std::string name, packId, commit, tree, api, source, archiveUrl, archive;
    std::atomic<unsigned> requests { 0 }, rangedRequests { 0 }, artworkRequests { 0 };
    std::atomic<bool> pianoSearched { false };
    std::atomic<bool> networkFailure { false }, holdArchive { false }, archiveEntered { false };
    std::mutex barrierMutex;
    std::condition_variable barrier;
};

struct CaseState
{
    std::shared_ptr<RepositoryTransport> transport;
    fs::path restore, originalDirectory, blank, capture, onlineCapture, compactCapture;
    fs::path catalogCache, catalogBackup;
    std::vector<int> originalWindow;
    float originalUiScale = 1.0f;
    std::int64_t originalPlayhead = 0;
    std::uint64_t generation = 0, cloneGeneration = 0;
    unsigned requests = 0;
    std::size_t initialInstalled = 0;
    bool injected = false, restored = false, finished = false, rendering = false;
    bool compactRequired = false;
    bool catalogIsolated = false, catalogBackedUp = false;
    std::optional<InstalledPack> installed;
    bool replacementDone = false, replacementSucceeded = true;
    unsigned cancelledLoadCompletions = 0;
    bool otherLoadDone = false, otherLoadSucceeded = false;
    std::string replacementError;
    std::string verificationReceipt, verificationSample;
};

bool fixturePack (const InstalledPack& pack, const CaseState& state)
{
    return pack.provider.sourceId == "sfzinstruments" && pack.provider.packId == state.transport->packId;
}

std::string restoreCatalogCache (ScenarioContext& ctx, CaseState& state)
{
    if (! state.catalogIsolated) return {};
    if (ctx.engine().getSfzLibrary().snapshot().busy) return "SFZ worker was still busy during catalog cache restoration";
    std::error_code error;
    const auto status = fs::symlink_status (state.catalogCache, error);
    if (error && error != std::errc::no_such_file_or_directory) return "Cannot inspect fixture catalog cache: " + error.message();
    if (fs::exists (status) && ! fs::is_regular_file (status)) return "Fixture catalog cache is no longer a regular file";
    error.clear();
    fs::remove (state.catalogCache, error);
    if (error) return "Cannot remove fixture catalog cache: " + error.message();
    if (state.catalogBackedUp)
    {
        fs::rename (state.catalogBackup, state.catalogCache, error);
        if (error) return "Cannot restore original catalog cache: " + error.message();
    }
    state.catalogIsolated = state.catalogBackedUp = false;
    return {};
}

std::shared_ptr<CaseState> prepare (GuiHost& host, ScenarioContext& ctx, const char* name)
{
    auto state = std::make_shared<CaseState>();
    state->transport = std::make_shared<RepositoryTransport> (ctx.tempDir().string() + name);
    state->restore = ctx.tempDir() / "restore.json";
    state->blank = ctx.tempDir() / "blank.json";
    state->capture = ctx.tempDir() / "sfz-library.png";
    state->onlineCapture = ctx.tempDir() / "sfz-online.png";
    state->compactCapture = ctx.tempDir() / "sfz-online-compact.png";
    const auto catalogKey = hash (Json { { "url", kCatalogUrl },
        { "headers", HttpHeaders { { "accept-encoding", "identity" } } } }.dump());
    state->catalogCache = dusk::fs::appDataDir() / "sfz-cache" / "repository" / (catalogKey + ".json");
    state->catalogBackup = state->catalogCache.parent_path()
        / (".scenario-catalog-" + hash (ctx.tempDir().string() + name).substr (0, 32));
    state->originalWindow = host.mainWindowSize();
    state->originalUiScale = static_cast<float> (host.uiScale());
    state->originalDirectory = currentSessionDirectory (ctx.session());
    state->originalPlayhead = ctx.engine().getTransport().getPlayhead();
    if (! ctx.expect (! state->transport->archive.empty(), "could not make the licensed SFZ ZIP fixture")
        || ! ctx.expect (SessionSerializer::save (ctx.session(), state->restore), "could not save the original session")) return {};
    const auto blank = std::make_unique<Session>();
    if (! ctx.expect (SessionSerializer::save (*blank, state->blank), "could not save a blank session")) return {};
    keepStage (host, ctx);
    // The shared catalog outlives its transport; retain it only within this fixture's lifetime.
    ctx.cleanup ([&ctx, state]
    {
        const auto error = restoreCatalogCache (ctx, *state);
        if (! error.empty()) ctx.note (error);
    });
    ctx.cleanup ([&host, &ctx, state]
    {
        host.closeSfzLibrary (kStripIndex);
        host.captureSfzSources (kStripIndex, false);
        if (auto* strip = host.strip (kStripIndex)) strip->closeEditor();
        if (auto* strip = host.strip (1)) strip->closeEditor();
        host.restoreUiScale (state->originalUiScale);
        if (state->originalWindow.size() >= 2)
            host.resizeMainWindow (state->originalWindow[0], state->originalWindow[1]);
        auto& engine = ctx.engine();
        auto& service = engine.getSfzLibrary();
        if (state->rendering) { engine.setAssetRenderActive (false); state->rendering = false; }
        engine.stop();
        if (state->finished) return;
        service.cancel();
        state->transport->release();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (2);
        while (service.snapshot().busy && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        drainModals (host);
        if (! state->restored)
        {
            reopenSavedSession (host, state->restore);
            applySessionDirectory (ctx.session(), state->originalDirectory);
            engine.getTransport().locate (state->originalPlayhead);
        }
        PackStore store (defaultPackStoreRoot());
        for (const auto& pack : service.snapshot().installed)
            if (fixturePack (pack, *state))
            {
                const auto result = store.remove (pack, engine.managedSfzInUse());
                if (! result) ctx.note ("Emergency SFZ pack cleanup: " + result.error);
            }
        if (state->injected && ! service.setTransportForTest ({}))
            ctx.note ("Emergency SFZ transport reset was still busy");
    });
    return state;
}

void pushClick (std::vector<Step>& steps, GuiHost& host, ScenarioContext& ctx, std::string control)
{
    // Tabs require a hovered frame before their overlapping hit target accepts a press.
    steps.push_back ({ 0, [] {}, [&host, control] { return host.sfzLibraryPointer (kStripIndex, control, 0.5f, false); },
                      "SFZ control did not appear: " + control, {}, kSettleBoundMs });
    steps.push_back ({ 0, [] {}, [&host, control] { return host.sfzLibraryPointer (kStripIndex, control, 0.5f, true); },
                      "SFZ control did not appear: " + control, {}, kSettleBoundMs });
    steps.push_back ({ 0, [&host, &ctx, control]
    { ctx.expect (host.sfzLibraryPointer (kStripIndex, control, 0.5f, false), "SFZ control release failed: " + control); } });
}

void pushCategory (std::vector<Step>& steps, GuiHost& host, ScenarioContext& ctx, const std::string& category,
                   std::function<std::size_t()> expectedCount = {})
{
    const auto compact = std::make_shared<bool> (false);
    steps.push_back ({ 0, [&host, compact]
    { *compact = host.sfzLibraryPointer (kStripIndex, "category-filter", 0.5f, false); } });
    steps.push_back ({ 0, [&host, compact]
    { if (*compact) host.sfzLibraryPointer (kStripIndex, "category-filter", 0.5f, true); } });
    steps.push_back ({ 0, [&host, compact]
    { if (*compact) host.sfzLibraryPointer (kStripIndex, "category-filter", 0.5f, false); } });
    if (expectedCount)
        steps.push_back ({ 0, [&host, &ctx, category, expectedCount]
        {
            ctx.expect (host.sfzLibraryPointer (kStripIndex,
                "category-" + category + "-count-" + std::to_string (expectedCount()), 0.5f, false),
                "the Installed category counted SFZ files instead of unique packs");
        } });
    pushClick (steps, host, ctx, "category-" + category);
}

void pushWorkClick (std::vector<Step>& steps, GuiHost& host, ScenarioContext& ctx,
                    const std::shared_ptr<CaseState>& state, const char* control, LibraryState expected)
{
    steps.push_back ({ 0, [&ctx, state] { state->generation = ctx.engine().getSfzLibrary().snapshot().generation; } });
    pushClick (steps, host, ctx, control);
    steps.push_back ({ 0, [&ctx, state, expected]
    {
        const auto snapshot = ctx.engine().getSfzLibrary().snapshot();
        ctx.expect (snapshot.state == expected, "SFZ operation ended in the wrong state: " + snapshot.error);
    }, [&ctx, state]
    {
        const auto snapshot = ctx.engine().getSfzLibrary().snapshot();
        return ! snapshot.busy && snapshot.generation > state->generation;
    }, "SFZ worker did not finish the panel request: " + std::string (control), {}, kSettleBoundMs });
}

void pushStart (std::vector<Step>& steps, GuiHost& host, ScenarioContext& ctx, const std::shared_ptr<CaseState>& state,
                const std::string& searchKey = "Return", bool fromShell = false)
{
    steps.push_back ({ 0, [&host, &ctx, state]
    {
        auto& service = ctx.engine().getSfzLibrary();
        state->initialInstalled = service.snapshot().installed.size();
        state->injected = service.setTransportForTest (state->transport);
        if (! ctx.expect (state->injected, "idle SFZ service rejected the fake transport"))
        { ctx.complete (ctx.verdict()); return; }
        if (! state->catalogIsolated)
        {
            std::error_code error;
            const auto status = fs::symlink_status (state->catalogCache, error);
            if (error == std::errc::no_such_file_or_directory) error.clear();
            if (! ctx.expect (! error && (! fs::exists (status) || fs::is_regular_file (status)),
                              "could not inspect the original catalog cache safely: " + error.message()))
            { ctx.complete (ctx.verdict()); return; }
            if (fs::exists (status))
            {
                const auto backup = fs::symlink_status (state->catalogBackup, error);
                if (error == std::errc::no_such_file_or_directory) error.clear();
                if (! ctx.expect (! error && ! fs::exists (backup), "catalog cache backup path is unavailable"))
                { ctx.complete (ctx.verdict()); return; }
                fs::rename (state->catalogCache, state->catalogBackup, error);
                if (! ctx.expect (! error, "could not quarantine the original catalog cache: " + error.message()))
                { ctx.complete (ctx.verdict()); return; }
                state->catalogBackedUp = true;
            }
            state->catalogIsolated = true;
        }
        ctx.expect (host.openSession (state->blank), "could not open the blank fixture session");
        host.switchToStage (GuiHost::Stage::Mixing);
        host.captureSfzSources (kStripIndex, true);
        ctx.expect (! ctx.engine().getChannelStrip (kStripIndex).isNativeMultisampleLoaded(), "blank strip retained an instrument");
        ctx.expect (host.clickInsert (kStripIndex), "empty strip insert chooser was not reachable");
    }, [&ctx] { return ! ctx.engine().getSfzLibrary().snapshot().busy; }, "SFZ service initialization did not finish" });
    steps.push_back ({ 0, [] {}, [&host] { return host.clickModalButton ("SFZ Manager..."); },
                      "insert chooser did not offer SFZ Manager", {}, kSettleBoundMs });
    steps.push_back ({ 0, [] {}, [&host] { return host.sfzLibraryOpen (kStripIndex); },
                      "SFZ Manager did not open from the empty strip chooser", {}, kSettleBoundMs });
    pushClick (steps, host, ctx, "installed-tab");
    steps.push_back ({ 0, [&host, &ctx, state]
    {
        ctx.expect (state->transport->requests.load() == 0, "opening Installed started an HTTP request");
        ctx.expect (! host.sfzLibraryPointer (kStripIndex, "preview", 0.5f, false), "All instruments displayed a placeholder image in Installed");
    } });
    pushClick (steps, host, ctx, "online-tab");
    steps.push_back ({ 0, [&host, &ctx, state]
    {
        ctx.expect (state->transport->requests.load() == 0, "opening Online started an HTTP request");
        ctx.expect (! host.sfzLibraryPointer (kStripIndex, "preview", 0.5f, false), "All instruments displayed a placeholder image in Online");
    } });
    pushClick (steps, host, ctx, "query");
    steps.push_back ({ 0, [&host, &ctx, state, searchKey, fromShell]
    {
        state->generation = ctx.engine().getSfzLibrary().snapshot().generation;
        if (fromShell)
        {
            ctx.expect (host.sfzLibraryInput (kStripIndex, "focus-out"), "SFZ child focus loss was not delivered");
            for (const char character : std::string ("Piano"))
                ctx.expect (host.pressKey (std::string (1, character), character),
                            "parent window did not give typed text to the SFZ search field");
            ctx.expect (host.pressKey (searchKey), "parent window did not give Enter to the SFZ search field");
        }
        else ctx.expect (host.sfzLibraryInput (kStripIndex, searchKey), "SFZ search key was not delivered");
    } });
    steps.push_back ({ 0, [&ctx, state, fromShell]
    {
        const auto snapshot = ctx.engine().getSfzLibrary().snapshot();
        ctx.expect (snapshot.state == LibraryState::Complete, "SFZ keyboard search failed: " + snapshot.error);
        if (fromShell)
            ctx.expect (state->transport->pianoSearched.load (std::memory_order_acquire),
                        "Enter did not submit the current Piano search text");
    }, [&ctx, state]
    {
        const auto snapshot = ctx.engine().getSfzLibrary().snapshot();
        return ! snapshot.busy && snapshot.generation > state->generation;
    }, "Enter did not submit the SFZ search", {}, kSettleBoundMs });
    steps.push_back ({ 0, [&ctx, state]
    {
        const auto snapshot = ctx.engine().getSfzLibrary().snapshot();
        ctx.expect (snapshot.page.items.size() == 4 && snapshot.page.items.front().packId == state->transport->packId
                    && snapshot.page.items.front().sourceId == "sfzinstruments"
                    && snapshot.page.items[1].sourceId == "musical-artifacts"
                    && snapshot.page.items[2].sourceId == "freepats"
                    && snapshot.page.items.back().sourceId == "versilian",
                    "explicit All repositories search did not combine all fixture sources");
    } });
    for (const std::string source : { "freepats", "versilian" })
    {
        steps.push_back ({ 0, [state] { state->requests = state->transport->requests.load(); } });
        pushClick (steps, host, ctx, "enabled-source");
        pushClick (steps, host, ctx, "source-" + source);
        steps.push_back ({ 0, [&ctx, state]
        { ctx.expect (state->transport->requests.load() == state->requests, "choosing a source started HTTP before Search"); } });
        pushWorkClick (steps, host, ctx, state, "browse", LibraryState::Complete);
        steps.push_back ({ 0, [&ctx, source]
        {
            const auto snapshot = ctx.engine().getSfzLibrary().snapshot();
            ctx.expect (snapshot.page.items.size() == 1 && snapshot.page.items.front().sourceId == source,
                        "the selected additional repository did not narrow the search");
        } });
    }
    pushClick (steps, host, ctx, "enabled-source");
    pushClick (steps, host, ctx, "source-all");
    pushWorkClick (steps, host, ctx, state, "browse", LibraryState::Complete);
    pushWorkClick (steps, host, ctx, state, "online-item-0", LibraryState::Complete);
    steps.push_back ({ 0, [&ctx, state]
    {
        const auto pack = ctx.engine().getSfzLibrary().snapshot().pack;
        if (ctx.expect (pack.has_value(), "selecting the repository did not resolve a pack"))
        {
            ctx.expect (pack->license.id == "CC0-1.0" && pack->license.text == kCc0 && pack->license.path == "LICENSE",
                        "the pack did not verify the actual CC0 license");
            ctx.expect (pack->releaseId == state->transport->commit && pack->expectedSha256.empty() && pack->versionPinned,
                        "GitHub archive verification labels differ from its commit pin");
            ctx.expect (pack->instruments.size() == 2, "the repository did not expose both SFZ paths");
        }
    } });
}

void pushInstalledSelection (std::vector<Step>& steps, GuiHost& host, ScenarioContext& ctx,
                             const std::shared_ptr<CaseState>& state)
{
    pushClick (steps, host, ctx, "installed-tab");
    steps.push_back ({ 0, [] {}, [&host, &ctx, state]
    {
        const auto installed = ctx.engine().getSfzLibrary().snapshot().installed;
        for (std::size_t i = 0; i < installed.size(); ++i)
            if (fixturePack (installed[i], *state))
                return host.sfzLibraryPointer (kStripIndex, "installed-item-" + std::to_string (i), 0.5f, true);
        return false;
    }, "installed SFZ version did not appear", {}, kSettleBoundMs });
    steps.push_back ({ 0, [&host, &ctx, state]
    {
        const auto installed = ctx.engine().getSfzLibrary().snapshot().installed;
        for (std::size_t i = 0; i < installed.size(); ++i)
            if (fixturePack (installed[i], *state))
            {
                state->installed = installed[i];
                ctx.expect (host.sfzLibraryPointer (kStripIndex, "installed-item-" + std::to_string (i), 0.5f, false),
                            "installed SFZ row release failed");
                return;
            }
        ctx.expect (false, "installed SFZ version disappeared");
    } });
}

void pushFinish (std::vector<Step>& steps, GuiHost& host, ScenarioContext& ctx, const std::shared_ptr<CaseState>& state)
{
    steps.push_back ({ 0, [&host, &ctx, state]
    {
        host.closeSfzLibrary (kStripIndex);
        if (auto* strip = host.strip (kStripIndex)) strip->closeEditor();
        if (auto* strip = host.strip (1)) strip->closeEditor();
        host.restoreUiScale (state->originalUiScale);
        if (state->originalWindow.size() >= 2)
            host.resizeMainWindow (state->originalWindow[0], state->originalWindow[1]);
        drainModals (host);
        ctx.engine().stop();
        reopenSavedSession (host, state->restore);
        applySessionDirectory (ctx.session(), state->originalDirectory);
        ctx.engine().getTransport().locate (state->originalPlayhead);
        state->restored = true;
    } });
    steps.push_back ({ 0, [&ctx, state]
    {
        auto& service = ctx.engine().getSfzLibrary();
        for (const auto& pack : service.snapshot().installed)
            if (fixturePack (pack, *state))
            {
                ctx.expect (service.remove (pack, ctx.engine().managedSfzInUse()), "fixture removal was rejected after session restoration");
                return;
            }
    }, [&ctx, state]
    {
        const auto used = ctx.engine().managedSfzInUse();
        return ! ctx.engine().getSfzLibrary().snapshot().busy
            && std::none_of (used.begin(), used.end(), [&state] (const auto& locator) { return locator.packId == state->transport->packId; });
    }, "restored session retained the fixture pack", {}, kSettleBoundMs });
    steps.push_back ({ 0, [&ctx, state]
    {
        auto& service = ctx.engine().getSfzLibrary();
        const auto snapshot = service.snapshot();
        const bool removed = ctx.expect (std::none_of (snapshot.installed.begin(), snapshot.installed.end(), [&state] (const auto& pack)
                    { return fixturePack (pack, *state); }), "fixture pack survived cleanup");
        const bool reset = ctx.expect (service.setTransportForTest ({}), "idle SFZ transport could not be restored");
        state->finished = removed && reset;
    }, [&ctx] { return ! ctx.engine().getSfzLibrary().snapshot().busy; }, "fixture removal did not finish", {}, kSettleBoundMs });
    steps.push_back ({ 0, [&ctx, state]
    {
        const auto error = restoreCatalogCache (ctx, *state);
        ctx.expect (error.empty(), error);
    } });
}

#if DUSKSTUDIO_HAS_MULTISAMPLE
std::optional<ScenarioResult> runSearchKey (GuiHost& host, ScenarioContext& ctx, bool fromShell)
{
    if (! ctx.engine().getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    auto state = prepare (host, ctx, fromShell ? "shell-enter" : "keypad-enter");
    if (! state) return ctx.verdict();
    auto steps = std::make_shared<std::vector<Step>>();
    pushStart (*steps, host, ctx, state, fromShell ? "Return" : "numpad enter", fromShell);
    pushFinish (*steps, host, ctx, state);
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

std::optional<ScenarioResult> runArtworkCategories (GuiHost& host, ScenarioContext& ctx)
{
    if (! ctx.engine().getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    auto state = prepare (host, ctx, "artwork-categories");
    if (! state) return ctx.verdict();
    state->capture = ctx.tempDir() / "sfz-drum-category.png";
    state->compactCapture = ctx.tempDir() / "sfz-all-instruments.png";
    state->onlineCapture = ctx.tempDir() / "sfz-all-online.png";
    auto steps = std::make_shared<std::vector<Step>>();
    pushStart (*steps, host, ctx, state);
    steps->push_back ({ 0, [&ctx, state]
    {
        auto& service = ctx.engine().getSfzLibrary();
        const auto snapshot = service.snapshot();
        if (! ctx.expect (snapshot.pack.has_value(), "artwork fixture pack was not resolved")) return;
        auto pack = *snapshot.pack;
        pack.displayName = "Karoryfer Big Rusty Drums";
        pack.presentation = {};
        state->generation = snapshot.generation;
        ctx.expect (service.install (std::move (pack)), "legacy artwork fixture installation was rejected");
    } });
    steps->push_back ({ 0, [&ctx]
    { ctx.expect (ctx.engine().getSfzLibrary().snapshot().state == LibraryState::Complete, "legacy artwork fixture did not install"); },
    [&ctx, state]
    {
        const auto snapshot = ctx.engine().getSfzLibrary().snapshot();
        return ! snapshot.busy && snapshot.generation > state->generation;
    }, "legacy artwork fixture did not finish", {}, kSettleBoundMs });
    pushClick (*steps, host, ctx, "installed-tab");
    pushCategory (*steps, host, ctx, "drums");
    pushInstalledSelection (*steps, host, ctx, state);
    steps->push_back ({ 0, [&ctx, state]
    {
        ctx.expect (state->installed.has_value() && state->installed->provider.presentation.categoryHint.empty()
            && state->installed->provider.presentation.artworkUrl.empty(), "fixture did not represent an older receipt without artwork metadata");
        state->requests = state->transport->requests.load();
    } });
    steps->push_back ({ 0, [&host] { host.sfzLibraryPointer (kStripIndex, "installed-tab", 0.5f, false); },
    [&host] { return host.sfzLibraryPointer (kStripIndex, "preview-category-drums", 0.5f, false); },
    "generic patch names did not inherit their drum-pack artwork category", {}, kSettleBoundMs });
    steps->push_back ({ 100, [&host, state] { host.captureSfzLibrary (kStripIndex, state->capture); } });
    steps->push_back ({ 0, [] {}, [state] { std::error_code error; return fs::is_regular_file (state->capture, error); },
    "drum category capture did not arrive" });
    pushCategory (*steps, host, ctx, "all", [state] { return state->initialInstalled + 1; });
    steps->push_back ({ 0, [&host, &ctx, state]
    {
        ctx.expect (! host.sfzLibraryPointer (kStripIndex, "preview", 0.5f, false), "All instruments retained the selected drum image");
        ctx.expect (state->transport->requests.load() == state->requests, "installed category selection contacted a repository");
        host.captureSfzLibrary (kStripIndex, state->compactCapture);
    } });
    steps->push_back ({ 0, [] {}, [state] { std::error_code error; return fs::is_regular_file (state->compactCapture, error); },
    "blank Installed preview capture did not arrive" });
    pushClick (*steps, host, ctx, "online-tab");
    pushCategory (*steps, host, ctx, "all");
    steps->push_back ({ 0, [&host, &ctx, state]
    {
        ctx.expect (! host.sfzLibraryPointer (kStripIndex, "preview", 0.5f, false), "All instruments retained an online image");
        host.captureSfzLibrary (kStripIndex, state->onlineCapture);
    } });
    steps->push_back ({ 0, [] {}, [state] { std::error_code error; return fs::is_regular_file (state->onlineCapture, error); },
    "blank Online preview capture did not arrive" });
    pushFinish (*steps, host, ctx, state);
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

std::optional<ScenarioResult> runWorkflow (GuiHost& host, ScenarioContext& ctx)
{
    if (! ctx.engine().getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    auto state = prepare (host, ctx, "workflow");
    if (! state) return ctx.verdict();
    auto steps = std::make_shared<std::vector<Step>>();
    pushStart (*steps, host, ctx, state);
    steps->push_back ({ 0, [] {}, [&host]
    { return host.sfzLibraryPointer (kStripIndex, "preview-source", 0.5f, false); },
    "repository artwork did not replace the generic image", {}, kSettleBoundMs });
    steps->push_back ({ 0, [&ctx, state]
    {
        ctx.expect (state->transport->artworkRequests.load() == 1, "selected repository artwork was not cached after its first request");
        state->requests = state->transport->requests.load();
    } });
    pushCategory (*steps, host, ctx, "piano");
    steps->push_back ({ 0, [&host, &ctx, state]
    {
        ctx.expect (! host.sfzLibraryPointer (kStripIndex, "online-item-3", 0.5f, false), "collection remained visible in Piano category");
        ctx.expect (state->transport->requests.load() == state->requests, "category grouping contacted a repository");
    } });
    pushCategory (*steps, host, ctx, "all");
    pushWorkClick (*steps, host, ctx, state, "online-item-0", LibraryState::Complete);
    pushClick (*steps, host, ctx, "source-link");
    pushClick (*steps, host, ctx, "license-link");
    pushClick (*steps, host, ctx, "demo-0");
    steps->push_back ({ 0, [&host, &ctx, state]
    {
        const auto pack = ctx.engine().getSfzLibrary().snapshot().pack;
        const auto links = host.sfzSourceRequests (kStripIndex);
        if (ctx.expect (pack.has_value() && links.size() == 3, "source and demo buttons did not request exactly three websites"))
        {
            ctx.expect (links[0] == pack->sourceUrl && links[1] == pack->license.sourceUrl && links[2] == kDemoPage,
                        "source button URLs differ from the selected version, verified license or published demo");
            ctx.expect (pack->presentation.stars == 7 && ! pack->presentation.favorites,
                        "the repository's popularity metadata was not retained");
        }
        ctx.expect (! state->transport->archiveEntered.load(), "listening to a demo downloaded the instrument archive");
    }, [&host] { return host.sfzSourceRequests (kStripIndex).size() >= 3; }, "source and demo buttons did not dispatch" });
    steps->push_back ({ 0, [&host, state] { host.captureSfzLibrary (kStripIndex, state->onlineCapture); } });
    steps->push_back ({ 0, [&ctx, state]
    {
        std::error_code error;
        ctx.expect (fs::file_size (state->onlineCapture, error) > 0 && ! error, "online SFZ library capture was not written");
    }, [state] { std::error_code error; return fs::is_regular_file (state->onlineCapture, error); }, "online SFZ library capture did not arrive" });
    steps->push_back ({ 0, [&host] { host.restoreUiScale (1.5f); } });
    steps->push_back ({ 100, [&host, &ctx, state]
    {
        const auto window = host.mainWindowSize();
        if (! ctx.expect (window.size() >= 4, "the main window reported no resize limits")) return;
        const auto width = (std::max) (window[2], (std::min) (820, window[0]));
        const auto height = (std::max) (window[3], (std::min) (680, window[1]));
        state->compactRequired = window[2] <= 820 && window[3] <= 680;
        const auto resized = host.resizeMainWindow (width, height);
        const auto actual = host.mainWindowSize();
        ctx.expect (resized,
                    "the SFZ library could not use a fitting compact main window: requested "
                        + std::to_string (width) + "x" + std::to_string (height) + ", actual "
                        + (actual.size() >= 2 ? std::to_string (actual[0]) + "x" + std::to_string (actual[1]) : "unknown")
                        + ", minimum " + std::to_string (window[2]) + "x" + std::to_string (window[3])
                        + ", scale " + std::to_string (host.uiScale()));
    }, [&host] { return std::abs (host.uiScale() - 1.5) < 0.001; }, "the compact UI scale did not take effect" });
    steps->push_back ({ 0, [&host, &ctx, state]
    {
        if (! host.sfzLibraryPointer (kStripIndex, "category-filter", 0.5f, false))
            ctx.note ("The native window minimum retains category columns at 150% scale; the workflow exercises those columns.");
        host.captureSfzLibrary (kStripIndex, state->compactCapture);
    }, [&host, state]
    {
        return host.sfzLibraryPointer (kStripIndex, "category-filter", 0.5f, false)
            || (! state->compactRequired && host.sfzLibraryPointer (kStripIndex, "category-all", 0.5f, false));
    }, "the resized SFZ browser did not expose usable category controls", {}, kSettleBoundMs });
    steps->push_back ({ 0, [&ctx, state]
    {
        std::error_code error;
        ctx.expect (fs::file_size (state->compactCapture, error) > 0 && ! error, "compact SFZ library capture was not written");
    }, [state] { std::error_code error; return fs::is_regular_file (state->compactCapture, error); }, "compact SFZ library capture did not arrive" });
    pushCategory (*steps, host, ctx, "piano");
    pushCategory (*steps, host, ctx, "all");
    pushWorkClick (*steps, host, ctx, state, "online-item-0", LibraryState::Complete);
    pushWorkClick (*steps, host, ctx, state, "enabled-download", LibraryState::Complete);
    pushInstalledSelection (*steps, host, ctx, state);
    steps->push_back ({ 0, [] {}, [&host]
    { return host.sfzLibraryPointer (kStripIndex, "preview-source", 0.5f, false); },
    "Installed did not reuse cached repository artwork", {}, kSettleBoundMs });
    steps->push_back ({ 0, [&ctx, state]
    { ctx.expect (state->transport->artworkRequests.load() == 1, "Installed fetched artwork instead of reusing its cache"); } });
    pushCategory (*steps, host, ctx, "piano");
    pushInstalledSelection (*steps, host, ctx, state);
    steps->push_back ({ 0, [&ctx, state]
    {
        if (! ctx.expect (state->installed.has_value(), "download was not published")) return;
        ctx.expect (state->installed->archiveSha256 == hash (state->transport->archive), "installed local archive checksum differs from downloaded bytes");
        ctx.expect (state->installed->instruments.size() == 3 && state->installed->programs.size() == 2,
                    "the installed pack did not distinguish its declared programs from internal SFZ files");
        ctx.expect (ctx.engine().getSfzLibrary().snapshot().installed.size() == state->initialInstalled + 1,
                    "download did not add exactly one installed version");
        state->verificationReceipt = fileBytes (state->installed->directory / ".dusk-pack.json");
        state->verificationSample = fileBytes (state->installed->directory / "Samples/tone.wav");
        state->requests = state->transport->requests.load();
    } });
    pushWorkClick (*steps, host, ctx, state, "enabled-verify", LibraryState::Complete);
    steps->push_back ({ 0, [&ctx, state]
    {
        if (! ctx.expect (state->installed.has_value(), "verification lost the installed selection")) return;
        const auto checked = ctx.engine().getSfzLibrary().snapshot();
        ctx.expect (checked.error == "Installed files match the installation record.", "verification did not report a matching record");
        ctx.expect (fileBytes (state->installed->directory / ".dusk-pack.json") == state->verificationReceipt,
                    "verification rewrote the installed receipt");
        auto changed = state->verificationSample;
        if (! ctx.expect (! changed.empty(), "verification fixture sample is empty")) return;
        changed.back() = static_cast<char> (changed.back() ^ 1);
        ctx.expect (dusk::fs::writeStringToFile (state->installed->directory / "Samples/tone.wav", changed),
                    "could not change the verification fixture sample");
    } });
    pushWorkClick (*steps, host, ctx, state, "enabled-verify", LibraryState::Failed);
    steps->push_back ({ 0, [&ctx, state]
    {
        if (! ctx.expect (state->installed.has_value(), "failed verification lost its selection")) return;
        ctx.expect (ctx.engine().getSfzLibrary().snapshot().failureKind == FailureKind::Integrity,
                    "changed installed sample did not report an integrity failure");
        ctx.expect (fileBytes (state->installed->directory / ".dusk-pack.json") == state->verificationReceipt,
                    "failed verification rewrote the receipt");
        ctx.expect (dusk::fs::writeStringToFile (state->installed->directory / "Samples/tone.wav", state->verificationSample),
                    "could not restore the verification fixture sample");
    } });
    pushWorkClick (*steps, host, ctx, state, "enabled-verify", LibraryState::Complete);
    steps->push_back ({ 0, [&ctx, state]
    {
        if (! ctx.expect (state->installed.has_value(), "verification recovery lost its selection")) return;
        ctx.expect (ctx.engine().getSfzLibrary().snapshot().failureKind == FailureKind::None,
                    "successful verification retained the earlier integrity failure");
        auto receipt = Json::parse (state->verificationReceipt);
        receipt.erase ("installed_tree_sha256_v1");
        ctx.expect (dusk::fs::writeStringToFile (state->installed->directory / ".dusk-pack.json", receipt.dump()),
                    "could not prepare the legacy verification receipt");
    } });
    pushWorkClick (*steps, host, ctx, state, "enabled-refresh-installed", LibraryState::Complete);
    pushInstalledSelection (*steps, host, ctx, state);
    steps->push_back ({ 0, [&host, &ctx, state]
    {
        if (! ctx.expect (state->installed.has_value(), "legacy receipt disappeared from Installed")) return;
        ctx.expect (state->installed->installedTreeSha256.empty(), "legacy receipt acquired an invented digest");
        ctx.expect (host.sfzLibraryPointer (kStripIndex, "verify", 0.5f, false)
                    && ! host.sfzLibraryPointer (kStripIndex, "enabled-verify", 0.5f, false),
                    "verification was available for a legacy receipt without a baseline");
        ctx.expect (dusk::fs::writeStringToFile (state->installed->directory / ".dusk-pack.json", state->verificationReceipt),
                    "could not restore the current verification receipt");
        ctx.expect (state->transport->requests.load() == state->requests,
                    "installed verification or refresh contacted a repository");
    }, [&host]
    {
        return host.sfzLibraryPointer (kStripIndex, "enabled-refresh-installed", 0.5f, false)
            && host.sfzLibraryPointer (kStripIndex, "verify", 0.5f, false)
            && ! host.sfzLibraryPointer (kStripIndex, "enabled-verify", 0.5f, false);
    }, "the idle Installed panel did not disable verification for its legacy receipt", {}, kSettleBoundMs });
    pushWorkClick (*steps, host, ctx, state, "enabled-refresh-installed", LibraryState::Complete);
    pushInstalledSelection (*steps, host, ctx, state);
    pushClick (*steps, host, ctx, "installed-query");
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (host.sfzLibraryInput (kStripIndex, "focus-out"), "installed search parent-key replay did not receive focus");
        for (const char c : std::string ("Two"))
            ctx.expect (host.pressKey (std::string (1, c), c), "installed search did not receive text");
    } });
    steps->push_back ({ 100, [&host, &ctx, state]
    {
        const auto installed = ctx.engine().getSfzLibrary().snapshot().installed;
        for (std::size_t i = 0; i < installed.size(); ++i)
            if (fixturePack (installed[i], *state))
            {
                ctx.expect (host.sfzLibraryPointer (kStripIndex, "installed-item-" + std::to_string (i), 0.5f, false),
                            "installed program search lost its containing pack");
            }
        ctx.expect (state->transport->artworkRequests.load() == 1, "installed filtering contacted the artwork source");
    } });
    pushClick (*steps, host, ctx, "installed-query");
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.sfzLibraryInput (kStripIndex, "command + a"), "installed search selection failed"); } });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.sfzLibraryInput (kStripIndex, "backspace"), "installed search clear failed"); } });
    pushClick (*steps, host, ctx, "instrument");
    pushClick (*steps, host, ctx, "instrument-choice-1");
    pushClick (*steps, host, ctx, "load");
    steps->push_back ({ 0, [&host, state]
    {
        host.restoreUiScale (state->originalUiScale);
        if (state->originalWindow.size() >= 2) host.resizeMainWindow (state->originalWindow[0], state->originalWindow[1]);
    } });
    steps->push_back ({ 0, [&host, &ctx, state]
    {
        const auto& slot = ctx.engine().getChannelStrip (kStripIndex).getNativeMultisampleSlot();
        const auto& track = ctx.session().track (kStripIndex);
        const auto expected = state->installed->directory / state->installed->instruments[1].relativePath;
        ctx.expect (sameFile (slot.getLoadedSoundfontPath(), expected),
                    "Load did not use the chosen extracted SFZ path: expected '" + expected.u8string()
                        + "', actual '" + slot.getLoadedSoundfontPath() + "'");
        ctx.expect (sameFile (track.nativeMultisamplePath.toStdString(), expected) && track.nativeMultisampleLocator
                    && *track.nativeMultisampleLocator == state->installed->locatorFor (state->installed->instruments[1]),
                    "loaded SFZ did not persist its portable managed locator");
        if (auto* strip = host.strip (kStripIndex)) strip->closeEditor();
    }, [&ctx, state]
    {
        auto& slot = ctx.engine().getChannelStrip (kStripIndex).getNativeMultisampleSlot();
        return state->installed && slot.getInstance() && slot.getInstance()->hasLoadedRuntime()
            && ctx.session().track (kStripIndex).nativeMultisampleLocator.has_value();
    }, "choosing Load did not produce a native SFZ runtime" });
    steps->push_back ({ 0, [&ctx, state]
    {
        auto& engine = ctx.engine();
        auto& service = engine.getSfzLibrary();
        const auto before = service.snapshot();
        const auto configuration = libraryLocationsFile();
        const bool configured = fs::exists (configuration);
        const auto bytes = fileBytes (configuration);
        const auto instrument = state->installed->directory / "Instruments/Two.sfz";
        const auto samples = state->installed->directory / "Samples/tone.wav";
        const auto instrumentBytes = fileBytes (instrument), sampleBytes = fileBytes (samples);
        const auto destination = ctx.tempDir() / "in-use-library";
        std::error_code error;
        if (! ctx.expect (fs::create_directory (destination, error) && ! error, "could not create the in-use Move destination")) return;
        std::string reason;
        ctx.expect (! engine.changeSfzLibraryLocation (destination, true, reason) && ! reason.empty(),
                    "Move accepted a library with a loaded managed instrument");
        const auto after = service.snapshot();
        ctx.expect (after.generation == before.generation && after.locations.active.root == before.locations.active.root
                    && after.locations.active.marker == before.locations.active.marker,
                    "refused in-use Move changed the active library or submitted worker activity");
        ctx.expect (fs::exists (configuration) == configured && fileBytes (configuration) == bytes,
                    "refused in-use Move changed the saved library configuration");
        ctx.expect (! instrumentBytes.empty() && ! sampleBytes.empty() && fileBytes (instrument) == instrumentBytes
                    && fileBytes (samples) == sampleBytes && fs::is_empty (destination, error) && ! error,
                    "refused in-use Move changed installed files or wrote to its destination");
    } });
    steps->push_back ({ 0, [&ctx]
    {
        CloneTrackAction clone (ctx.session(), ctx.engine(), kStripIndex, 1);
        ctx.expect (clone.perform(), "managed SFZ track could not be cloned");
    } });
    steps->push_back ({ 0, [&host, &ctx, state]
    {
        auto& slot = ctx.engine().getChannelStrip (1).getNativeMultisampleSlot();
        ctx.expect (ctx.session().track (1).nativeMultisampleLocator == ctx.session().track (kStripIndex).nativeMultisampleLocator
                    && sameFile (slot.getLoadedSoundfontPath(), state->installed->directory / "Instruments/Two.sfz"),
                    "asynchronous clone lost its managed version or entrypoint");
        state->cloneGeneration = slot.generation();
        auto* strip = host.strip (1);
        if (ctx.expect (strip != nullptr, "cloned track has no strip"))
        {
            strip->refreshInsertButton();
            ctx.expect (strip->openEditor(), "cloned SFZ editor could not open");
        }
    }, [&ctx]
    {
        auto& slot = ctx.engine().getChannelStrip (1).getNativeMultisampleSlot();
        return ! ctx.engine().hasPendingMultisampleLoads() && slot.getInstance() && slot.getInstance()->hasLoadedRuntime();
    }, "managed SFZ clone did not produce a native runtime" });
    steps->push_back ({ 0, [] {}, [&host] { return host.clickModalButton ("Browse..."); },
                       "cloned SFZ editor did not offer Browse", {}, kSettleBoundMs });
    steps->push_back ({ 0, [&host, &ctx, state]
    {
        if (! ctx.expect (host.focusFileName(), "soundfont browser has no editable file field")) return;
        typeReplacing (host, (state->installed->directory / "Instruments/One.sfz").string());
        ctx.expect (host.clickModalButton ("Open"), "soundfont browser did not offer Open");
    }, [&host] { return ! host.fileBrowserFolder().empty() && ! host.fileBrowserScanning(); },
                       "Browse did not open a settled soundfont browser", {}, kSettleBoundMs });
    steps->push_back ({ 0, [&host, &ctx, state]
    {
        auto& slot = ctx.engine().getChannelStrip (1).getNativeMultisampleSlot();
        const auto& track = ctx.session().track (1);
        const auto expected = state->installed->directory / "Instruments/One.sfz";
        const auto* instance = slot.getInstance();
        ctx.expect (instance != nullptr && instance->hasLoadedRuntime()
                    && sameFile (slot.getLoadedSoundfontPath(), expected), "cloned editor did not load the selected replacement");
        ctx.expect (sameFile (track.nativeMultisamplePath.toStdString(), expected)
                    && track.nativeMultisampleLocator == state->installed->locatorFor (state->installed->instruments[0]),
                    "manual Browse within a managed pack did not rebind its instrument locator");
    }, [&host, &ctx, state]
    {
        auto& slot = ctx.engine().getChannelStrip (1).getNativeMultisampleSlot();
        auto* strip = host.strip (1);
        return ! ctx.engine().hasPendingMultisampleLoads() && slot.generation() > state->cloneGeneration
            && strip != nullptr && strip->hasOpenEditor();
    }, "accepted cloned-editor replacement did not reopen its new editor" });
    steps->push_back ({ 0, [&ctx, state]
    {
        ctx.engine().setAssetRenderActive (true); state->rendering = true;
        const auto& instrument = state->installed->instruments[1];
        const auto path = state->installed->directory / instrument.relativePath;
        const auto locator = state->installed->locatorFor (instrument);
        ctx.expect (ctx.engine().requestNativeMultisampleLoad (1, path, locator,
            [state] (bool, std::string) { ++state->cancelledLoadCompletions; }), "rendering did not accept a queued replacement");
        ctx.expect (ctx.engine().requestNativeMultisampleLoad (2, path, locator,
            [state] (bool success, std::string) { state->otherLoadDone = true; state->otherLoadSucceeded = success; }),
                    "rendering did not accept the other track's independent load");
    } });
    steps->push_back ({ 0, [] {}, [&host] { return host.clickModalButton ("Clear"); },
                       "the old SFZ editor did not offer Clear", {}, kSettleBoundMs });
    steps->push_back ({ 0, [&ctx, state]
    {
        ctx.engine().setAssetRenderActive (false); state->rendering = false;
    }, [&ctx]
    {
        auto& slot = ctx.engine().getChannelStrip (1).getNativeMultisampleSlot();
        return slot.getInstance() && ! slot.getInstance()->hasLoadedRuntime() && slot.getLoadedSoundfontPath().empty()
            && ! ctx.session().track (1).nativeMultisampleLocator;
    }, "Clear did not unload the old instrument and managed locator", {}, kSettleBoundMs });
    steps->push_back ({ 0, [&host, &ctx, state]
    {
        auto& slot = ctx.engine().getChannelStrip (1).getNativeMultisampleSlot();
        ctx.expect (slot.getInstance() && ! slot.getInstance()->hasLoadedRuntime() && slot.getLoadedSoundfontPath().empty()
                    && ctx.session().track (1).nativeMultisamplePath.isEmpty() && ! ctx.session().track (1).nativeMultisampleLocator,
                    "a queued replacement resurrected the instrument after Clear");
        ctx.expect (state->cancelledLoadCompletions == 0, "Clear dispatched a cancelled or duplicate load alert callback");
        ctx.expect (state->otherLoadDone && state->otherLoadSucceeded
                    && ctx.session().track (2).nativeMultisampleLocator == state->installed->locatorFor (state->installed->instruments[1]),
                    "cancelling one track also cancelled the other track's load");
        if (auto* strip = host.strip (1)) strip->closeEditor();
        ctx.expect (host.openSfzLibrary (kStripIndex), "SFZ library could not reopen over its loaded instrument");
    }, [&ctx]
    {
        ctx.engine().serviceMultisampleLoads();
        return ! ctx.engine().hasPendingMultisampleLoads();
    }, "cancelled and independent multisample worker results did not drain" });
    pushInstalledSelection (*steps, host, ctx, state);
    steps->push_back ({ 0, [&ctx, state] { state->generation = ctx.engine().getSfzLibrary().snapshot().generation; } });
    pushClick (*steps, host, ctx, "remove");
    steps->push_back ({ 0, [&host, &ctx, state]
    {
        ctx.expect (ctx.engine().getSfzLibrary().snapshot().generation == state->generation,
                    "Remove submitted deletion of a version used by the current session");
        ctx.expect (fs::is_regular_file (state->installed->directory / "Instruments/Two.sfz"), "used pack was deleted");
        host.captureSfzLibrary (kStripIndex, state->capture);
        ctx.expect (ctx.engine().requestNativeMultisampleLoad (kStripIndex, ctx.tempDir() / "missing.sfz", {},
            [state] (bool success, std::string error)
            { state->replacementSucceeded = success; state->replacementError = std::move (error); state->replacementDone = true; }),
            "missing replacement request was rejected");
    } });
    steps->push_back ({ 0, [&ctx, state]
    {
        auto& slot = ctx.engine().getChannelStrip (kStripIndex).getNativeMultisampleSlot();
        ctx.expect (! state->replacementSucceeded && ! state->replacementError.empty(), "missing replacement did not report failure");
        ctx.expect (slot.getInstance() && slot.getInstance()->hasLoadedRuntime()
                    && sameFile (slot.getLoadedSoundfontPath(), state->installed->directory / "Instruments/Two.sfz"),
                    "failed replacement discarded the previous working instrument");
        ctx.expect (ctx.session().track (kStripIndex).nativeMultisampleLocator
                    && ctx.session().track (kStripIndex).nativeMultisampleLocator->packId == state->transport->packId,
                    "failed replacement changed the managed locator");
        state->transport->networkFailure.store (true, std::memory_order_release);
    }, [state] { return state->replacementDone; }, "missing replacement did not finish" });
    steps->push_back ({ 0, [&ctx, state]
    {
        std::error_code error;
        ctx.expect (fs::file_size (state->capture, error) > 0 && ! error, "native SFZ library capture was not written");
    }, [state] { std::error_code error; return fs::is_regular_file (state->capture, error); }, "native SFZ library capture did not arrive" });
    pushClick (*steps, host, ctx, "online-tab");
    pushWorkClick (*steps, host, ctx, state, "browse", LibraryState::Failed);
    steps->push_back ({ 0, [&ctx, state]
    {
        const auto snapshot = ctx.engine().getSfzLibrary().snapshot();
        ctx.expect (snapshot.page.items.size() == 4 && snapshot.page.items.front().packId == state->transport->packId,
                    "failed refresh discarded the last good repository page");
        ctx.expect (snapshot.error.find ("Retry-After: 25") != std::string::npos,
                    "repository failure discarded actionable retry information");
        state->requests = state->transport->requests.load();
    } });
    pushWorkClick (*steps, host, ctx, state, "cached", LibraryState::Complete);
    pushWorkClick (*steps, host, ctx, state, "online-item-0", LibraryState::Complete);
    steps->push_back ({ 0, [&ctx, state]
    {
        const auto snapshot = ctx.engine().getSfzLibrary().snapshot();
        ctx.expect (snapshot.cachedResponse && snapshot.pack && snapshot.pack->license.text == kCc0
                    && snapshot.pack->releaseId == state->transport->commit, "offline cached resolve lost the verified version or license");
        ctx.expect (state->transport->requests.load() == state->requests, "Browse cached or cached resolve started an HTTP request");
    } });
    pushFinish (*steps, host, ctx, state);
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

std::optional<ScenarioResult> runStorageMove (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("native SFZ library settings are not built");
   #else
    if (! ctx.engine().getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    const auto initial = ctx.engine().getSfzLibrary().snapshot();
    if (initial.busy || ! initial.installed.empty() || ! initial.locations.error.empty())
        return ScenarioResult::skip ("requires an idle, empty SFZ library in a private profile");
    struct StorageState
    {
        fs::path configuration, originalRoot, destination, originalPack;
        std::string bytes, originalMarker, samples;
        bool configured = false, refreshQueued = false;
        std::uint64_t generation = 0;
        std::optional<InstalledPack> before;
        std::shared_ptr<CaseState> fixture;
    };
    const auto storage = std::make_shared<StorageState>();
    storage->configuration = libraryLocationsFile();
    storage->configured = fs::exists (storage->configuration);
    storage->bytes = fileBytes (storage->configuration);
    storage->originalRoot = initial.locations.active.root;
    storage->originalMarker = initial.locations.active.marker;
    storage->destination = ctx.tempDir() / "moved-sfz-library";
    {
        std::error_code error;
        if (! ctx.expect (fs::create_directory (storage->destination, error) && ! error,
                          "could not create the storage Move destination")) return ctx.verdict();
    }
    const auto restoreConfiguration = [&ctx, storage]
    {
        if (storage->refreshQueued) return true;
        auto& service = ctx.engine().getSfzLibrary();
        if (service.snapshot().busy) return false;
        std::error_code error;
        if (storage->configured)
        {
            if (! dusk::fs::writeStringToFile (storage->configuration, storage->bytes)) return false;
        }
        else
        {
            fs::remove (storage->configuration, error);
            if (error) return false;
        }
        storage->generation = service.snapshot().generation;
        storage->refreshQueued = service.requestInstalled();
        return storage->refreshQueued;
    };
    // Registered first so fixture/session cleanup finishes before the configuration refresh.
    ctx.cleanup ([&host, &ctx, storage, restoreConfiguration]
    {
        host.closeAudioSettings();
        if (storage->fixture)
            for (const auto& root : { storage->originalRoot, storage->destination })
            {
                PackStore store (root);
                for (const auto& pack : store.installed())
                    if (fixturePack (pack, *storage->fixture))
                    {
                        const auto removed = store.remove (pack, ctx.engine().managedSfzInUse());
                        if (! removed) ctx.note ("Storage fixture cleanup: " + removed.error);
                    }
            }
        if (! restoreConfiguration()) ctx.note ("Could not restore SFZ library configuration and request its refresh");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (2);
        auto& service = ctx.engine().getSfzLibrary();
        while (service.snapshot().busy && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        if (service.snapshot().busy) ctx.note ("Storage fixture refresh did not finish during cleanup");
    });
    auto state = prepare (host, ctx, "storage-move");
    if (! state) return ctx.verdict();
    storage->fixture = state;
    auto steps = std::make_shared<std::vector<Step>>();
    pushStart (*steps, host, ctx, state);
    steps->push_back ({ 0, [&ctx, state]
    {
        if (! ctx.expect (state->initialInstalled == 0, "private storage fixture no longer has an empty library"))
            ctx.complete (ctx.verdict());
    } });
    pushWorkClick (*steps, host, ctx, state, "enabled-download", LibraryState::Complete);
    pushInstalledSelection (*steps, host, ctx, state);
    steps->push_back ({ 0, [&host, &ctx, state, storage]
    {
        const auto snapshot = ctx.engine().getSfzLibrary().snapshot();
        if (! ctx.expect (state->installed.has_value() && snapshot.installed.size() == 1
                          && fixturePack (snapshot.installed.front(), *state),
                          "storage Move requires only its installed fixture pack"))
        { ctx.complete (ctx.verdict()); return; }
        storage->before = state->installed;
        storage->originalPack = state->installed->directory;
        storage->samples = fileBytes (storage->originalPack / "Samples/tone.wav");
        if (! ctx.expect (state->installed->instruments.size() == 3 && state->installed->programs.size() == 2
                          && ! storage->samples.empty(), "storage fixture did not expose three raw SFZ files, two programs and samples"))
        { ctx.complete (ctx.verdict()); return; }
        host.closeSfzLibrary (kStripIndex);
        ctx.expect (host.openAudioSettings(), "Settings did not open for SFZ library relocation");
    } });
    for (const std::string control : { "tab-sfz-library", "sfz-move", "sfz-folder" })
    {
        steps->push_back ({ 0, [] {}, [&host, control] { return host.pointerAudioSettings (control, 0.5f, false); },
                           "SFZ Settings control did not appear: " + control, {}, kSettleBoundMs });
        steps->push_back ({ 0, [] {}, [&host, control] { return host.clickAudioSettingsControl (control); },
                           "SFZ Settings control could not be clicked: " + control, {}, kSettleBoundMs });
    }
    steps->push_back ({ 0, [&host, &ctx, storage]
    {
        if (! ctx.expect (host.clickFileBrowserControl (true), "SFZ folder browser has no path field")) return;
        typeReplacing (host, storage->destination.string());
        ctx.expect (host.pressPeerKey ("Return"), "SFZ folder browser did not accept the typed path");
    }, [&host] { return ! host.fileBrowserFolder().empty() && ! host.fileBrowserScanning(); },
                       "Settings did not open the SFZ folder browser", {}, kSettleBoundMs });
    steps->push_back ({ 0, [&host, &ctx, state]
    {
        state->generation = ctx.engine().getSfzLibrary().snapshot().generation;
        ctx.expect (host.clickModalButton ("Open"), "SFZ folder browser did not offer Open");
    }, [&host, storage]
    { return sameFile (host.fileBrowserFolder().u8string(), storage->destination) && ! host.fileBrowserScanning(); },
                       "SFZ folder browser did not navigate to the Move destination", {}, kSettleBoundMs });
    steps->push_back ({ 0, [&ctx, state, storage]
    {
        const auto snapshot = ctx.engine().getSfzLibrary().snapshot();
        if (! ctx.expect (snapshot.state == LibraryState::Complete && snapshot.installed.size() == 1,
                          "SFZ storage Move failed: " + snapshot.error))
        { ctx.complete (ctx.verdict()); return; }
        const auto& moved = snapshot.installed.front();
        if (! ctx.expect (fixturePack (moved, *state) && moved.instruments.size() == 3 && moved.programs.size() == 2,
                          "SFZ storage Move changed the fixture identity or program counts"))
        { ctx.complete (ctx.verdict()); return; }
        ctx.expect (sameFile (snapshot.locations.active.root.u8string(), storage->destination)
                    && sameFile (moved.directory.u8string(), storage->destination / "packs"
                        / PackStore::identity (storage->before->locatorFor (storage->before->instruments.front()))),
                    "SFZ storage Move did not publish under the selected folder");
        ctx.expect (moved.archiveSha256 == storage->before->archiveSha256 && moved.provider.releaseId == storage->before->provider.releaseId,
                    "SFZ storage Move changed the archive digest or release identity");
        for (std::size_t i = 0; i < moved.instruments.size(); ++i)
            ctx.expect (moved.locatorFor (moved.instruments[i]) == storage->before->locatorFor (storage->before->instruments[i])
                        && fs::is_regular_file (moved.directory / moved.instruments[i].relativePath),
                        "SFZ storage Move changed or lost a raw instrument locator");
        for (std::size_t i = 0; i < moved.programs.size(); ++i)
            ctx.expect (moved.locatorFor (moved.programs[i]) == storage->before->locatorFor (storage->before->programs[i]),
                        "SFZ storage Move changed a declared program locator");
        ctx.expect (! storage->samples.empty() && fileBytes (moved.directory / "Samples/tone.wav") == storage->samples,
                    "SFZ storage Move changed or lost the instrument samples");
        ctx.expect (! fs::exists (storage->originalPack), "SFZ storage Move retained the original pack copy");
        state->installed = moved;
    }, [&ctx, state]
    {
        const auto snapshot = ctx.engine().getSfzLibrary().snapshot();
        return ! snapshot.busy && snapshot.generation > state->generation;
    }, "SFZ storage Move worker did not finish", {}, kSettleBoundMs });
    steps->push_back ({ 0, [&host] { host.closeAudioSettings(); },
                       [&host] { return host.audioSettingsOpen(); }, "Settings did not return after folder selection", {}, kSettleBoundMs });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.openSfzLibrary (kStripIndex), "SFZ Manager did not reopen after moving the library"); },
    [&host] { return ! host.audioSettingsOpen(); }, "Settings did not close after SFZ storage Move", {}, kSettleBoundMs });
    pushInstalledSelection (*steps, host, ctx, state);
    pushClick (*steps, host, ctx, "load");
    steps->push_back ({ 0, [&host, &ctx, state]
    {
        const auto& slot = ctx.engine().getChannelStrip (kStripIndex).getNativeMultisampleSlot();
        const auto& track = ctx.session().track (kStripIndex);
        ctx.expect (track.nativeMultisampleLocator && std::any_of (state->installed->programs.begin(), state->installed->programs.end(),
            [&] (const auto& instrument)
            { return *track.nativeMultisampleLocator == state->installed->locatorFor (instrument)
                && sameFile (slot.getLoadedSoundfontPath(), state->installed->directory / instrument.relativePath); }),
            "loading the moved pack lost its managed locator or used an old path");
        if (auto* strip = host.strip (kStripIndex)) strip->closeEditor();
    }, [&ctx]
    {
        auto& slot = ctx.engine().getChannelStrip (kStripIndex).getNativeMultisampleSlot();
        return ! ctx.engine().hasPendingMultisampleLoads() && slot.getInstance() && slot.getInstance()->hasLoadedRuntime();
    }, "the moved instrument did not load its sample runtime", {}, kSettleBoundMs });
    pushFinish (*steps, host, ctx, state);
    steps->push_back ({ 0, [&ctx, restoreConfiguration]
    { ctx.expect (restoreConfiguration(), "SFZ library configuration could not be restored after fixture removal"); } });
    steps->push_back ({ 0, [&ctx, storage]
    {
        const auto snapshot = ctx.engine().getSfzLibrary().snapshot();
        ctx.expect (snapshot.state == LibraryState::Complete && snapshot.installed.empty()
                    && snapshot.locations.active.root == storage->originalRoot && snapshot.locations.active.marker == storage->originalMarker,
                    "restored SFZ configuration did not refresh the empty original library");
        ctx.expect (fs::exists (storage->configuration) == storage->configured && fileBytes (storage->configuration) == storage->bytes,
                    "storage scenario did not restore the exact original configuration bytes");
    }, [&ctx, storage]
    {
        const auto snapshot = ctx.engine().getSfzLibrary().snapshot();
        return storage->refreshQueued && ! snapshot.busy && snapshot.generation > storage->generation;
    }, "restored SFZ library configuration did not refresh", {}, kSettleBoundMs });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

std::optional<ScenarioResult> runPauseCancel (GuiHost& host, ScenarioContext& ctx)
{
    if (! ctx.engine().getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    auto state = prepare (host, ctx, "pause-cancel");
    if (! state) return ctx.verdict();
    auto steps = std::make_shared<std::vector<Step>>();
    pushStart (*steps, host, ctx, state);
    steps->push_back ({ 0, [state] { state->transport->holdArchive.store (true, std::memory_order_release); } });
    pushClick (*steps, host, ctx, "enabled-download");
    steps->push_back ({ 0, [&ctx, state]
    {
        ctx.expect (! ctx.engine().getSfzLibrary().setTransportForTest ({}), "busy SFZ service accepted a transport replacement");
        ctx.engine().setAssetRenderActive (true); state->rendering = true;
    }, [&ctx, state]
    {
        return state->transport->archiveEntered.load (std::memory_order_acquire)
            && ctx.engine().getSfzLibrary().snapshot().progress.completedBytes > 0;
    }, "fixture download did not reach its bounded progress barrier", {}, kSettleBoundMs });
    steps->push_back ({ 0, [&host, &ctx, state]
    {
        const auto snapshot = ctx.engine().getSfzLibrary().snapshot();
        ctx.expect (snapshot.installed.size() == state->initialInstalled, "paused archive was published before extraction finished");
        ctx.expect (! ctx.engine().getSfzLibrary().setTransportForTest ({}), "paused SFZ service accepted a transport replacement");
        host.captureSfzLibrary (kStripIndex, state->capture);
        ctx.engine().setAssetRenderActive (false); state->rendering = false;
        state->transport->release();
    }, [&ctx]
    {
        const auto snapshot = ctx.engine().getSfzLibrary().snapshot();
        return ! snapshot.busy && snapshot.state == LibraryState::Paused;
    }, "rendering did not pause the streaming SFZ download", {}, kSettleBoundMs });
    pushWorkClick (*steps, host, ctx, state, "resume", LibraryState::Complete);
    steps->push_back ({ 0, [&ctx, state]
    {
        std::error_code error;
        ctx.expect (fs::file_size (state->capture, error) > 0 && ! error, "paused SFZ library capture was not written");
    }, [state] { std::error_code error; return fs::is_regular_file (state->capture, error); }, "paused SFZ library capture did not arrive" });
    steps->push_back ({ 0, [&ctx, state]
    {
        ctx.expect (state->transport->rangedRequests.load() == 1, "Resume did not request the verified archive suffix");
        ctx.expect (ctx.engine().getSfzLibrary().snapshot().installed.size() == state->initialInstalled + 1,
                    "Resume did not publish exactly one complete pack");
    } });
    pushInstalledSelection (*steps, host, ctx, state);
    pushWorkClick (*steps, host, ctx, state, "remove", LibraryState::Complete);
    steps->push_back ({ 0, [&ctx, state]
    {
        const auto installed = ctx.engine().getSfzLibrary().snapshot().installed;
        ctx.expect (std::none_of (installed.begin(), installed.end(), [&state] (const auto& pack) { return fixturePack (pack, *state); }),
                    "unused installed version was not removed");
        state->transport->archiveEntered.store (false, std::memory_order_release);
        state->transport->holdArchive.store (true, std::memory_order_release);
    } });
    pushClick (*steps, host, ctx, "online-tab");
    pushWorkClick (*steps, host, ctx, state, "online-item-0", LibraryState::Complete);
    pushClick (*steps, host, ctx, "enabled-download");
    steps->push_back ({ 0, [] {}, [&ctx, state]
    {
        return state->transport->archiveEntered.load (std::memory_order_acquire)
            && ctx.engine().getSfzLibrary().snapshot().progress.completedBytes > 0;
    }, "second download did not reach its cancellation barrier", {}, kSettleBoundMs });
    pushClick (*steps, host, ctx, "cancel");
    steps->push_back ({ 0, [&ctx, state]
    {
        state->transport->release();
        const auto snapshot = ctx.engine().getSfzLibrary().snapshot();
        ctx.expect (snapshot.installed.size() == state->initialInstalled, "Cancel published an incomplete archive");
        ctx.expect (! ctx.engine().getSfzLibrary().resume(), "cancelled download can still resume");
    }, [&ctx]
    {
        const auto snapshot = ctx.engine().getSfzLibrary().snapshot();
        return ! snapshot.busy && snapshot.state == LibraryState::Cancelled;
    }, "Cancel did not stop the bounded SFZ transfer", {}, kSettleBoundMs });
    pushFinish (*steps, host, ctx, state);
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

std::optional<ScenarioResult> runRelink (GuiHost& host, ScenarioContext& ctx, bool fail)
{
    if (! ctx.engine().getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    auto state = prepare (host, ctx, fail ? "relink-failure" : "relink-success");
    if (! state) return ctx.verdict();
    struct RelinkState
    {
        ManagedSfzLocator original, replacement;
        std::string encoded, originalPath, choice;
        fs::path target, held;
        bool moved = false;
    };
    auto relink = std::make_shared<RelinkState>();
    ctx.cleanup ([&ctx, relink]
    {
        if (! relink->moved) return;
        std::error_code error;
        fs::rename (relink->held, relink->target, error);
        if (error) ctx.note ("Could not restore the relink fixture: " + error.message());
    });
    auto steps = std::make_shared<std::vector<Step>>();
    pushStart (*steps, host, ctx, state);
    pushWorkClick (*steps, host, ctx, state, "enabled-download", LibraryState::Complete);
    pushInstalledSelection (*steps, host, ctx, state);
    steps->push_back ({ 0, [&host, &ctx, state, relink]
    {
        if (! ctx.expect (state->installed.has_value(), "relink fixture was not installed"))
        { ctx.complete (ctx.verdict()); return; }
        const auto& pack = *state->installed;
        const auto program = std::find_if (pack.programs.begin(), pack.programs.end(),
            [] (const auto& entry) { return entry.relativePath == "Instruments/Two.sfz"; });
        if (! ctx.expect (program != pack.programs.end(), "relink fixture did not declare its second program"))
        { ctx.complete (ctx.verdict()); return; }
        const auto instrument = std::find_if (pack.instruments.begin(), pack.instruments.end(),
            [&] (const auto& entry) { return entry.id == program->id; });
        if (! ctx.expect (instrument != pack.instruments.end(), "the declared program has no native choice"))
        { ctx.complete (ctx.verdict()); return; }
        relink->choice = "instrument-choice-" + std::to_string (std::distance (pack.instruments.begin(), instrument));
        relink->replacement = pack.locatorFor (*program);
        relink->original = relink->replacement;
        relink->original.releaseId = std::string (40, 'a');
        relink->original.archiveSha256 = std::string (64, 'a');
        relink->target = pack.directory / fs::u8path (program->relativePath);
        relink->held = relink->target;
        relink->held += ".relink-held-" + hash (ctx.tempDir().u8string()).substr (0, 12);
        const auto stale = ctx.tempDir() / "old-relink.sfz";
        if (! ctx.expect (dusk::fs::writeStringToFile (stale, "<region> sample=*sine key=60"),
                          "could not write the old relink instrument"))
        { ctx.complete (ctx.verdict()); return; }
        std::string error;
        auto saved = NativeMultisampleSlot::prime (stale, ctx.engine().getCurrentSampleRate(),
                                                   ctx.engine().getCurrentBlockSize(), error);
        if (! ctx.expect (static_cast<bool> (saved), "could not prime the old relink instrument: " + error))
        { ctx.complete (ctx.verdict()); return; }
        saved.instance->getOverrides().masterVolDb.store (-13.0f);
        saved.instance->getOverrides().masterTuneCents.store (27.0f);
        saved.instance->getOverrides().polyphony.store (29);
        std::vector<std::uint8_t> bytes;
        if (! ctx.expect (saved.instance->saveState (bytes), "could not save the relink controls"))
        { ctx.complete (ctx.verdict()); return; }
        saved = {};
        std::error_code removeError;
        if (! ctx.expect (fs::remove (stale, removeError) && ! removeError, "could not remove the old relink instrument"))
        { ctx.complete (ctx.verdict()); return; }
        relink->encoded = dusk::base64::encode (bytes.data(), bytes.size());
        relink->originalPath = stale.u8string();
        auto& track = ctx.session().track (kStripIndex);
        track.mode.store (static_cast<int> (Track::Mode::Midi));
        track.nativeMultisampleLocator = relink->original;
        track.nativeMultisamplePath = relink->originalPath;
        track.nativeMultisampleStateBase64 = relink->encoded;
        host.closeSfzLibrary (kStripIndex);
        ctx.expect (host.openSfzLibrary (kStripIndex), "could not reopen the manager for the offline slot");
    } });
    pushClick (*steps, host, ctx, "installed-tab");
    for (const bool down : { false, true, false })
        steps->push_back ({ 0, [] {}, [&host, &ctx, relink, down]
        {
            const auto installed = ctx.engine().getSfzLibrary().snapshot().installed;
            for (std::size_t i = 0; i < installed.size(); ++i)
            {
                const auto& pack = installed[i];
                const auto& wanted = relink->replacement;
                if (pack.provider.sourceId == wanted.sourceId && pack.provider.packId == wanted.packId
                    && pack.provider.releaseId == wanted.releaseId && pack.archiveSha256 == wanted.archiveSha256)
                    return host.sfzLibraryPointer (kStripIndex, "installed-item-" + std::to_string (i), 0.5f, down);
            }
            return false;
        }, "the exact replacement version was not selectable", {}, kSettleBoundMs });
    pushClick (*steps, host, ctx, "instrument");
    for (const bool down : { false, true, false })
        steps->push_back ({ 0, [] {}, [&host, relink, down]
        { return host.sfzLibraryPointer (kStripIndex, relink->choice, 0.5f, down); },
        "the declared replacement program was not selectable", {}, kSettleBoundMs });
    steps->push_back ({ 0, [&ctx, state, relink, fail]
    {
        state->requests = state->transport->requests.load();
        if (fail)
        {
            std::error_code error;
            fs::rename (relink->target, relink->held, error);
            relink->moved = ! error;
            if (! ctx.expect (! error, "could not make the selected program unavailable: " + error.message()))
                ctx.complete (ctx.verdict());
        }
    } });
    pushClick (*steps, host, ctx, "enabled-relink");
    steps->push_back ({ 0, [&host, &ctx, state, relink, fail]
    {
        const auto& track = ctx.session().track (kStripIndex);
        const auto& slot = ctx.engine().getChannelStrip (kStripIndex).getNativeMultisampleSlot();
        const auto* instance = slot.getInstance();
        if (fail)
        {
            ctx.expect (track.nativeMultisampleLocator == relink->original
                && track.nativeMultisamplePath.toStdString() == relink->originalPath
                && track.nativeMultisampleStateBase64.toStdString() == relink->encoded,
                "failed native relink changed the saved identity or controls");
            ctx.expect (! instance || ! instance->hasLoadedRuntime(), "failed native relink loaded an instrument");
            std::error_code error;
            fs::rename (relink->held, relink->target, error);
            relink->moved = static_cast<bool> (error);
            ctx.expect (! error, "could not restore the selected relink program: " + error.message());
            dismissAlert (host);
        }
        else
        {
            ctx.expect (track.nativeMultisampleLocator == relink->replacement,
                        "native relink did not save the selected program's complete locator");
            ctx.expect (sameFile (track.nativeMultisamplePath.toStdString(), relink->target)
                && sameFile (slot.getLoadedSoundfontPath(), relink->target), "native relink used the old or wrong program path");
            if (ctx.expect (instance && instance->hasLoadedRuntime(), "the relinked runtime is offline"))
            {
                const auto& controls = instance->getOverrides();
                ctx.expect (std::abs (controls.masterVolDb.load() + 13.0f) < 1.0e-6f
                    && std::abs (controls.masterTuneCents.load() - 27.0f) < 1.0e-6f
                    && controls.polyphony.load() == 29, "native relink reset the saved controls");
            }
            ctx.expect (! host.sfzLibraryOpen (kStripIndex), "successful relink left the manager open");
            const auto file = ctx.tempDir() / "relinked.json";
            ctx.expect (SessionSerializer::save (ctx.session(), file), "could not save the relinked session");
            auto reopened = std::make_unique<Session>();
            ctx.expect (SessionSerializer::load (*reopened, file), "could not reopen the relinked session");
            ctx.expect (reopened->track (kStripIndex).nativeMultisampleLocator == relink->replacement
                && reopened->track (kStripIndex).nativeMultisampleStateBase64.toStdString() == relink->encoded,
                "session round trip lost the relinked identity or controls");
        }
        ctx.expect (state->transport->requests.load() == state->requests, "native relink contacted a repository");
    }, [&host, &ctx, fail]
    {
        ctx.engine().serviceMultisampleLoads();
        if (ctx.engine().hasPendingMultisampleLoads()) return false;
        if (fail) return host.modalText().find ("Couldn't relink soundfont") != std::string::npos;
        const auto* instance = ctx.engine().getChannelStrip (kStripIndex).getNativeMultisampleSlot().getInstance();
        return instance && instance->hasLoadedRuntime();
    }, "the native relink did not finish" });
    pushFinish (*steps, host, ctx, state);
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}
#else
std::optional<ScenarioResult> runRelink (GuiHost&, ScenarioContext&, bool) { return ScenarioResult::skip ("SFZ support is not built"); }
std::optional<ScenarioResult> runSearchKey (GuiHost&, ScenarioContext&, bool) { return ScenarioResult::skip ("SFZ support is not built"); }
std::optional<ScenarioResult> runArtworkCategories (GuiHost&, ScenarioContext&) { return ScenarioResult::skip ("SFZ support is not built"); }
std::optional<ScenarioResult> runWorkflow (GuiHost&, ScenarioContext&) { return ScenarioResult::skip ("SFZ support is not built"); }
std::optional<ScenarioResult> runStorageMove (GuiHost&, ScenarioContext&) { return ScenarioResult::skip ("SFZ support is not built"); }
std::optional<ScenarioResult> runPauseCancel (GuiHost&, ScenarioContext&) { return ScenarioResult::skip ("SFZ support is not built"); }
#endif

const ScenarioRegistrar workflow { Scenario {
    "gui.sfz_repository_workflow", { "gui", "sfz", "repository", "multisample" }, Needs::Engine | Needs::Gui,
    {}, {}, 180000, runWorkflow
} };
const ScenarioRegistrar artworkCategories { Scenario {
    "gui.sfz_artwork_categories", { "gui", "sfz", "repository", "multisample" }, Needs::Engine | Needs::Gui,
    {}, {}, 60000, runArtworkCategories
} };
const ScenarioRegistrar storageMove { Scenario {
    "gui.sfz_library_storage_move", { "gui", "sfz", "repository", "multisample", "settings" }, Needs::Engine | Needs::Gui,
    {}, {}, 90000, runStorageMove
} };
const ScenarioRegistrar pauseCancel { Scenario {
    "gui.sfz_repository_pause_cancel", { "gui", "sfz", "repository", "multisample" }, Needs::Engine | Needs::Gui,
    {}, {}, 150000, runPauseCancel
} };
const ScenarioRegistrar relinkInstalled { Scenario {
    "gui.sfz_relink_installed_program", { "gui", "sfz", "repository", "multisample" }, Needs::Engine | Needs::Gui,
    {}, {}, 90000, [] (GuiHost& host, ScenarioContext& ctx) { return runRelink (host, ctx, false); }
} };
const ScenarioRegistrar relinkFailure { Scenario {
    "gui.sfz_failed_relink_keeps_identity", { "gui", "sfz", "repository", "multisample" }, Needs::Engine | Needs::Gui,
    {}, {}, 90000, [] (GuiHost& host, ScenarioContext& ctx) { return runRelink (host, ctx, true); }
} };
const ScenarioRegistrar keypadEnter { Scenario {
    "gui.sfz_search_keypad_enter", { "gui", "sfz", "repository", "multisample" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000, [] (GuiHost& host, ScenarioContext& ctx) { return runSearchKey (host, ctx, false); }
} };
const ScenarioRegistrar shellEnter { Scenario {
    "gui.sfz_search_shell_enter", { "gui", "sfz", "repository", "multisample" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000, [] (GuiHost& host, ScenarioContext& ctx) { return runSearchKey (host, ctx, true); }
} };
} // namespace
} // namespace duskstudio::scenario::gui
