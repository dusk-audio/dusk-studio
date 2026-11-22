#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../audiofile/FileReader.h"
#include "../../../session/Session.h"
#include "../../../dsp/ChannelStrip.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// A take recorded with PRINT on is the channel strip's output, which trails
// the input by the strip's delay: the compressors' stage latency, the
// oversampled EQ, an insert's latency and the delay compensation the track
// carries. These record the same clicks on a dry and a printed track and find
// them on the same samples.
namespace duskstudio::scenario
{
namespace
{
constexpr double kRate = ScenarioContext::kSampleRate;
constexpr int kFrames = ScenarioContext::kBlockSize;
constexpr int kDry = 0;
constexpr int kPrinted = 1;
constexpr int kOther = 2;
constexpr float kClick = 0.5f;

std::string str (std::int64_t v) { return std::to_string (v); }

// Input fed to the engine a block at a time: silence with a click at each
// listed input sample, counted from the first block fed, on input 1 (left) and
// input 2 (right).
struct Feed
{
    std::vector<std::int64_t> left, right;
    std::int64_t fed = 0;
    std::array<float, kFrames> inL {}, inR {}, outL {}, outR {};

    void blocks (AudioEngine& engine, int count)
    {
        for (int block = 0; block < count; ++block)
        {
            inL.fill (0.0f);
            inR.fill (0.0f);
            for (const auto at : left)
                if (at >= fed && at < fed + kFrames) inL[(std::size_t) (at - fed)] = kClick;
            for (const auto at : right)
                if (at >= fed && at < fed + kFrames) inR[(std::size_t) (at - fed)] = kClick;
            const float* inputs[] = { inL.data(), inR.data() };
            float* outputs[] = { outL.data(), outR.data() };
            engine.audioDeviceIOCallback (inputs, 2, outputs, 2, kFrames, {});
            fed += kFrames;
        }
    }
};

struct Click
{
    std::int64_t at = 0;
    bool inverted = false;
    bool operator== (const Click& other) const noexcept
    {
        return at == other.at && inverted == other.inverted;
    }
};

std::string describe (const std::vector<Click>& clicks)
{
    std::string text;
    for (const auto& click : clicks)
        text += (text.empty() ? "" : ", ") + str (click.at) + (click.inverted ? " (-)" : " (+)");
    return text.empty() ? "none" : text;
}

// A take channel's clicks on the timeline: each sample that stands out of the
// silence and is the largest within 8 samples either side. Clicks are fed at
// least 20 samples apart.
std::vector<Click> clicksIn (const AudioTake& take, int channel)
{
    std::vector<Click> clicks;
    auto reader = dusk::audio::FileReader::open (take.filePath());
    if (reader == nullptr) return clicks;
    const auto frames = take.lengthInSamples;
    std::vector<float> left ((std::size_t) frames, 0.0f), right (left);
    float* dest[] = { left.data(), right.data() };
    if (reader->read (dest, std::min (2, reader->info().numChannels), take.sourceOffset, frames) != frames)
        return clicks;
    const auto& x = channel == 0 ? left : right;
    for (std::int64_t i = 0; i < frames; ++i)
    {
        const float level = std::abs (x[(std::size_t) i]);
        if (level < 0.2f) continue;
        bool peak = true;
        for (auto j = std::max<std::int64_t> (0, i - 8); j < std::min (frames, i + 9) && peak; ++j)
        {
            const float other = std::abs (x[(std::size_t) j]);
            peak = j < i ? other < level : (j == i || other <= level);
        }
        if (peak) clicks.push_back ({ take.timelineStart + i, x[(std::size_t) i] < 0.0f });
    }
    return clicks;
}

// The dry track records input 1 (and 2, stereo) as it arrives. The printed
// track records its strip, which flips the polarity, so a printed click reads
// negative, and runs a compressor set to leave a half-scale click alone.
// Everything changed here goes back when the case ends.
bool setUpTracks (ScenarioContext& ctx, int oversampling, bool stereo)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    ctx.cleanup ([&engine, &session, factorWas = session.oversamplingFactor.load()]
    {
        session.oversamplingFactor.store (factorWas);
        engine.prepareForSelfTest (kRate, kFrames);
    });
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
        auto& strip = track.strip;
        ctx.keep (track.printEffects);
        ctx.keep (track.inputSourceR);
        for (auto* value : { &strip.phaseInvert, &strip.compEnabled, &strip.eqEnabled })
            ctx.keep (*value);
        ctx.keep (strip.compMode);
        ctx.cleanup ([&strip, values = strip.comp.values()] { strip.comp.setValues (values); });
        track.mode.store ((int) (stereo ? Track::Mode::Stereo : Track::Mode::Mono));
        track.inputSource.store (0);
        track.inputSourceR.store (1);
        track.recordArmed.store (true);
        track.printEffects.store (t == kPrinted);
        strip.phaseInvert.store (t == kPrinted);
        strip.eqEnabled.store (false);
        strip.compMode.store (2);
        strip.comp.set (CompParamId::VcaThreshold, 0.0f);
        strip.compEnabled.store (t == kPrinted);
    }
    session.recomputeRtCounters();
    session.oversamplingFactor.store (oversampling);
    engine.prepareForSelfTest (kRate, kFrames);
    ctx.note ("strip compressor latency " + str (engine.getStripCompLatencySamples())
              + ", oversampled EQ " + str (engine.getChannelStrip (kPrinted).getOversamplingLatencySamples()));
    return ctx.expect (engine.getStripCompLatencySamples() > 0,
                       "an engaged strip compressor gave the strips no latency to print through");
}

