#include <catch2/catch_test_macros.hpp>

#include "dsp/CompressorCore.h"
#include "engine/CompModeMap.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Adapter parity: Dusk's CompressorCore is compared sample-for-sample with the
// authoritative framework-free MultiCompDSP while both receive the parameter
// contract used by Dusk's channel, bus and master strips. The raw core reference
// explicitly selects 1x internal processing and disables analog noise, matching
// the adapter's constructor/prepare policy. This catches mapping drift without
// treating the deliberately re-voiced DAF core as a transcription of the old
// JUCE implementation.

namespace
{
constexpr double kTwoPi = 6.28318530717958647692;

// Both sides execute the same core with the same inputs -> expect exact parity.
constexpr float kAudioTol = 1.0e-7f;
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

// Everything Dusk drives on the donor. Defaults mirror the app's parameter
// contract so an unset field is identical on both sides.
struct CompParams
{
    int   mode = 0;
    bool  bypass = false;
    float mix = 100.0f;
    bool  autoMakeup = false;
    float scHp = 0.0f;
    float optoPeakReduction = 0.0f, optoGain = 50.0f;
    bool  optoLimit = false;

    float fetInput = 0.0f, fetOutput = 0.0f, fetAttack = 0.2f, fetRelease = 400.0f;
    float fetThreshold = -10.0f;
    int   fetRatio = 0;

    float vcaThreshold = 0.0f, vcaRatio = 4.0f, vcaAttack = 1.0f, vcaRelease = 100.0f, vcaOutput = 0.0f;
    bool  vcaOverEasy = false;
    int   vcaDetectorMode = 0;

