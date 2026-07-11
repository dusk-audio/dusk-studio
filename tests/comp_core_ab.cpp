#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "UniversalCompressor.h"          // JUCE donor (ground truth)
#include "UniversalCompressorDSP.hpp"      // duskaudio:: JUCE-free core

#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// A/B parity: the JUCE-free duskaudio::UniversalCompressorDSP must match the JUCE
// UniversalCompressor sample-for-sample when both are driven exactly as Dusk
// Studio's channel strips drive the donor (setMinimalProcessing(false),
// setInternalOversamplingEnabled(false), oversampling param left at its 2x
// default, noise forced off, raw APVTS atoms written before every processBlock).
// JUCE output is the reference; the core is a verbatim transcription, so any
// nonzero diff is a genuine slip to hunt down.

namespace
{
constexpr double kTwoPi = 6.28318530717958647692;

// Per-sample audio tolerance. Transcription is verbatim -> expect 0.0f.
constexpr float kAudioTol = 1.0e-7f;
// GR meter is a dB read-back through the same block-delay ring on both sides.
constexpr float kGrTol = 1.0e-6f;

inline float lin (float db) { return std::pow (10.0f, db * 0.05f); }
inline int jmaxI (int a, int b) { return a > b ? a : b; }

struct Rng
{
    uint32_t s;
    float next() noexcept
    {
        s = s * 1664525u + 1013904223u;
        return (float) ((s >> 8) & 0xFFFFFF) / (float) 0x1000000 * 2.0f - 1.0f;
    }
};

// Deterministic stereo material: sustained multi-tone + 0.18 s level steps
// (attack/release + voltage-sag territory), periodic transient clicks
// (delayed-attack / release-memory paths) and a touch of noise. `asym` skews
// L vs R so the stereo-link paths see a real inter-channel imbalance.
void makeSignal (std::vector<float>& L, std::vector<float>& R,
                 int total, double sr, uint32_t seed, bool asym)
{
    L.assign ((size_t) total, 0.0f);
    R.assign ((size_t) total, 0.0f);
    Rng rng { seed };

    const double f1 = 110.0, f2 = 440.0, f3 = 1730.0;
    double p1 = 0.0, p2 = 0.0, p3 = 0.0;
    const int clickPeriod = jmaxI ((int) (sr * 0.13), 1);
    const int clickLen    = jmaxI ((int) (sr * 0.001), 1);
    const float segDb[6] = { -18.0f, -3.0f, -9.0f, 0.0f, -24.0f, -6.0f };

    for (int i = 0; i < total; ++i)
    {
        const double t = (double) i / sr;
        const int seg = (int) (t / 0.18) % 6;
        const float ampL = lin (segDb[seg]);
        const float ampR = asym ? lin (segDb[(seg + 3) % 6]) * 0.6f : ampL;

        const float sL = 0.6f * (float) std::sin (p1) + 0.3f * (float) std::sin (p2)
                       + 0.25f * (float) std::sin (p3);
        const float sR = asym ? (0.6f * (float) std::sin (p1 + 0.5) + 0.3f * (float) std::sin (p2)
                                 + 0.25f * (float) std::sin (p3 * 1.01))
                              : sL;
        p1 += kTwoPi * f1 / sr;
        p2 += kTwoPi * f2 / sr;
        p3 += kTwoPi * f3 / sr;

        const float n = 0.02f * rng.next();
        float clk = 0.0f;
        if ((i % clickPeriod) < clickLen) clk = rng.next() > 0.0f ? 0.9f : -0.9f;

        L[(size_t) i] = ampL * sL + n + clk;
        R[(size_t) i] = ampR * sR + n * 0.8f + clk * (asym ? 0.5f : 1.0f);
    }
}

// Everything Dusk drives on the donor. Defaults mirror the JUCE APVTS defaults
// so an unset field is identical on both sides.
struct CompParams
{
    int   mode = 0;
    bool  bypass = false;
    float mix = 100.0f;
    bool  autoMakeup = false;
    float scHp = 0.0f;
    float stereoLink = 100.0f;
    int   stereoLinkMode = 0;

    float optoPeakReduction = 0.0f, optoGain = 50.0f;
    bool  optoLimit = false;