// How far the printed track's strip output trails its input.
int stripDelay (AudioEngine& engine)
{
    auto& strip = engine.getChannelStrip (kPrinted);
    return engine.getStripCompLatencySamples() + strip.getOversamplingLatencySamples()
         + strip.getPdcCompensationSamples() + strip.getInsertPluginLatencySamples();
}

void clearTakes (ScenarioContext& ctx)
{
    for (const int t : { kDry, kPrinted })
    {
        ctx.session().track (t).regions.clear();
        ctx.session().track (t).takes.clear();
    }
    ctx.engine().getPlaybackEngine().preparePlayback();
}

const AudioTake* onlyTake (ScenarioContext& ctx, int track, const std::string& leg)
{
    const auto& takes = ctx.session().track (track).takes;
    if (! ctx.expect (takes.size() == 1, leg + ": track " + str (track + 1) + " holds "
                                         + str ((std::int64_t) takes.size()) + " takes, not one"))
        return nullptr;
    return &takes.front();
}

const AudioTake* passTake (ScenarioContext& ctx, int track, int pass)
{
    for (const auto& take : ctx.session().track (track).takes)
        if (take.provenance.loopPassOrdinal == pass) return &take;
    ctx.expect (false, "track " + str (track + 1) + " has no take for pass " + str (pass));
    return nullptr;
}

// The dry take holds a click at each of `at` and the printed take the same
// clicks, flipped.
void expectSameClicks (ScenarioContext& ctx, const AudioTake& dry, const AudioTake& printed,
                       const std::vector<std::int64_t>& at, int channel, const std::string& leg)
{
    const auto dryClicks = clicksIn (dry, channel);
    const auto printedClicks = clicksIn (printed, channel);
    std::vector<Click> wantDry, wantPrinted;
    for (const auto sample : at)
    {
        wantDry.push_back ({ sample, false });
        wantPrinted.push_back ({ sample, true });
    }
    ctx.note (leg + ": dry clicks " + describe (dryClicks) + ", printed " + describe (printedClicks));
    ctx.expect (dryClicks == wantDry, leg + ": the dry take holds clicks at " + describe (dryClicks)
                                          + ", not at " + describe (wantDry));
    ctx.expect (printedClicks == wantPrinted, leg + ": the printed take holds clicks at "
                                                  + describe (printedClicks) + ", the dry take at "
                                                  + describe (dryClicks));
}

void expectSameStart (ScenarioContext& ctx, const AudioTake& dry, const AudioTake& printed,
                      std::int64_t start, const std::string& leg)
{
    ctx.expect (dry.timelineStart == start && printed.timelineStart == start,
                leg + ": the dry take starts at " + str (dry.timelineStart) + " and the printed one at "
                    + str (printed.timelineStart) + ", not at " + str (start));
}

