#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../BounceEngine.h"
#include "../../audiofile/FileReader.h"
#include "../../audiofile/FileWriter.h"
#include "../../../session/Session.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// The compressors' latency is held per mixer stage (the strips, the buses, the
// master) and only while a stage needs it: every member of an engaged stage
// carries it, a stage nobody engages never gets it, and switching a compressor
// while playing never moves the timing. These drive the whole engine.
namespace duskstudio::scenario
{
namespace
{
constexpr double kRate = ScenarioContext::kSampleRate;
constexpr int kFrames = ScenarioContext::kBlockSize;
constexpr int kRenderTimeoutMs = 90000;
constexpr std::int64_t kAt = 24000;

using SessionFile = std::decay_t<decltype (std::declval<const Session&>().getSessionDirectory())>;

SessionFile sessionFile (const std::filesystem::path& path)
{
    return SessionFile (path.u8string().c_str());
}

std::filesystem::path pathOf (const SessionFile& file)
{
    return std::filesystem::u8path (file.getFullPathName().toStdString());
}

// A click at `at` in a mono file of `frames` samples.
bool writeClicks (const std::filesystem::path& path, int frames, const std::vector<int>& at)
{
    std::vector<float> samples ((std::size_t) frames, 0.0f);
    for (const int i : at) samples[(std::size_t) i] = 0.5f;
    dusk::audio::WriteSpec spec;
    spec.sampleRate = kRate;
    spec.numChannels = 1;
    spec.bitsPerSample = 32;
    auto writer = dusk::audio::FileWriter::create (path, spec);
    const float* channels[] = { samples.data() };
    return writer != nullptr && writer->write (channels, 1, (std::int64_t) samples.size())
        && writer->flush();
}

void placeRegion (ScenarioContext& ctx, int track, const std::filesystem::path& file,
                  std::int64_t start, std::int64_t length)
{
    AudioRegion region;
    region.file = sessionFile (file);
    region.timelineStart = start;
    region.lengthInSamples = length;
    region.numChannels = 1;
    ctx.session().track (track).mode.store ((int) Track::Mode::Mono, std::memory_order_relaxed);
    ctx.session().track (track).regions.push_back (region);
}

std::optional<std::vector<float>> readLeft (const std::filesystem::path& path)
{
    auto reader = dusk::audio::FileReader::open (path);
    if (reader == nullptr) return std::nullopt;
    const auto info = reader->info();
    std::vector<float> left ((std::size_t) info.numFrames, 0.0f), right (left);
    float* dest[] = { left.data(), right.data() };
    reader->read (dest, std::min (2, info.numChannels), 0, info.numFrames);
    return left;
}

std::int64_t argmaxAbs (const std::vector<float>& samples, std::int64_t from = 0, std::int64_t to = -1)
{
    if (to < 0) to = (std::int64_t) samples.size();
    std::int64_t at = from;
    for (std::int64_t i = from; i < to; ++i)
        if (std::abs (samples[(std::size_t) i]) > std::abs (samples[(std::size_t) at])) at = i;
    return at;
}

// Every compressor setting a case touches goes back as it was.
void keepCompressors (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto& strip = session.track (t).strip;
        ctx.keep (strip.compEnabled);
        ctx.keep (strip.compMode);
        ctx.keep (strip.faderDb);
        ctx.keep (strip.auxSendPreFader[0]);
        const auto values = strip.comp.values();
        ctx.cleanup ([&strip, values] { strip.comp.setValues (values); });
    }
    for (int b = 0; b < Session::kNumBuses; ++b)
    {
        auto& bus = session.bus (b).strip;
        ctx.keep (bus.compEnabled);
        const auto values = bus.comp.values();
        ctx.cleanup ([&bus, values] { bus.comp.setValues (values); });
    }
    auto& master = session.master();
    ctx.keep (master.compEnabled);
    ctx.keep (master.tapeEnabled);
    const auto values = master.comp.values();
    ctx.cleanup ([&master, values] { master.comp.setValues (values); });
}

// Compressors that leave a click at half scale alone, so where it lands is
// all that changes.
void transparentStripComp (ChannelStripParams& strip)
{
    strip.compMode.store (2);
    strip.comp.set (CompParamId::VcaThreshold, 0.0f);
}

void transparentBusComp (CompParams& comp)
{
    comp.set (CompParamId::BusThreshold, 15.0f);
}

// Mutes every track but `audible`, plays from the top and returns where on
// the output the loudest sample was, within `through` samples.
std::int64_t landsAt (ScenarioContext& ctx, int audible, int tracks, std::int64_t through)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    for (int t = 0; t < tracks; ++t)
        session.track (t).strip.mute.store (t != audible, std::memory_order_relaxed);
    session.recomputeRtCounters();
    engine.getTransport().setPlayhead (0);
    engine.getPlaybackEngine().preparePlayback();
    engine.play();
    std::int64_t at = -1;
    float best = 0.0f;
    for (std::int64_t done = 0; done < through; done += kFrames)
    {
        ctx.pump (1);
        const auto& out = ctx.lastBlock (0);
        for (int i = 0; i < kFrames; ++i)
            if (std::abs (out[(std::size_t) i]) > best)
            {
                best = std::abs (out[(std::size_t) i]);
                at = done + i;
            }
    }
    engine.stop();
    return best > 0.05f ? at : -1;
}