    float busThreshold = 0.0f, busMakeup = 0.0f, busMix = 100.0f;
    int   busRatio = 0, busAttack = 2, busRelease = 1;
};

void applyReference (duskaudio::MultiCompDSP& c, const CompParams& p)
{
    using Parameter = duskaudio::MultiCompDSP::Parameter;
    auto set = [&] (Parameter parameter, float value)
        { c.setParameter (parameter, value); };

    c.setMode (p.mode);
    c.setBypass (p.bypass);
    c.setMix (p.mix);
    set (Parameter::AutoMakeup, p.autoMakeup ? 1.0f : 0.0f);
    set (Parameter::SidechainHP, p.scHp);

    set (Parameter::OptoPeakReduction, p.optoPeakReduction);
    const float optoGainDb = duskstudio::comp::optoGainPctToMakeupDb (p.optoGain);
    set (Parameter::OptoGain, duskaudio::optoGainDbToKnob (optoGainDb));
    set (Parameter::OptoLimit, p.optoLimit ? 1.0f : 0.0f);

    set (Parameter::FetInput, p.fetInput);
    set (Parameter::FetOutput, p.fetOutput);
    set (Parameter::FetAttack, p.fetAttack);
    set (Parameter::FetRelease, p.fetRelease);
    set (Parameter::FetRatio, static_cast<float> (p.fetRatio));
    set (Parameter::FetThreshold, p.fetThreshold);

    set (Parameter::VcaThreshold, p.vcaThreshold);
    set (Parameter::VcaRatio, p.vcaRatio);
    set (Parameter::VcaAttack, p.vcaAttack);
    set (Parameter::VcaRelease, p.vcaRelease);
    set (Parameter::VcaOutput, p.vcaOutput);
    set (Parameter::VcaOverEasy, p.vcaOverEasy ? 1.0f : 0.0f);
    set (Parameter::VcaClassicDetector, p.vcaDetectorMode != 0 ? 1.0f : 0.0f);

    set (Parameter::BusThreshold, p.busThreshold);
    set (Parameter::BusRatio, static_cast<float> (p.busRatio));
    set (Parameter::BusAttack, static_cast<float> (p.busAttack));
    set (Parameter::BusRelease, static_cast<float> (p.busRelease));
    set (Parameter::BusMakeup, p.busMakeup);
    set (Parameter::BusMix, p.busMix);
}

void applyCore (duskstudio::CompressorCore& c, const CompParams& p)
{
    c.setMode (p.mode);
    c.setBypass (p.bypass);
    c.setMix (p.mix);
    c.setAutoMakeup (p.autoMakeup);
    c.setSidechainHp (p.scHp);
    c.setOptoPeakReduction (p.optoPeakReduction);
    c.setOptoGain (p.optoGain);
    c.setOptoLimit (p.optoLimit);

    c.setFetInput (p.fetInput);
    c.setFetOutput (p.fetOutput);
    c.setFetAttack (p.fetAttack);
    c.setFetRelease (p.fetRelease);
    c.setFetRatio (p.fetRatio);
    c.setFetThreshold (p.fetThreshold);
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

void runReference (const std::vector<float>& inL, const std::vector<float>& inR,
                   std::vector<float>& outL, std::vector<float>& outR, std::vector<float>& gr,
                   double sr, int prepBlock, int chunk, int nch,
                   const CompParams& base, const ParamHook& hook)
{
    duskaudio::MultiCompDSP c;
    c.setOversampling (0);
    c.setParameter (duskaudio::MultiCompDSP::Parameter::NoiseEnable, 0.0f);
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
        applyReference (c, p);

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

void runCore (const std::vector<float>& inL, const std::vector<float>& inR,
              std::vector<float>& outL, std::vector<float>& outR, std::vector<float>& gr,
              double sr, int prepBlock, int chunk, int nch,
              const CompParams& base, const ParamHook& hook)
{
    duskstudio::CompressorCore c;
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

// Full adapter/reference run for one parameter set + signal. REQUIREs audio
// and GR parity, surfacing actual diffs when a mapping slips.
void checkParity (const std::string& label, const CompParams& base,
                  double sr, int block, double seconds, int nch,
                  uint32_t seed, bool asym, const ParamHook& hook = nullptr)
{
    const int total = (int) (seconds * sr);
    std::vector<float> inL, inR;
    makeSignal (inL, inR, total, sr, seed, asym);

    std::vector<float> rL, rR, rGr, cL, cR, cGr;
    runReference (inL, inR, rL, rR, rGr, sr, block, block, nch, base, hook);
    runCore (inL, inR, cL, cR, cGr, sr, block, block, nch, base, hook);

    const float aDiff = audioMaxDiff (rL, rR, cL, cR);
    const float gDiff = grMaxDiff (rGr, cGr);
    UNSCOPED_INFO (label << ": audioMaxDiff=" << aDiff << "  grMaxDiff=" << gDiff);
    REQUIRE (aDiff <= kAudioTol);
    REQUIRE (gDiff <= kGrTol);
}
} // namespace

//==============================================================================
// 1. Opto (mode 0)
//==============================================================================
TEST_CASE ("comp adapter: Opto matches raw donor core", "[compab]")
{
    CompParams p; p.mode = 0;

    SECTION ("peak-reduction / gain / limit sweep (48k)")
    {
        for (float pr : { 20.0f, 60.0f, 90.0f })
            for (float g : { 0.0f, 50.0f, 65.0f, 100.0f })
                for (bool lim : { false, true })
                {
                    p.optoPeakReduction = pr; p.optoGain = g; p.optoLimit = lim;
                    checkParity ("opto pr=" + std::to_string ((int) pr) + " g=" + std::to_string ((int) g)
                                 + " lim=" + std::to_string (lim),
                                 p, 48000.0, 512, 3.0, 2, 0xA11CE, false);
                }
    }

    SECTION ("release slew across sample rates (sustained)")
    {
        p.optoPeakReduction = 80.0f; p.optoGain = 55.0f;
        for (double sr : { 44100.0, 48000.0, 96000.0 })
            checkParity ("opto sr=" + std::to_string ((int) sr), p, sr, 512, 3.0, 2, 0x0470, false);
    }
}

//==============================================================================
// 2. FET (mode 1)
//==============================================================================
TEST_CASE ("comp adapter: FET matches raw donor core", "[compab]")
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
                    checkParity ("fet ratio=" + std::to_string (ratio) + " atk=" + std::to_string (atk)
                                 + " rel=" + std::to_string (rel),
                                 p, 48000.0, 512, 2.0, 2, 0xFE7, false);
                }
    }

