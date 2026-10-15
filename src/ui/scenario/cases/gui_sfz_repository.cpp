#include "../GuiScenarioSupport.h"
#include "../../../engine/sfz/SfzLibraryService.h"
#include "../../../foundation/Json.h"
#include "../../../session/RegionEditActions.h"
#include "../../../session/SessionSerializer.h"

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
        if (request.url == "https://raw.githubusercontent.com/sfzinstruments/sfzinstruments.github.io/master/data/sfz/instruments.yml"
            || request.url == kDemoPage)
        {
            const bool page = request.url == kDemoPage;
            const auto bytes = page ? std::string ("<audio src=\"/assets/audio/woodwinds/dusk_gui.mp3\" controls></audio>")
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
        const Json repo { { "id", std::stoull (packId) }, { "name", name }, { "description", "Two CC0 SFZ fixture instruments" },
                          { "stargazers_count", 7 },
                          { "owner", { { "login", "sfzinstruments" } } }, { "private", false }, { "disabled", false },
                          { "license", { { "spdx_id", "CC0-1.0" } } } };
        if (request.url == "https://api.github.com/orgs/sfzinstruments/repos?type=public&sort=full_name&per_page=50&page=1") reply = Json::array ({ repo });
        else if (request.url == "https://musical-artifacts.com/artifacts.json?formats=sfz&page=1&q=")
            reply = Json::array ({ { { "id", std::stoull (packId) }, { "name", "Community Piano" },
                                    { "author", "Fixture author" }, { "license", "cc-zero" },
                                    { "formats", Json::array ({ "sfz" }) } } });
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

    void release()
    {
        holdArchive.store (false, std::memory_order_release);
        barrier.notify_all();
    }

    std::string name, packId, commit, tree, api, source, archiveUrl, archive;
    std::atomic<unsigned> requests { 0 }, rangedRequests { 0 };
    std::atomic<bool> networkFailure { false }, holdArchive { false }, archiveEntered { false };
    std::mutex barrierMutex;
    std::condition_variable barrier;
};

struct CaseState
{
    std::shared_ptr<RepositoryTransport> transport;
    fs::path restore, originalDirectory, blank, capture, onlineCapture, compactCapture;
    std::vector<int> originalWindow;
    float originalUiScale = 1.0f;
    std::int64_t originalPlayhead = 0;
    std::uint64_t generation = 0, cloneGeneration = 0;
    unsigned requests = 0;
    std::size_t initialInstalled = 0;
    bool injected = false, restored = false, finished = false, rendering = false;
    std::optional<InstalledPack> installed;
    bool replacementDone = false, replacementSucceeded = true;
    unsigned cancelledLoadCompletions = 0;
    bool otherLoadDone = false, otherLoadSucceeded = false;
    std::string replacementError;
};

bool fixturePack (const InstalledPack& pack, const CaseState& state)
{
    return pack.provider.sourceId == "sfzinstruments" && pack.provider.packId == state.transport->packId;
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
    state->originalWindow = host.mainWindowSize();
    state->originalUiScale = static_cast<float> (host.uiScale());
    state->originalDirectory = currentSessionDirectory (ctx.session());
    state->originalPlayhead = ctx.engine().getTransport().getPlayhead();
    if (! ctx.expect (! state->transport->archive.empty(), "could not make the licensed SFZ ZIP fixture")
        || ! ctx.expect (SessionSerializer::save (ctx.session(), state->restore), "could not save the original session")) return {};
    const auto blank = std::make_unique<Session>();
    if (! ctx.expect (SessionSerializer::save (*blank, state->blank), "could not save a blank session")) return {};
    keepStage (host, ctx);
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
    }, "SFZ worker did not finish the panel request", {}, kSettleBoundMs });
}

void pushStart (std::vector<Step>& steps, GuiHost& host, ScenarioContext& ctx, const std::shared_ptr<CaseState>& state)
{
    steps.push_back ({ 0, [&host, &ctx, state]
    {
        auto& service = ctx.engine().getSfzLibrary();
        state->initialInstalled = service.snapshot().installed.size();
        state->injected = service.setTransportForTest (state->transport);
        if (! ctx.expect (state->injected, "idle SFZ service rejected the fake transport"))
        { ctx.complete (ctx.verdict()); return; }
        ctx.expect (host.openSession (state->blank), "could not open the blank fixture session");
        host.switchToStage (GuiHost::Stage::Mixing);
        host.captureSfzSources (kStripIndex, true);
        ctx.expect (! ctx.engine().getChannelStrip (kStripIndex).isNativeMultisampleLoaded(), "blank strip retained an instrument");
        ctx.expect (host.clickInsert (kStripIndex), "empty strip insert chooser was not reachable");
    }, [&ctx] { return ! ctx.engine().getSfzLibrary().snapshot().busy; }, "SFZ service initialization did not finish" });
    steps.push_back ({ 0, [] {}, [&host] { return host.clickModalButton ("SFZ Library..."); },
                      "insert chooser did not offer SFZ Library", {}, kSettleBoundMs });
    steps.push_back ({ 0, [] {}, [&host] { return host.sfzLibraryOpen (kStripIndex); },
                      "SFZ Library did not open from the empty strip chooser", {}, kSettleBoundMs });
    pushClick (steps, host, ctx, "installed-tab");
    steps.push_back ({ 0, [&ctx, state]
    { ctx.expect (state->transport->requests.load() == 0, "opening Installed started an HTTP request"); } });
    pushClick (steps, host, ctx, "online-tab");
    steps.push_back ({ 0, [&ctx, state]
    { ctx.expect (state->transport->requests.load() == 0, "opening Online started an HTTP request"); } });
    pushWorkClick (steps, host, ctx, state, "browse", LibraryState::Complete);
    steps.push_back ({ 0, [&ctx, state]
    {
        const auto snapshot = ctx.engine().getSfzLibrary().snapshot();
        ctx.expect (snapshot.page.items.size() == 2 && snapshot.page.items.front().packId == state->transport->packId
                    && snapshot.page.items.front().sourceId == "sfzinstruments"
                    && snapshot.page.items.back().sourceId == "musical-artifacts",
                    "explicit All repositories search did not combine both fixture sources");
    } });
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
}

