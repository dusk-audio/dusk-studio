#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../audiofile/FileReader.h"
#include "../../audiofile/FileWriter.h"
#include "../../../session/Session.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// The engine suspends processing around a save, a plug-in load and Record, and
// a device callback that lands meanwhile plays silence. A compressor that was
// compressing picks up again where it was: no level jump after the silence,
// and none at the start of a take printed through it. What its delay held from
// before the silence never plays.
namespace duskstudio::scenario
{
namespace
{
constexpr double kRate = ScenarioContext::kSampleRate;
constexpr int kFrames = ScenarioContext::kBlockSize;
constexpr double kTwoPi = 6.283185307179586;
constexpr double kToneHz = 1000.0;
constexpr float kToneLevel = 0.5f;
// Long enough for every mode's attack and release to settle on the tone.
constexpr int kSettleBlocks = 240;
// Levels are read as the peak of each millisecond, one period of the tone.
constexpr int kWindow = 48;
// What is measured after the silence, from the first sample the compressor
// plays of what came after it.
constexpr int kMeasured = 4800;
// A compressor that saw one block of silence instead rises by under 1 dB:
// measured at 0.3 to 1.0 dB on the four kinds.
constexpr double kMaxRiseDb = 1.0;
// The latency a stage held from before the silence must not play: that much of
// the tone peaks at 0.05 to 0.1. A compressor reset there instead starts on
// the new input with its filters' pre-ring, under 2e-4.
constexpr float kHeldCeiling = 1.0e-3f;

std::string str (std::int64_t v) { return std::to_string (v); }

std::string db (double v)
{
    char text[32] {};
    std::snprintf (text, sizeof text, "%.2f dB", v);
    return text;
}

float tone (std::int64_t i)
{
    return kToneLevel * (float) std::sin (kTwoPi * kToneHz * (double) i / kRate);
}

float peakOf (const std::vector<float>& v, std::size_t from, std::size_t to)
{
    float peak = 0.0f;
    for (auto i = from; i < std::min (to, v.size()); ++i)
        peak = std::max (peak, std::abs (v[i]));
    return peak;
}

// The loudest millisecond of v over [from, to), in dB against `settled`.
double worstRiseDb (const std::vector<float>& v, std::size_t from, std::size_t to, float settled)
{
    double worst = -200.0;
    for (auto w = from; w + kWindow <= std::min (to, v.size()); w += kWindow)
    {
        const float peak = peakOf (v, w, w + kWindow);
        worst = std::max (worst, 20.0 * std::log10 (std::max (1.0e-9, (double) peak / (double) settled)));
    }
    return worst;
}

using SessionFile = std::decay_t<decltype (std::declval<const Session&>().getSessionDirectory())>;

bool writeTone (const std::filesystem::path& path, int frames)
{
    std::vector<float> samples ((std::size_t) frames);
    for (int i = 0; i < frames; ++i) samples[(std::size_t) i] = tone (i);
    dusk::audio::WriteSpec spec;
    spec.sampleRate = kRate;
    spec.numChannels = 1;
    spec.bitsPerSample = 32;
    auto writer = dusk::audio::FileWriter::create (path, spec);
    const float* channels[] = { samples.data() };
    return writer != nullptr && writer->write (channels, 1, (std::int64_t) samples.size())
        && writer->flush();
}

// Every compressor setting a case touches goes back as it was.
void keepCompressors (ScenarioContext& ctx, int tracks)
{
    auto& session = ctx.session();
    for (int t = 0; t < tracks; ++t)
    {
        auto& strip = session.track (t).strip;
        for (auto* value : { &strip.compEnabled, &strip.eqEnabled })
            ctx.keep (*value);
        ctx.keep (strip.compMode);
        ctx.cleanup ([&strip, values = strip.comp.values()] { strip.comp.setValues (values); });
    }
    auto& bus = session.bus (0).strip;
    ctx.keep (bus.compEnabled);
    ctx.cleanup ([&bus, values = bus.comp.values()] { bus.comp.setValues (values); });
    auto& master = session.master();
    for (auto* value : { &master.compEnabled, &master.tapeEnabled, &master.eqEnabled })
        ctx.keep (*value);
    ctx.cleanup ([&master, values = master.comp.values()] { master.comp.setValues (values); });
}

// ----------------------------------------------------------------- console

enum class Where { Strip, Bus, Master };

const char* nameOf (Where where)
{
    switch (where)
    {
        case Where::Strip: return "strip";
        case Where::Bus:   break;
        case Where::Master: return "master";
    }
    return "bus";
}

// Plays the tone through one engaged compressor until it settles, then one
// callback lands while processing is suspended, as one does under a save or a
// plug-in load. After the silence the compressor's output is back at the level
// it had settled at, and the samples its stage held from before are silent.
void suspendWhilePlaying (ScenarioContext& ctx, Where where)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& strip = session.track (0).strip;
    auto& bus = session.bus (0).strip;
    auto& master = session.master();
    const std::string leg = nameOf (where);

