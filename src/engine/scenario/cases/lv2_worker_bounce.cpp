#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../BounceEngine.h"
#include "../../audiofile/FileReader.h"
#include "../../audiofile/FileWriter.h"
#include "../../../dsp/ChannelStrip.h"
#include "../../../session/Session.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace duskstudio::scenario
{
namespace
{
#if DUSKSTUDIO_HAS_NATIVE_LV2
constexpr double kRate = ScenarioContext::kSampleRate;
constexpr int kTrack = 0;
constexpr const char* kPluginUri = "urn:duskstudio:test:worker";
constexpr std::int64_t kStart = 12000;
constexpr int kLength = 24000;
constexpr int kLiveTimeoutMs = 10000;
constexpr int kRenderTimeoutMs = 90000;

// The bounce API takes the framework's file type; naming it through Session's
// own getter keeps this file free of the framework header.
using SessionFile = std::decay_t<decltype (std::declval<const Session&>().getSessionDirectory())>;

bool writeTone (const std::filesystem::path& path)
{
    std::vector<float> samples ((std::size_t) kLength);
    for (int i = 0; i < kLength; ++i)
        samples[(std::size_t) i] = 0.25f * std::sin (2.0f * 3.14159265f * 440.0f * (float) i / (float) kRate);
    dusk::audio::WriteSpec spec;
    spec.sampleRate = kRate;
    spec.numChannels = 1;
    spec.bitsPerSample = 32;
    auto writer = dusk::audio::FileWriter::create (path, spec);
    const float* channels[] = { samples.data() };
    return writer != nullptr && writer->write (channels, 1, kLength) && writer->flush();
}

// Peak of the left channel while the region plays, and before it starts.
struct Peaks { float before = 0.0f, during = 0.0f; };

std::optional<Peaks> readPeaks (const std::filesystem::path& path)
{
    auto reader = dusk::audio::FileReader::open (path);
    if (reader == nullptr) return std::nullopt;
    const auto frames = reader->info().numFrames;
    std::vector<float> left ((std::size_t) frames, 0.0f), right ((std::size_t) frames, 0.0f);
    float* dest[] = { left.data(), right.data() };
    reader->read (dest, std::min (2, reader->info().numChannels), 0, frames);
    Peaks peaks;
    for (std::int64_t i = 0; i < frames; ++i)
    {
        const float s = std::abs (left[(std::size_t) i]);
        if (i < kStart - 256) peaks.before = std::max (peaks.before, s);
        else if (i >= kStart + 256 && i < kStart + kLength - 256) peaks.during = std::max (peaks.during, s);
    }
    return peaks;
}

void bounceThen (ScenarioContext& ctx, const std::filesystem::path& out,
                 std::function<void (std::optional<Peaks>)> then)
{
    struct Run
    {
        std::unique_ptr<BounceEngine> bounce;
        std::atomic<bool> finished { false };
        bool ok = false;
        std::string error;
    };
    auto run = std::make_shared<Run>();
    run->bounce = std::make_unique<BounceEngine> (ctx.engine(), ctx.session());
    ctx.cleanup ([run] { run->bounce.reset(); });
    run->bounce->onFinished = [raw = run.get()] (bool ok, std::string error)
    {
        raw->ok = ok;
        raw->error = std::move (error);
        raw->finished.store (true, std::memory_order_release);
    };
    if (! run->bounce->start (SessionFile (out.u8string().c_str()), kRate, 1024, 0.2,
                              BounceEngine::Mode::MasterMix, BounceEngine::Format::Wav))
    {
        ctx.complete (ScenarioResult::fail ("the bounce refused to start: " + run->bounce->getLastError()));
        return;
    }
    ctx.waitUntil ([run] { return run->finished.load (std::memory_order_acquire)
                                  && ! run->bounce->isRendering(); },
                   kRenderTimeoutMs,
                   [&ctx, run, out, then]
                   {
                       if (! ctx.expect (run->ok, "the bounce failed: " + run->error))
                       {
                           ctx.complete (ctx.verdict());
                           return;
                       }
                       then (readPeaks (out));
                   },
                   "the bounce never finished");
}

// The fixture's output is its input times a gain only its LV2 Worker produces
// (Target / 100), so sound through the slot proves the worker round trip ran.
std::optional<ScenarioResult> runWorkerBounce (ScenarioContext& ctx)
{
    auto& strip = ctx.engine().getChannelStrip (kTrack);
    auto& slot = strip.getNativeLv2Slot();
    std::string error;
    if (! slot.load (*ctx.fixture ("worker.lv2"), kRate, ScenarioContext::kBlockSize, error, kPluginUri))
        return ScenarioResult::fail ("the worker fixture did not load: " + error);
    ctx.cleanup ([&strip] { strip.unloadNativeLv2(); });

    const auto source = ctx.tempDir() / "tone.wav";
    if (! writeTone (source))
        return ScenarioResult::fail ("could not write the source take");
    AudioRegion region;
    region.file = SessionFile (source.u8string().c_str());
    region.timelineStart = kStart;
    region.lengthInSamples = kLength;
    region.numChannels = 1;
    ctx.session().track (kTrack).mode.store ((int) Track::Mode::Mono, std::memory_order_relaxed);
    ctx.session().track (kTrack).regions.push_back (region);

    // Live: blocks at the loop's own pace until the worker thread has answered.
    auto blocks = std::make_shared<int> (0);
    ctx.waitUntil ([&slot, blocks]
                   {
                       std::array<float, ScenarioContext::kBlockSize> left {}, right {};
                       left.fill (0.5f);
                       right.fill (0.5f);
                       slot.processStereo (left.data(), right.data(), left.data(), right.data(),
                                           ScenarioContext::kBlockSize);
                       ++*blocks;
                       return std::abs (left.back() - 0.25f) < 1.0e-6f;
                   },
                   kLiveTimeoutMs,
                   [&ctx, &slot, blocks]
                   {
                       ctx.note ("live blocks until the worker answered: " + std::to_string (*blocks));
                       // Unloading keeps a slot's bypass, so a failed or timed-out
                       // dry pass must not leave track 0 dry for later scenarios.
                       ctx.cleanup ([&slot] { slot.setBypassed (false); });
                       slot.setBypassed (true);
                       bounceThen (ctx, ctx.sessionDir() / "dry.wav", [&ctx, &slot] (std::optional<Peaks> dry)
                       {
                           if (! ctx.expect (dry.has_value() && dry->during > 0.05f,
                                             "the dry reference bounce did not print the region"))
                           {
                               ctx.complete (ctx.verdict());
                               return;
                           }
                           slot.setBypassed (false);
                           bounceThen (ctx, ctx.sessionDir() / "wet.wav", [&ctx, dry] (std::optional<Peaks> wet)
                           {
                               if (ctx.expect (wet.has_value(), "the bounce through the fixture wrote no readable file"))
                               {
                                   const float ratio = dry->during > 0.0f ? wet->during / dry->during : 0.0f;
                                   ctx.note ("peaks: dry " + std::to_string (dry->during) + ", through the fixture "
                                             + std::to_string (wet->during) + ", ratio " + std::to_string (ratio));
                                   ctx.expect (wet->before < 1.0e-4f, "audio printed before the region starts");
                                   ctx.expect (std::abs (ratio - 0.5f) < 0.01f,
                                               "the bounce did not carry the gain the fixture's worker produced");
                               }
                               ctx.complete (ctx.verdict());
                           });
                       });
                   },
                   "the worker fixture never answered in live blocks");
    return std::nullopt;
}
#endif

const ScenarioRegistrar registrar { Scenario {
    "lv2.worker_processes_and_bounces",
    { "lv2", "worker", "bounce", "plugin" },
    Needs::Engine,
    { "worker.lv2" },
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if DUSKSTUDIO_HAS_NATIVE_LV2
        return runWorkerBounce (ctx);
       #else
        (void) ctx;
        return ScenarioResult::skip ("built without the native LV2 host");
       #endif
    },
    120000
} };
} // namespace
} // namespace duskstudio::scenario