int stripCompLatency (AudioEngine& engine)
{
    return engine.getChannelStrip (0).getCompressor (false).latencySamples();
}

std::string str (std::int64_t v) { return std::to_string (v); }

// ------------------------------------------------------------------ strips

// The same click on two tracks, one with its compressor engaged, lands on the
// same sample at the master, and where the reported latency says. With every
// compressor off and the transport stopped, the strips' latency goes, and
// the click lands that much earlier.
ScenarioResult stripStageAlignsTracks (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    keepCompressors (ctx);
    const auto source = ctx.tempDir() / "click.wav";
    if (! writeClicks (source, 4800, { 0 }))
        return ScenarioResult::fail ("could not write the click");
    placeRegion (ctx, 0, source, kAt, 4800);
    placeRegion (ctx, 1, source, kAt, 4800);
    auto& compressed = session.track (1).strip;
    transparentStripComp (compressed);
    compressed.compEnabled.store (true);
    engine.prepareForSelfTest (kRate, kFrames);

    const int latency = stripCompLatency (engine);
    ctx.note ("strip compressor latency " + str (latency) + ", reported "
              + str (engine.getStripCompLatencySamples()));
    if (! ctx.expect (latency > 0 && engine.getStripCompLatencySamples() == latency,
                      "a session with a strip compressor engaged does not hold its latency"))
        return ctx.verdict();

    const auto through = kAt + 8 * kFrames;
    const auto plain = landsAt (ctx, 0, 2, through);
    const auto withComp = landsAt (ctx, 1, 2, through);
    const auto onLatency = engine.getMixLatencySamples();
    ctx.note ("plain track at " + str (plain) + ", compressed track at " + str (withComp)
              + ", reported mix latency " + str (onLatency));
    ctx.expect (plain >= 0 && plain == withComp,
                "the compressed track played " + str (withComp - plain) + " samples after the plain one");
    ctx.expect (plain - kAt == onLatency, "the click lands " + str (plain - kAt)
                + " samples late, the reported latency is " + str (onLatency));

    compressed.compEnabled.store (false);
    ctx.pump (16);   // stopped: the stage fades its latency out
    ctx.expect (engine.getStripCompLatencySamples() == 0,
                "the strips kept their latency with every compressor off and the transport stopped");
    const auto off = landsAt (ctx, 0, 2, through);
    ctx.note ("with the latency gone the click is at " + str (off) + ", reported mix latency "
              + str (engine.getMixLatencySamples()));
    ctx.expect (off == plain - latency, "the click did not move back by the compressor latency");
    ctx.expect (off - kAt == engine.getMixLatencySamples(), "the reported latency does not match the click");
    return ctx.verdict();
}

// ------------------------------------------------------------------- buses