// A printed take stops short of the dry one by the strip's delay: that much of
// what was played was still in the strip when the transport stopped.
void expectEndsShortBy (ScenarioContext& ctx, const AudioTake& dry, const AudioTake& printed,
                        int delay, const std::string& leg)
{
    ctx.expect (printed.lengthInSamples == dry.lengthInSamples - delay,
                leg + ": the printed take is " + str (printed.lengthInSamples) + " samples long and the dry one "
                    + str (dry.lengthInSamples) + ", where the strip's delay is " + str (delay));
}

// Records from a stopped transport at `from`, the clicks `at` samples into the
// take, and checks both takes.
void recordFromStopped (ScenarioContext& ctx, std::int64_t from, const std::vector<std::int64_t>& at,
                        const std::string& leg)
{
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    const int delay = stripDelay (engine);
    Feed feed;
    feed.left = at;
    transport.setPlayhead (from);
    engine.record();
    if (! ctx.expect (transport.isRecording(), leg + ": Record did not start"))
        return;
    feed.blocks (engine, 48);
    engine.stop();
    const auto* dry = onlyTake (ctx, kDry, leg);
    const auto* printed = onlyTake (ctx, kPrinted, leg);
    if (dry == nullptr || printed == nullptr) return;
    expectSameStart (ctx, *dry, *printed, from, leg);
    std::vector<std::int64_t> expected;
    for (const auto sample : at) expected.push_back (from + sample);
    expectSameClicks (ctx, *dry, *printed, expected, 0, leg);
    expectEndsShortBy (ctx, *dry, *printed, delay, leg);
}

// ------------------------------------------------------------------ linear

// From a stopped and a rolling start, the printed take's clicks land on the
// dry take's samples. PRINT is read when Record is pressed: switching it
// during a take changes nothing until the next one.
ScenarioResult printLandsWithDry (ScenarioContext& ctx, int oversampling)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    if (! setUpTracks (ctx, oversampling, false))
        return ctx.verdict();
    const int delay = stripDelay (engine);
    ctx.note ("the printed track's strip delays its input by " + str (delay) + " samples");

    recordFromStopped (ctx, 48000, { 1000, 9000 }, "from stopped");
    clearTakes (ctx);

    // A click 10 samples before Record is pressed is still inside the strip
    // when the take starts; it must stay out of both takes.
    {
        const std::string leg = "from rolling";
        static constexpr std::int64_t kFrom = 96000;
        static constexpr int kPreBlocks = 20;
        static constexpr std::int64_t kPress = kPreBlocks * kFrames;
        session.track (kPrinted).inputMonitor.store (true);
        Feed feed;
        feed.left = { kPress - 10, kPress + 10, kPress + 6000 };
        transport.setPlayhead (kFrom);
        engine.play();
        feed.blocks (engine, kPreBlocks);
        const auto start = transport.getPlayhead();
        engine.record();
        if (ctx.expect (transport.isRecording(), leg + ": Record did not start while playing"))
        {
            feed.blocks (engine, 40);
            engine.stop();
            const auto* dry = onlyTake (ctx, kDry, leg);
            const auto* printed = onlyTake (ctx, kPrinted, leg);
            if (dry != nullptr && printed != nullptr)
            {
                ctx.expect (start == kFrom + kPress, leg + ": the transport was at " + str (start)
                                                         + " when Record was pressed");
                expectSameStart (ctx, *dry, *printed, start, leg);
                expectSameClicks (ctx, *dry, *printed, { start + 10, start + 6000 }, 0, leg);
                expectEndsShortBy (ctx, *dry, *printed, delay, leg);
            }
        }
        session.track (kPrinted).inputMonitor.store (false);
        clearTakes (ctx);
    }

    {
        const std::string leg = "PRINT switched during the take";
        static constexpr std::int64_t kFrom = 144000;
        Feed feed;
        feed.left = { 1000, 8000 };
        transport.setPlayhead (kFrom);
        engine.record();
        if (ctx.expect (transport.isRecording(), leg + ": Record did not start"))
        {
            feed.blocks (engine, 16);
            session.track (kPrinted).printEffects.store (false);
            session.track (kDry).printEffects.store (true);
            feed.blocks (engine, 24);
            engine.stop();
            session.track (kPrinted).printEffects.store (true);
            session.track (kDry).printEffects.store (false);
            const auto* dry = onlyTake (ctx, kDry, leg);
            const auto* printed = onlyTake (ctx, kPrinted, leg);
            if (dry != nullptr && printed != nullptr)
                expectSameClicks (ctx, *dry, *printed, { kFrom + 1000, kFrom + 8000 }, 0, leg);
        }
    }
    return ctx.verdict();
}

