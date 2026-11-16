#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../audiofile/FileWriter.h"
#include "../../device/DeviceManager.h"
#include "../../device/IODevice.h"
#include "../../device/IODeviceCallback.h"
#include "../../../session/Session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#if defined(DUSKSTUDIO_HAS_PIPEWIRE)
 #include <cerrno>
 #include <csignal>
 #include <fcntl.h>
 #include <spawn.h>
 #include <sys/wait.h>
 #include <unistd.h>
#endif

// A PipeWire graph whose clock something outside the app moves while it plays.
// The cases force the clock through the graph's settings metadata, so they run
// only on the private null graph the regression runner owns (bb-pipewire-clock)
// and skip anywhere else.
namespace duskstudio::scenario
{
namespace
{
#if defined(DUSKSTUDIO_HAS_PIPEWIRE)
constexpr int kTrack = 2;
constexpr int kToneSeconds = 40;
constexpr int kWindowMs = 2000;

using SessionFile = std::decay_t<decltype (std::declval<const Session&>().getSessionDirectory())>;

std::string hz (double rate) { return std::to_string (std::llround (rate)) + " Hz"; }

// The graph a case may force a clock on: the one the opt-in variable names,
// which must also be the one the app plays through, and which holds the
// marker only the harness that started it writes.
std::optional<std::filesystem::path> ownedGraph (std::string& refusal)
{
    const char* fixture = std::getenv ("DUSKSTUDIO_PIPEWIRE_CLOCK_FIXTURE");
    const char* runtime = std::getenv ("PIPEWIRE_RUNTIME_DIR");
    if (fixture == nullptr || runtime == nullptr)
        return std::nullopt;
    std::error_code a, b, c;
    const auto dir = std::filesystem::canonical (std::filesystem::u8path (fixture), a);
    const auto run = std::filesystem::canonical (std::filesystem::u8path (runtime), b);
    if (a || b || dir != run || std::getenv ("PIPEWIRE_REMOTE") != nullptr
        || ! std::filesystem::exists (dir / "owned-null-graph", c))
    {
        refusal = "the PipeWire clock fixture is not the owned null graph the app plays through";
        return std::nullopt;
    }
    return dir;
}

// Sets one key of the graph's settings metadata with pw-metadata, run with an
// environment that reaches the owned graph and nothing else.
bool setGraphSetting (const std::filesystem::path& graph, const char* key, int value, std::string& error)
{
    const std::string runtimeVar = "PIPEWIRE_RUNTIME_DIR=" + graph.string();
    const char* path = std::getenv ("PATH");
    const std::string pathVar = std::string ("PATH=") + (path != nullptr ? path : "/usr/bin:/bin");
    std::string args[] = { "pw-metadata", "-n", "settings", "0", key, std::to_string (value) };
    char* argv[] = { args[0].data(), args[1].data(), args[2].data(), args[3].data(),
                     args[4].data(), args[5].data(), nullptr };
    std::string envs[] = { runtimeVar, pathVar };
    char* envp[] = { envs[0].data(), envs[1].data(), nullptr };

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init (&actions);
    posix_spawn_file_actions_addopen (&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    const int spawned = posix_spawnp (&pid, "pw-metadata", &actions, nullptr, argv, envp);
    posix_spawn_file_actions_destroy (&actions);
    if (spawned != 0)
    {
        error = "could not run pw-metadata to set " + std::string (key);
        return false;
    }

    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (5);
    for (;;)
    {
        const pid_t done = waitpid (pid, &status, WNOHANG);
        if (done == pid) break;
        if (done < 0 && errno != EINTR) { error = "lost pw-metadata"; return false; }
        if (std::chrono::steady_clock::now() > deadline)
        {
            kill (pid, SIGKILL);
            waitpid (pid, &status, 0);
            error = "pw-metadata did not finish setting " + std::string (key);
            return false;
        }
        usleep (2000);
    }
    if (! WIFEXITED (status) || WEXITSTATUS (status) != 0)
    {
        error = "pw-metadata could not set " + std::string (key);
        return false;
    }
    return true;
}

bool writeTone (const std::filesystem::path& path, int rate)
{
    // 440 whole cycles a second, so the same second repeats without a seam.
    std::vector<float> second ((std::size_t) rate);
    for (int i = 0; i < rate; ++i)
        second[(std::size_t) i] = 0.25f * (float) std::sin (2.0 * 3.141592653589793 * 440.0 * i / rate);
    dusk::audio::WriteSpec spec;
    spec.sampleRate = rate;
    spec.numChannels = 1;
    spec.bitsPerSample = 32;
    auto writer = dusk::audio::FileWriter::create (path, spec);
    if (writer == nullptr) return false;
    const float* channels[] = { second.data() };
    for (int s = 0; s < kToneSeconds; ++s)
        if (! writer->write (channels, 1, rate)) return false;
    return writer->flush();
}

// Playhead and wall-clock time at one instant, to tell later whether the
// engine rendered the audio time that passed.
struct Mark
{
    std::int64_t playhead = 0;
    std::chrono::steady_clock::time_point at;
};

Mark markNow (AudioEngine& engine)
{
    return { engine.getTransport().getCommittedPlayhead(), std::chrono::steady_clock::now() };
}

// Share of the wall-clock time since `from` that the transport advanced over.
double renderedShare (AudioEngine& engine, const Mark& from, double rate)
{
    const auto now = markNow (engine);
    const double seconds = std::chrono::duration<double> (now.at - from.at).count();
    if (seconds <= 0.0 || rate <= 0.0) return 0.0;
    return (double) (now.playhead - from.playhead) / (seconds * rate);
}

// What a case carries from one deferred step to the next.
struct ClockRun
{
    std::filesystem::path graph;
    double rate = 0.0, newRate = 0.0;
    int block = 0, forcedBlock = 0;
    Mark mark;
    std::vector<std::string> rateAlerts;
    bool openingAnnounced = false, rateAnnounced = false, processingHeld = false;
};

// Opens the owned graph's device with nothing forced, or with `forcedRate`
// forced first, and arranges for the graph and the engine to be handed back
// as they were however the case ends.
std::shared_ptr<ClockRun> openOwnedGraph (ScenarioContext& ctx, const std::filesystem::path& graph,
                                          int forcedRate = 0)
{
    auto& engine = ctx.engine();
    ctx.cleanup ([&engine, graph]
    {
        std::string ignored;
        engine.stop();
        setGraphSetting (graph, "clock.force-quantum", 0, ignored);
        setGraphSetting (graph, "clock.force-rate", 0, ignored);
        engine.detachAudioCallback();
        engine.getDeviceManager().closeDevice();
        engine.prepareForSelfTest (ScenarioContext::kSampleRate, ScenarioContext::kBlockSize);
    });
    std::string error;
    if (! ctx.expect (setGraphSetting (graph, "clock.force-quantum", 0, error)
                      && setGraphSetting (graph, "clock.force-rate", forcedRate, error), error))
        return nullptr;

    engine.reattachAudioCallback();
    error = engine.getDeviceManager().initialise (2, 2, {}, false);
    if (! ctx.expect (error.empty(), "could not open the private null graph: " + error))
        return nullptr;
    auto* device = engine.getDeviceManager().getCurrentDevice();
    if (! ctx.expect (device != nullptr && device->isPlaying(), "the private device did not start"))
        return nullptr;

    auto run = std::make_shared<ClockRun>();
    run->graph = graph;
    run->rate = engine.getCurrentSampleRate();
    run->block = engine.getCurrentBlockSize();
    run->forcedBlock = std::min (run->block * 4, 2048);
    run->newRate = std::abs (run->rate - 48000.0) < 0.5 ? 44100.0 : 48000.0;
    ctx.note ("opened at " + std::to_string (run->block) + " samples, " + hz (run->rate));
    if (! ctx.expect (run->rate > 0.0 && run->block > 0, "the private device opened with no clock"))
        return nullptr;
    return run;
}

// Plays a tone from the start of the timeline and calls `then` once the engine
// renders it and the master meter shows it.
void playTone (ScenarioContext& ctx, std::shared_ptr<ClockRun> run, std::function<void()> then)
{
    auto& engine = ctx.engine();
    const auto tone = ctx.tempDir() / "tone.wav";
    if (! ctx.expect (writeTone (tone, (int) run->rate), "could not write the tone"))
    {
        ctx.complete (ctx.verdict());
        return;
    }
    AudioRegion region;
    region.file = SessionFile (tone.u8string().c_str());
    region.timelineStart = 0;
    region.lengthInSamples = (std::int64_t) kToneSeconds * (std::int64_t) run->rate;
    region.numChannels = 1;
    ctx.session().track (kTrack).mode.store ((int) Track::Mode::Mono, std::memory_order_relaxed);
    ctx.session().track (kTrack).regions.push_back (region);

    engine.getTransport().setLoopEnabled (false);
    engine.getTransport().setPlayhead (0);
    engine.play();
    ctx.waitUntil ([&ctx, &engine, run]
                   {
                       return engine.getTransport().getCommittedPlayhead() > (std::int64_t) run->block * 16
                           && ctx.session().master().meterPostMasterLDb.load (std::memory_order_relaxed) > -30.0f;
                   }, 5000, std::move (then), "the tone did not play through the private device");
}

// Checks, `ms` from now, that the engine rendered the audio time since `run->mark`
// at `rate` and that the newest block it rendered was heard.
void expectRenderedAfter (ScenarioContext& ctx, std::shared_ptr<ClockRun> run, int ms, double rate,
                          std::string what, std::function<void()> then)
{
    auto& engine = ctx.engine();
    ctx.later (ms / 4, [&ctx]
    {
        // Peak of the newest block only, so a stale reading cannot pass for sound.
        ctx.session().master().meterPostMasterLDb.store (-100.0f, std::memory_order_relaxed);
    });
    ctx.later (ms, [&ctx, &engine, run, rate, what = std::move (what), then = std::move (then)]
    {
        const double share = renderedShare (engine, run->mark, rate);
        const float level = ctx.session().master().meterPostMasterLDb.load (std::memory_order_relaxed);
        ctx.note (what + ": rendered " + std::to_string (share) + " of the time, newest block at "
                  + std::to_string (level) + " dBFS");
        ctx.expect (engine.getTransport().isPlaying(), what + ": the transport stopped");
        ctx.expect (share > 0.75, what + ": the engine stopped rendering (" + std::to_string (share)
                                      + " of the time rendered)");
        ctx.expect (level > -30.0f, what + ": the output went silent");
        then();
    });
}
#endif

// The graph's quantum goes up to four times the opened block while a tone
// plays: every sample is still rendered and heard, the engine keeps its block
// size while the transport rolls, and takes the new one once it stops.
std::optional<ScenarioResult> longerCycleKeepsPlaying (ScenarioContext& ctx)
{
#if ! defined(DUSKSTUDIO_HAS_PIPEWIRE)
    (void) ctx;
    return ScenarioResult::skip ("built without the native PipeWire backend");
#else
    std::string refusal;
    const auto graph = ownedGraph (refusal);
    if (! graph)
        return refusal.empty() ? ScenarioResult::skip ("requires the owned private PipeWire graph "
                                                       "(DUSKSTUDIO_PIPEWIRE_CLOCK_FIXTURE)")
                               : ScenarioResult::fail (refusal);
    auto run = openOwnedGraph (ctx, *graph);
    if (run == nullptr) return ctx.verdict();
    if (! ctx.expect (run->forcedBlock > run->block, "the graph cannot run a quantum longer than "
                                                         + std::to_string (run->block)))
        return ctx.verdict();

    playTone (ctx, run, [&ctx, run]
    {
        auto& engine = ctx.engine();
        std::string error;
        if (! ctx.expect (setGraphSetting (run->graph, "clock.force-quantum", run->forcedBlock, error), error))
        {
            ctx.complete (ctx.verdict());
            return;
        }
        run->mark = markNow (engine);
        expectRenderedAfter (ctx, run, kWindowMs, run->rate,
                             "with " + std::to_string (run->forcedBlock) + "-sample cycles", [&ctx, &engine, run]
        {
            ctx.expect (engine.getCurrentBlockSize() == run->block,
                        "the engine re-prepared while the transport rolled");
            engine.stop();
            ctx.waitUntil ([&engine, run] { return engine.getCurrentBlockSize() == run->forcedBlock; }, 3000,
                           [&ctx, &engine, run]
                           {
                               ctx.expect (std::abs (engine.getCurrentSampleRate() - run->rate) < 0.5,
                                           "a new block size moved the sample rate");
                               ctx.complete (ctx.verdict());
                           },
                           "the engine did not take the graph's new quantum once the transport stopped");
        });
    });
    return std::nullopt;
#endif
}

std::optional<ScenarioResult> longerCycleKeepsMastering (ScenarioContext& ctx)
{
#if ! defined(DUSKSTUDIO_HAS_PIPEWIRE)
    (void) ctx;
    return ScenarioResult::skip ("built without the native PipeWire backend");
#else
    std::string refusal;
    const auto graph = ownedGraph (refusal);
    if (! graph)
        return refusal.empty() ? ScenarioResult::skip ("requires the owned private PipeWire graph "
                                                       "(DUSKSTUDIO_PIPEWIRE_CLOCK_FIXTURE)")
                               : ScenarioResult::fail (refusal);
    auto run = openOwnedGraph (ctx, *graph);
    if (run == nullptr) return ctx.verdict();
    if (! ctx.expect (run->forcedBlock > run->block, "the graph cannot run a longer quantum"))
        return ctx.verdict();

    auto& engine = ctx.engine();
    auto& player = engine.getMasteringPlayer();
    const auto oldStage = engine.getStage();
    ctx.cleanup ([&engine, &player, oldStage]
    {
        player.stop();
        player.unloadFile();
        engine.setStage (oldStage);
    });
    const auto tone = ctx.tempDir() / "mastering-tone.wav";
    if (! ctx.expect (writeTone (tone, (int) run->rate), "could not write the mastering tone"))
        return ctx.verdict();
    engine.setStage (AudioEngine::Stage::Mastering);
    if (! ctx.expect (player.loadFile (SessionFile (tone.u8string().c_str())),
                      "could not load the mastering tone"))
        return ctx.verdict();
    player.play();
    ctx.waitUntil ([&ctx, &engine, &player, run]
                   {
                       return player.getPlayhead() > (std::int64_t) run->block * 16
                           && engine.playbackServiceIdleForScenario()
                           && ctx.session().mastering().meterPostMasterLDb.load (std::memory_order_relaxed) > -30.0f;
                   }, 5000,
                   [&ctx, &engine, &player, run]
    {
        std::string error;
        if (! ctx.expect (setGraphSetting (run->graph, "clock.force-quantum", run->forcedBlock, error), error))
        {
            ctx.complete (ctx.verdict());
            return;
        }
        run->mark = { player.getPlayhead(), std::chrono::steady_clock::now() };
        ctx.later (kWindowMs / 4, [&ctx]
        {
            ctx.session().mastering().meterPostMasterLDb.store (-100.0f, std::memory_order_relaxed);
        });
        ctx.later (kWindowMs, [&ctx, &engine, &player, run]
        {
            const double seconds = std::chrono::duration<double> (
                std::chrono::steady_clock::now() - run->mark.at).count();
            const double share = (double) (player.getPlayhead() - run->mark.playhead) / (seconds * run->rate);
            ctx.expect (player.isPlaying() && engine.getTransport().isStopped(),
                        "the graph change interrupted mastering playback");
            ctx.expect (share > 0.75, "mastering did not render the elapsed audio time");
            ctx.expect (ctx.session().mastering().meterPostMasterLDb.load (std::memory_order_relaxed) > -30.0f,
                        "mastering output went silent after the graph change");
            ctx.expect (engine.getCurrentBlockSize() == run->block
                            && engine.getDeviceManager().getSetup().bufferSize == run->block,
                        "the engine or settings adopted a new block size during mastering playback");
            player.stop();
            ctx.waitUntil ([&engine, run]
                           {
                               return engine.getCurrentBlockSize() == run->forcedBlock
                                   && engine.getDeviceManager().getSetup().bufferSize == run->forcedBlock;
                           }, 3000, [&ctx, &engine]
                           {
                               ctx.waitUntil ([&engine] { return engine.playbackServiceIdleForScenario(); }, 3000,
                                              [&ctx] { ctx.complete (ctx.verdict()); },
                                              "the playback service kept polling after the clock was adopted");
                           },
                           "the engine and settings did not adopt the new block size after mastering stopped");
        });
    }, "the mastering tone did not play with its playback service idle");
    return std::nullopt;
#endif
}

// The graph moves to another rate while a tone plays: the engine follows it at
// once, the transport stops, the alert says so, and the engine then renders at
// the new rate.
std::optional<ScenarioResult> rateChangeFollows (ScenarioContext& ctx)
{
#if ! defined(DUSKSTUDIO_HAS_PIPEWIRE)
    (void) ctx;
    return ScenarioResult::skip ("built without the native PipeWire backend");
#else
    std::string refusal;
    const auto graph = ownedGraph (refusal);
    if (! graph)
        return refusal.empty() ? ScenarioResult::skip ("requires the owned private PipeWire graph "
                                                       "(DUSKSTUDIO_PIPEWIRE_CLOCK_FIXTURE)")
                               : ScenarioResult::fail (refusal);
    auto run = openOwnedGraph (ctx, *graph);
    if (run == nullptr) return ctx.verdict();

    // The session's audio was made at the rate the device opened at, so the
    // alert also has to say what the new rate does to it.
    auto& engine = ctx.engine();
    const double sessionRate = ctx.session().sessionSampleRate;
    ctx.cleanup ([&ctx, &engine, sessionRate]
    {
        engine.setDeviceRateChangedSink ({});
        ctx.session().sessionSampleRate = sessionRate;
    });
    ctx.session().sessionSampleRate = run->rate;
    engine.setDeviceRateChangedSink ([run] (std::string message) { run->rateAlerts.push_back (std::move (message)); });

    playTone (ctx, run, [&ctx, &engine, run]
    {
        std::string error;
        if (! ctx.expect (setGraphSetting (run->graph, "clock.force-rate", (int) run->newRate, error), error))
        {
            ctx.complete (ctx.verdict());
            return;
        }
        ctx.waitUntil ([&engine, run]
                       {
                           return std::abs (engine.getCurrentSampleRate() - run->newRate) < 0.5
                               && engine.getTransport().isStopped() && ! run->rateAlerts.empty();
                       }, 4000,
                       [&ctx, &engine, run]
                       {
                           auto* device = engine.getDeviceManager().getCurrentDevice();
                           ctx.expect (device != nullptr
                                           && engine.getCurrentBlockSize() == device->getCurrentBufferSizeSamples(),
                                       "the engine did not prepare at the block size the graph runs");
                           const std::string expected =
                               "Another application or a system audio setting changed the audio device's "
                               "sample rate from " + hz (run->rate) + " to " + hz (run->newRate) + ", so Dusk "
                               "Studio now runs at " + hz (run->newRate) + ". A change of rate stops the "
                               "transport; a take in progress is kept up to where it stopped. This session's "
                               "audio was made at " + hz (run->rate) + ": recorded tracks play at the wrong "
                               "speed and pitch until the device runs at " + hz (run->rate) + " again.";
                           ctx.expect (run->rateAlerts.size() == 1 && run->rateAlerts.front() == expected,
                                       "the rate-change alert does not read as the manual quotes it: "
                                           + run->rateAlerts.front());
                           engine.play();
                           run->mark = markNow (engine);
                           expectRenderedAfter (ctx, run, kWindowMs, run->newRate, "at " + hz (run->newRate),
                                                [&ctx, &engine]
                                                {
                                                    engine.stop();
                                                    ctx.complete (ctx.verdict());
                                                });
                       },
                       "the engine did not follow the graph to " + hz (run->newRate)
                           + ", stop the transport and say so");
    });
    return std::nullopt;
#endif
}

std::optional<ScenarioResult> deferredRateChangeFollows (ScenarioContext& ctx)
{
#if ! defined(DUSKSTUDIO_HAS_PIPEWIRE)
    (void) ctx;
    return ScenarioResult::skip ("built without the native PipeWire backend");
#else
    std::string refusal;
    const auto graph = ownedGraph (refusal);
    if (! graph)
        return refusal.empty() ? ScenarioResult::skip ("requires the owned private PipeWire graph "
                                                       "(DUSKSTUDIO_PIPEWIRE_CLOCK_FIXTURE)")
                               : ScenarioResult::fail (refusal);
    auto run = openOwnedGraph (ctx, *graph);
    if (run == nullptr) return ctx.verdict();
    auto& engine = ctx.engine();
    ctx.cleanup ([&engine, run]
    {
        engine.getDeviceManager().removeChangeListener (run.get());
        if (run->processingHeld) engine.resumeProcessing();
        engine.setDeviceRateChangedSink ({});
    });
    engine.setDeviceRateChangedSink ([run] (std::string message) { run->rateAlerts.push_back (std::move (message)); });
    engine.getDeviceManager().addChangeListener (run.get(), [&engine, run]
    {
        const auto moved = engine.getDeviceManager().getMovedDeviceClock();
        if (! moved.isSet()) run->openingAnnounced = true;
        else if (run->processingHeld && std::abs (moved.sampleRate - run->newRate) < 0.5)
            run->rateAnnounced = true;
    });
    ctx.waitUntil ([run] { return run->openingAnnounced; }, 3000, [&ctx, &engine, run]
    {
        playTone (ctx, run, [&ctx, &engine, run]
        {
            engine.suspendProcessing();
            run->processingHeld = true;
            std::string error;
            if (! ctx.expect (setGraphSetting (run->graph, "clock.force-rate", (int) run->newRate, error), error))
            {
                ctx.complete (ctx.verdict());
                return;
            }
            ctx.waitUntil ([run] { return run->rateAnnounced; }, 3000, [&ctx, &engine, run]
            {
                ctx.expect (engine.getTransport().isPlaying() && engine.isAudioCallbackRegistered(),
                            "suspending processing stopped the transport or detached its callback");
                ctx.expect (std::abs (engine.getCurrentSampleRate() - run->rate) < 0.5 && run->rateAlerts.empty(),
                            "the clock was followed while processing was suspended");
                engine.resumeProcessing();
                run->processingHeld = false;
                ctx.waitUntil ([&engine, run]
                               {
                                   return std::abs (engine.getCurrentSampleRate() - run->newRate) < 0.5
                                       && engine.getTransport().isStopped() && ! run->rateAlerts.empty();
                               }, 4000, [&ctx, &engine, run]
                               {
                                   ctx.expect (run->rateAlerts.size() == 1, "the deferred rate change raised multiple alerts");
                                   engine.play();
                                   run->mark = markNow (engine);
                                   expectRenderedAfter (ctx, run, kWindowMs, run->newRate, "after the deferred rate change",
                                                        [&ctx, &engine]
                                                        {
                                                            engine.stop();
                                                            ctx.complete (ctx.verdict());
                                                        });
                               }, "the announced rate was not followed when processing resumed during playback");
            }, "the rate change was not announced while processing was suspended");
        });
    }, "the opening device notification was not delivered");
    return std::nullopt;
#endif
}

std::optional<ScenarioResult> restartReportsAdoptedRate (ScenarioContext& ctx)
{
#if ! defined(DUSKSTUDIO_HAS_PIPEWIRE)
    (void) ctx;
    return ScenarioResult::skip ("built without the native PipeWire backend");
#else
    std::string refusal;
    const auto graph = ownedGraph (refusal);
    if (! graph)
        return refusal.empty() ? ScenarioResult::skip ("requires the owned private PipeWire graph "
                                                       "(DUSKSTUDIO_PIPEWIRE_CLOCK_FIXTURE)")
                               : ScenarioResult::fail (refusal);
    auto run = openOwnedGraph (ctx, *graph);
    if (run == nullptr) return ctx.verdict();
    if (! ctx.expect (run->forcedBlock > run->block, "the graph cannot run a longer quantum"))
        return ctx.verdict();

    struct StopObserver final : device::IODeviceCallback
    {
        std::function<void()> stopped;
        void audioDeviceIOCallback (const float* const*, int, float* const* outputs, int channels,
                                    int samples, const device::CallbackContext&) override
        {
            for (int channel = 0; channel < channels; ++channel)
                if (outputs[channel] != nullptr) std::fill_n (outputs[channel], samples, 0.0f);
        }
        void audioDeviceAboutToStart (device::IODevice*) override {}
        void audioDeviceStopped() override
        {
            if (auto action = std::move (stopped)) action();
        }
    };
    auto observer = std::make_shared<StopObserver>();
    auto& engine = ctx.engine();
    auto* device = engine.getDeviceManager().getCurrentDevice();
    ctx.cleanup ([&engine, observer]
    {
        observer->stopped = {};
        engine.getDeviceManager().removeCallback (observer.get());
        engine.setDeviceRateChangedSink ({});
    });
    engine.getDeviceManager().addCallback (observer.get());
    engine.setDeviceRateChangedSink ([run] (std::string message) { run->rateAlerts.push_back (std::move (message)); });
    observer->stopped = [&ctx, run, device]
    {
        std::string error;
        if (! ctx.expect (setGraphSetting (run->graph, "clock.force-rate", (int) run->newRate, error), error))
            return;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (3);
        while (std::chrono::steady_clock::now() < deadline)
        {
            const auto moved = device->getMovedClock();
            if (moved.isSet() && std::abs (moved.sampleRate - run->newRate) < 0.5)
            {
                run->rateAnnounced = true;
                return;
            }
            usleep (2000);
        }
        ctx.expect (false, "the graph did not change rate during the quantum restart");
    };
    std::string error;
    if (! ctx.expect (setGraphSetting (run->graph, "clock.force-quantum", run->forcedBlock, error), error))
        return ctx.verdict();
    ctx.waitUntil ([&engine, run]
                   {
                       return std::abs (engine.getCurrentSampleRate() - run->newRate) < 0.5;
                   }, 5000, [&ctx, run]
                   {
                       ctx.expect (run->rateAnnounced, "the rate did not move between stop and restart");
                       ctx.expect (run->rateAlerts.size() == 1, "the adopted rate did not raise exactly one alert");
                       if (! run->rateAlerts.empty())
                           ctx.expect (run->rateAlerts.front().find ("from " + hz (run->rate) + " to " + hz (run->newRate))
                                           != std::string::npos,
                                       "the alert does not name the previous and adopted rates");
                       ctx.complete (ctx.verdict());
                   }, "the restarted device did not adopt the graph's final rate");
    return std::nullopt;
#endif
}

// A graph already forced to another rate than the one asked for: the device
// opens at the graph's rate, and the engine prepares and renders at it.
std::optional<ScenarioResult> openAdoptsGraphRate (ScenarioContext& ctx)
{
#if ! defined(DUSKSTUDIO_HAS_PIPEWIRE)
    (void) ctx;
    return ScenarioResult::skip ("built without the native PipeWire backend");
#else
    std::string refusal;
    const auto graph = ownedGraph (refusal);
    if (! graph)
        return refusal.empty() ? ScenarioResult::skip ("requires the owned private PipeWire graph "
                                                       "(DUSKSTUDIO_PIPEWIRE_CLOCK_FIXTURE)")
                               : ScenarioResult::fail (refusal);
    static constexpr int kForcedRate = 44100;
    auto run = openOwnedGraph (ctx, *graph, kForcedRate);
    if (run == nullptr) return ctx.verdict();
    auto* device = ctx.engine().getDeviceManager().getCurrentDevice();
    const bool deviceAtGraphRate = ctx.expect (std::abs (device->getCurrentSampleRate() - kForcedRate) < 0.5,
                                               "the device reports " + hz (device->getCurrentSampleRate())
                                                   + ", not the graph's " + hz (kForcedRate));
    const bool engineAtGraphRate = ctx.expect (std::abs (run->rate - kForcedRate) < 0.5,
                                               "the engine prepared at " + hz (run->rate) + ", not the graph's "
                                                   + hz (kForcedRate));
    if (! deviceAtGraphRate || ! engineAtGraphRate) return ctx.verdict();

    playTone (ctx, run, [&ctx, run]
    {
        run->mark = markNow (ctx.engine());
        expectRenderedAfter (ctx, run, kWindowMs / 2, run->rate, "at the graph's " + hz (run->rate), [&ctx]
        {
            ctx.engine().stop();
            ctx.complete (ctx.verdict());
        });
    });
    return std::nullopt;
#endif
}

const ScenarioRegistrar longerCycleRegistrar { Scenario {
    "device.pipewire_longer_cycle_keeps_playing", { "device", "transport", "pipewire", "pipewire-clock" },
    Needs::Engine, {}, [] (ScenarioContext& ctx) { return longerCycleKeepsPlaying (ctx); } } };
const ScenarioRegistrar masteringCycleRegistrar { Scenario {
    "device.pipewire_longer_cycle_keeps_mastering", { "device", "mastering", "pipewire", "pipewire-clock" },
    Needs::Engine, {}, [] (ScenarioContext& ctx) { return longerCycleKeepsMastering (ctx); } } };
const ScenarioRegistrar rateChangeRegistrar { Scenario {
    "device.pipewire_rate_change_follows", { "device", "transport", "pipewire", "pipewire-clock" },
    Needs::Engine, {}, [] (ScenarioContext& ctx) { return rateChangeFollows (ctx); } } };
const ScenarioRegistrar deferredRateChangeRegistrar { Scenario {
    "device.pipewire_deferred_rate_change_follows", { "device", "transport", "pipewire", "pipewire-clock" },
    Needs::Engine, {}, [] (ScenarioContext& ctx) { return deferredRateChangeFollows (ctx); } } };
const ScenarioRegistrar adoptedRateRegistrar { Scenario {
    "device.pipewire_restart_reports_adopted_rate", { "device", "pipewire", "pipewire-clock" },
    Needs::Engine, {}, [] (ScenarioContext& ctx) { return restartReportsAdoptedRate (ctx); } } };
const ScenarioRegistrar openRateRegistrar { Scenario {
    "device.pipewire_open_adopts_graph_rate", { "device", "pipewire", "pipewire-clock" },
    Needs::Engine, {}, [] (ScenarioContext& ctx) { return openAdoptsGraphRate (ctx); } } };
} // namespace
} // namespace duskstudio::scenario