    SECTION ("all-buttons (ratio 4) sustained deep GR + transients, across rates")
    {
        p.fetRatio = 4; p.fetInput = 24.0f; p.fetOutput = 0.0f;
        p.fetAttack = 0.1f; p.fetRelease = 300.0f; p.fetThreshold = -30.0f;
        for (double sr : { 44100.0, 48000.0, 96000.0 })
            checkParity ("fet allbuttons sr=" + std::to_string ((int) sr), p, sr, 512, 2.5, 2, 0xB77, false);
    }
}

//==============================================================================
// 3. VCA (mode 2)
//==============================================================================
TEST_CASE ("comp adapter: VCA matches raw donor core", "[compab]")
{
    CompParams p; p.mode = 2; p.vcaRatio = 4.0f;

    SECTION ("OverEasy on/off + threshold knee sweep")
    {
        for (bool oe : { false, true })
            for (float toff : { -6.0f, -5.0f, -2.5f, 0.0f, 2.5f, 5.0f, 6.0f })
            {
                p.vcaOverEasy = oe; p.vcaThreshold = toff;
                checkParity ("vca oe=" + std::to_string (oe) + " thr=" + std::to_string (toff),
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
                checkParity ("vca det=" + std::to_string (det) + " sr=" + std::to_string ((int) sr),
                             p, sr, 512, 1.6, 2, 0xD37, false);
            }
    }

    SECTION ("fast-attack overshoot")
    {
        p.vcaThreshold = -28.0f; p.vcaRatio = 20.0f; p.vcaAttack = 0.1f; p.vcaRelease = 30.0f;
        checkParity ("vca fast attack", p, 48000.0, 512, 1.6, 2, 0xFA57, false);
    }
}

//==============================================================================
// 4. Bus (mode 3)
//==============================================================================
TEST_CASE ("comp adapter: Bus matches raw donor core", "[compab]")
{
    CompParams p; p.mode = 3;

    SECTION ("attack/release indices incl auto-release (transient-dense + sustained)")
    {
        for (int atk : { 0, 2, 5 })
            for (int rel : { 0, 1, 4 })
            {
                p.busThreshold = -12.0f; p.busRatio = 1; p.busAttack = atk; p.busRelease = rel;
                checkParity ("bus atk=" + std::to_string (atk) + " rel=" + std::to_string (rel),
                             p, 48000.0, 512, 2.0, 2, 0xB5, false);
            }
    }

    SECTION ("default fully-linked path with asymmetric L-R")
    {
        p.busThreshold = -15.0f; p.busRatio = 2; p.busAttack = 2; p.busRelease = 1;
        checkParity ("bus link=100", p, 48000.0, 512, 2.0, 2, 0xA57, true);
    }

    SECTION ("makeup hot input + postGain ordering + bus_mix on linked path")
    {
        p.busThreshold = -24.0f; p.busRatio = 2; p.busMakeup = 12.0f;
        for (float mix : { 40.0f, 100.0f })
        {
            p.busMix = mix;
            checkParity ("bus makeup mix=" + std::to_string ((int) mix), p, 48000.0, 512, 2.0, 2, 0x60, true);
        }
    }

    SECTION ("mono 1ch (non-linked path)")
    {
        p.busThreshold = -15.0f; p.busRatio = 1;
        checkParity ("bus mono", p, 48000.0, 512, 1.6, 1, 0x0110, false);
    }
}

//==============================================================================
// 5. Bypass toggle: delayed-dry parity through the transition + un-bypass fade
//==============================================================================
TEST_CASE ("comp adapter: bypass toggle and fade match raw donor core", "[compab]")
{
    CompParams base; base.mode = 1; base.fetRatio = 2; base.fetInput = 12.0f;
    base.fetThreshold = -20.0f; base.autoMakeup = true;

    // Active blocks 0-9, bypass 10-24 through the core's delayed-dry path,
    // then active again from block 25 through its bypass ramp.
    auto hook = [] (CompParams& p, int blk) { p.bypass = (blk >= 10 && blk < 25); };
    checkParity ("bypass toggle", base, 48000.0, 512, 3.0, 2, 0xB1FA, false, hook);
}

//==============================================================================
// 6. Partial mix (dry-ring parity)
//==============================================================================
TEST_CASE ("comp adapter: partial mix matches raw donor core", "[compab]")
{
    CompParams p; p.mode = 1; p.fetRatio = 1; p.fetInput = 10.0f; p.fetThreshold = -18.0f;
    p.mix = 40.0f;
    checkParity ("fet mix=40", p, 48000.0, 512, 2.0, 2, 0x4140, false);
}

//==============================================================================
// 7. Block-size invariance: 64 / 160 / 1024 chunking, both sides, audio only.
//==============================================================================
TEST_CASE ("comp adapter: block-size invariance matches raw donor core", "[compab]")
{
    CompParams p; p.mode = 1; p.fetRatio = 2; p.fetInput = 14.0f; p.fetThreshold = -20.0f;

    const double sr = 48000.0;
    const int prepBlock = 1024;
    const int total = (int) (1.5 * sr);
    std::vector<float> inL, inR;
    makeSignal (inL, inR, total, sr, 0xB10C, false);

    std::vector<float> rRefL, rRefR, cRefL, cRefR, dummyGr;
    runReference (inL, inR, rRefL, rRefR, dummyGr, sr, prepBlock, prepBlock, 2, p, nullptr);
    runCore (inL, inR, cRefL, cRefR, dummyGr, sr, prepBlock, prepBlock, 2, p, nullptr);

    for (int chunk : { 64, 160, 1024 })
    {
        std::vector<float> rL, rR, cL, cR;
        runReference (inL, inR, rL, rR, dummyGr, sr, prepBlock, chunk, 2, p, nullptr);
        runCore (inL, inR, cL, cR, dummyGr, sr, prepBlock, chunk, 2, p, nullptr);

        const float rSelf = audioMaxDiff (rL, rR, rRefL, rRefR);
        const float cSelf = audioMaxDiff (cL, cR, cRefL, cRefR);
        const float cross = audioMaxDiff (rL, rR, cL, cR);
        UNSCOPED_INFO ("chunk=" << chunk << " refSelf=" << rSelf
                       << " adapterSelf=" << cSelf << " cross=" << cross);
        REQUIRE (rSelf <= kAudioTol);
        REQUIRE (cSelf <= kAudioTol);
        REQUIRE (cross <= kAudioTol);
    }
}

//==============================================================================
// 8. GR meter parity — explicit, deep compression.
//==============================================================================
TEST_CASE ("comp adapter: GR meter matches raw donor core", "[compab]")
{
    const double sr = 48000.0;
    const int block = 500;
    const int total = (int) (2.0 * sr);
    std::vector<float> inL, inR;
    makeSignal (inL, inR, total, sr, 0x6DAA, false);

    CompParams p; p.mode = 2; p.vcaThreshold = -24.0f; p.vcaRatio = 10.0f;

    std::vector<float> rL, rR, rGr, cL, cR, cGr;
    runReference (inL, inR, rL, rR, rGr, sr, block, block, 2, p, nullptr);
    runCore (inL, inR, cL, cR, cGr, sr, block, block, 2, p, nullptr);

    const float gDiff = grMaxDiff (rGr, cGr);
    UNSCOPED_INFO ("GR maxDiff=" << gDiff << " (blocks=" << rGr.size() << ")");
    REQUIRE (rGr.size() == cGr.size());
    REQUIRE (gDiff <= kGrTol);
}

//==============================================================================
// 9. Auto-makeup on/off (FET + VCA). Opto forces gain internally when on.
//==============================================================================
TEST_CASE ("comp adapter: auto-makeup matches raw donor core", "[compab]")
{
    SECTION ("FET auto-makeup on")
    {
        CompParams p; p.mode = 1; p.fetRatio = 3; p.fetInput = 18.0f; p.fetThreshold = -24.0f;
        p.autoMakeup = true;
        checkParity ("fet auto-makeup", p, 48000.0, 512, 2.5, 2, 0xA07, false);
    }
    SECTION ("VCA auto-makeup on")
    {
        CompParams p; p.mode = 2; p.vcaThreshold = -26.0f; p.vcaRatio = 8.0f;
        p.autoMakeup = true;
        checkParity ("vca auto-makeup", p, 48000.0, 512, 2.5, 2, 0xA08, false);
    }
    SECTION ("Opto auto-makeup on (internal gain)")
    {
        CompParams p; p.mode = 0; p.optoPeakReduction = 75.0f; p.autoMakeup = true;
        checkParity ("opto auto-makeup", p, 48000.0, 512, 3.0, 2, 0xA09, false);
    }
}
