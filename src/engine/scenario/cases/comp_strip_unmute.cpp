#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../audiofile/FileWriter.h"
#include "../../../session/Session.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// A compressor whose strip is muted or soloed out, or whose bus is, while the
// programme plays on picks up again with the gain reduction it had when it is
// heard again, as it does after its strip sleeps: the level that comes back is
// the level it was compressing to. What it held from before never plays.
namespace duskstudio::scenario
{
namespace
{
constexpr double kRate = ScenarioContext::kSampleRate;
constexpr int kFrames = ScenarioContext::kBlockSize;
constexpr double kTwoPi = 6.283185307179586;
constexpr float kToneLevel = 0.5f;
// Every mode settles on the tone in 2 s, FET's slow tail included.
constexpr int kSettleBlocks = 375;
// About half a second muted.
constexpr int kCutBlocks = 94;
// What is read once a strip or bus is heard again: 100 ms.
constexpr int kMeasured = 4800;
// Reset there instead, the compressors came back 1.6 to 3 dB over their
// settled peak on a strip, under its 20 ms fade-in, and 7 to 15 dB on a bus,
// which has none. Kept, they come back within 0.1 dB, and a bus fed by a
// strip that was out within 0.9 dB: the bus compressor runs on the silence
// it drains before it sleeps.
constexpr double kMaxRiseDb = 1.0;
// Until the mix latency has passed after the block a strip or bus is heard
// again, the master plays what the stages held from before (the compressor
// stage's latency, then the master tape's), which must be silence. The tone
// the compressor held would reach 0.007 to 0.014 there under a strip's
// fade-in; a compressor reset there instead plays the start of the new input
// early through its filters, 0.003 on a bus, which has no fade-in.
constexpr float kHeldCeiling = 1.0e-3f;

std::string db (double v)
{
    char text[32] {};
    std::snprintf (text, sizeof text, "%+.2f dB", v);
    return text;
}

float tone (std::int64_t i)
{
    return kToneLevel * (float) std::sin (kTwoPi * 1000.0 * (double) i / kRate);
}

float peakOf (const std::vector<float>& v, std::size_t from, std::size_t to)
{
    float peak = 0.0f;
    for (auto i = from; i < std::min (to, v.size()); ++i)
        peak = std::max (peak, std::abs (v[i]));
    return peak;
}

float rmsOf (const std::vector<float>& v, std::size_t from, std::size_t to)
{
    double sum = 0.0;
    std::size_t n = 0;
    for (auto i = from; i < std::min (to, v.size()); ++i, ++n)
        sum += (double) v[i] * (double) v[i];
    return n == 0 ? 0.0f : (float) std::sqrt (sum / (double) n);
}

double ratioDb (float a, float b)
{
    return 20.0 * std::log10 (std::max (1.0e-9, (double) a) / std::max (1.0e-9, (double) b));
}

using SessionFile = std::decay_t<decltype (std::declval<const Session&>().getSessionDirectory())>;

// The tone, mono, with silence over [silentFrom, silentTo).
bool writeTone (const std::filesystem::path& path, int frames, int silentFrom, int silentTo)
{
    std::vector<float> samples ((std::size_t) frames);
    for (int i = 0; i < frames; ++i)
        samples[(std::size_t) i] = i >= silentFrom && i < silentTo ? 0.0f : tone (i);
    dusk::audio::WriteSpec spec;
    spec.sampleRate = kRate;
    spec.numChannels = 1;
    spec.bitsPerSample = 32;
    auto writer = dusk::audio::FileWriter::create (path, spec);
    const float* channels[] = { samples.data() };
    return writer != nullptr && writer->write (channels, 1, (std::int64_t) samples.size())
        && writer->flush();
}

void placeRegion (ScenarioContext& ctx, const std::filesystem::path& file, std::int64_t length)
{
    AudioRegion region;
    region.file = SessionFile (file.u8string().c_str());
    region.timelineStart = 0;
    region.lengthInSamples = length;
    region.numChannels = 1;
    auto& regions = ctx.session().track (0).regions;
    regions.clear();
    regions.push_back (region);
}

void keepSettings (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    for (int t = 0; t < 2; ++t)
    {
        auto& strip = session.track (t).strip;
        for (auto* value : { &strip.compEnabled, &strip.eqEnabled })
            ctx.keep (*value);
        ctx.keep (strip.compMode);
        ctx.cleanup ([&strip, values = strip.comp.values()] { strip.comp.setValues (values); });
    }
    for (int b = 0; b < 2; ++b)
    {
        auto& bus = session.bus (b).strip;
        for (auto* value : { &bus.faderDb, &bus.liveFaderDb })
            ctx.keep (*value);
        ctx.keep (bus.compEnabled);
        ctx.cleanup ([&bus, values = bus.comp.values()] { bus.comp.setValues (values); });
    }
    auto& master = session.master();
    for (auto* value : { &master.compEnabled, &master.tapeEnabled, &master.eqEnabled })
        ctx.keep (*value);
    ctx.keep (session.deviceCaptureChannels);
    ctx.cleanup ([&session] { session.recomputeRtCounters(); });
}

struct Result
{
    double peakDb = 0.0;
    double rmsDb = 0.0;
    float reductionDb = 0.0f;
    float heldPeak = 0.0f;
};

// Plays from the top until the compressor settles, runs `cut`, plays on for
// kCutBlocks, runs `restore`, and reads the master over 100 ms, from `skip`
// samples after the block it is heard in again (or from where the tone comes
// back, when `fromSound`), against the 100 ms before the cut.
Result playThrough (ScenarioContext& ctx, const std::function<void()>& cut,
                    const std::function<void()>& restore, const std::function<float()>& reduction,
                    int skip = 0, bool fromSound = false)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    session.recomputeRtCounters();
    engine.stop();
    engine.prepareForSelfTest (kRate, kFrames);
    engine.getTransport().setPlayhead (0);
    engine.getPlaybackEngine().preparePlayback();
    engine.play();
    std::vector<float> out;
    const auto pump = [&] (int blocks)
    {
        for (int b = 0; b < blocks; ++b)
        {
            if (out.size() % (16 * kFrames) == 0) engine.servicePlaybackForScenario();
            ctx.pump (1);
            const auto& block = ctx.lastBlock (0);
            out.insert (out.end(), block.begin(), block.end());
        }
    };
    pump (kSettleBlocks);
    Result result;
    result.reductionDb = reduction();
    const auto settledFrom = out.size() - (std::size_t) kMeasured;
    const float settledPeak = peakOf (out, settledFrom, out.size());
    const float settledRms = rmsOf (out, settledFrom, out.size());
    cut();
    session.recomputeRtCounters();
    pump (kCutBlocks);
    restore();
    session.recomputeRtCounters();
    const auto heard = out.size();
    auto from = heard + (std::size_t) skip;
    pump ((skip + 2 * kMeasured) / kFrames + 4);
    engine.stop();
    result.heldPeak = peakOf (out, heard, heard + (std::size_t) engine.getMixLatencySamples());
    if (fromSound)
        while (from < out.size() && std::abs (out[from]) < 1.0e-3f) ++from;
    result.peakDb = ratioDb (peakOf (out, from, from + kMeasured), settledPeak);
    result.rmsDb = ratioDb (rmsOf (out, from, from + kMeasured), settledRms);
    return result;
}

struct Mode
{
    const char* name;
    int mode;
    CompParamId control;
    float value;
};

ScenarioResult unmuteKeepsCompression (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    keepSettings (ctx);
    static constexpr int kToneFrames = 4 * 48000;
    static constexpr int kCutAt = kSettleBlocks * kFrames;
    const auto steady = ctx.tempDir() / "tone.wav";
    const auto gapped = ctx.tempDir() / "gapped.wav";
    if (! writeTone (steady, kToneFrames, kToneFrames, kToneFrames)
        || ! writeTone (gapped, kToneFrames, kCutAt, kCutAt + kCutBlocks * kFrames))
        return ScenarioResult::fail ("could not write the tone");
    session.master().tapeEnabled.store (false);
    session.master().eqEnabled.store (false);
    session.master().compEnabled.store (false);
    auto& strip = session.track (0).strip;
    auto& other = session.track (1).strip;
    strip.eqEnabled.store (false);

    const auto report = [&ctx] (const std::string& leg, const Result& r, bool held)
    {
        ctx.note (leg + ": " + db (r.reductionDb) + " of reduction; the first 100 ms peak "
                  + db (r.peakDb) + ", RMS " + db (r.rmsDb) + " against the settled level");
        if (! held) return;
        ctx.expect (r.peakDb <= kMaxRiseDb && r.rmsDb <= kMaxRiseDb,
                    leg + ": it came back " + db (r.peakDb) + " peak, " + db (r.rmsDb)
                        + " RMS over the level it had settled at");
        ctx.expect (r.peakDb >= -kMaxRiseDb,
                    leg + ": it did not come back to the level it had settled at, peak " + db (r.peakDb));
        ctx.expect (r.heldPeak <= kHeldCeiling,
                    leg + ": the master was not silent for the mix latency after it was heard again, peak "
                        + std::to_string (r.heldPeak));
    };

    const Mode modes[] {
        { "Opto", 0, CompParamId::OptoPeakReduction, 55.0f },
        { "FET", 1, CompParamId::FetThreshold, -10.0f },
        { "VCA", 2, CompParamId::VcaThreshold, -20.0f },
    };
    const auto stripReduction = [&engine] { return engine.getChannelStrip (0).getCurrentGrDb(); };
    for (const auto& m : modes)
    {
        const std::string name = std::string ("strip ") + m.name;
        strip.compMode.store (m.mode);
        strip.comp.set (m.control, m.value);
        strip.compEnabled.store (true);
        placeRegion (ctx, steady, kToneFrames);
        report (name + " muted", playThrough (ctx, [&] { strip.mute.store (true); },
                                              [&] { strip.mute.store (false); }, stripReduction), true);
        report (name + " soloed out", playThrough (ctx, [&] { other.solo.store (true); },
                                                   [&] { other.solo.store (false); }, stripReduction), true);
        // A compressor that heard the half second of silence instead, for
        // reference: it released, and attacks again when the tone returns.
        placeRegion (ctx, gapped, kToneFrames);
        report (name + " fed silence", playThrough (ctx, [] {}, [] {}, stripReduction, 0, true), false);
    }

    strip.compEnabled.store (false);

    auto& bus = session.bus (0).strip;
    bus.comp.set (CompParamId::BusThreshold, -6.0f);
    bus.compEnabled.store (true);
    strip.busAssign[0].store (true);
    placeRegion (ctx, steady, kToneFrames);
    const auto busReduction = [&bus] { return bus.meterGrDb.load(); };
    report ("bus muted", playThrough (ctx, [&] { bus.mute.store (true); }, [&] { bus.mute.store (false); },
                                      busReduction), true);
    // Soloing bus 2 mutes every track that feeds no soloed bus, so the track
    // feeds bus 2 too, its fader down, and keeps playing into bus 1.
    {
        auto& quiet = session.bus (1).strip;
        quiet.faderDb.store (ChannelStripParams::kFaderMinDb);
        strip.busAssign[1].store (true);
        report ("bus soloed out", playThrough (ctx, [&] { quiet.solo.store (true); },
                                               [&] { quiet.solo.store (false); }, busReduction), true);
        strip.busAssign[1].store (false);
    }
    // A bus-routed strip muted or soloed out fades back in through its bus
    // alone, with the bus compressing and with it flat. Part of it reaching
    // the master straight while it fades in came back 7.3 dB over the settled
    // level with the bus compressing; with it flat both ways carry the same.
    for (const bool compressed : { true, false })
    {
        bus.compEnabled.store (compressed);
        const std::string kind = compressed ? " (bus compressed)" : " (bus flat)";
        report ("bus-routed strip muted" + kind,
                playThrough (ctx, [&] { strip.mute.store (true); }, [&] { strip.mute.store (false); },
                             busReduction), true);
        report ("bus-routed strip soloed out" + kind,
                playThrough (ctx, [&] { other.solo.store (true); }, [&] { other.solo.store (false); },
                             busReduction), true);
    }
    bus.compEnabled.store (true);
    // Unrouted, the track plays straight to the master; its bus gain fades
    // back over 20 ms when it is routed again, so that is left out.
    report ("bus with the track routed away and back",
            playThrough (ctx, [&] { strip.busAssign[0].store (false); }, [&] { strip.busAssign[0].store (true); },
                         busReduction, 1200), false);
    placeRegion (ctx, gapped, kToneFrames);
    report ("bus fed silence", playThrough (ctx, [] {}, [] {}, busReduction, 0, true), false);
    bus.mute.store (false);
    session.bus (1).strip.solo.store (false);
    strip.busAssign[0].store (false);
    bus.compEnabled.store (false);
    return ctx.verdict();
}

const ScenarioRegistrar unmuteRegistrar { Scenario {
    "comp.unmute_keeps_compression", { "comp", "mute", "solo", "strip", "bus" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return unmuteKeepsCompression (ctx); } } };
} // namespace
} // namespace duskstudio::scenario
