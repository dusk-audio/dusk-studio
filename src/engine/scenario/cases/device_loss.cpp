#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../RecordManager.h"
#include "../../device/DeviceManager.h"
#include "../../device/IODevice.h"
#include "../../sfz/SfzLibraryService.h"
#include "../../../foundation/Fs.h"
#include "../../../session/Session.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace duskstudio::scenario
{
namespace
{
constexpr int kTrack = 3;
constexpr std::int64_t kTakeStart = 4800;

// An endpoint that reports itself and never streams. audioDeviceAboutToStart
// only reads what the device says about itself, so this is enough to put the
// engine into the live-device state a disconnect is detected against - and the
// suite must never open real hardware.
class StubDevice final : public device::IODevice
{
public:
    StubDevice (double sampleRate, int blockSize)
        : rate (sampleRate), block (blockSize)
    {
        ins.setRange (0, kNumInputs, true);
        outs.setRange (0, 2, true);
    }

    std::string getName() const override { return "Scenario Stub"; }

    std::vector<std::string> getOutputChannelNames() override { return { "out 1", "out 2" }; }
    std::vector<std::string> getInputChannelNames() override
    {
        std::vector<std::string> names;
        for (int i = 0; i < kNumInputs; ++i)
            names.push_back ("in " + std::to_string (i + 1));
        return names;
    }
    std::vector<double> getAvailableSampleRates() override { return { rate }; }
    std::vector<int>    getAvailableBufferSizes() override { return { block }; }
    int                 getDefaultBufferSize() override    { return block; }

    std::string open (const device::ChannelSet&, const device::ChannelSet&, double, int) override
    {
        return {};
    }
    void close() override {}
    bool isOpen() override { return true; }
    void start (device::IODeviceCallback*) override {}
    void stop() override {}
    bool isPlaying() override { return false; }
    std::string getLastError() override { return {}; }

    int    getCurrentBufferSizeSamples() override { return block; }
    double getCurrentSampleRate() override        { return rate; }
    int    getCurrentBitDepth() override          { return 32; }

    device::ChannelSet getActiveOutputChannels() const override { return outs; }
    device::ChannelSet getActiveInputChannels() const override  { return ins; }

    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override  { return 0; }
    int getXRunCount() const noexcept override { return 0; }

private:
    static constexpr int kNumInputs = 16;

    double rate;
    int block;
    device::ChannelSet ins, outs;
};

// The world is prepared offline for the next scenario, so anything that opens
// or drops a device has to hand it back at the offline rate.
void restoreOfflinePrepare (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    ctx.keep (ctx.session().deviceCaptureChannels);
    ctx.cleanup ([&engine]
    {
        engine.setDeviceLostAlertSink ({});
        engine.prepareForSelfTest (ScenarioContext::kSampleRate, ScenarioContext::kBlockSize);
    });
}

void armTrack (ScenarioContext& ctx)
{
    auto& track = ctx.session().track (kTrack);
    track.mode.store ((int) Track::Mode::Mono, std::memory_order_relaxed);
    track.inputSource.store (-2, std::memory_order_relaxed);
    track.recordArmed.store (true, std::memory_order_relaxed);
    ctx.session().recomputeRtCounters();
}

// Pulling the interface out mid-take stops the transport, commits what was
// recorded, and says so in the window.
std::optional<ScenarioResult> lossStopsAndCommits (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    restoreOfflinePrepare (ctx);

    auto banner = std::make_shared<std::string>();
    auto fired = std::make_shared<bool> (false);
    engine.setDeviceLostAlertSink ([banner, fired] (std::string message)
    {
        *banner = std::move (message);
        *fired = true;
    });

    // The world closed the device to run offline, which is itself a deliberate
    // change - and the detector rightly refuses to call one of those a loss.
    engine.getDeviceManager().clearDeviceChangePendingForTest();

    StubDevice device { ScenarioContext::kSampleRate, ScenarioContext::kBlockSize };
    engine.audioDeviceAboutToStart (&device);

    armTrack (ctx);
    transport.setPlayhead (kTakeStart);
    engine.record();
    if (! ctx.expect (transport.isRecording(), "Record did not start with the track armed"))
        return ctx.verdict();
    ctx.pump (16);

    // What a backend reports when the endpoint goes away, in the order it
    // reports it: the callback stops, then the device list changes.
    engine.audioDeviceStopped();

    ctx.expect (! transport.isRecording() && ! transport.isPlaying(),
                "the transport kept rolling after the device stopped");
    ctx.expect (! engine.getRecordManager().isActive(),
                "the recorder was left active with no device");

    const auto& regions = ctx.session().track (kTrack).regions;
    if (ctx.expect (regions.size() == 1, "the interrupted take did not commit as one region; "
                                             + std::to_string (regions.size()) + " regions"))
    {
        ctx.expect (regions[0].timelineStart == kTakeStart,
                    "the committed take does not start where recording did");
        ctx.expect (regions[0].lengthInSamples > 0, "the committed take is empty");
    }

    engine.onDeviceManagerChanged();
    ctx.waitUntil ([fired] { return *fired; }, 5000,
                   [&ctx, banner]
                   {
                       ctx.expect (*banner ==
                                       "The active audio device has disconnected. Open Audio "
                                       "Settings to select a new device.",
                                   "the disconnect banner does not read as the manual quotes it: "
                                       + *banner);
                       ctx.complete (ctx.verdict());
                   },
                   "no disconnect banner was raised");
    return std::nullopt;
}

// A device that comes back - or a different one in its place - re-prepares the
// engine at whatever it reports, and the callback runs again on the new shape.
ScenarioResult reconnectReprepares (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    restoreOfflinePrepare (ctx);

    StubDevice first { ScenarioContext::kSampleRate, ScenarioContext::kBlockSize };
    engine.audioDeviceAboutToStart (&first);
    engine.audioDeviceStopped();
    ctx.expect (engine.getCurrentSampleRate() == 0.0 && engine.getCurrentBlockSize() == 0,
                "losing the device left a prepared rate and block size behind");

    StubDevice second { 96000.0, 512 };
    engine.audioDeviceAboutToStart (&second);
    ctx.expect (engine.getCurrentSampleRate() == 96000.0,
                "the engine did not re-prepare at the reconnected device's sample rate");
    ctx.expect (engine.getCurrentBlockSize() == 512,
                "the engine did not re-prepare at the reconnected device's block size");

    // The playhead only advances past the callback's undersized-buffer bail, so
    // this is how a scenario sees that blocks are being rendered again.
    constexpr int kBlocks = 4;
    transport.setPlayhead (0);
    engine.play();
    ctx.pump (kBlocks);
    engine.stop();
    ctx.expect (transport.getPlayhead() == (std::int64_t) kBlocks * ScenarioContext::kBlockSize,
                "the callback did not render after the device came back; playhead at "
                    + std::to_string (transport.getPlayhead()));
    return ctx.verdict();
}

// With no device open there is no sample rate to record at, so Record refuses,
// says why, and leaves the transport, the recorder and the armed track as they
// were.
ScenarioResult noDeviceRefusesRecord (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    restoreOfflinePrepare (ctx);

    auto refusal = std::make_shared<std::string>();
    engine.setRecordBlockedSink ([refusal] (auto message) { *refusal = message.toStdString(); });
    ctx.cleanup ([&engine] { engine.setRecordBlockedSink ({}); });

    StubDevice device { ScenarioContext::kSampleRate, ScenarioContext::kBlockSize };
    engine.audioDeviceAboutToStart (&device);
    engine.audioDeviceStopped();
    if (! ctx.expect (! (engine.getCurrentSampleRate() > 0.0), "closing the device left a sample rate behind"))
        return ctx.verdict();

    armTrack (ctx);
    transport.setPlayhead (kTakeStart);
    engine.record();

    ctx.expect (transport.isStopped() && transport.getPlayhead() == kTakeStart,
                "Record moved the transport with no audio device open");
    ctx.expect (! engine.getRecordManager().isActive(), "the recorder armed with no audio device open");
    ctx.expect (*refusal == "No audio device is open.\n\nOpen Settings \xE2\x86\x92 Audio and select "
                            "a device before recording.",
                "the refusal does not say that no device is open: '" + *refusal + "'");
    ctx.expect (ctx.session().track (kTrack).regions.empty(), "a take was committed with no audio device open");
    std::error_code ignored;
    const auto audioDir = ctx.sessionDir() / "audio";
    ctx.expect (! std::filesystem::exists (audioDir, ignored) || std::filesystem::is_empty (audioDir, ignored),
                "a take file was written with no audio device open");
    return ctx.verdict();
}

#if DUSKSTUDIO_HAS_MULTISAMPLE
enum class TakeEnd { Mastering, DeviceStop, DeviceLost, ForeignDeviceStop, OfflineDetach };

std::optional<ScenarioResult> interruptedTakeResumesAssets (ScenarioContext& ctx, TakeEnd route)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    ctx.cleanup ([&engine, &session, tempo = session.tempoMap]
    {
        session.tempoMap = tempo;
        engine.publishTempoMap();
    });
    ctx.keep (session.tempoBpm);
    ctx.keep (session.midiMergeEnabled);
    ctx.keep (session.countInEnabled);
    ctx.keep (session.lastRecordPointSamples);
    restoreOfflinePrepare (ctx);
    const auto previousStage = engine.getStage();
    ctx.cleanup ([&engine, route, previousStage, registered = engine.isAudioCallbackRegistered()]
    {
        engine.setOfflineRenderActive (false);
        engine.stop();
        if (route == TakeEnd::DeviceStop)
        {
            engine.reattachAudioCallback();
            engine.servicePlayback();
        }
        if (! registered) engine.detachAudioCallback();
        engine.setStage (previousStage);
        engine.cancelNativeMultisampleLoad (kTrack + 1);
        engine.getSfzLibrary().cancel();
    });
    const auto soundfont = ctx.tempDir() / "resume.sfz";
    if (! ctx.expect (dusk::fs::writeStringToFile (soundfont, "<region> sample=*sine key=60"),
                      "could not create the asset-resume instrument")) return ctx.verdict();
    struct LoadState { bool done = false, succeeded = false; std::string error; };
    const auto load = std::make_shared<LoadState>();
    ctx.waitUntil ([&engine] { return ! engine.getSfzLibrary().snapshot().busy; }, 5000,
        [&ctx, &engine, route, soundfont, load]
        {
            StubDevice device { ScenarioContext::kSampleRate, ScenarioContext::kBlockSize };
            engine.audioDeviceAboutToStart (&device);
            engine.getDeviceManager().clearDeviceChangePendingForTest();
            armTrack (ctx);
            ctx.session().tempoBpm.store (120.0f);
            ctx.session().tempoMap.clear();
            engine.publishTempoMap();
            ctx.session().midiMergeEnabled.store (true);
            ctx.session().countInEnabled.store (false);
            ctx.session().track (kTrack).mode.store ((int) Track::Mode::Midi);
            ctx.session().track (kTrack).midiInputIndex.store (engine.getVirtualKeyboardInputIndex());
            ctx.session().track (kTrack).midiChannel.store (1);
            ctx.session().track (kTrack + 1).mode.store ((int) Track::Mode::Midi);
            engine.getTransport().setPlayhead (kTakeStart);
            engine.record();
            if (! ctx.expect (engine.getRecordManager().isActive(), "the interrupted take did not start"))
            { ctx.complete (ctx.verdict()); return; }
            dusk::MidiBuffer notes;
            const std::uint8_t down[] { 0x90, 60, 100 }, up[] { 0x80, 60, 0 };
            notes.addEvent (down, 3, 0);
            notes.addEvent (up, 3, 100);
            ctx.pumpWithMidi (engine.getVirtualKeyboardInputIndex(), std::move (notes));
            ctx.pump (1);
            ctx.expect (engine.requestNativeMultisampleLoad (kTrack + 1, soundfont, {},
                [load] (bool success, std::string error)
                { load->done = true; load->succeeded = success; load->error = std::move (error); }),
                "a take did not accept a queued instrument load");
            ctx.expect (engine.getSfzLibrary().requestInstalled(), "could not queue library work during the take");
            ctx.waitUntil ([&engine] { const auto state = engine.getSfzLibrary().snapshot();
                return ! state.busy && state.state == sfz::LibraryState::Paused; }, 5000,
                [&ctx, &engine, route, load]
                {
                    engine.serviceMultisampleLoads();
                    ctx.expect (! load->done && engine.hasPendingMultisampleLoads(),
                                "instrument loading did not wait for the take");
                    ctx.expect (! engine.getSfzLibrary().resume(), "library work resumed during recording");
                    if (route == TakeEnd::Mastering) engine.setStage (AudioEngine::Stage::Mastering);
                    else if (route == TakeEnd::DeviceStop)
                    {
                        if (! ctx.expect (! engine.isAudioCallbackRegistered(), "the stub device unexpectedly registered a callback"))
                        { ctx.complete (ctx.verdict()); return; }
                        engine.restartDspWhenIdle();
                        engine.audioDeviceStopped();
                        ctx.expect (! engine.isAudioCallbackRegistered(), "DSP restart re-entered the stopped callback");
                    }
                    else if (route == TakeEnd::DeviceLost) engine.onDeviceManagerChanged();
                    else if (route == TakeEnd::OfflineDetach)
                    {
                        engine.setOfflineRenderActive (true);
                        engine.detachAudioCallback();
                        ctx.expect (! engine.getRecordManager().isActive(), "offline handoff left the take unpublished");
                        engine.setOfflineRenderActive (false);
                    }
                    else
                    {
                        std::thread stopped ([&engine] { engine.audioDeviceStopped(); });
                        stopped.join();
                        ctx.expect (engine.getRecordManager().isActive(), "a foreign stopped callback destroyed the capture");
                        engine.getRecordManager().serviceMidiCapture();
                        ctx.expect (engine.getRecordManager().midiCapturePreview (kTrack) != nullptr,
                                    "the owner could not inspect capture before deferred finalization");
                    }
                    const auto yielded = std::make_shared<bool> (false);
                    ctx.later (0, [yielded] { *yielded = true; });
                    ctx.waitUntil ([&engine, yielded] { return *yielded && engine.getTransport().isStopped()
                        && ! engine.getRecordManager().isActive(); }, 5000,
                        [&ctx, &engine, route, load]
                        {
                            engine.servicePlayback();
                            if (route == TakeEnd::DeviceStop)
                            {
                                ctx.expect (! engine.isAudioCallbackRegistered(), "deferred DSP restart reattached a detached callback");
                                engine.reattachAudioCallback();
                                engine.servicePlayback();
                                ctx.expect (engine.isAudioCallbackRegistered(), "DSP restart lost an explicitly restored callback");
                                engine.detachAudioCallback();
                            }
                            const auto& regions = ctx.session().track (kTrack).midiRegions.current();
                            ctx.expect (regions.size() == 1 && regions.front().timelineStart == kTakeStart
                                        && regions.front().lengthInTicks == 10 && regions.front().lengthInSamples == 500
                                        && regions.front().notes == std::vector<MidiNote> { { 1, 60, 100, 0, 2 } },
                                        "take finalization lost or retimed the captured MIDI after the device stopped");
                            ctx.expect (engine.getSfzLibrary().resume(), "take finalization left library work paused");
                            if (engine.getCurrentSampleRate() <= 0.0)
                            {
                                StubDevice reconnected { ScenarioContext::kSampleRate, ScenarioContext::kBlockSize };
                                engine.audioDeviceAboutToStart (&reconnected);
                            }
                            ctx.waitUntil ([&engine, load]
                            {
                                engine.serviceMultisampleLoads();
                                return load->done && ! engine.hasPendingMultisampleLoads()
                                    && ! engine.getSfzLibrary().snapshot().busy;
                            }, 5000, [&ctx, &engine, load]
                            {
                                ctx.expect (load->succeeded, "queued instrument did not load after the take: " + load->error);
                                ctx.expect (engine.getSfzLibrary().snapshot().state == sfz::LibraryState::Complete,
                                            "library work did not finish after explicit Resume");
                                ctx.complete (ctx.verdict());
                            }, "asset work did not finish after take finalization");
                        }, "the interrupted take did not finalize on its owner thread");
                }, "library work did not enter its recording pause");
        }, "the asset-resume scenario found a busy library");
    return std::nullopt;
}

const ScenarioRegistrar masteringAssetsRegistrar { Scenario {
    "device.mastering_switch_resumes_assets", { "device", "record", "sfz" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return interruptedTakeResumesAssets (ctx, TakeEnd::Mastering); } } };
const ScenarioRegistrar stoppedAssetsRegistrar { Scenario {
    "device.stop_resumes_assets", { "device", "record", "sfz" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return interruptedTakeResumesAssets (ctx, TakeEnd::DeviceStop); } } };
const ScenarioRegistrar lostAssetsRegistrar { Scenario {
    "device.loss_resumes_assets", { "device", "record", "sfz" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return interruptedTakeResumesAssets (ctx, TakeEnd::DeviceLost); } } };
const ScenarioRegistrar foreignStopRegistrar { Scenario {
    "device.foreign_stop_defers_capture_finalization", { "device", "record", "midi", "sfz" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return interruptedTakeResumesAssets (ctx, TakeEnd::ForeignDeviceStop); } } };
const ScenarioRegistrar offlineDetachRegistrar { Scenario {
    "device.offline_handoff_finalizes_capture", { "device", "record", "midi", "sfz", "bounce" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return interruptedTakeResumesAssets (ctx, TakeEnd::OfflineDetach); } } };
#endif

const ScenarioRegistrar lossRegistrar { Scenario {
    "device.loss_stops_and_commits", { "device", "record", "transport" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return lossStopsAndCommits (ctx); } } };
const ScenarioRegistrar noDeviceRegistrar { Scenario {
    "device.none_open_refuses_record", { "device", "record" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return noDeviceRefusesRecord (ctx); } } };
const ScenarioRegistrar reconnectRegistrar { Scenario {
    "device.reconnect_reprepares", { "device", "transport" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return reconnectReprepares (ctx); } } };
} // namespace
} // namespace duskstudio::scenario