#if DUSKSTUDIO_HAS_MULTISAMPLE
std::optional<ScenarioResult> runWorkflow (GuiHost& host, ScenarioContext& ctx)
{
    if (! ctx.engine().getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    auto state = prepare (host, ctx, "workflow");
    if (! state) return ctx.verdict();
    auto steps = std::make_shared<std::vector<Step>>();
    pushStart (*steps, host, ctx, state);
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
    steps->push_back ({ 100, [&host, &ctx]
    {
        const auto window = host.mainWindowSize();
        if (! ctx.expect (window.size() >= 4, "the main window reported no resize limits")) return;
        const auto width = (std::max) (window[2], (std::min) (820, window[0]));
        const auto height = (std::max) (window[3], (std::min) (680, window[1]));
        const auto resized = host.resizeMainWindow (width, height);
        const auto actual = host.mainWindowSize();
        ctx.expect (resized,
                    "the SFZ library could not use a fitting compact main window: requested "
                        + std::to_string (width) + "x" + std::to_string (height) + ", actual "
                        + (actual.size() >= 2 ? std::to_string (actual[0]) + "x" + std::to_string (actual[1]) : "unknown")
                        + ", minimum " + std::to_string (window[2]) + "x" + std::to_string (window[3])
                        + ", scale " + std::to_string (host.uiScale()));
    }, [&host] { return std::abs (host.uiScale() - 1.5) < 0.001; }, "the compact UI scale did not take effect" });
    steps->push_back ({ 150, [&host, state] { host.captureSfzLibrary (kStripIndex, state->compactCapture); } });
    steps->push_back ({ 0, [&ctx, state]
    {
        std::error_code error;
        ctx.expect (fs::file_size (state->compactCapture, error) > 0 && ! error, "compact SFZ library capture was not written");
    }, [state] { std::error_code error; return fs::is_regular_file (state->compactCapture, error); }, "compact SFZ library capture did not arrive" });
    pushWorkClick (*steps, host, ctx, state, "download", LibraryState::Complete);
    pushInstalledSelection (*steps, host, ctx, state);
    steps->push_back ({ 0, [&ctx, state]
    {
        if (! ctx.expect (state->installed.has_value(), "download was not published")) return;
        ctx.expect (state->installed->archiveSha256 == hash (state->transport->archive), "installed local archive checksum differs from downloaded bytes");
        ctx.expect (state->installed->instruments.size() == 2, "installed SFZ choices do not match extracted files");
        ctx.expect (ctx.engine().getSfzLibrary().snapshot().installed.size() == state->initialInstalled + 1,
                    "download did not add exactly one installed version");
    } });
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
        ctx.expect (snapshot.page.items.size() == 2 && snapshot.page.items.front().packId == state->transport->packId,
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

std::optional<ScenarioResult> runPauseCancel (GuiHost& host, ScenarioContext& ctx)
{
    if (! ctx.engine().getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    auto state = prepare (host, ctx, "pause-cancel");
    if (! state) return ctx.verdict();
    auto steps = std::make_shared<std::vector<Step>>();
    pushStart (*steps, host, ctx, state);
    steps->push_back ({ 0, [state] { state->transport->holdArchive.store (true, std::memory_order_release); } });
    pushClick (*steps, host, ctx, "download");
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
    pushClick (*steps, host, ctx, "download");
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
#else
std::optional<ScenarioResult> runWorkflow (GuiHost&, ScenarioContext&) { return ScenarioResult::skip ("SFZ support is not built"); }
std::optional<ScenarioResult> runPauseCancel (GuiHost&, ScenarioContext&) { return ScenarioResult::skip ("SFZ support is not built"); }
#endif

const ScenarioRegistrar workflow { Scenario {
    "gui.sfz_repository_workflow", { "gui", "sfz", "repository", "multisample" }, Needs::Engine | Needs::Gui,
    {}, {}, 180000, runWorkflow
} };
const ScenarioRegistrar pauseCancel { Scenario {
    "gui.sfz_repository_pause_cancel", { "gui", "sfz", "repository", "multisample" }, Needs::Engine | Needs::Gui,
    {}, {}, 150000, runPauseCancel
} };
} // namespace
} // namespace duskstudio::scenario::gui