    strip.compEnabled.store (where == Where::Strip);
    bus.compEnabled.store (where == Where::Bus);
    master.compEnabled.store (where == Where::Master);
    strip.busAssign[0].store (where == Where::Bus);
    engine.stop();
    engine.prepareForSelfTest (kRate, kFrames);
    const int held = engine.getStripCompLatencySamples() + engine.getBusCompLatencySamples()
                   + engine.getMasterCompLatencySamples();
    if (! ctx.expect (held > 0, leg + ": the engaged compressor gave its stage no latency"))
        return;

    engine.getTransport().setPlayhead (0);
    engine.getPlaybackEngine().preparePlayback();
    engine.play();
    std::vector<float> before;
    for (int b = 0; b < kSettleBlocks; ++b)
    {
        if (b % 16 == 0) engine.servicePlaybackForScenario();
        ctx.pump (1);
        const auto& block = ctx.lastBlock (0);
        before.insert (before.end(), block.begin(), block.end());
    }
    const float settled = peakOf (before, before.size() - 4 * kFrames, before.size());
    const float reductionDb = where == Where::Strip ? engine.getChannelStrip (0).getCurrentGrDb()
                            : where == Where::Bus   ? bus.meterGrDb.load()
                                                    : master.meterGrDb.load();

    const auto gated = engine.getGatedBlockCount();
    engine.suspendProcessing();
    ctx.pump (1);
    engine.resumeProcessing();
    ctx.expect (engine.getGatedBlockCount() == gated + 1,
                leg + ": the callback during the suspension was not gated");

    // Delays after the stage (the master tape's) hold audio of their own, so
    // what the stage held reaches the output just before the mix latency.
    const int mix = engine.getMixLatencySamples();
    std::vector<float> after;
    for (int b = 0; (int) after.size() < mix + kMeasured; ++b)
    {
        if (b % 16 == 0) engine.servicePlaybackForScenario();
        ctx.pump (1);
        const auto& block = ctx.lastBlock (0);
        after.insert (after.end(), block.begin(), block.end());
    }
    engine.stop();