// The printed track's compressor off, the only one setUpTracks engages, so the
// strips hold no latency until one is engaged again.
bool compressorsOff (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    ctx.session().track (kPrinted).strip.compEnabled.store (false);
    engine.prepareForSelfTest (kRate, kFrames);
    return ctx.expect (engine.getStripCompLatencySamples() == 0,
                       "with every compressor off the strips kept their latency");
}

// A take keeps the delay its strip had when it started. The first compressor
// engaged while recording brings the strips' latency in, and the printed
// take, which never jumps, carries the rest of the way that much later.
ScenarioResult printKeepsItsStartingDelay (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    if (! setUpTracks (ctx, 1, false))
        return ctx.verdict();
    const int stageLatency = engine.getStripCompLatencySamples();
    if (! compressorsOff (ctx))
        return ctx.verdict();

    static constexpr std::int64_t kFrom = 48000;
    Feed feed;
    feed.left = { 1000, 9000 };
    transport.setPlayhead (kFrom);
    engine.record();
    if (! ctx.expect (transport.isRecording(), "Record did not start"))
        return ctx.verdict();
    feed.blocks (engine, 16);
    session.track (kPrinted).strip.compEnabled.store (true);
    feed.blocks (engine, 32);
    engine.stop();
    ctx.expect (engine.getStripCompLatencySamples() == stageLatency,
                "the compressor engaged while recording did not bring the strips' latency in");
    const auto* dry = onlyTake (ctx, kDry, "compressor engaged during the take");
    const auto* printed = onlyTake (ctx, kPrinted, "compressor engaged during the take");
    if (dry == nullptr || printed == nullptr)
        return ctx.verdict();
    const auto dryClicks = clicksIn (*dry, 0);
    const auto printedClicks = clicksIn (*printed, 0);
    ctx.note ("dry clicks " + describe (dryClicks) + ", printed " + describe (printedClicks));
    ctx.expect (dryClicks == std::vector<Click> { { kFrom + 1000, false }, { kFrom + 9000, false } },
                "the dry take holds clicks at " + describe (dryClicks));
    ctx.expect (printedClicks == std::vector<Click> { { kFrom + 1000, true }, { kFrom + 9000 + stageLatency, true } },
                "the printed take holds clicks at " + describe (printedClicks) + "; the click before the "
                    "compressor came in belongs on the dry one's sample, the one after it "
                    + str (stageLatency) + " samples later");
    ctx.expect (printed->lengthInSamples == dry->lengthInSamples,
                "a take that started with no delay ends short of the dry one");
    return ctx.verdict();
}

// One bar of count-in: three beats at 300 bpm, which ends half way through a
// block.
constexpr std::int64_t kCountIn = 28800;

void countInOn (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    ctx.keep (session.tempoBpm);
    ctx.keep (session.beatsPerBar);
    session.tempoBpm.store (300.0f);
    session.beatsPerBar.store (3);
    session.countInEnabled.store (true);
}

// A count-in records nothing, so a take starts where its first sample is
// recorded, not where Record was pressed. The first compressor, engaged during
// the count-in, brings the strips' latency in before then: the printed take
// carries it from its first sample and lands with the dry one, and what was
// played in the count-in stays out of both.
ScenarioResult printCountInLandsWithDry (ScenarioContext& ctx, bool stereo)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    if (! setUpTracks (ctx, 1, stereo))
        return ctx.verdict();
    const int stageLatency = engine.getStripCompLatencySamples();
    if (! compressorsOff (ctx))
        return ctx.verdict();
    countInOn (ctx);

    const std::string leg = stereo ? "stereo count-in" : "count-in";
    static constexpr std::int64_t kFrom = 96000;
    Feed feed;
    feed.left = { kCountIn - 10, kCountIn + 10, kCountIn + 6000 };
    if (stereo)
        feed.right = { kCountIn - 30, kCountIn + 3000, kCountIn + 6040 };
    transport.setPlayhead (kFrom);
    engine.record();
    if (! ctx.expect (transport.isRecording() && transport.getPlayhead() == kFrom - kCountIn,
                      leg + ": Record did not roll back a bar to " + str (kFrom - kCountIn)
                          + "; the playhead is at " + str (transport.getPlayhead())))
        return ctx.verdict();
    feed.blocks (engine, 40);
    session.track (kPrinted).strip.compEnabled.store (true);
    feed.blocks (engine, 120);
    engine.stop();
    ctx.expect (engine.getStripCompLatencySamples() == stageLatency,
                leg + ": the compressor engaged during the count-in did not bring the strips' latency in");
    const auto* dry = onlyTake (ctx, kDry, leg);
    const auto* printed = onlyTake (ctx, kPrinted, leg);
    if (dry == nullptr || printed == nullptr)
        return ctx.verdict();
    expectSameStart (ctx, *dry, *printed, kFrom, leg);
    expectSameClicks (ctx, *dry, *printed, { kFrom + 10, kFrom + 6000 }, 0, leg + " left");
    if (stereo)
        expectSameClicks (ctx, *dry, *printed, { kFrom + 3000, kFrom + 6040 }, 1, leg + " right");
    expectEndsShortBy (ctx, *dry, *printed, stripDelay (engine), leg);
    return ctx.verdict();
}

