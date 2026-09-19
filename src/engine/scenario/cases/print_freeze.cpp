#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../BounceEngine.h"
#include "../../audiofile/FileReader.h"
#include "../../audiofile/FileWriter.h"
#include "../../../session/Session.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace duskstudio::scenario
{
namespace
{
constexpr int kTrack = 0;
constexpr int kFrames = ScenarioContext::kBlockSize;
constexpr double kRate = ScenarioContext::kSampleRate;
constexpr double kTwoPi = 6.283185307179586;
constexpr int kRenderTimeoutMs = 90000;

// The engine's file type, named through Session's own getter so this file stays
// free of the framework header.
using SessionFile = std::decay_t<decltype (std::declval<const Session&>().getSessionDirectory())>;

std::filesystem::path pathOf (const SessionFile& file)
{
    return std::filesystem::u8path (file.getFullPathName().toStdString());
}

double db (double level, double reference)
{
    return 20.0 * std::log10 (std::max (level, 1.0e-9) / std::max (reference, 1.0e-9));
}

// RMS of a file's first channel over [from, to).
double fileRms (const std::filesystem::path& path, std::int64_t from, std::int64_t to)
{
    auto reader = dusk::audio::FileReader::open (path);
    if (reader == nullptr) return 0.0;
    const auto frames = reader->info().numFrames;
    to = std::min (to, frames);
    if (to <= from) return 0.0;
    std::vector<float> left ((std::size_t) frames), right ((std::size_t) frames);
    float* dest[] = { left.data(), right.data() };
    reader->read (dest, std::min (2, reader->info().numChannels), 0, frames);
    double sum = 0.0;
    for (auto i = from; i < to; ++i) sum += (double) left[(std::size_t) i] * left[(std::size_t) i];
    return std::sqrt (sum / (double) (to - from));
}

void restoreStrip (ScenarioContext& ctx)
{
    auto& track = ctx.session().track (kTrack);
    auto& strip = track.strip;
    ctx.cleanup ([&track, &strip]
    {
        track.printEffects.store (false);
        strip.eqEnabled.store (false);
        strip.hpfEnabled.store (false);
        strip.hpfFreq.store (20.0f);
        strip.compEnabled.store (false);
    });
}

// ------------------------------------------------------------------ PRINT

// Records `seconds` of a tone arriving on input 1 onto an empty track, and
// returns the take.
const AudioRegion* recordTone (ScenarioContext& ctx, double hz, float amp, double seconds)
{
    auto& engine = ctx.engine();
    auto& track = ctx.session().track (kTrack);
    track.regions.clear();
    engine.getPlaybackEngine().preparePlayback();
    track.mode.store ((int) Track::Mode::Mono);
    track.inputSource.store (-2);
    track.recordArmed.store (true);
    ctx.session().recomputeRtCounters();

    std::array<float, kFrames> inL {}, inR {}, outL {}, outR {};
    const float* inputs[] = { inL.data(), inR.data() };
    float* outputs[] = { outL.data(), outR.data() };
    engine.getTransport().setPlayhead (0);
    engine.record();
    if (! ctx.expect (engine.getTransport().isRecording(), "Record did not start with the track armed"))
        return nullptr;
    double phase = 0.0;
    const double step = kTwoPi * hz / kRate;
    const int blocks = (int) (seconds * kRate / kFrames);
    for (int block = 0; block < blocks; ++block)
    {
        for (auto& s : inL)
        {
            s = amp * (float) std::sin (phase);
            phase = std::fmod (phase + step, kTwoPi);
        }
        engine.audioDeviceIOCallback (inputs, 2, outputs, 2, kFrames, {});
    }
    engine.stop();
    track.recordArmed.store (false);
    ctx.session().recomputeRtCounters();
    return track.regions.empty() ? nullptr : &track.regions.back();
}

// With PRINT off the take is the dry input; with it on the take carries the
// channel's EQ and compressor.
ScenarioResult printCommitsTheStrip (ScenarioContext& ctx)
{
    restoreStrip (ctx);
    auto& track = ctx.session().track (kTrack);
    auto& strip = track.strip;
    // A 300 Hz HPF against a 60 Hz tone: a cut the take cannot miss.
    strip.eqEnabled.store (true);
    strip.hpfEnabled.store (true);
    strip.hpfFreq.store (300.0f);
    constexpr float kAmp = 0.25f;
    const double dryRms = kAmp / std::sqrt (2.0);
    const std::int64_t from = (std::int64_t) (0.25 * kRate), to = (std::int64_t) (0.75 * kRate);

    const auto* dry = recordTone (ctx, 60.0, kAmp, 1.0);
    if (! ctx.expect (dry != nullptr, "recording with PRINT off made no take"))
        return ctx.verdict();
    const double dryTake = db (fileRms (pathOf (dry->file), from, to), dryRms);

    track.printEffects.store (true);
    const auto* printed = recordTone (ctx, 60.0, kAmp, 1.0);
    if (! ctx.expect (printed != nullptr, "recording with PRINT on made no take"))
        return ctx.verdict();
    const double printedTake = db (fileRms (pathOf (printed->file), from, to), dryRms);

    ctx.note ("60 Hz take against the input: PRINT off " + std::to_string (dryTake)
              + " dB, PRINT on " + std::to_string (printedTake) + " dB");
    ctx.expect (std::abs (dryTake) < 0.5, "with PRINT off the take is not the dry input");
    ctx.expect (printedTake < -12.0, "with PRINT on the take does not carry the channel's HPF");

    // The compressor prints too: a hot 1 kHz tone into a hard FET setting.
    strip.eqEnabled.store (false);
    strip.compEnabled.store (true);
    strip.compMode.store (1);
    const float fetThreshold = strip.compFetThresholdDb.load();
    ctx.cleanup ([&strip, fetThreshold] { strip.compFetThresholdDb.store (fetThreshold); });
    strip.compFetThresholdDb.store (-30.0f);
    const auto* compressed = recordTone (ctx, 1000.0, 0.5f, 1.0);
    if (ctx.expect (compressed != nullptr, "recording with the compressor on made no take"))
    {
        const double squeezed = db (fileRms (pathOf (compressed->file), from, to), 0.5 / std::sqrt (2.0));
        ctx.note ("1 kHz take with the compressor printed: " + std::to_string (squeezed) + " dB");
        ctx.expect (squeezed < -3.0, "with PRINT on the take does not carry the compressor");
    }
    return ctx.verdict();
}

// ------------------------------------------------------------------ FREEZE

// A click at `at` in a mono source, placed on track 1 at `start`.
bool placeClick (ScenarioContext& ctx, std::int64_t start, std::int64_t at, std::int64_t length)
{
    std::vector<float> samples ((std::size_t) length, 0.0f);
    samples[(std::size_t) at] = 0.5f;
    const auto path = ctx.tempDir() / "click.wav";
    dusk::audio::WriteSpec spec;
    spec.sampleRate = kRate;
    spec.numChannels = 1;
    spec.bitsPerSample = 32;
    auto writer = dusk::audio::FileWriter::create (path, spec);
    const float* channels[] = { samples.data() };
    if (writer == nullptr || ! writer->write (channels, 1, length) || ! writer->flush())
        return false;
    writer.reset();

    AudioRegion region;
    region.file = SessionFile (path.u8string().c_str());
    region.timelineStart = start;
    region.lengthInSamples = length;
    region.numChannels = 1;
    auto& track = ctx.session().track (kTrack);
    track.mode.store ((int) Track::Mode::Mono);
    track.regions.push_back (region);
    ctx.engine().getPlaybackEngine().preparePlayback();
    return true;
}

struct Click
{
    std::int64_t at = -1;
    float peak = 0.0f;
};

// Plays from bar 1 and reports where on the output the click peaks, and how
// loud.
Click playClick (ScenarioContext& ctx, std::int64_t through)
{
    auto& engine = ctx.engine();
    engine.getTransport().setPlayhead (0);
    engine.getPlaybackEngine().preparePlayback();
    engine.play();
    std::int64_t best = -1;
    float peak = 0.0f;
    for (std::int64_t done = 0; done < through; done += kFrames)
    {
        ctx.pump (1);
        const auto& block = ctx.lastBlock (0);
        for (int i = 0; i < kFrames; ++i)
            if (std::abs (block[(std::size_t) i]) > peak)
            {
                peak = std::abs (block[(std::size_t) i]);
                best = done + i;
            }
    }
    engine.stop();
    return { peak > 0.05f ? best : -1, peak };
}

// FREEZE renders the track and plays the render with the track's DSP out of
// the way, at 1x and at 4x: the frozen track is heard, and turning an EQ band
// up does not touch it.
std::optional<ScenarioResult> freezeRendersAndBypasses (ScenarioContext& ctx)
{
    static constexpr std::int64_t kStart = 24000;
    static constexpr std::int64_t kClick = 4800;
    static constexpr std::int64_t kLength = 9600;
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    const int factorWas = session.oversamplingFactor.load();
    auto& strip = session.track (kTrack).strip;
    ctx.cleanup ([&engine, &session, &strip, factorWas]
    {
        strip.eqEnabled.store (false);
        strip.hfGainDb.store (0.0f);
        engine.unfreezeTrack (kTrack);
        session.oversamplingFactor.store (factorWas);
        engine.prepareForSelfTest (kRate, kFrames);
    });

    if (! placeClick (ctx, kStart, kClick, kLength))
        return ScenarioResult::fail ("could not write the source take");

    struct Pass
    {
        int factor;
        std::int64_t live = -1;
    };
    auto passes = std::make_shared<std::vector<Pass>> (std::vector<Pass> { { 1 }, { 4 } });
    auto next = std::make_shared<std::function<void (std::size_t)>>();
    std::weak_ptr<std::function<void (std::size_t)>> weakNext = next;

    *next = [&ctx, &engine, &session, &strip, passes, weakNext] (std::size_t index)
    {
        if (index >= passes->size())
        {
            ctx.complete (ctx.verdict());
            return;
        }
        auto& pass = (*passes)[index];
        engine.unfreezeTrack (kTrack);
        session.oversamplingFactor.store (pass.factor);
        engine.prepareForSelfTest (kRate, kFrames);
        const auto live = playClick (ctx, kStart + kLength);
        pass.live = live.at;
        if (! ctx.expect (pass.live >= 0, std::to_string (pass.factor) + "x: the live track was silent"))
        {
            ctx.complete (ctx.verdict());
            return;
        }
        // The same boost has to move the live click, or the frozen check below
        // proves nothing.
        strip.eqEnabled.store (true);
        strip.hfGainDb.store (15.0f);
        const float liveBoosted = playClick (ctx, kStart + kLength).peak;
        strip.eqEnabled.store (false);
        strip.hfGainDb.store (0.0f);
        ctx.expect (liveBoosted > 1.4f * live.peak,
                    std::to_string (pass.factor) + "x: an HF boost did not move the live click");

        SessionFile out;
        std::int64_t length = 0;
        if (! ctx.expect (engine.freezePrepare (kTrack, out, length),
                          "freeze refused: " + engine.getLastFreezeError().toStdString()))
        {
            ctx.complete (ctx.verdict());
            return;
        }
        auto bounce = std::make_shared<BounceEngine> (engine, session);
        ctx.cleanup ([bounce] { bounce->cancel(); });
        auto finished = std::make_shared<std::atomic<bool>> (false);
        auto ok = std::make_shared<std::atomic<bool>> (false);
        bounce->onFinished = [finished, ok] (bool success, std::string)
        {
            ok->store (success);
            finished->store (true, std::memory_order_release);
        };
        if (! ctx.expect (bounce->startFreeze (kTrack, out, length, kRate),
                          "the freeze render did not start"))
        {
            ctx.complete (ctx.verdict());
            return;
        }
        // The pending wait owns the chain from here, so it outlives this call.
        auto self = weakNext.lock();
        ctx.waitUntil ([finished, bounce] { return finished->load (std::memory_order_acquire)
                                                   && ! bounce->isRendering(); },
                       kRenderTimeoutMs,
                       [&ctx, &engine, &session, &strip, passes, index, out, length, ok, self]
                       {
                           auto& done = (*passes)[index];
                           if (! ctx.expect (ok->load(), std::to_string (done.factor) + "x: the freeze render failed"))
                           {
                               ctx.complete (ctx.verdict());
                               return;
                           }
                           engine.commitFreeze (kTrack, out, length);
                           ctx.expect (session.track (kTrack).frozen.load(),
                                       std::to_string (done.factor) + "x: the track is not frozen");
                           ctx.expect (std::filesystem::exists (pathOf (out)),
                                       std::to_string (done.factor) + "x: the frozen audio is not on disk");
                           const auto frozen = playClick (ctx, kStart + kLength);
                           strip.eqEnabled.store (true);
                           strip.hfGainDb.store (15.0f);
                           const auto boosted = playClick (ctx, kStart + kLength);
                           strip.eqEnabled.store (false);
                           strip.hfGainDb.store (0.0f);
                           const auto label = std::to_string (done.factor) + "x: ";
                           ctx.note (label + "live click at " + std::to_string (done.live) + ", frozen at "
                                     + std::to_string (frozen.at) + " peak " + std::to_string (frozen.peak)
                                     + ", with HF +15 dB peak " + std::to_string (boosted.peak));
                           ctx.expect (frozen.at >= 0, label + "the frozen track is silent");
                           ctx.expect (std::abs (boosted.peak - frozen.peak) < 1.0e-4f,
                                       label + "the channel EQ still acts on the frozen track");
                           (*self) (index + 1);
                       },
                       "the freeze render never finished");
    };
    (*next) (0);
    return std::nullopt;
}

std::optional<ScenarioResult> run (ScenarioResult (*body) (ScenarioContext&), ScenarioContext& ctx)
{
    return body (ctx);
}

const ScenarioRegistrar printRegistrar { Scenario {
    "strip.print_commits_the_strip", { "strip", "record", "dsp" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (printCommitsTheStrip, ctx); } } };
const ScenarioRegistrar freezeRegistrar { Scenario {
    "strip.freeze_renders_and_bypasses", { "strip", "freeze" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return freezeRendersAndBypasses (ctx); }, 180000 } };
} // namespace
} // namespace duskstudio::scenario