// A bus with its compressor engaged stays aligned with a track routed straight
// to the master and with an aux return: both carry the buses' latency too.
ScenarioResult busStageAlignsBypassPaths (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    keepCompressors (ctx);
    const auto source = ctx.tempDir() / "click.wav";
    if (! writeClicks (source, 4800, { 0 }))
        return ScenarioResult::fail ("could not write the click");
    for (int t = 0; t < 3; ++t) placeRegion (ctx, t, source, kAt, 4800);
    session.track (1).strip.busAssign[0].store (true);
    // Track 3 reaches the master through aux 1 only: a pre-fader send with
    // its fader down.
    auto& sender = session.track (2).strip;
    sender.faderDb.store (ChannelStripParams::kFaderMinDb);
    sender.auxSendPreFader[0].store (true);
    sender.auxSendDb[0].store (0.0f);
    auto& bus = session.bus (0).strip;
    transparentBusComp (bus.comp);
    bus.compEnabled.store (true);
    engine.prepareForSelfTest (kRate, kFrames);

    const int latency = engine.getBusStrip (0).getCompressor().latencySamples();
    ctx.note ("bus compressor latency " + str (latency) + ", reported " + str (engine.getBusCompLatencySamples()));
    if (! ctx.expect (latency > 0 && engine.getBusCompLatencySamples() == latency,
                      "a session with a bus compressor engaged does not hold its latency"))
        return ctx.verdict();

    const auto through = kAt + 8 * kFrames;
    const auto direct = landsAt (ctx, 0, 3, through);
    const auto viaBus = landsAt (ctx, 1, 3, through);
    const auto viaAux = landsAt (ctx, 2, 3, through);
    ctx.note ("direct at " + str (direct) + ", via the bus at " + str (viaBus) + ", via aux 1 at "
              + str (viaAux) + ", reported mix latency " + str (engine.getMixLatencySamples())
              + ", bus " + str (engine.getBusOutputLatencySamples()) + ", aux "
              + str (engine.getAuxReturnLatencySamples()));
    ctx.expect (direct >= 0 && viaBus == direct,
                "the bus-routed track played " + str (viaBus - direct) + " samples after the direct one");
    ctx.expect (viaAux == direct,
                "the aux return played " + str (viaAux - direct) + " samples after the direct track");
    ctx.expect (direct - kAt == engine.getMixLatencySamples(), "the reported latency does not match the click");
    ctx.expect (engine.getBusOutputLatencySamples() == engine.getTrackOutputLatencySamples() + latency,
                "the reported bus output latency does not carry the bus compressor's");
    ctx.expect (engine.getAuxReturnLatencySamples() == engine.getTrackOutputLatencySamples() + latency,
                "the reported aux return latency does not carry the bus compressor's");
    return ctx.verdict();
}

// ------------------------------------------------------------- switching

// Engaged when playback starts, a strip's and a bus's compressor switched out
// and back in while playing leave every click where the first one landed, on
// a track routed straight to the master and on one through the bus.
ScenarioResult switchingWhilePlayingKeepsTiming (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    keepCompressors (ctx);
    static constexpr int kEvery = 9600;
    static constexpr int kClicks = 8;
    std::vector<int> clicks;
    for (int c = 0; c < kClicks; ++c) clicks.push_back (c * kEvery);
    const auto source = ctx.tempDir() / "clicks.wav";
    if (! writeClicks (source, kClicks * kEvery, clicks))
        return ScenarioResult::fail ("could not write the clicks");
    placeRegion (ctx, 0, source, kAt, kClicks * kEvery);
    placeRegion (ctx, 1, source, kAt, kClicks * kEvery);
    session.track (1).strip.busAssign[0].store (true);
    auto& strip = session.track (0).strip;
    auto& bus = session.bus (0).strip;
    transparentStripComp (strip);
    transparentBusComp (bus.comp);
    strip.compEnabled.store (true);
    bus.compEnabled.store (true);
    engine.prepareForSelfTest (kRate, kFrames);

    // Off just after the second click, on after the fourth, off after the
    // sixth: each switch's fades are done long before the next click.
    const auto engagedAt = [] (std::int64_t timeline)
    {
        const auto since = timeline - kAt;
        return ! ((since > 1 * kEvery + 2400 && since < 3 * kEvery + 2400)
                  || since > 5 * kEvery + 2400);
    };
    for (int audible = 0; audible < 2; ++audible)
    {
        for (int t = 0; t < 2; ++t)
            session.track (t).strip.mute.store (t != audible);
        session.recomputeRtCounters();
        strip.compEnabled.store (true);
        bus.compEnabled.store (true);
        engine.getTransport().setPlayhead (0);
        engine.getPlaybackEngine().preparePlayback();
        engine.play();
        std::vector<float> out;
        const auto total = kAt + kClicks * kEvery + 4 * kFrames;
        for (std::int64_t done = 0; done < total; done += kFrames)
        {
            const bool engaged = engagedAt (done);
            strip.compEnabled.store (engaged);
            bus.compEnabled.store (engaged);
            ctx.pump (1);
            const auto& block = ctx.lastBlock (0);
            out.insert (out.end(), block.begin(), block.end());
        }
        engine.stop();

        std::int64_t first = -1;
        bool steady = true;
        std::string where;
        for (int c = 0; c < kClicks; ++c)
        {
            const auto from = kAt + (std::int64_t) c * kEvery;
            const auto at = argmaxAbs (out, from, from + kEvery / 2) - from;
            where += " " + str (at);
            if (first < 0) first = at;
            steady = steady && at == first;
        }
        ctx.note (std::string (audible == 0 ? "direct" : "through the bus") + " clicks land at" + where);
        ctx.expect (steady, std::string (audible == 0 ? "the direct track" : "the bus-routed track")
                                + " moved when its compressor was switched while playing");
        ctx.expect (first == engine.getMixLatencySamples(),
                    "the clicks do not land where the reported latency says");
    }
    return ctx.verdict();
}