// Puts Tape Machine 2 on another track, whose latency gives the printed track
// delay compensation. False when it gives none.
bool latentInsertOnOtherTrack (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& other = engine.getChannelStrip (kOther);
    ctx.keep (other.insertMode);
    std::string error;
    if (! ctx.expect (other.loadBuiltin ("dusk.builtin.tape", error), "could not load Tape Machine 2: " + error))
        return false;
    ctx.cleanup ([&other] { other.unloadBuiltin(); });
    other.insertMode.store (ChannelStrip::kInsertPlugin);
    ctx.pump (4);
    const int insertLatency = other.getInsertPluginLatencySamples();
    ctx.note ("Tape Machine 2 on track " + str (kOther + 1) + " reports " + str (insertLatency)
              + " samples; the printed track's compensation is "
              + str (engine.getChannelStrip (kPrinted).getPdcCompensationSamples()));
    if (! ctx.expect (insertLatency > 0, "Tape Machine 2 reports no latency, so there is no compensation to print through"))
        return false;
    return ctx.expect (engine.getChannelStrip (kPrinted).getPdcCompensationSamples() == insertLatency,
                       "the printed track is not compensated for the other track's insert");
}

// A plug-in with a latency on another track gives the printed track delay
// compensation, which is on its print path too. The plug-in stays off the
// printed track: Tape Machine 2's own click peaks past its reported latency,
// which is its sound, so the insert's own latency is held by the strip's tests
// with a plug-in that delays by exactly what it reports.
ScenarioResult printFollowsDelayCompensation (ScenarioContext& ctx)
{
    if (! setUpTracks (ctx, 1, false) || ! latentInsertOnOtherTrack (ctx))
        return ctx.verdict();
    recordFromStopped (ctx, 48000, { 1000, 9000 }, "through delay compensation");
    return ctx.verdict();
}

// ------------------------------------------------------------------- punch

constexpr std::int64_t kPunchIn = 60000, kPunchOut = 70000;

void punchOn (ScenarioContext& ctx)
{
    auto& transport = ctx.engine().getTransport();
    ctx.cleanup ([&transport, in = transport.getPunchIn(), out = transport.getPunchOut(),
                  on = transport.isPunchEnabled()]
    {
        transport.setPunchRange (in, out);
        transport.setPunchEnabled (on);
    });
    transport.setPunchRange (kPunchIn, kPunchOut);
    transport.setPunchEnabled (true);
}

// Clicks 5 samples either side of the punch-in and the punch-out and one well
// inside, as input samples of a feed whose first block plays at `from`.
std::vector<std::int64_t> punchClicks (std::int64_t from)
{
    std::vector<std::int64_t> at;
    for (const auto sample : { kPunchIn - 5, kPunchIn + 5, kPunchIn + 5000, kPunchOut - 5, kPunchOut + 5 })
        at.push_back (sample - from);
    return at;
}

// The blocks a feed from `from` takes to roll well past the punch-out.
int blocksPastPunchOut (std::int64_t from)
{
    return (int) ((kPunchOut + 4096 - from) / kFrames);
}

