#include <catch2/catch_test_macros.hpp>

#include <TubeEQProcessor.h>

#include <core/MultiQTube.hpp>

#include <cmath>
#include <random>
#include <vector>

// MultiQTube.hpp claims to be a verbatim, framework-free transcription of
// TubeEQProcessor.h (same magic numbers, same sample-level op order). MasterBus
// swaps the JUCE TubeEQProcessor for the core, so this pins the swap as null:
// identical parameter surface driven through both, measured against a ~1e-6
// bound. The Parameters structs are field-identical, so one templated builder
// fills both.
namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int    kBlock      = 512;
constexpr int    kBlocks     = 40;
constexpr int    kTotal      = kBlock * kBlocks;   // 20480 samples
constexpr int    kWarmup     = kBlock * 16;        // 8192 samples before measuring

// Deterministic stereo excitation: log sine sweep + periodic impulses + noise.
// L and R differ (offset phase + independent noise) so the per-channel HF
// inductor state and transformer paths are genuinely exercised.
void makeInput (std::vector<float>& L, std::vector<float>& R)
{
    L.assign ((size_t) kTotal, 0.0f);
    R.assign ((size_t) kTotal, 0.0f);

    std::mt19937 rng (0x54554245u);   // "TUBE"
    std::uniform_real_distribution<float> noise (-1.0f, 1.0f);

    const double f0 = 50.0, f1 = 15000.0;
    const double logRatio = std::log (f1 / f0);

    double phaseL = 0.0, phaseR = 1.3;
    for (int n = 0; n < kTotal; ++n)
    {
        const double t = (double) n / (double) kTotal;
        const double freq = f0 * std::exp (logRatio * t);
        const double dphase = 2.0 * M_PI * freq / kSampleRate;
        phaseL += dphase;
        phaseR += dphase;

        float l = 0.30f * (float) std::sin (phaseL) + 0.10f * noise (rng);
        float r = 0.30f * (float) std::sin (phaseR) + 0.10f * noise (rng);
        if ((n % 4096) == 0) { l += 0.5f; r -= 0.5f; }

        L[(size_t) n] = l;
        R[(size_t) n] = r;
    }
}

// The two Parameters structs share every field name, so one template fills both.
template <typename P>
P makeParams (float lfBoost, float lfBoostFreq, float lfAtten,
              float hfBoost, float hfBoostFreq, float hfBw,
              float hfAtten, float hfAttenFreq,
              bool midEnabled, float midLowPeak, float midDip, float midHighPeak,
              float outGain, float tubeDrive)
{
    P p {};
    p.lfBoostGain      = lfBoost;
    p.lfBoostFreq      = lfBoostFreq;
    p.lfAttenGain      = lfAtten;
    p.hfBoostGain      = hfBoost;
    p.hfBoostFreq      = hfBoostFreq;
    p.hfBoostBandwidth = hfBw;
    p.hfAttenGain      = hfAtten;
    p.hfAttenFreq      = hfAttenFreq;
    p.midEnabled       = midEnabled;
    p.midLowFreq  = 500.0f;  p.midLowPeak  = midLowPeak;
    p.midDipFreq  = 700.0f;  p.midDip      = midDip;
    p.midHighFreq = 3000.0f; p.midHighPeak = midHighPeak;
    p.inputGain   = 0.0f;
    p.outputGain  = outGain;
    p.tubeDrive   = tubeDrive;
    p.bypass      = false;
    return p;
}