// -------------------------------------------------------------- lifetime

// A session that engages no compressor never holds a compressor latency, and
// one that switches its last compressor out while playing holds it until the
// transport stops.
ScenarioResult stageLatencyOnlyWhileNeeded (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    keepCompressors (ctx);
    engine.prepareForSelfTest (kRate, kFrames);
    const auto reported = [&engine]
    {
        return engine.getStripCompLatencySamples() + engine.getBusCompLatencySamples()
             + engine.getMasterCompLatencySamples();
    };
    const auto base = engine.getMixLatencySamples();

    engine.getTransport().setPlayhead (0);
    engine.play();
    int heldBlocks = 0;
    for (int b = 0; b < 200; ++b)
    {
        ctx.pump (1);
        if (reported() != 0) ++heldBlocks;
    }
    engine.stop();
    ctx.pump (4);
    ctx.expect (heldBlocks == 0 && reported() == 0,
                "a session with no compressor engaged held a compressor latency");

    auto& strip = session.track (0).strip;
    auto& master = session.master();
    master.tapeEnabled.store (false);
    strip.compEnabled.store (true);
    master.compEnabled.store (true);
    engine.play();
    ctx.pump (20);
    const int latency = stripCompLatency (engine);
    ctx.expect (engine.getStripCompLatencySamples() == latency
                    && engine.getMasterCompLatencySamples() == latency,
                "engaging a compressor while playing did not bring in its stage's latency");
    strip.compEnabled.store (false);
    master.compEnabled.store (false);
    ctx.pump (40);
    ctx.expect (engine.getStripCompLatencySamples() == latency
                    && engine.getMasterCompLatencySamples() == latency,
                "switching the last compressor out while playing moved the timing");
    engine.stop();
    ctx.pump (16);
    ctx.note ("mix latency with nothing engaged " + str (base) + ", after the stop "
              + str (engine.getMixLatencySamples()));
    ctx.expect (reported() == 0 && engine.getMixLatencySamples() == base,
                "the compressor latency stayed after the transport stopped with every compressor off");
    return ctx.verdict();
}

// ------------------------------------------------------------------ click

// The metronome is added after the master, placed back by every delay the mix
// carries, so with a strip, a bus and the master compressor engaged it still
// starts on the beat the audio does.
ScenarioResult clickFollowsTheStages (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    keepCompressors (ctx);
    ctx.keep (session.metronomeEnabled);
    ctx.keep (session.metronomeClickWhilePlaying);
    ctx.keep (session.tempoBpm);
    ctx.keep (session.metronomeVolDb);
    ctx.cleanup ([&engine] { engine.stop(); });
    session.metronomeEnabled.store (true);
    session.metronomeClickWhilePlaying.store (true);
    session.tempoBpm.store (120.0f);
    session.metronomeVolDb.store (0.0f);

    for (const bool engaged : { false, true })
    {
        session.track (0).strip.compEnabled.store (engaged);
        session.bus (0).strip.compEnabled.store (engaged);
        session.master().compEnabled.store (engaged);
        engine.stop();
        engine.prepareForSelfTest (kRate, kFrames);
        static constexpr int kLeadIn = 100;
        transport.setPlayhead (24000 - kLeadIn);
        engine.play();
        std::vector<float> out;
        for (int b = 0; b < 4; ++b)
        {
            ctx.pump (1);
            const auto& block = ctx.lastBlock (0);
            out.insert (out.end(), block.begin(), block.end());
        }
        engine.stop();
        // The sine click is zero at its first sample and audible at the next.
        const int expected = kLeadIn + 1 + engine.getMixLatencySamples();
        const auto first = std::find_if (out.begin(), out.end(),
                                         [] (float v) { return std::abs (v) > 1.0e-5f; });
        const int onset = first == out.end() ? -1 : (int) (first - out.begin());
        ctx.note (std::string (engaged ? "compressors in" : "compressors out") + ": click at "
                  + str (onset) + ", expected " + str (expected));
        ctx.expect (onset == expected, std::string (engaged ? "with" : "without")
                                           + " compressors the click starts at " + str (onset)
                                           + ", not " + str (expected));
    }
    return ctx.verdict();
}