// Both takes hold the window and the clicks played inside it.
void expectPunchTakes (ScenarioContext& ctx, const std::string& leg)
{
    const auto* dry = onlyTake (ctx, kDry, leg);
    const auto* printed = onlyTake (ctx, kPrinted, leg);
    if (dry == nullptr || printed == nullptr)
        return;
    expectSameStart (ctx, *dry, *printed, kPunchIn, leg);
    ctx.expect (dry->lengthInSamples == kPunchOut - kPunchIn && printed->lengthInSamples == kPunchOut - kPunchIn,
                leg + ": the dry take is " + str (dry->lengthInSamples) + " samples long and the printed one "
                    + str (printed->lengthInSamples) + ", the window " + str (kPunchOut - kPunchIn));
    expectSameClicks (ctx, *dry, *printed, { kPunchIn + 5, kPunchIn + 5000, kPunchOut - 5 }, 0, leg);
}

// The punch window holds the clicks played inside it, on both tracks, and
// neither the one just before the punch-in nor the one just after the
// punch-out.
ScenarioResult printPunchHoldsTheWindow (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    if (! setUpTracks (ctx, 1, false))
        return ctx.verdict();
    punchOn (ctx);
    static constexpr std::int64_t kFrom = 50000;
    Feed feed;
    feed.left = punchClicks (kFrom);
    transport.setPlayhead (kFrom);
    engine.record();
    if (! ctx.expect (transport.isRecording() && transport.getPlayhead() == kFrom,
                      "Record did not start rolling from before the punch-in"))
        return ctx.verdict();
    feed.blocks (engine, blocksPastPunchOut (kFrom));
    engine.stop();
    expectPunchTakes (ctx, "punch");
    return ctx.verdict();
}

// Record pressed with a pre-roll before the punch-in, and in the pre-roll a
// latent insert on another track is bypassed: the printed track, silent there,
// drops its delay compensation at once. The punch take is recorded
// with the delay its strip has at the punch-in, so it lands with the dry one.
ScenarioResult printPreRollPunchLandsWithDry (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    if (! setUpTracks (ctx, 1, false) || ! latentInsertOnOtherTrack (ctx))
        return ctx.verdict();
    punchOn (ctx);
    ctx.keep (session.preRollSeconds);
    session.preRollEnabled.store (true);
    session.preRollSeconds.store (0.25f);
    static constexpr std::int64_t kFrom = kPunchIn - 12000;
    static constexpr int kBypassAfter = 8;
    static constexpr int kBeforePunchIn = (int) ((kPunchIn - kFrom) / kFrames);
    Feed feed;
    feed.left = punchClicks (kFrom);
    transport.setPlayhead (kPunchIn);
    engine.record();
    if (! ctx.expect (transport.isRecording() && transport.getPlayhead() == kFrom,
                      "Record did not roll back to the pre-roll at " + str (kFrom) + "; the playhead is at "
                          + str (transport.getPlayhead())))
        return ctx.verdict();
    auto& strip = engine.getChannelStrip (kPrinted);
    feed.blocks (engine, kBypassAfter);
    const int delayBeforeBypass = strip.getProcessedLatencySamples();
    engine.getChannelStrip (kOther).insertMode.store (ChannelStrip::kInsertEmpty);
    feed.blocks (engine, kBeforePunchIn - kBypassAfter);
    const int delayAtPunchIn = strip.getProcessedLatencySamples();
    ctx.note ("the printed strip delayed its input by " + str (delayBeforeBypass) + " samples before the bypass and by "
              + str (delayAtPunchIn) + " just before the punch-in");
    ctx.expect (delayAtPunchIn == stripDelay (engine) && delayAtPunchIn < delayBeforeBypass,
                "the printed track did not drop its delay compensation in the pre-roll");
    feed.blocks (engine, blocksPastPunchOut (kFrom) - kBeforePunchIn);
    engine.stop();
    expectPunchTakes (ctx, "pre-roll punch");
    return ctx.verdict();
}

// -------------------------------------------------------------------- loop

constexpr std::int64_t kLoopLength = 4096;

