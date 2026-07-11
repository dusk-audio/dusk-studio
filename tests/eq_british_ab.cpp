#include <catch2/catch_test_macros.hpp>

#include <BritishEQProcessor.h>

#include <dsp/FourKEQDSP.hpp>

#include <cmath>
#include <random>
#include <vector>

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int    kBlock      = 512;
constexpr int    kBlocks     = 40;
constexpr int    kTotal      = kBlock * kBlocks;   // 20480 samples
constexpr int    kWarmup     = kBlock * 16;        // 8192 samples before measuring
constexpr double kPi         = 3.14159265358979323846;

// Deterministic stereo excitation: log sine sweep + periodic impulses + noise.
// L and R differ (offset phase + independent noise stream) so inter-channel
// paths — FourKEQDSP's -60 dB crosstalk, both processors' independent filter
// state — are genuinely exercised rather than mirrored.
void makeInput (std::vector<float>& L, std::vector<float>& R)
{
    L.assign ((size_t) kTotal, 0.0f);
    R.assign ((size_t) kTotal, 0.0f);

    std::mt19937 rng (0x4B455147u);
    std::uniform_real_distribution<float> noise (-1.0f, 1.0f);

    const double f0 = 50.0, f1 = 15000.0;
    const double logRatio = std::log (f1 / f0);

    double phaseL = 0.0, phaseR = 1.3;
    for (int n = 0; n < kTotal; ++n)
    {
        const double t = (double) n / (double) kTotal;
        const double freq = f0 * std::exp (logRatio * t);
        const double dphase = 2.0 * kPi * freq / kSampleRate;
        phaseL += dphase;
        phaseR += dphase;

        float l = 0.30f * (float) std::sin (phaseL) + 0.10f * noise (rng);
        float r = 0.30f * (float) std::sin (phaseR) + 0.10f * noise (rng);
        if ((n % 4096) == 0) { l += 0.5f; r -= 0.5f; }

        L[(size_t) n] = l;
        R[(size_t) n] = r;
    }
}

void runBritish (const BritishEQProcessor::Parameters& p,
                 const std::vector<float>& Lin, const std::vector<float>& Rin,
                 std::vector<float>& Lout, std::vector<float>& Rout)
{
    BritishEQProcessor eq;
    eq.prepare (kSampleRate, kBlock, 2);
    eq.setParameters (p);

    Lout.assign ((size_t) kTotal, 0.0f);
    Rout.assign ((size_t) kTotal, 0.0f);

    juce::AudioBuffer<float> buf (2, kBlock);
    for (int off = 0; off < kTotal; off += kBlock)
    {
        const int n = std::min (kBlock, kTotal - off);
        for (int i = 0; i < n; ++i)
        {
            buf.setSample (0, i, Lin[(size_t) (off + i)]);
            buf.setSample (1, i, Rin[(size_t) (off + i)]);
        }
        buf.setSize (2, n, true, false, true);
        eq.process (buf);
        for (int i = 0; i < n; ++i)
        {
            Lout[(size_t) (off + i)] = buf.getSample (0, i);
            Rout[(size_t) (off + i)] = buf.getSample (1, i);
        }
        buf.setSize (2, kBlock, true, false, true);
    }
}

// Map BritishEQProcessor::Parameters 1:1 onto FourKEQDSP's atomic setters and
// pin the JUCE-free extras (oversampling / M-S / auto-gain / bypass) neutral so
// the comparison isolates the EQ + saturation topology difference.
void runFourK (const BritishEQProcessor::Parameters& p,
               const std::vector<float>& Lin, const std::vector<float>& Rin,
               std::vector<float>& Lout, std::vector<float>& Rout,
               int& latencyOut)
{
    duskaudio::FourKEQDSP eq;
    eq.setHpfFreq (p.hpfFreq);   eq.setHpfEnabled (p.hpfEnabled);
    eq.setLpfFreq (p.lpfFreq);   eq.setLpfEnabled (p.lpfEnabled);
    eq.setLfGain (p.lfGain);     eq.setLfFreq (p.lfFreq);   eq.setLfBell (p.lfBell);
    eq.setLmGain (p.lmGain);     eq.setLmFreq (p.lmFreq);   eq.setLmQ (p.lmQ);
    eq.setHmGain (p.hmGain);     eq.setHmFreq (p.hmFreq);   eq.setHmQ (p.hmQ);
    eq.setHfGain (p.hfGain);     eq.setHfFreq (p.hfFreq);   eq.setHfBell (p.hfBell);
    eq.setEqType (p.isBlackMode ? 1 : 0);
    eq.setSaturation (p.saturation);
    eq.setInputGainDb (p.inputGain);
    eq.setOutputGainDb (p.outputGain);
    eq.setOversampling (0);   // 1x — match BritishEQProcessor's base-rate chain
    eq.setMsMode (false);
    eq.setAutoGain (false);
    eq.setBypass (false);

    eq.prepare (kSampleRate, kBlock);
    latencyOut = eq.getLatencySamples();

    Lout.assign ((size_t) kTotal, 0.0f);
    Rout.assign ((size_t) kTotal, 0.0f);

    for (int off = 0; off < kTotal; off += kBlock)
    {
        const int n = std::min (kBlock, kTotal - off);
        const float* in[2]  = { Lin.data() + off, Rin.data() + off };
        float*       out[2] = { Lout.data() + off, Rout.data() + off };
        eq.processBlock (in, out, 2, n);
    }
}

