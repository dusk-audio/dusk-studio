#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../../session/Session.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <string>

namespace duskstudio::scenario
{
namespace
{
constexpr int kTrack = 0;
constexpr int kFrames = ScenarioContext::kBlockSize;
constexpr int kWarmBlocks = 60;
constexpr int kMeasureBlocks = 40;
constexpr double kTwoPi = 6.283185307179586;

struct Levels
{
    double left = 0.0, right = 0.0, aux = 0.0;
    std::array<float, kFrames> lastLeft {}, lastRight {};
};

double db (double level, double reference)
{
    return 20.0 * std::log10 (std::max (level, 1.0e-9) / std::max (reference, 1.0e-9));
}

// A steady tone into inputs 1 and 2 at the given amplitudes, warmed up and then
// measured: RMS at the master outputs and in aux lane 1's return.
Levels playTone (ScenarioContext& ctx, double hz, float ampL, float ampR)
{
    auto& engine = ctx.engine();
    std::array<float, kFrames> inL {}, inR {}, outL {}, outR {}, auxL {}, auxR {};
    const float* inputs[] = { inL.data(), inR.data() };
    float* outputs[] = { outL.data(), outR.data() };
    engine.setAuxStemCapture (0, auxL.data(), auxR.data());

    Levels levels;
    double phase = 0.0, sumL = 0.0, sumR = 0.0, sumAux = 0.0;
    const double step = kTwoPi * hz / ScenarioContext::kSampleRate;
    for (int block = 0; block < kWarmBlocks + kMeasureBlocks; ++block)
    {
        for (int i = 0; i < kFrames; ++i)
        {
            const auto s = (float) std::sin (phase);
            inL[(std::size_t) i] = ampL * s;
            inR[(std::size_t) i] = ampR * s;
            phase = std::fmod (phase + step, kTwoPi);
        }
        auxL.fill (0.0f);
        auxR.fill (0.0f);
        engine.audioDeviceIOCallback (inputs, 2, outputs, 2, kFrames, {});
        if (block < kWarmBlocks) continue;
        for (int i = 0; i < kFrames; ++i)
        {
            const auto k = (std::size_t) i;
            sumL += (double) outL[k] * outL[k];
            sumR += (double) outR[k] * outR[k];
            sumAux += (double) auxL[k] * auxL[k];
        }
    }
    engine.setAuxStemCapture (0, nullptr, nullptr);

    const double n = (double) kMeasureBlocks * kFrames;
    levels.left = std::sqrt (sumL / n);
    levels.right = std::sqrt (sumR / n);
    levels.aux = std::sqrt (sumAux / n);
    levels.lastLeft = outL;
    levels.lastRight = outR;
    return levels;
}

// Track 1 hears its live input (mono from input 1, or stereo from inputs 1 and
// 2) with its DSP flat, and the master adds nothing of its own. Everything the
// cases here change on the track, its strip, bus 1 or the master is put back as
// it was when the case ends.
void liveInput (ScenarioContext& ctx, Track::Mode mode)
{
    auto& session = ctx.session();
    auto& track = session.track (kTrack);
    auto& strip = track.strip;
    auto& bus = session.bus (0).strip;
    auto& master = session.master();

    for (auto* value : { &track.mode, &track.inputSource, &track.inputSourceR, &strip.compMode,
                         &strip.compFetRatio })
        ctx.keep (*value);
    for (auto* value : { &track.inputMonitor, &strip.busAssign[0], &strip.auxSendsBypassed,
                         &strip.auxSendPreFader[0], &strip.eqEnabled, &strip.hpfEnabled,
                         &strip.lpfEnabled, &strip.phaseInvert, &strip.compEnabled, &bus.eqEnabled,
                         &master.monoSum, &master.eqEnabled, &master.compEnabled, &master.tapeEnabled })
        ctx.keep (*value);
    for (auto* value : { &strip.faderDb, &strip.pan, &strip.auxSendDb[0], &strip.hpfFreq, &strip.lpfFreq,
                         &strip.compFetThresholdDb, &strip.compVcaThreshDb, &strip.compVcaRatio,
                         &bus.eqLfGainDb, &bus.eqMidGainDb, &bus.eqHfGainDb })
        ctx.keep (*value);

    track.mode.store ((int) mode);
    track.inputSource.store (-2);
    track.inputSourceR.store (-2);
    track.inputMonitor.store (true);
    master.eqEnabled.store (false);
    master.compEnabled.store (false);
    master.tapeEnabled.store (false);
    session.recomputeRtCounters();
    ctx.engine().prepareForSelfTest (ScenarioContext::kSampleRate, kFrames);
}

// ---------------------------------------------------------------- filters

// The HPF runs 20 to 300 Hz and the LPF 3 to 20 kHz; at 20 Hz and 20 kHz each
// is effectively off, and at the other end it cuts well into the band. Both
// sit in the EQ section, whose status light bypasses them with the bands.
ScenarioResult filterRanges (ScenarioContext& ctx)
{
    liveInput (ctx, Track::Mode::Mono);
    auto& strip = ctx.session().track (kTrack).strip;
    strip.eqEnabled.store (true);

    const double lowRef = playTone (ctx, 60.0, 0.25f, 0.0f).left;
    strip.hpfEnabled.store (true);
    strip.hpfFreq.store (20.0f);
    const double hpfOpen = db (playTone (ctx, 60.0, 0.25f, 0.0f).left, lowRef);
    strip.hpfFreq.store (300.0f);
    const double hpfShut = db (playTone (ctx, 60.0, 0.25f, 0.0f).left, lowRef);
    strip.hpfEnabled.store (false);
    ctx.note ("60 Hz through the HPF: " + std::to_string (hpfOpen) + " dB at 20 Hz, "
              + std::to_string (hpfShut) + " dB at 300 Hz");
    ctx.expect (std::abs (hpfOpen) < 0.5, "the HPF at 20 Hz is not effectively off");
    ctx.expect (hpfShut < -12.0, "the HPF at 300 Hz does not cut 60 Hz");

    const double highRef = playTone (ctx, 10000.0, 0.25f, 0.0f).left;
    strip.lpfEnabled.store (true);
    strip.lpfFreq.store (20000.0f);
    const double lpfOpen = db (playTone (ctx, 10000.0, 0.25f, 0.0f).left, highRef);
    strip.lpfFreq.store (3000.0f);
    const double lpfShut = db (playTone (ctx, 10000.0, 0.25f, 0.0f).left, highRef);
    ctx.note ("10 kHz through the LPF: " + std::to_string (lpfOpen) + " dB at 20 kHz, "
              + std::to_string (lpfShut) + " dB at 3 kHz");
    ctx.expect (std::abs (lpfOpen) < 0.5, "the LPF at 20 kHz is not effectively off");
    ctx.expect (lpfShut < -12.0, "the LPF at 3 kHz does not cut 10 kHz");
    return ctx.verdict();
}

// ---------------------------------------------------------------- polarity

// The phase button turns the channel's signal upside down and changes nothing
// else.
ScenarioResult phaseInverts (ScenarioContext& ctx)
{
    liveInput (ctx, Track::Mode::Mono);
    auto& strip = ctx.session().track (kTrack).strip;

    const auto straight = playTone (ctx, 440.0, 0.25f, 0.0f);
    strip.phaseInvert.store (true);
    const auto inverted = playTone (ctx, 440.0, 0.25f, 0.0f);

    float worst = 0.0f, peak = 0.0f;
    for (int i = 0; i < kFrames; ++i)
    {
        const auto k = (std::size_t) i;
        worst = std::max (worst, std::abs (straight.lastLeft[k] + inverted.lastLeft[k]));
        peak = std::max (peak, std::abs (straight.lastLeft[k]));
    }
    ctx.note ("peak " + std::to_string (peak) + ", largest sum of the two runs "
              + std::to_string (worst));
    ctx.expect (peak > 0.05f, "the channel was silent");
    ctx.expect (worst < 0.01f * peak, "phase invert did not mirror the signal sample for sample");
    return ctx.verdict();
}

// ---------------------------------------------------------------- stereo

// A stereo track pans by balance: hard left silences R, and never moves R's
// audio across into L the way re-summing would.
ScenarioResult stereoPanIsBalance (ScenarioContext& ctx)
{
    liveInput (ctx, Track::Mode::Stereo);
    auto& strip = ctx.session().track (kTrack).strip;

    const auto centre = playTone (ctx, 440.0, 0.25f, 0.25f);
    strip.pan.store (-1.0f);
    const auto left = playTone (ctx, 440.0, 0.25f, 0.25f);
    const auto rightOnly = playTone (ctx, 440.0, 0.0f, 0.25f);
    ctx.note ("centre L/R " + std::to_string (centre.left) + "/" + std::to_string (centre.right)
              + ", hard left L/R " + std::to_string (left.left) + "/" + std::to_string (left.right)
              + ", R input only at hard left L " + std::to_string (rightOnly.left));
    ctx.expect (centre.left > 0.05 && centre.right > 0.05, "the stereo track was not heard on both sides");
    ctx.expect (db (left.right, centre.right) < -60.0, "hard left did not silence R");
    ctx.expect (db (rightOnly.left, centre.left) < -60.0, "hard left moved R's audio into L");
    return ctx.verdict();
}

// MONO sums the master to mono on both outputs.
ScenarioResult masterMonoSums (ScenarioContext& ctx)
{
    liveInput (ctx, Track::Mode::Stereo);
    auto& master = ctx.session().master();

    const auto stereo = playTone (ctx, 440.0, 0.25f, 0.0f);
    master.monoSum.store (true);
    const auto mono = playTone (ctx, 440.0, 0.25f, 0.0f);
    ctx.note ("L-only input: stereo L/R " + std::to_string (stereo.left) + "/" + std::to_string (stereo.right)
              + ", mono L/R " + std::to_string (mono.left) + "/" + std::to_string (mono.right));
    ctx.expect (stereo.left > 0.05 && db (stereo.right, stereo.left) < -60.0,
                "a left-only input was not left-only with MONO off");
    ctx.expect (mono.left > 0.02 && std::abs (db (mono.left, mono.right)) < 0.1,
                "MONO did not put the same signal on both outputs");
    return ctx.verdict();
}

// ---------------------------------------------------------------- bus EQ

// The bus EQ's three bands - a 300 Hz low shelf, an 800 Hz bell and a 2 kHz
// high shelf - each go to +/-9 dB on the knob. Like the channel EQ the curves
// follow the console's own markings, which read about 7 dB at the band's
// centre at the 9 dB mark.
ScenarioResult busEqReachesNineDb (ScenarioContext& ctx)
{
    liveInput (ctx, Track::Mode::Mono);
    auto& session = ctx.session();
    auto& strip = session.track (kTrack).strip;
    auto& bus = session.bus (0).strip;
    strip.busAssign[0].store (true);
    bus.eqEnabled.store (true);

    struct Band { const char* name; std::atomic<float>& gain; double hz; float db; };
    const Band bands[] = {
        { "LF", bus.eqLfGainDb, 60.0, 9.0f },
        { "MID", bus.eqMidGainDb, 800.0, -9.0f },
        { "HF", bus.eqHfGainDb, 10000.0, 9.0f },
    };
    for (const auto& band : bands)
    {
        const double flat = playTone (ctx, band.hz, 0.1f, 0.0f).left;
        band.gain.store (band.db);
        const double shaped = db (playTone (ctx, band.hz, 0.1f, 0.0f).left, flat);
        band.gain.store (0.0f);
        ctx.note (std::string (band.name) + " at " + std::to_string (band.db) + " dB moves "
                  + std::to_string (band.hz) + " Hz by " + std::to_string (shaped) + " dB");
        ctx.expect (shaped * band.db > 0.0 && std::abs (shaped) > 6.0 && std::abs (shaped) < 9.5,
                    std::string ("the bus ") + band.name + " band at "
                        + std::to_string ((int) band.db) + " dB did not move its band by the marked amount");
    }
    return ctx.verdict();
}

// ---------------------------------------------------------------- sends

// A send is post-fader by default, so pulling the fader down takes it away;
// pre-fader it carries on. Bypassing the sends silences all of them and
// bringing them back restores the level they had.
ScenarioResult sendsPrePostAndBypass (ScenarioContext& ctx)
{
    liveInput (ctx, Track::Mode::Mono);
    auto& strip = ctx.session().track (kTrack).strip;
    strip.auxSendDb[0].store (0.0f);

    const double post = playTone (ctx, 440.0, 0.25f, 0.0f).aux;
    strip.faderDb.store (ChannelStripParams::kFaderMinDb);
    const double postFaderDown = playTone (ctx, 440.0, 0.25f, 0.0f).aux;
    strip.auxSendPreFader[0].store (true);
    const double preFaderDown = playTone (ctx, 440.0, 0.25f, 0.0f).aux;
    strip.faderDb.store (0.0f);
    strip.auxSendPreFader[0].store (false);
    ctx.note ("send level: post " + std::to_string (post) + ", post with the fader down "
              + std::to_string (postFaderDown) + ", pre with the fader down " + std::to_string (preFaderDown));
    ctx.expect (post > 0.02, "the send did not reach the aux");
    ctx.expect (db (postFaderDown, post) < -60.0, "a post-fader send survived the fader going down");
    ctx.expect (std::abs (db (preFaderDown, post)) < 0.5, "a pre-fader send followed the fader");

    strip.auxSendsBypassed.store (true);
    const double bypassed = playTone (ctx, 440.0, 0.25f, 0.0f).aux;
    strip.auxSendsBypassed.store (false);
    const double restored = playTone (ctx, 440.0, 0.25f, 0.0f).aux;
    ctx.note ("bypassed " + std::to_string (bypassed) + ", restored " + std::to_string (restored));
    ctx.expect (db (bypassed, post) < -60.0, "bypassing the sends did not silence them");
    ctx.expect (std::abs (db (restored, post)) < 0.5, "the sends did not come back at their level");
    return ctx.verdict();
}

// ---------------------------------------------------------------- compressor

// Each compressor mode keeps its own settings: after a turn in another mode,
// switching back sounds exactly as it did.
ScenarioResult compModesKeepTheirSettings (ScenarioContext& ctx)
{
    liveInput (ctx, Track::Mode::Mono);
    auto& strip = ctx.session().track (kTrack).strip;

    strip.compEnabled.store (true);
    strip.compMode.store (1);
    strip.compFetThresholdDb.store (-30.0f);
    strip.compFetRatio.store (3);
    const double fet = playTone (ctx, 440.0, 0.5f, 0.0f).left;

    strip.compMode.store (2);
    strip.compVcaThreshDb.store (0.0f);
    strip.compVcaRatio.store (2.0f);
    const double vca = playTone (ctx, 440.0, 0.5f, 0.0f).left;

    strip.compMode.store (1);
    const double fetAgain = playTone (ctx, 440.0, 0.5f, 0.0f).left;
    strip.compMode.store (2);
    const double vcaAgain = playTone (ctx, 440.0, 0.5f, 0.0f).left;

    ctx.note ("FET " + std::to_string (fet) + " -> " + std::to_string (fetAgain)
              + ", VCA " + std::to_string (vca) + " -> " + std::to_string (vcaAgain));
    ctx.expect (std::abs (db (fet, vca)) > 3.0, "the two settings did not sound different, so nothing was proven");
    ctx.expect (std::abs (db (fetAgain, fet)) < 0.2, "FET did not sound the same after a turn in VCA");
    ctx.expect (std::abs (db (vcaAgain, vca)) < 0.2, "VCA did not sound the same after a turn in FET");
    ctx.expect (std::abs (strip.compFetThresholdDb.load() + 30.0f) < 1.0e-6f
                    && std::abs (strip.compVcaThreshDb.load()) < 1.0e-6f,
                "switching modes changed another mode's threshold");
    return ctx.verdict();
}

std::optional<ScenarioResult> run (ScenarioResult (*body) (ScenarioContext&), ScenarioContext& ctx)
{
    return body (ctx);
}

const ScenarioRegistrar filterRegistrar { Scenario {
    "strip.filter_ranges", { "strip", "dsp" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (filterRanges, ctx); } } };
const ScenarioRegistrar phaseRegistrar { Scenario {
    "strip.phase_inverts", { "strip", "dsp" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (phaseInverts, ctx); } } };
const ScenarioRegistrar panRegistrar { Scenario {
    "strip.stereo_pan_is_balance", { "strip", "dsp" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (stereoPanIsBalance, ctx); } } };
const ScenarioRegistrar monoRegistrar { Scenario {
    "master.mono_sums", { "master", "dsp" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (masterMonoSums, ctx); } } };
const ScenarioRegistrar busEqRegistrar { Scenario {
    "bus.eq_reaches_nine_db", { "bus", "dsp" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (busEqReachesNineDb, ctx); } } };
const ScenarioRegistrar sendsRegistrar { Scenario {
    "strip.sends_pre_post_and_bypass", { "strip", "aux", "dsp" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (sendsPrePostAndBypass, ctx); } } };
const ScenarioRegistrar compModesRegistrar { Scenario {
    "strip.comp_modes_keep_their_settings", { "strip", "comp", "dsp" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (compModesKeepTheirSettings, ctx); } } };
} // namespace
} // namespace duskstudio::scenario