// Each pass of a loop take from `start` holds the clicks listed for it on both
// tracks. Every pass but the last is the loop's length; the last is `lastPass`
// long on the dry track and ends the strip's delay short of that on the
// printed one.
void expectPasses (ScenarioContext& ctx, std::int64_t start, std::int64_t lastPass,
                   const std::vector<std::vector<std::int64_t>>& expected)
{
    const int delay = stripDelay (ctx.engine());
    const int passes = (int) expected.size();
    for (int pass = 1; pass <= passes; ++pass)
    {
        const auto leg = "pass " + str (pass);
        const auto* dry = passTake (ctx, kDry, pass);
        const auto* printed = passTake (ctx, kPrinted, pass);
        if (dry == nullptr || printed == nullptr) continue;
        expectSameStart (ctx, *dry, *printed, start, leg);
        if (pass < passes)
        {
            ctx.expect (dry->lengthInSamples == kLoopLength && printed->lengthInSamples == kLoopLength,
                        leg + ": the dry pass is " + str (dry->lengthInSamples) + " samples long and the printed one "
                            + str (printed->lengthInSamples) + ", the loop " + str (kLoopLength));
            ctx.expect (! dry->provenance.partialPass && ! printed->provenance.partialPass,
                        leg + ": a whole pass is marked partial");
        }
        else
        {
            ctx.expect (dry->lengthInSamples == lastPass, leg + ": the dry pass is "
                                                              + str (dry->lengthInSamples) + " samples long");
            expectEndsShortBy (ctx, *dry, *printed, delay, leg);
        }
        expectSameClicks (ctx, *dry, *printed, expected[(std::size_t) pass - 1], 0, leg);
    }
}

// Every pass of a loop take keeps the clicks played in it on both tracks,
// including those just before a seam, which reach the printed track only after
// the transport has wrapped, and one on a pass's last sample.
ScenarioResult printLoopPassesAlign (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    if (! setUpTracks (ctx, 1, false))
        return ctx.verdict();
    static constexpr std::int64_t kStart = 48000, kLength = kLoopLength, kEnd = kStart + kLength;
    static constexpr std::int64_t kLastPass = 1024;
    transport.setLoopRange (kStart, kEnd);
    transport.setLoopEnabled (true);
    // Input sample f plays at kStart + f % kLength, in pass f / kLength + 1.
    Feed feed;
    feed.left = { kLength - 10, kLength + 3, 2 * kLength - 40, 2 * kLength + 100, 3 * kLength - 1,
                  3 * kLength + 500 };
    transport.setPlayhead (kStart);
    engine.record();
    if (! ctx.expect (transport.isRecording(), "Record did not start in the loop"))
        return ctx.verdict();
    feed.blocks (engine, (int) ((3 * kLength + kLastPass) / kFrames));
    engine.stop();
    expectPasses (ctx, kStart, kLastPass,
                  { { kEnd - 10 }, { kStart + 3, kEnd - 40 }, { kStart + 100, kEnd - 1 }, { kStart + 500 } });
    return ctx.verdict();
}

// A loop take's count-in records nothing either: the compressor engaged in it
// is in the delay the printed passes are recorded with, from the first.
ScenarioResult printCountInLoopLandsWithDry (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    if (! setUpTracks (ctx, 1, false))
        return ctx.verdict();
    const int stageLatency = engine.getStripCompLatencySamples();
    if (! compressorsOff (ctx))
        return ctx.verdict();
    countInOn (ctx);
    static constexpr std::int64_t kStart = 96000, kLength = kLoopLength, kEnd = kStart + kLength;
    static constexpr std::int64_t kLastPass = 1152;
    static constexpr int kEngageAfter = 40;
    transport.setLoopRange (kStart, kEnd);
    transport.setLoopEnabled (true);
    // After the count-in, input sample kCountIn + f plays at kStart + f % kLength,
    // in pass f / kLength + 1.
    Feed feed;
    feed.left = { kCountIn - 10, kCountIn + 3, kCountIn + kLength - 10, kCountIn + kLength + 100,
                  kCountIn + 2 * kLength + 500 };
    transport.setPlayhead (kStart);
    engine.record();
    if (! ctx.expect (transport.isRecording() && transport.getPlayhead() == kStart - kCountIn,
                      "Record did not roll back a bar before the loop to " + str (kStart - kCountIn)
                          + "; the playhead is at " + str (transport.getPlayhead())))
        return ctx.verdict();
    feed.blocks (engine, kEngageAfter);
    session.track (kPrinted).strip.compEnabled.store (true);
    feed.blocks (engine, (int) ((kCountIn + 2 * kLength + kLastPass) / kFrames) - kEngageAfter);
    engine.stop();
    ctx.expect (engine.getStripCompLatencySamples() == stageLatency,
                "the compressor engaged during the count-in did not bring the strips' latency in");
    expectPasses (ctx, kStart, kLastPass, { { kStart + 3, kEnd - 10 }, { kStart + 100 }, { kStart + 500 } });
    return ctx.verdict();
}