    float fetInput = 0.0f, fetOutput = 0.0f, fetAttack = 0.2f, fetRelease = 400.0f;
    float fetThreshold = -10.0f, fetTransient = 0.0f;
    int   fetRatio = 0, fetCurveMode = 0;

    float vcaThreshold = 0.0f, vcaRatio = 4.0f, vcaAttack = 1.0f, vcaRelease = 100.0f, vcaOutput = 0.0f;
    bool  vcaOverEasy = false;
    int   vcaDetectorMode = 0;

    float busThreshold = 0.0f, busMakeup = 0.0f, busMix = 100.0f;
    int   busRatio = 0, busAttack = 2, busRelease = 1;
};

void applyJuce (UniversalCompressor& c, const CompParams& p)
{
    auto& a = c.getParameters();
    auto set = [&] (const char* id, float v)
    {
        if (auto* atom = a.getRawParameterValue (id))
            atom->store (v, std::memory_order_relaxed);
    };
    set ("mode", (float) p.mode);
    set ("bypass", p.bypass ? 1.0f : 0.0f);
    set ("mix", p.mix);
    set ("auto_makeup", p.autoMakeup ? 1.0f : 0.0f);
    set ("sidechain_hp", p.scHp);
    set ("stereo_link", p.stereoLink);
    set ("stereo_link_mode", (float) p.stereoLinkMode);
    set ("noise_enable", 0.0f);   // Dusk forces analog noise off

    set ("opto_peak_reduction", p.optoPeakReduction);
    set ("opto_gain", p.optoGain);
    set ("opto_limit", p.optoLimit ? 1.0f : 0.0f);

    set ("fet_input", p.fetInput);
    set ("fet_output", p.fetOutput);
    set ("fet_attack", p.fetAttack);
    set ("fet_release", p.fetRelease);
    set ("fet_ratio", (float) p.fetRatio);
    set ("fet_threshold", p.fetThreshold);
    set ("fet_curve_mode", (float) p.fetCurveMode);
    set ("fet_transient", p.fetTransient);

    set ("vca_threshold", p.vcaThreshold);
    set ("vca_ratio", p.vcaRatio);
    set ("vca_attack", p.vcaAttack);
    set ("vca_release", p.vcaRelease);
    set ("vca_output", p.vcaOutput);
    set ("vca_overeasy", p.vcaOverEasy ? 1.0f : 0.0f);
    set ("vca_detector_mode", (float) p.vcaDetectorMode);

    set ("bus_threshold", p.busThreshold);
    set ("bus_ratio", (float) p.busRatio);
    set ("bus_attack", (float) p.busAttack);
    set ("bus_release", (float) p.busRelease);
    set ("bus_makeup", p.busMakeup);
    set ("bus_mix", p.busMix);
}

void applyCore (duskaudio::UniversalCompressorDSP& c, const CompParams& p)
{
    c.setMode (p.mode);
    c.setBypass (p.bypass);
    c.setMix (p.mix);
    c.setAutoMakeup (p.autoMakeup);
    c.setSidechainHp (p.scHp);
    c.setStereoLink (p.stereoLink);
    c.setStereoLinkMode (p.stereoLinkMode);

    c.setOptoPeakReduction (p.optoPeakReduction);
    c.setOptoGain (p.optoGain);
    c.setOptoLimit (p.optoLimit);

    c.setFetInput (p.fetInput);
    c.setFetOutput (p.fetOutput);
    c.setFetAttack (p.fetAttack);
    c.setFetRelease (p.fetRelease);
    c.setFetRatio (p.fetRatio);
    c.setFetThreshold (p.fetThreshold);
    c.setFetCurveMode (p.fetCurveMode);
    c.setFetTransient (p.fetTransient);

    c.setVcaThreshold (p.vcaThreshold);
    c.setVcaRatio (p.vcaRatio);
    c.setVcaAttack (p.vcaAttack);
    c.setVcaRelease (p.vcaRelease);
    c.setVcaOutput (p.vcaOutput);
    c.setVcaOverEasy (p.vcaOverEasy);
    c.setVcaDetectorMode (p.vcaDetectorMode);

    c.setBusThreshold (p.busThreshold);
    c.setBusRatio (p.busRatio);
    c.setBusAttack (p.busAttack);
    c.setBusRelease (p.busRelease);
    c.setBusMakeup (p.busMakeup);
    c.setBusMix (p.busMix);
}

using ParamHook = std::function<void (CompParams&, int /*blockIdx*/)>;

// Drive the JUCE donor exactly like ChannelStrip: 2/2 play config, prepare at
// prepBlock, then feed `chunk`-sized blocks (params written before each).
void runJuce (const std::vector<float>& inL, const std::vector<float>& inR,
              std::vector<float>& outL, std::vector<float>& outR, std::vector<float>& gr,
              double sr, int prepBlock, int chunk, int nch,
              const CompParams& base, const ParamHook& hook)
{
    UniversalCompressor c;
    c.setMinimalProcessing (false);
    c.setInternalOversamplingEnabled (false);
    c.setPlayConfigDetails (2, 2, sr, prepBlock);
    c.prepareToPlay (sr, prepBlock);
    c.reset();

    const int total = (int) inL.size();
    outL.assign ((size_t) total, 0.0f);
    outR.assign ((size_t) total, 0.0f);
    gr.clear();

    juce::MidiBuffer midi;
    int blk = 0;
    for (int off = 0; off < total; off += chunk, ++blk)
    {
        const int n = std::min (chunk, total - off);
        CompParams p = base;
        if (hook) hook (p, blk);
        applyJuce (c, p);

        juce::AudioBuffer<float> buf (nch, n);
        for (int i = 0; i < n; ++i)
        {
            buf.setSample (0, i, inL[(size_t) (off + i)]);
            if (nch > 1) buf.setSample (1, i, inR[(size_t) (off + i)]);
        }
        midi.clear();
        c.processBlock (buf, midi);

        for (int i = 0; i < n; ++i)
        {
            outL[(size_t) (off + i)] = buf.getSample (0, i);
            outR[(size_t) (off + i)] = nch > 1 ? buf.getSample (1, i) : buf.getSample (0, i);
        }
        gr.push_back (c.getGainReduction());
    }
}

void runCore (const std::vector<float>& inL, const std::vector<float>& inR,
              std::vector<float>& outL, std::vector<float>& outR, std::vector<float>& gr,
              double sr, int prepBlock, int chunk, int nch,
              const CompParams& base, const ParamHook& hook)
{
    duskaudio::UniversalCompressorDSP c;
    c.prepare (sr, prepBlock);
    c.reset();

    const int total = (int) inL.size();
    outL.assign ((size_t) total, 0.0f);
    outR.assign ((size_t) total, 0.0f);
    gr.clear();

    std::vector<float> tL ((size_t) chunk, 0.0f), tR ((size_t) chunk, 0.0f);
    int blk = 0;
    for (int off = 0; off < total; off += chunk, ++blk)
    {
        const int n = std::min (chunk, total - off);
        CompParams p = base;
        if (hook) hook (p, blk);
        applyCore (c, p);

        const float* inP[2]  = { &inL[(size_t) off], nch > 1 ? &inR[(size_t) off] : &inL[(size_t) off] };
        float*       outP[2] = { tL.data(), tR.data() };
        c.processBlock (inP, outP, nch, n);

        for (int i = 0; i < n; ++i)
        {
            outL[(size_t) (off + i)] = tL[(size_t) i];
            outR[(size_t) (off + i)] = nch > 1 ? tR[(size_t) i] : tL[(size_t) i];
        }
        gr.push_back (c.getGainReduction());
    }
}

float audioMaxDiff (const std::vector<float>& aL, const std::vector<float>& aR,
                    const std::vector<float>& bL, const std::vector<float>& bR)
{
    float d = 0.0f;
    const size_t n = std::min (aL.size(), bL.size());
    for (size_t i = 0; i < n; ++i)
    {
        d = std::max (d, std::abs (aL[i] - bL[i]));
        d = std::max (d, std::abs (aR[i] - bR[i]));
    }
    return d;
}

float grMaxDiff (const std::vector<float>& a, const std::vector<float>& b)
{
    float d = 0.0f;
    const size_t n = std::min (a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
        d = std::max (d, std::abs (a[i] - b[i]));
    return d;
}

// Full A/B for one param set + signal. REQUIREs audio (and GR) parity and
// surfaces the actual diffs via UNSCOPED_INFO so a slip prints its magnitude.
void checkAB (const std::string& label, const CompParams& base,
              double sr, int block, double seconds, int nch,
              uint32_t seed, bool asym, const ParamHook& hook = nullptr)
{
    const int total = (int) (seconds * sr);
    std::vector<float> inL, inR;
    makeSignal (inL, inR, total, sr, seed, asym);

    std::vector<float> jL, jR, jGr, cL, cR, cGr;
    runJuce (inL, inR, jL, jR, jGr, sr, block, block, nch, base, hook);
    runCore (inL, inR, cL, cR, cGr, sr, block, block, nch, base, hook);

    const float aDiff = audioMaxDiff (jL, jR, cL, cR);
    const float gDiff = grMaxDiff (jGr, cGr);
    UNSCOPED_INFO (label << ": audioMaxDiff=" << aDiff << "  grMaxDiff=" << gDiff);
    REQUIRE (aDiff <= kAudioTol);
    REQUIRE (gDiff <= kGrTol);
}
} // namespace

//==============================================================================
// 1. Opto (mode 0)
//==============================================================================
TEST_CASE ("comp A/B: Opto matches JUCE", "[compab]")
{
    CompParams p; p.mode = 0;

    SECTION ("peak-reduction / gain / limit sweep (48k)")
    {
        for (float pr : { 20.0f, 60.0f, 90.0f })
            for (float g : { 50.0f, 65.0f })
                for (bool lim : { false, true })
                {
                    p.optoPeakReduction = pr; p.optoGain = g; p.optoLimit = lim;
                    checkAB ("opto pr=" + std::to_string ((int) pr) + " g=" + std::to_string ((int) g)
                             + " lim=" + std::to_string (lim),
                             p, 48000.0, 512, 3.0, 2, 0xA11CE, false);
                }
    }

    SECTION ("release slew across sample rates (sustained)")
    {
        p.optoPeakReduction = 80.0f; p.optoGain = 55.0f;
        for (double sr : { 44100.0, 48000.0, 96000.0 })
            checkAB ("opto sr=" + std::to_string ((int) sr), p, sr, 512, 3.0, 2, 0x0470, false);
    }
}

//==============================================================================
// 2. FET (mode 1)
//==============================================================================
TEST_CASE ("comp A/B: FET matches JUCE", "[compab]")
{
    CompParams p; p.mode = 1;

    SECTION ("gain / attack / release / ratio sweep")
    {
        for (int ratio : { 0, 1, 2, 3, 4 })
            for (float atk : { 0.05f, 5.0f })
                for (float rel : { 80.0f, 600.0f })
                {
                    p.fetRatio = ratio; p.fetInput = 12.0f; p.fetOutput = -3.0f;
                    p.fetAttack = atk; p.fetRelease = rel; p.fetThreshold = -18.0f;
                    checkAB ("fet ratio=" + std::to_string (ratio) + " atk=" + std::to_string (atk)
                             + " rel=" + std::to_string (rel),
                             p, 48000.0, 512, 2.0, 2, 0xFE7, false);
                }
    }

    SECTION ("all-buttons (ratio 4) sustained deep GR + transients, across rates")
    {
        p.fetRatio = 4; p.fetInput = 24.0f; p.fetOutput = 0.0f;
        p.fetAttack = 0.1f; p.fetRelease = 300.0f; p.fetThreshold = -30.0f;
        for (double sr : { 44100.0, 48000.0, 96000.0 })
            checkAB ("fet allbuttons sr=" + std::to_string ((int) sr), p, sr, 512, 2.5, 2, 0xB77, false);
    }

    SECTION ("curve mode Measured")
    {
        p.fetRatio = 4; p.fetCurveMode = 1; p.fetInput = 20.0f; p.fetThreshold = -28.0f;
        checkAB ("fet measured curve", p, 48000.0, 512, 2.0, 2, 0xC0FFEE, false);
    }
}

//==============================================================================
// 3. VCA (mode 2)
//==============================================================================
TEST_CASE ("comp A/B: VCA matches JUCE", "[compab]")
{
    CompParams p; p.mode = 2; p.vcaRatio = 4.0f;

    SECTION ("OverEasy on/off + threshold knee sweep")
    {
        for (bool oe : { false, true })
            for (float toff : { -6.0f, -5.0f, -2.5f, 0.0f, 2.5f, 5.0f, 6.0f })
            {
                p.vcaOverEasy = oe; p.vcaThreshold = toff;
                checkAB ("vca oe=" + std::to_string (oe) + " thr=" + std::to_string (toff),
                         p, 48000.0, 512, 1.6, 2, 0x0CA, false);
            }
    }

    SECTION ("detector mode 0/1 + attack/release banding, across rates")
    {
        p.vcaThreshold = -20.0f; p.vcaRatio = 8.0f;
        for (int det : { 0, 1 })
            for (double sr : { 44100.0, 48000.0, 96000.0 })
            {
                p.vcaDetectorMode = det;
                checkAB ("vca det=" + std::to_string (det) + " sr=" + std::to_string ((int) sr),
                         p, sr, 512, 1.6, 2, 0xD37, false);
            }
    }

    SECTION ("fast-attack overshoot")
    {
        p.vcaThreshold = -28.0f; p.vcaRatio = 20.0f; p.vcaAttack = 0.1f; p.vcaRelease = 30.0f;
        checkAB ("vca fast attack", p, 48000.0, 512, 1.6, 2, 0xFA57, false);
    }
}

//==============================================================================
// 4. Bus (mode 3)
//==============================================================================
TEST_CASE ("comp A/B: Bus matches JUCE", "[compab]")
{
    CompParams p; p.mode = 3;

    SECTION ("attack/release indices incl auto-release (transient-dense + sustained)")
    {
        for (int atk : { 0, 2, 5 })
            for (int rel : { 0, 1, 4 })
            {
                p.busThreshold = -12.0f; p.busRatio = 1; p.busAttack = atk; p.busRelease = rel;
                checkAB ("bus atk=" + std::to_string (atk) + " rel=" + std::to_string (rel),
                         p, 48000.0, 512, 2.0, 2, 0xB5, false);
            }
    }

    SECTION ("stereo link default / explicit 0.0 / 0.5 / 1.0 with asymmetric L-R")
    {
        p.busThreshold = -15.0f; p.busRatio = 2; p.busAttack = 2; p.busRelease = 1;
        for (float link : { 0.0f, 50.0f, 100.0f })
        {
            p.stereoLink = link;
            checkAB ("bus link=" + std::to_string ((int) link), p, 48000.0, 512, 2.0, 2, 0xA57, true);
        }
    }

    SECTION ("makeup hot input + postGain ordering + bus_mix on linked path")
    {
        p.busThreshold = -24.0f; p.busRatio = 2; p.busMakeup = 12.0f;
        for (float mix : { 40.0f, 100.0f })
        {
            p.busMix = mix;
            checkAB ("bus makeup mix=" + std::to_string ((int) mix), p, 48000.0, 512, 2.0, 2, 0x60, true);
        }
    }

    SECTION ("mono 1ch (non-linked path)")
    {
        p.busThreshold = -15.0f; p.busRatio = 1;
        checkAB ("bus mono", p, 48000.0, 512, 1.6, 1, 0x0110, false);
    }
}

//==============================================================================
// 5. Bypass toggle: delayed-dry parity through the transition + un-bypass fade
//==============================================================================
TEST_CASE ("comp A/B: bypass toggle + un-bypass fade", "[compab]")
{
    CompParams base; base.mode = 1; base.fetRatio = 2; base.fetInput = 12.0f;
    base.fetThreshold = -20.0f; base.autoMakeup = true;

    // active blocks 0-9, bypass 10-24 (60-sample delayed dry), active again 25+
    // (5 ms fade + auto-makeup accumulator reset).
    auto hook = [] (CompParams& p, int blk) { p.bypass = (blk >= 10 && blk < 25); };
    checkAB ("bypass toggle", base, 48000.0, 512, 3.0, 2, 0xB1FA, false, hook);
}

//==============================================================================
// 6. Partial mix (dry-ring parity)
//==============================================================================
TEST_CASE ("comp A/B: partial mix dry ring", "[compab]")
{
    CompParams p; p.mode = 1; p.fetRatio = 1; p.fetInput = 10.0f; p.fetThreshold = -18.0f;
    p.mix = 40.0f;
    checkAB ("fet mix=40", p, 48000.0, 512, 2.0, 2, 0x4140, false);
}

//==============================================================================
// 7. Block-size invariance: 64 / 160 / 1024 chunking, both sides, audio only
//    (GR meter delay is measured in BLOCKS, so it legitimately differs by chunk).
//==============================================================================
TEST_CASE ("comp A/B: block-size invariance", "[compab]")
{
    CompParams p; p.mode = 1; p.fetRatio = 2; p.fetInput = 14.0f; p.fetThreshold = -20.0f;

    const double sr = 48000.0;
    const int prepBlock = 1024;
    const int total = (int) (1.5 * sr);
    std::vector<float> inL, inR;
    makeSignal (inL, inR, total, sr, 0xB10C, false);

    std::vector<float> jRefL, jRefR, cRefL, cRefR, dummyGr;
    runJuce (inL, inR, jRefL, jRefR, dummyGr, sr, prepBlock, prepBlock, 2, p, nullptr);
    runCore (inL, inR, cRefL, cRefR, dummyGr, sr, prepBlock, prepBlock, 2, p, nullptr);

    for (int chunk : { 64, 160, 1024 })
    {
        std::vector<float> jL, jR, cL, cR;
        runJuce (inL, inR, jL, jR, dummyGr, sr, prepBlock, chunk, 2, p, nullptr);
        runCore (inL, inR, cL, cR, dummyGr, sr, prepBlock, chunk, 2, p, nullptr);

        const float jSelf = audioMaxDiff (jL, jR, jRefL, jRefR);
        const float cSelf = audioMaxDiff (cL, cR, cRefL, cRefR);
        const float cross = audioMaxDiff (jL, jR, cL, cR);
        UNSCOPED_INFO ("chunk=" << chunk << " jSelf=" << jSelf << " cSelf=" << cSelf << " cross=" << cross);
        REQUIRE (jSelf <= kAudioTol);
        REQUIRE (cSelf <= kAudioTol);
        REQUIRE (cross <= kAudioTol);
    }
}

//==============================================================================
// 8. GR meter parity (block-delay behaviour) — explicit, deep compression.
//==============================================================================
TEST_CASE ("comp A/B: GR meter block-delay parity", "[compab]")
{
    const double sr = 48000.0;
    const int block = 500;   // 60/500 -> ceil = 1 block delay
    const int total = (int) (2.0 * sr);
    std::vector<float> inL, inR;
    makeSignal (inL, inR, total, sr, 0x6DAA, false);

    CompParams p; p.mode = 2; p.vcaThreshold = -24.0f; p.vcaRatio = 10.0f;

    std::vector<float> jL, jR, jGr, cL, cR, cGr;
    runJuce (inL, inR, jL, jR, jGr, sr, block, block, 2, p, nullptr);
    runCore (inL, inR, cL, cR, cGr, sr, block, block, 2, p, nullptr);

    const float gDiff = grMaxDiff (jGr, cGr);
    UNSCOPED_INFO ("GR block-delay maxDiff=" << gDiff << " (blocks=" << jGr.size() << ")");
    REQUIRE (jGr.size() == cGr.size());
    REQUIRE (gDiff <= kGrTol);
}

//==============================================================================
// 9. Auto-makeup on/off (FET + VCA). Opto forces gain internally when on.
//==============================================================================
TEST_CASE ("comp A/B: auto-makeup FET + VCA", "[compab]")
{
    SECTION ("FET auto-makeup on")
    {
        CompParams p; p.mode = 1; p.fetRatio = 3; p.fetInput = 18.0f; p.fetThreshold = -24.0f;
        p.autoMakeup = true;
        checkAB ("fet auto-makeup", p, 48000.0, 512, 2.5, 2, 0xA07, false);
    }
    SECTION ("VCA auto-makeup on")
    {
        CompParams p; p.mode = 2; p.vcaThreshold = -26.0f; p.vcaRatio = 8.0f;
        p.autoMakeup = true;
        checkAB ("vca auto-makeup", p, 48000.0, 512, 2.5, 2, 0xA08, false);
    }
    SECTION ("Opto auto-makeup on (internal gain)")
    {
        CompParams p; p.mode = 0; p.optoPeakReduction = 75.0f; p.autoMakeup = true;
        checkAB ("opto auto-makeup", p, 48000.0, 512, 3.0, 2, 0xA09, false);
    }
}