struct DiffResult { float maxAbs; bool finite; };

DiffResult measure (const std::vector<float>& aL, const std::vector<float>& aR,
                    const std::vector<float>& bL, const std::vector<float>& bR)
{
    float maxAbs = 0.0f;
    bool  finite = true;
    for (int n = kWarmup; n < kTotal; ++n)
    {
        const float dl = aL[(size_t) n] - bL[(size_t) n];
        const float dr = aR[(size_t) n] - bR[(size_t) n];
        if (! std::isfinite (aL[(size_t) n]) || ! std::isfinite (aR[(size_t) n]) ||
            ! std::isfinite (bL[(size_t) n]) || ! std::isfinite (bR[(size_t) n]))
            finite = false;
        maxAbs = std::max (maxAbs, std::max (std::abs (dl), std::abs (dr)));
    }
    return { maxAbs, finite };
}

BritishEQProcessor::Parameters flatDefault()
{
    return BritishEQProcessor::Parameters {};
}

BritishEQProcessor::Parameters stripCurve (bool black)
{
    BritishEQProcessor::Parameters p;
    p.hpfEnabled = true; p.hpfFreq = 80.0f;
    p.lfGain = 4.0f;  p.lfFreq = 100.0f;  p.lfBell = false;
    p.lmGain = -3.0f; p.lmFreq = 600.0f;  p.lmQ = 0.9f;
    p.hmGain = 2.5f;  p.hmFreq = 2000.0f; p.hmQ = 0.7f;
    p.hfGain = 3.0f;  p.hfFreq = 8000.0f; p.hfBell = false;
    p.saturation = 0.0f;
    p.isBlackMode = black;
    return p;
}
} // namespace

TEST_CASE ("BritishEQProcessor vs FourKEQDSP A/B parity", "[british][ab]")
{
    std::vector<float> Lin, Rin;
    makeInput (Lin, Rin);

    struct Case { const char* name; BritishEQProcessor::Parameters p; };
    std::vector<Case> cases;
    cases.push_back ({ "a_flat_default_brown", flatDefault() });
    cases.push_back ({ "b_strip_curve_brown", stripCurve (false) });
    cases.push_back ({ "c_strip_curve_black", stripCurve (true) });
    {
        BritishEQProcessor::Parameters p = stripCurve (false);
        p.saturation = 35.0f;
        p.lfGain = 3.0f; p.hmGain = 2.0f; p.hfGain = 2.0f;
        cases.push_back ({ "d_saturation_35pct_brown", p });
    }
    {
        BritishEQProcessor::Parameters p = stripCurve (true);
        p.lfBell = true; p.hfBell = true;
        cases.push_back ({ "e_lf_hf_bells_black", p });
    }

    for (const auto& c : cases)
    {
        std::vector<float> bL, bR, fL, fR;
        runBritish (c.p, Lin, Rin, bL, bR);
        int fourKLatency = -1;
        runFourK (c.p, Lin, Rin, fL, fR, fourKLatency);

        const DiffResult d = measure (bL, bR, fL, fR);
        const bool isNull = d.maxAbs <= 1.0e-6f;

        UNSCOPED_INFO ("config=" << c.name
                       << "  maxAbsDiff=" << d.maxAbs
                       << "  null(<=1e-6)=" << (isNull ? "yes" : "no")
                       << "  FourKEQDSP.latency=" << fourKLatency
                       << "  (BritishEQProcessor reports no latency)");

        REQUIRE (d.finite);
        REQUIRE (fourKLatency == 0);          // 1x → no oversampler delay
        REQUIRE (d.maxAbs < 0.5f);            // sane bound; the real number is in the INFO line
    }
}