void runTube (const TubeEQProcessor::Parameters& p,
              const std::vector<float>& Lin, const std::vector<float>& Rin,
              std::vector<float>& Lout, std::vector<float>& Rout)
{
    TubeEQProcessor eq;
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

void runMultiQ (const duskaudio::MultiQTube::Parameters& p,
                const std::vector<float>& Lin, const std::vector<float>& Rin,
                std::vector<float>& Lout, std::vector<float>& Rout,
                int& latencyOut)
{
    duskaudio::MultiQTube eq;
    eq.prepare (kSampleRate, kBlock, 2);   // no setOversampling → 1x, matching
                                            // MasterBus (its own wrap does OS)
    eq.setParameters (p);
    latencyOut = eq.getLatencySamples();

    Lout.assign ((size_t) kTotal, 0.0f);
    Rout.assign ((size_t) kTotal, 0.0f);

    for (int off = 0; off < kTotal; off += kBlock)
    {
        const int n = std::min (kBlock, kTotal - off);
        float* ch[2] = { Lout.data() + off, Rout.data() + off };
        for (int i = 0; i < n; ++i)
        {
            ch[0][i] = Lin[(size_t) (off + i)];
            ch[1][i] = Rin[(size_t) (off + i)];
        }
        eq.process (ch, 2, n);
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
} // namespace

TEST_CASE ("TubeEQProcessor vs MultiQTube A/B parity", "[tube][ab]")
{
    std::vector<float> Lin, Rin;
    makeInput (Lin, Rin);

    struct Case { const char* name;
                  float lfBoost, lfBoostFreq, lfAtten,
                        hfBoost, hfBoostFreq, hfBw, hfAtten, hfAttenFreq;
                  bool  midEnabled; float midLowPeak, midDip, midHighPeak,
                        outGain, tubeDrive; };

    std::vector<Case> cases = {
        // name                     lfB  lfF  lfA  hfB  hfF   bw  hfA  hfAf  midEn low dip high out  drive
        { "a_flat_default",         0,   60,  0,   0, 8000, 0.5f, 0, 10000, false, 0,  0,  0,   0.0f, 0.0f },
        { "b_masterbus_tube_02",    0,   60,  0,   0, 8000, 0.5f, 0, 10000, false, 0,  0,  0,   0.0f, 0.02f },
        { "c_lf_boost_atten",     6.0f, 100, 4.0f, 0, 8000, 0.5f, 0, 10000, false, 0,  0,  0,   0.0f, 0.02f },
        { "d_hf_boost_atten",       0,   60,  0, 5.0f,5000,0.3f,3.0f,10000, false, 0,  0,  0,   0.0f, 0.02f },
        { "e_mid_section",          0,   60,  0,   0, 8000, 0.5f, 0, 10000, true,  4.0f,3.0f,2.0f,0.0f,0.02f },
        { "f_full_curve",         4.0f, 100, 2.0f,4.0f,5000,0.4f,2.0f,10000, true, 3.0f,2.0f,3.0f, 1.5f,0.05f },
    };

    for (const auto& c : cases)
    {
        const auto tp = makeParams<TubeEQProcessor::Parameters> (
            c.lfBoost, c.lfBoostFreq, c.lfAtten, c.hfBoost, c.hfBoostFreq, c.hfBw,
            c.hfAtten, c.hfAttenFreq, c.midEnabled, c.midLowPeak, c.midDip,
            c.midHighPeak, c.outGain, c.tubeDrive);
        const auto mp = makeParams<duskaudio::MultiQTube::Parameters> (
            c.lfBoost, c.lfBoostFreq, c.lfAtten, c.hfBoost, c.hfBoostFreq, c.hfBw,
            c.hfAtten, c.hfAttenFreq, c.midEnabled, c.midLowPeak, c.midDip,
            c.midHighPeak, c.outGain, c.tubeDrive);

        std::vector<float> tL, tR, mL, mR;
        runTube (tp, Lin, Rin, tL, tR);
        int multiQLatency = -1;
        runMultiQ (mp, Lin, Rin, mL, mR, multiQLatency);

        const DiffResult d = measure (tL, tR, mL, mR);
        const bool isNull = d.maxAbs <= 1.0e-6f;

        UNSCOPED_INFO ("config=" << c.name
                       << "  maxAbsDiff=" << d.maxAbs
                       << "  null(<=1e-6)=" << (isNull ? "yes" : "no")
                       << "  MultiQTube.latency=" << multiQLatency);

        REQUIRE (d.finite);
        REQUIRE (multiQLatency == 0);   // 1x → no oversampler delay, matching TubeEQProcessor
        REQUIRE (isNull);               // verbatim transcription → bit-exact within 1e-6
    }
}
