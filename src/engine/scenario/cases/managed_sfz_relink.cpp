#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../sfz/SfzLibraryLocations.h"
#include "../../sfz/SfzLibraryService.h"
#include "../../../foundation/Base64.h"
#include "../../../foundation/Fs.h"
#include "../../../session/SessionSerializer.h"

#if DUSKSTUDIO_HAS_MULTISAMPLE
#include "../../multisample/DuskMultisampleProcessor.h"

#include <sodium.h>

#include <array>
#include <atomic>
#include <cmath>
#include <memory>

namespace duskstudio::scenario
{
namespace
{
constexpr int kTrack = 2;
enum class RelinkResult { Loaded, Missing, Cancelled, Frozen, CancelledBeforeCommit, SupersededBeforeCommit };

class NoNetwork final : public sfz::HttpTransport
{
public:
    std::atomic<int> requests { 0 };
    sfz::OperationResult perform (const sfz::HttpRequest&, const sfz::HttpHeadersCallback&,
        const sfz::HttpBodyCallback&, const sfz::OperationControl&) override
    {
        requests.fetch_add (1);
        return { sfz::OperationStatus::Failed, "Unexpected network request" };
    }
};

std::string hash (const std::string& text)
{
    if (sodium_init() < 0) return {};
    std::array<unsigned char, crypto_hash_sha256_BYTES> bytes {};
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> encoded {};
    crypto_hash_sha256 (bytes.data(), reinterpret_cast<const unsigned char*> (text.data()), text.size());
    sodium_bin2hex (encoded.data(), encoded.size(), bytes.data(), bytes.size());
    return encoded.data();
}

std::optional<ScenarioResult> managedRelink (ScenarioContext& ctx, RelinkResult expected)
{
    auto& engine = ctx.engine();
    auto& library = engine.getSfzLibrary();
    if (engine.getChannelStrip (kTrack).getNativeMultisampleSlot().getInstance() != nullptr)
        return ScenarioResult::skip ("requires an empty soundfont slot");
    auto& originalTrack = ctx.session().track (kTrack);
    const auto previousLocator = originalTrack.nativeMultisampleLocator;
    const auto previousPath = originalTrack.nativeMultisamplePath;
    const auto previousState = originalTrack.nativeMultisampleStateBase64;
    const auto previousMode = originalTrack.mode.load();
    const auto previousFrozen = originalTrack.frozen.load();
    auto network = std::make_shared<NoNetwork>();
    struct State
    {
        bool done = false, success = false, held = false, beforeCalled = false;
        bool nextDone = false, nextSuccess = false;
        std::string error, nextError;
        std::optional<sfz::InstalledPack> installed;
    };
    auto state = std::make_shared<State>();
    ctx.cleanup ([&engine, state, previousLocator, previousPath, previousState, previousMode, previousFrozen]
    {
        engine.cancelNativeMultisampleLoad (kTrack);
        if (state->held) engine.setAssetRenderActive (false);
        engine.suspendProcessing();
        engine.getChannelStrip (kTrack).unloadNativeMultisample();
        engine.resumeProcessing();
        auto& track = engine.getSession().track (kTrack);
        track.nativeMultisampleLocator = previousLocator;
        track.nativeMultisamplePath = previousPath;
        track.nativeMultisampleStateBase64 = previousState;
        track.mode.store (previousMode);
        track.frozen.store (previousFrozen);
        engine.getSfzLibrary().setTransportForTest ({});
        if (state->installed)
        {
            const auto locations = sfz::readLibraryLocations();
            sfz::PackStore (locations.active.root, locations.active.marker).remove (*state->installed, {});
        }
    });
    ctx.waitUntil ([&library] { return ! library.snapshot().busy; }, 5000,
        [&ctx, &engine, &library, network, state, expected]
        {
            if (! ctx.expect (library.setTransportForTest (network), "could not install the no-network transport"))
            { ctx.complete (ctx.verdict()); return; }
            const auto locations = sfz::readLibraryLocations();
            sfz::PackStore store (locations.active.root, locations.active.marker);
            std::string error;
            const auto staging = store.createStaging (error);
            if (! ctx.expect (! staging.empty(), "could not stage the relink fixture: " + error))
            { ctx.complete (ctx.verdict()); return; }
            ctx.expect (dusk::fs::writeStringToFile (staging / "Piano.sfz", "<region> sample=*sine key=60"),
                        "could not write the relink fixture");
            sfz::ProviderPack provider;
            provider.sourceId = "musical-artifacts";
            provider.packId = "994016";
            provider.releaseId = provider.expectedSha256 = std::string (64, 'b');
            provider.displayName = "Relink fixture";
            provider.sourceUrl = "https://musical-artifacts.com/artifacts/994016";
            provider.downloadUrl = provider.sourceUrl + "/piano.zip";
            provider.versionPinned = true;
            const std::string license = "CC0-1.0 https://creativecommons.org/publicdomain/zero/1.0/";
            provider.license = { "CC0-1.0", license, {}, hash (license), provider.sourceUrl, {} };
            provider.instruments.push_back ({ "piano", "Piano", "Piano.sfz", false });
            auto published = store.publish (provider, provider.expectedSha256, staging, { "Piano.sfz" });
            if (! ctx.expect (static_cast<bool> (published), "could not publish the relink fixture: " + published.error))
            { std::error_code ignored; std::filesystem::remove_all (staging, ignored); ctx.complete (ctx.verdict()); return; }
            state->installed = std::move (published.pack);
            const auto replacement = state->installed->locatorFor (state->installed->instruments.front());
            auto original = replacement;
            original.releaseId = original.archiveSha256 = std::string (64, 'a');
            auto& track = ctx.session().track (kTrack);
            track.mode.store ((int) Track::Mode::Midi);
            track.nativeMultisampleLocator = original;
            const auto staleFile = ctx.tempDir() / "old-version.sfz";
            if (! ctx.expect (dusk::fs::writeStringToFile (staleFile, "<region> sample=*sine key=60"),
                              "could not write the old instrument"))
            { ctx.complete (ctx.verdict()); return; }
            auto saved = NativeMultisampleSlot::prime (staleFile, engine.getCurrentSampleRate(),
                                                       engine.getCurrentBlockSize(), error);
            if (! ctx.expect (static_cast<bool> (saved), "could not prime the old instrument: " + error))
            { ctx.complete (ctx.verdict()); return; }
            saved.instance->getOverrides().masterVolDb.store (-13.0f);
            saved.instance->getOverrides().masterTuneCents.store (27.0f);
            saved.instance->getOverrides().polyphony.store (29);
            std::vector<std::uint8_t> bytes;
            if (! ctx.expect (saved.instance->saveState (bytes), "could not save the offline instrument controls"))
            { ctx.complete (ctx.verdict()); return; }
            saved = {};
            std::error_code removeError;
            if (! ctx.expect (std::filesystem::remove (staleFile, removeError) && ! removeError,
                              "could not remove the old instrument"))
            { ctx.complete (ctx.verdict()); return; }
            const auto oldPath = staleFile.u8string();
            track.nativeMultisamplePath = oldPath;
            const auto encoded = dusk::base64::encode (bytes.data(), bytes.size());
            track.nativeMultisampleStateBase64 = encoded;
            auto foreign = replacement;
            foreign.packId = "994017";
            ctx.expect (! engine.requestManagedSfzRelink (kTrack, foreign), "relink accepted another pack");
            auto requested = replacement;
            if (expected == RelinkResult::Missing) requested.releaseId = std::string (64, 'c');
            if (expected == RelinkResult::Cancelled || expected == RelinkResult::Frozen)
            { engine.setAssetRenderActive (true); state->held = true; }
            std::function<void()> beforeCommit;
            if (expected == RelinkResult::CancelledBeforeCommit || expected == RelinkResult::SupersededBeforeCommit)
                beforeCommit = [&ctx, &engine, state, expected, replacement]
                {
                    state->beforeCalled = true;
                    if (expected == RelinkResult::CancelledBeforeCommit) engine.cancelNativeMultisampleLoad (kTrack);
                    else ctx.expect (engine.requestManagedSfzRelink (kTrack, replacement,
                        [state] (bool success, std::string message)
                        { state->nextDone = true; state->nextSuccess = success; state->nextError = std::move (message); }),
                        "could not supersede the prepared relink");
                };
            ctx.expect (engine.requestManagedSfzRelink (kTrack, requested,
                [state] (bool success, std::string message)
                { state->done = true; state->success = success; state->error = std::move (message); }, std::move (beforeCommit)),
                "the offline slot did not accept an explicit relink");
            if (expected == RelinkResult::Cancelled || expected == RelinkResult::Frozen)
            {
                if (expected == RelinkResult::Cancelled) engine.cancelNativeMultisampleLoad (kTrack);
                else track.frozen.store (true);
                engine.setAssetRenderActive (false);
                state->held = false;
            }
            ctx.waitUntil ([&engine, state, expected]
            {
                engine.serviceMultisampleLoads();
                const bool finished = expected == RelinkResult::SupersededBeforeCommit ? state->nextDone
                    : expected == RelinkResult::Cancelled || expected == RelinkResult::CancelledBeforeCommit || state->done;
                return ! engine.hasPendingMultisampleLoads() && finished;
            }, 10000, [&ctx, &engine, network, state, expected, replacement, original, encoded, oldPath]
            {
                auto& current = ctx.session().track (kTrack);
                if (expected == RelinkResult::Loaded || expected == RelinkResult::SupersededBeforeCommit)
                {
                    if (expected == RelinkResult::SupersededBeforeCommit)
                    {
                        ctx.expect (state->beforeCalled && state->done && ! state->success,
                                    "the superseded relink committed after its callback");
                        ctx.expect (state->nextSuccess, "the newer relink was discarded: " + state->nextError);
                    }
                    else ctx.expect (state->success, "explicit relink failed: " + state->error);
                    ctx.expect (current.nativeMultisampleLocator == replacement, "relink did not replace the saved identity");
                    const auto resolvedPath = dusk::fs::nativeIOPath (state->installed->directory / "Piano.sfz");
                    const auto savedPath = dusk::fs::nativeIOPath (std::filesystem::u8path (current.nativeMultisamplePath.toStdString()));
                    std::error_code savedPathError;
                    ctx.expect (std::filesystem::equivalent (savedPath, resolvedPath, savedPathError) && ! savedPathError,
                                "relink did not save the installed instrument path");
                    const auto* instance = engine.getChannelStrip (kTrack).getNativeMultisampleSlot().getInstance();
                    if (ctx.expect (instance && instance->hasLoadedRuntime(), "relinked instrument is offline"))
                    {
                        const auto loadedPath = dusk::fs::nativeIOPath (std::filesystem::u8path (instance->getLoadedPathSnapshot()));
                        std::error_code loadedPathError;
                        ctx.expect (std::filesystem::equivalent (loadedPath, resolvedPath, loadedPathError) && ! loadedPathError,
                                    "relink loaded the path from the stale state blob");
                        const auto& controls = instance->getOverrides();
                        ctx.expect (std::abs (controls.masterVolDb.load() + 13.0f) < 1.0e-6f
                            && std::abs (controls.masterTuneCents.load() - 27.0f) < 1.0e-6f
                            && controls.polyphony.load() == 29, "relink reset the saved instrument controls");
                    }
                    const auto file = ctx.tempDir() / "relinked-session.json";
                    ctx.expect (SessionSerializer::save (ctx.session(), file), "could not save the relinked session");
                    auto reopened = std::make_unique<Session>();
                    ctx.expect (SessionSerializer::load (*reopened, file), "could not reopen the relinked session");
                    ctx.expect (reopened->track (kTrack).nativeMultisampleLocator == replacement,
                                "reopening restored the unavailable version");
                    ctx.expect (reopened->track (kTrack).nativeMultisampleStateBase64.toStdString() == encoded,
                                "reopening lost the saved controls");
                }
                else
                {
                    ctx.expect (! state->success, "an unavailable or cancelled relink succeeded");
                    ctx.expect (current.nativeMultisampleLocator == original, "failed relink replaced the old identity");
                    ctx.expect (current.nativeMultisamplePath.toStdString() == oldPath, "failed relink changed the saved path");
                    ctx.expect (current.nativeMultisampleStateBase64.toStdString() == encoded, "failed relink changed saved controls");
                    if (expected == RelinkResult::CancelledBeforeCommit)
                        ctx.expect (state->beforeCalled, "the cancellation did not reach the commit callback");
                }
                ctx.expect (network->requests.load() == 0, "relink or session restoration contacted a repository");
                ctx.complete (ctx.verdict());
            }, "the explicit relink did not finish");
        }, "the relink scenario found a busy library");
    return std::nullopt;
}

const ScenarioRegistrar relinkRegistrar { Scenario {
    "sfz.offline_slot_relinks_to_installed_version", { "sfz", "session" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return managedRelink (ctx, RelinkResult::Loaded); } } };
const ScenarioRegistrar missingRegistrar { Scenario {
    "sfz.failed_relink_keeps_offline_identity", { "sfz", "session" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return managedRelink (ctx, RelinkResult::Missing); } } };
const ScenarioRegistrar cancelledRegistrar { Scenario {
    "sfz.cancelled_relink_keeps_offline_identity", { "sfz", "session" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return managedRelink (ctx, RelinkResult::Cancelled); } } };
const ScenarioRegistrar frozenRegistrar { Scenario {
    "sfz.relink_refuses_a_track_frozen_while_loading", { "sfz", "session" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return managedRelink (ctx, RelinkResult::Frozen); } } };
const ScenarioRegistrar commitCancelRegistrar { Scenario {
    "sfz.relink_callback_cannot_commit_a_cancelled_load", { "sfz", "session" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return managedRelink (ctx, RelinkResult::CancelledBeforeCommit); } } };
const ScenarioRegistrar commitSupersedeRegistrar { Scenario {
    "sfz.relink_callback_preserves_a_newer_load", { "sfz", "session" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return managedRelink (ctx, RelinkResult::SupersededBeforeCommit); } } };
} // namespace
} // namespace duskstudio::scenario
#endif