// ------------------------------------------------------------------ stereo

ScenarioResult printStereoLandsWithDry (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    if (! setUpTracks (ctx, 1, true))
        return ctx.verdict();
    static constexpr std::int64_t kFrom = 48000;
    Feed feed;
    feed.left = { 1000, 5000 };
    feed.right = { 3000, 5040 };
    transport.setPlayhead (kFrom);
    engine.record();
    if (! ctx.expect (transport.isRecording(), "Record did not start"))
        return ctx.verdict();
    feed.blocks (engine, 32);
    engine.stop();
    const auto* dry = onlyTake (ctx, kDry, "stereo");
    const auto* printed = onlyTake (ctx, kPrinted, "stereo");
    if (dry == nullptr || printed == nullptr)
        return ctx.verdict();
    ctx.expect (dry->numChannels == 2 && printed->numChannels == 2, "a stereo track recorded a mono take");
    expectSameStart (ctx, *dry, *printed, kFrom, "stereo");
    expectSameClicks (ctx, *dry, *printed, { kFrom + 1000, kFrom + 5000 }, 0, "stereo left");
    expectSameClicks (ctx, *dry, *printed, { kFrom + 3000, kFrom + 5040 }, 1, "stereo right");
    expectEndsShortBy (ctx, *dry, *printed, stripDelay (engine), "stereo");
    return ctx.verdict();
}

const ScenarioRegistrar linearRegistrar { Scenario {
    "record.print_lands_with_dry", { "record", "print", "comp" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return printLandsWithDry (ctx, 1); } } };
const ScenarioRegistrar oversampledRegistrar { Scenario {
    "record.print_lands_with_dry_at_2x", { "record", "print", "comp", "oversampling" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return printLandsWithDry (ctx, 2); } } };
const ScenarioRegistrar startingDelayRegistrar { Scenario {
    "record.print_keeps_its_starting_delay", { "record", "print", "comp" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return printKeepsItsStartingDelay (ctx); } } };
const ScenarioRegistrar pdcRegistrar { Scenario {
    "record.print_follows_delay_compensation", { "record", "print", "pdc", "builtin" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return printFollowsDelayCompensation (ctx); } } };
const ScenarioRegistrar punchRegistrar { Scenario {
    "record.print_punch_holds_the_window", { "record", "print", "punch" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return printPunchHoldsTheWindow (ctx); } } };
const ScenarioRegistrar loopRegistrar { Scenario {
    "record.print_loop_passes_align", { "record", "print", "loop" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return printLoopPassesAlign (ctx); } } };
const ScenarioRegistrar stereoRegistrar { Scenario {
    "record.print_stereo_lands_with_dry", { "record", "print", "stereo" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return printStereoLandsWithDry (ctx); } } };
const ScenarioRegistrar countInRegistrar { Scenario {
    "record.print_count_in_lands_with_dry", { "record", "print", "comp", "count-in" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return printCountInLandsWithDry (ctx, false); } } };
const ScenarioRegistrar countInStereoRegistrar { Scenario {
    "record.print_count_in_stereo_lands_with_dry", { "record", "print", "comp", "count-in", "stereo" },
    Needs::Engine, {},
    [] (ScenarioContext& ctx) { return printCountInLandsWithDry (ctx, true); } } };
const ScenarioRegistrar preRollPunchRegistrar { Scenario {
    "record.print_pre_roll_punch_lands_with_dry", { "record", "print", "punch", "pre-roll", "pdc", "builtin" },
    Needs::Engine, {},
    [] (ScenarioContext& ctx) { return printPreRollPunchLandsWithDry (ctx); } } };
const ScenarioRegistrar countInLoopRegistrar { Scenario {
    "record.print_count_in_loop_lands_with_dry", { "record", "print", "loop", "comp", "count-in" },
    Needs::Engine, {},
    [] (ScenarioContext& ctx) { return printCountInLoopLandsWithDry (ctx); } } };
} // namespace
} // namespace duskstudio::scenario