    const float heldPeak = peakOf (after, (std::size_t) (mix - held), (std::size_t) mix);
    const double rise = worstRiseDb (after, (std::size_t) mix, (std::size_t) (mix + kMeasured), settled);
    ctx.note (leg + ": " + db (reductionDb) + " of gain reduction when suspended; after the silence the "
              + str (held) + " samples the stage held peak at " + std::to_string (heldPeak)
              + " and the loudest millisecond is " + db (rise) + " against the settled level");
    ctx.expect (heldPeak <= kHeldCeiling,
                leg + ": what the compressor stage held from before the silence played after it, peak "
                    + std::to_string (heldPeak));
    ctx.expect (rise <= kMaxRiseDb,
                leg + ": after the suspension the compressor came back " + db (rise)
                    + " above the level it had settled at");
}

ScenarioResult suspensionKeepsCompression (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    keepCompressors (ctx, 1);
    const auto source = ctx.tempDir() / "tone.wav";
    static constexpr int kToneFrames = 3 * 48000;
    if (! writeTone (source, kToneFrames))
        return ScenarioResult::fail ("could not write the tone");
    AudioRegion region;
    region.file = SessionFile (source.u8string().c_str());
    region.timelineStart = 0;
    region.lengthInSamples = kToneFrames;
    region.numChannels = 1;
    session.track (0).regions.push_back (region);

    auto& strip = session.track (0).strip;
    strip.eqEnabled.store (false);
    strip.compMode.store (2);
    strip.comp.set (CompParamId::VcaThreshold, -20.0f);
    session.bus (0).strip.comp.set (CompParamId::BusThreshold, -6.0f);
    session.master().comp.set (CompParamId::BusThreshold, -6.0f);
    session.master().tapeEnabled.store (false);
    session.master().eqEnabled.store (false);
    session.recomputeRtCounters();

    for (const auto where : { Where::Strip, Where::Bus, Where::Master })
        suspendWhilePlaying (ctx, where);
    return ctx.verdict();
}

// ------------------------------------------------------------------- print

constexpr int kDry = 0;
constexpr int kPrinted = 1;

// Input fed to the engine a block at a time: the tone on input 1, counted from
// the first block fed, so it runs on through a block the engine gates.
struct ToneFeed
{
    std::int64_t fed = 0;
    std::array<float, kFrames> inL {}, inR {}, outL {}, outR {};