// ---------------------------------------------------------------- renders

struct RenderRun
{
    std::unique_ptr<BounceEngine> bounce;
    std::atomic<bool> finished { false };
    bool ok = false;
};

// Starts a render with `begin` and calls `then` once it is done.
void renderThen (ScenarioContext& ctx, const std::function<bool (BounceEngine&)>& begin,
                 std::function<void (bool ok)> then)
{
    auto run = std::make_shared<RenderRun>();
    run->bounce = std::make_unique<BounceEngine> (ctx.engine(), ctx.session(), OfflineRenderQuality::MatchPlayback);
    ctx.cleanup ([run] { run->bounce.reset(); });
    run->bounce->onFinished = [raw = run.get()] (bool ok, std::string)
    {
        raw->ok = ok;
        raw->finished.store (true, std::memory_order_release);
    };
    if (! begin (*run->bounce))
    {
        ctx.complete (ScenarioResult::fail ("the render refused to start"));
        return;
    }
    ctx.waitUntil ([run] { return run->finished.load (std::memory_order_acquire) && ! run->bounce->isRendering(); },
                   kRenderTimeoutMs, [run, then] { then (run->ok); }, "the render never finished");
}

// A mix bounce and a track freeze rendered with compressors engaged land
// their clicks on the samples the same renders put them with every
// compressor off; the frozen track then plays where the live ones do.
std::optional<ScenarioResult> rendersAlignedWithCompressors (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    keepCompressors (ctx);
    ctx.cleanup ([&engine] { engine.unfreezeTrack (0); });
    const auto source = ctx.tempDir() / "click.wav";
    if (! writeClicks (source, 4800, { 0 }))
        return ScenarioResult::fail ("could not write the click");
    // Track 1 straight to the master, track 2 through bus 1 a little later.
    placeRegion (ctx, 0, source, kAt, 4800);
    placeRegion (ctx, 1, source, kAt + 6000, 4800);
    session.track (1).strip.busAssign[0].store (true);
    for (int t = 0; t < 2; ++t) transparentStripComp (session.track (t).strip);
    transparentBusComp (session.bus (0).strip.comp);
    transparentBusComp (session.master().comp);
    session.master().tapeEnabled.store (false);

    struct Results
    {
        std::int64_t mix[2][2] { { -1, -1 }, { -1, -1 } };   // [engaged][track]
        std::int64_t frozen[2] { -1, -1 };
    };
    auto results = std::make_shared<Results>();
    const auto setEngaged = [&session] (bool on)
    {
        session.track (0).strip.compEnabled.store (on);
        session.track (1).strip.compEnabled.store (on);
        session.bus (0).strip.compEnabled.store (on);
        session.master().compEnabled.store (on);
    };

    auto step = std::make_shared<std::function<void (int)>>();
    std::weak_ptr<std::function<void (int)>> weakStep = step;
    *step = [&ctx, &engine, &session, results, setEngaged, weakStep, source] (int pass)
    {
        auto self = weakStep.lock();
        if (pass == 4)
        {
            const auto& r = *results;
            ctx.note ("mix, off: " + str (r.mix[0][0]) + " / " + str (r.mix[0][1]) + "; engaged: "
                      + str (r.mix[1][0]) + " / " + str (r.mix[1][1]) + "; frozen click off "
                      + str (r.frozen[0]) + ", engaged " + str (r.frozen[1]));
            ctx.expect (r.mix[0][0] == kAt && r.mix[0][1] == kAt + 6000,
                        "the bounce with every compressor off moved the clicks");
            ctx.expect (r.mix[1][0] == r.mix[0][0] && r.mix[1][1] == r.mix[0][1],
                        "the bounce with compressors engaged is not aligned with the one without");
            ctx.expect (r.frozen[0] == kAt && r.frozen[1] == r.frozen[0],
                        "the freeze rendered with the compressor engaged is not aligned with the one without");

            // Played back frozen beside a live track that keeps the strips'
            // latency on, the frozen click lands where the live one does.
            session.track (1).strip.busAssign[0].store (false);
            session.track (1).regions.clear();
            placeRegion (ctx, 1, source, kAt, 4800);
            session.track (1).strip.compEnabled.store (true);
            engine.prepareForSelfTest (kRate, kFrames);
            const auto live = landsAt (ctx, 1, 2, kAt + 8 * kFrames);
            const auto frozen = landsAt (ctx, 0, 2, kAt + 8 * kFrames);
            ctx.note ("live compressed track at " + str (live) + ", frozen track at " + str (frozen));
            ctx.expect (live >= 0 && frozen == live,
                        "the frozen track played " + str (frozen - live) + " samples off the live one");
            ctx.complete (ctx.verdict());
            return;
        }
        const bool engaged = (pass % 2) == 1;
        setEngaged (engaged);
        engine.prepareForSelfTest (kRate, kFrames);
        if (pass < 2)
        {
            const auto out = ctx.sessionDir() / (std::string ("mix-") + (engaged ? "on" : "off") + ".wav");
            renderThen (ctx, [out] (BounceEngine& bounce)
                        {
                            return bounce.start (sessionFile (out), kRate, 1024, 0.2,
                                                 BounceEngine::Mode::MasterMix, BounceEngine::Format::Wav, 320, 24);
                        },
                        [&ctx, results, engaged, out, self, pass] (bool ok)
                        {
                            const auto mix = readLeft (out);
                            if (ctx.expect (ok && mix.has_value(), "the mix bounce failed"))
                            {
                                results->mix[engaged][0] = argmaxAbs (*mix, 0, kAt + 3000);
                                results->mix[engaged][1] = argmaxAbs (*mix, kAt + 3000, (std::int64_t) mix->size());
                            }
                            (*self) (pass + 1);
                        });
            return;
        }
        // The freeze renders track 1 on its own, its compressor engaged or not.
        SessionFile out;
        std::int64_t length = 0;
        engine.unfreezeTrack (0);
        if (! ctx.expect (engine.freezePrepare (0, out, length), "freeze refused"))
        {
            ctx.complete (ctx.verdict());
            return;
        }
        renderThen (ctx, [out, length] (BounceEngine& bounce)
                    { return bounce.startFreeze (0, out, length, kRate); },
                    [&ctx, &engine, results, engaged, out, length, self, pass] (bool ok)
                    {
                        const auto frozen = readLeft (pathOf (out));
                        if (ctx.expect (ok && frozen.has_value(), "the freeze render failed"))
                            results->frozen[engaged] = argmaxAbs (*frozen);
                        engine.commitFreeze (0, out, length);
                        engine.prepareForSelfTest (kRate, kFrames);
                        (*self) (pass + 1);
                    });
    };
    (*step) (0);
    return std::nullopt;
}