    void blocks (AudioEngine& engine, int count)
    {
        for (int block = 0; block < count; ++block)
        {
            for (int i = 0; i < kFrames; ++i) inL[(std::size_t) i] = tone (fed + i);
            const float* inputs[] = { inL.data(), inR.data() };
            float* outputs[] = { outL.data(), outR.data() };
            engine.audioDeviceIOCallback (inputs, 2, outputs, 2, kFrames, {});
            fed += kFrames;
        }
    }
};

std::vector<float> takeSamples (const AudioTake& take)
{
    std::vector<float> left ((std::size_t) take.lengthInSamples, 0.0f), right (left);
    auto reader = dusk::audio::FileReader::open (take.filePath());
    if (reader == nullptr) return {};
    float* dest[] = { left.data(), right.data() };
    if (reader->read (dest, std::min (2, reader->info().numChannels), take.sourceOffset, take.lengthInSamples)
        != take.lengthInSamples)
        return {};
    return left;
}

// Record pressed while playing, with the take printed through an Opto
// compressor already compressing what is played into it. Record suspends
// processing; with a device callback landing in that suspension, the printed
// take starts at the level the compressor had settled at, as it does when no
// callback lands there.
ScenarioResult printAfterSuspensionKeepsCompression (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    keepCompressors (ctx, 2);
    ctx.cleanup ([&engine, offsetWas = engine.getRecordingLatencyOffsetSamples()]
    {
        engine.setRecordingLatencyOffsetSamples (offsetWas);
    });
    for (auto* value : { &session.countInEnabled, &session.preRollEnabled, &session.postRollEnabled })
    {
        ctx.keep (*value);
        value->store (false);
    }
    ctx.keep (session.deviceCaptureChannels);
    session.deviceCaptureChannels.store (2);
    engine.setRecordingLatencyOffsetSamples (0);
    for (const int t : { kDry, kPrinted })
    {
        auto& track = session.track (t);
        ctx.keep (track.printEffects);
        track.inputSource.store (0);
        track.recordArmed.store (true);
        track.inputMonitor.store (t == kPrinted);
        track.printEffects.store (t == kPrinted);
        track.strip.eqEnabled.store (false);
        track.strip.compEnabled.store (t == kPrinted);
    }
    auto& printed = session.track (kPrinted).strip;
    printed.compMode.store (0);
    printed.comp.set (CompParamId::OptoPeakReduction, 55.0f);
    session.recomputeRtCounters();

    struct Take
    {
        double riseDb = 0.0;
        double reductionDb = 0.0;
    };
    const auto recordOnce = [&] (bool gated, Take& result)
    {
        const std::string leg = gated ? "a callback in Record's suspension" : "no callback in Record's suspension";
        for (const int t : { kDry, kPrinted })
        {
            session.track (t).regions.clear();
            session.track (t).takes.clear();
        }
        engine.prepareForSelfTest (kRate, kFrames);
        engine.getPlaybackEngine().preparePlayback();
        transport.setPlayhead (96000);
        engine.play();
        ToneFeed feed;
        feed.blocks (engine, kSettleBlocks);
        const auto gatedBefore = engine.getGatedBlockCount();
        // A device callback that lands while Record prepares the take; Record's
        // own resume ends the suspension.
        if (gated)
        {
            engine.suspendProcessing();
            feed.blocks (engine, 1);
        }
        const auto pressed = std::chrono::steady_clock::now();
        engine.record();
        const auto took = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - pressed).count();
        ctx.note (leg + ": Record took " + std::to_string (took) + " ms on the message thread");
        if (! ctx.expect (transport.isRecording(), leg + ": Record did not start while playing"))
            return false;
        ctx.expect (engine.getGatedBlockCount() == gatedBefore + (gated ? 1 : 0),
                    leg + ": the engine gated " + str (engine.getGatedBlockCount() - gatedBefore) + " callbacks");
        feed.blocks (engine, 96);
        engine.stop();

        const auto& dryTakes = session.track (kDry).takes;
        const auto& printedTakes = session.track (kPrinted).takes;
        if (! ctx.expect (dryTakes.size() == 1 && printedTakes.size() == 1, leg + ": a track did not record one take"))
            return false;
        const auto dry = takeSamples (dryTakes.front());
        const auto print = takeSamples (printedTakes.front());
        if (! ctx.expect (print.size() > (std::size_t) (2 * kMeasured) && dry.size() >= print.size(),
                          leg + ": the takes are too short to measure"))
            return false;
        const float settled = peakOf (print, print.size() - (std::size_t) kMeasured, print.size());
        result.riseDb = worstRiseDb (print, 0, (std::size_t) kMeasured, settled);
        result.reductionDb = 20.0 * std::log10 (std::max (1.0e-9f, settled)
                                                / std::max (1.0e-9f, peakOf (dry, 0, dry.size())));
        ctx.note (leg + ": the printed take settles " + db (result.reductionDb) + " under the dry one; its loudest "
                  "millisecond in the first 100 ms rises " + db (result.riseDb) + " above that");
        return true;
    };

    Take plain, suspended;
    if (! recordOnce (false, plain) || ! recordOnce (true, suspended))
        return ctx.verdict();
    ctx.expect (plain.reductionDb < -3.0,
                "the Opto compressor did not compress the printed take: " + db (plain.reductionDb));
    ctx.expect (plain.riseDb <= kMaxRiseDb,
                "with no callback in Record's suspension the printed take starts " + db (plain.riseDb)
                    + " above its settled level");
    ctx.expect (suspended.riseDb <= kMaxRiseDb,
                "with a callback in Record's suspension the printed take starts " + db (suspended.riseDb)
                    + " above its settled level, against " + db (plain.riseDb) + " with none");
    return ctx.verdict();
}

const ScenarioRegistrar consoleRegistrar { Scenario {
    "comp.suspension_keeps_compression", { "comp", "suspend", "strip", "bus" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return suspensionKeepsCompression (ctx); } } };
const ScenarioRegistrar printRegistrar { Scenario {
    "record.print_after_suspension_keeps_compression", { "record", "print", "comp", "suspend" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return printAfterSuspensionKeepsCompression (ctx); } } };
} // namespace
} // namespace duskstudio::scenario