const ScenarioRegistrar stripRegistrar { Scenario {
    "comp.strip_stage_aligns_tracks", { "comp", "pdc", "strip" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return stripStageAlignsTracks (ctx); } } };
const ScenarioRegistrar busRegistrar { Scenario {
    "comp.bus_stage_aligns_direct_and_aux", { "comp", "pdc", "bus" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return busStageAlignsBypassPaths (ctx); } } };
const ScenarioRegistrar switchingRegistrar { Scenario {
    "comp.switching_while_playing_keeps_timing", { "comp", "pdc" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return switchingWhilePlayingKeepsTiming (ctx); } } };
const ScenarioRegistrar lifetimeRegistrar { Scenario {
    "comp.stage_latency_only_while_needed", { "comp", "pdc" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return stageLatencyOnlyWhileNeeded (ctx); } } };
const ScenarioRegistrar clickRegistrar { Scenario {
    "comp.click_follows_the_stages", { "comp", "pdc", "metronome" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return clickFollowsTheStages (ctx); } } };
const ScenarioRegistrar rendersRegistrar { Scenario {
    "comp.renders_aligned_with_compressors", { "comp", "pdc", "bounce", "freeze" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return rendersAlignedWithCompressors (ctx); }, 240000 } };
} // namespace
} // namespace duskstudio::scenario
