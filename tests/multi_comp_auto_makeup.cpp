#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/MultiCompStage.h"
#include "session/CompParams.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using duskstudio::CompBandField;
using duskstudio::CompParamId;
using duskstudio::CompParams;
using duskstudio::CompStage;
using duskstudio::MultiCompStage;

namespace
{
constexpr double kSr    = 48000.0;
constexpr int    kBlock = 256;

// A steady programme: 24 equal tones, log-spaced 40 Hz to 12 kHz (equal
// energy per octave, so every Multiband band has some), random fixed phases,
// each channel its own phases, scaled to unit RMS per channel.
struct Programme
{
    std::vector<float> L, R;
};

const Programme& programme (int samples)
{
    static Programme p;
    if ((int) p.L.size() >= samples) return p;
    static constexpr int kTones = 24;
    std::uint32_t seed = 0x2545f491u;
    const auto nextPhase = [&seed]
    {
        seed = seed * 1664525u + 1013904223u;
        return 6.283185307179586 * (double) (seed >> 8) / 16777216.0;
    };
    std::array<double, kTones> hz {}, phaseL {}, phaseR {};
    for (int k = 0; k < kTones; ++k)
    {
        hz[(std::size_t) k] = 40.0 * std::pow (12000.0 / 40.0, (double) k / (kTones - 1));
        phaseL[(std::size_t) k] = nextPhase();
        phaseR[(std::size_t) k] = nextPhase();
    }
    p.L.assign ((std::size_t) samples, 0.0f);
    p.R.assign ((std::size_t) samples, 0.0f);
    const double amp = std::sqrt (2.0 / kTones);
    for (int k = 0; k < kTones; ++k)
    {
        const double w = 6.283185307179586 * hz[(std::size_t) k] / kSr;
        for (int i = 0; i < samples; ++i)
        {
            p.L[(std::size_t) i] += (float) (amp * std::sin (w * i + phaseL[(std::size_t) k]));
            p.R[(std::size_t) i] += (float) (amp * std::sin (w * i + phaseR[(std::size_t) k]));
        }
    }
    return p;
}

double rmsDb (const std::vector<float>& L, const std::vector<float>& R, int from, int to)
{
    double sum = 0.0;
    for (int i = from; i < to; ++i)
        sum += (double) L[(std::size_t) i] * L[(std::size_t) i] + (double) R[(std::size_t) i] * R[(std::size_t) i];
    return 10.0 * std::log10 (std::max (sum / (2.0 * std::max (1, to - from)), 1.0e-24));
}

struct Kind
{
    const char* name;
    CompStage stage;
    int mode;
    // The control swept for the level loss, from no reduction to the most.
    float from, to;
    // The control that takes about 20, 24 and 30 dB off the programme at
    // -6 dBFS RMS with Auto Makeup off (the level table below, at donor
    // 713c97b1; 20 is interpolated between its 18 and 24 dB rows).
    float at20, at24, at30;
};

const Kind kKinds[] {
    { "Opto",      CompStage::Strip,     0,   0.0f, 100.0f,  54.20f,  65.25f,  75.04f },  // opto_peak_reduction
    { "FET",       CompStage::Strip,     1,   0.0f, -60.0f,  -7.39f, -11.77f, -18.10f },  // fet_threshold, 20:1
    { "VCA",       CompStage::Strip,     2,   0.0f, -55.0f, -23.52f, -28.04f, -34.79f },  // vca_threshold, compression 80
    { "Bus",       CompStage::Bus,       0,  15.0f, -30.0f,   2.50f,  -1.02f,  -6.17f },  // bus_threshold, 10:1
    { "Multiband", CompStage::Multiband, 0,   0.0f, -60.0f, -35.66f, -40.46f, -47.45f },  // every band's threshold, 20:1
};

void setControl (CompParams& comp, const Kind& k, float control, bool autoMakeup)
{
    comp.set (CompParamId::AutoMakeup, autoMakeup ? 1.0f : 0.0f);
    switch (k.stage)
    {
        case CompStage::Strip:
            if (k.mode == 0) comp.set (CompParamId::OptoPeakReduction, control);
            if (k.mode == 1) { comp.set (CompParamId::FetRatio, 3.0f); comp.set (CompParamId::FetThreshold, control); }
            if (k.mode == 2) { comp.set (CompParamId::VcaCompression, 80.0f); comp.set (CompParamId::VcaThreshold, control); }
            break;
        case CompStage::Bus:
            comp.set (CompParamId::BusRatio, 2.0f);
            comp.set (CompParamId::BusThreshold, control);
            break;
        case CompStage::Multiband:
            for (int band = 0; band < duskstudio::kCompBandCount; ++band)
            {
                comp.set (duskstudio::compBandParam (band, CompBandField::Ratio), 1.0f);
                comp.set (duskstudio::compBandParam (band, CompBandField::Threshold), control);
            }
            break;
    }
}

struct Segment
{
    double seconds;
    double levelDb;
    bool autoMakeup;
    // MultiCompStage::reset() before the segment.
    bool resetFirst = false;
};

struct Rendered
{
    std::vector<float> inL, inR, L, R;
    std::vector<int> starts;   // each segment's first sample, then the end
};

// The programme in segments, each at its own level and Auto Makeup setting,
// through one stereo stage in kBlock blocks; a setting changes, and a reset
// happens, at its segment's first sample.
Rendered renderSegments (const Kind& k, float control, const std::vector<Segment>& segments)
{
    Rendered r;
    int total = 0;
    for (const auto& s : segments)
    {
        r.starts.push_back (total);
        total += (int) (s.seconds * kSr);
    }
    r.starts.push_back (total);
    const auto& src = programme (total);
    r.inL.resize ((std::size_t) total);
    r.inR.resize ((std::size_t) total);
    for (std::size_t s = 0; s < segments.size(); ++s)
    {
        const float gain = (float) std::pow (10.0, segments[s].levelDb / 20.0);
        for (int i = r.starts[s]; i < r.starts[s + 1]; ++i)
        {
            r.inL[(std::size_t) i] = gain * src.L[(std::size_t) i];
            r.inR[(std::size_t) i] = gain * src.R[(std::size_t) i];
        }
    }
    r.L = r.inL;
    r.R = r.inR;

    MultiCompStage stage (k.stage);
    stage.prepare (kSr, kBlock, 2);
    CompParams comp;
    for (std::size_t s = 0; s < segments.size(); ++s)
    {
        if (segments[s].resetFirst) stage.reset();
        setControl (comp, k, control, segments[s].autoMakeup);
        stage.pushParameters (comp, true, k.mode);
        for (int offset = r.starts[s]; offset < r.starts[s + 1]; offset += kBlock)
            stage.process (r.L.data() + offset, r.R.data() + offset, std::min (kBlock, r.starts[s + 1] - offset));
    }
    return r;
}

// Auto Makeup on from the start, which it reaches within a second, so its
// last half second of 1.75 shows the match; then off, and from 0.45 s on,
// when the makeup has glided out, the compressor's own level.
struct HeavyRun
{
    double inDb, offDb, onDb;
};

HeavyRun heavyRun (const Kind& k, float control)
{
    const auto r = renderSegments (k, control, { { 1.75, -6.0, true }, { 0.75, -6.0, false } });
    const int off = r.starts[1], end = r.starts.back();
    return { rmsDb (r.inL, r.inR, 0, end), rmsDb (r.L, r.R, off + (int) (0.45 * kSr), end),
             rmsDb (r.L, r.R, off - (int) (0.5 * kSr), off) };
}

double peakDb (const std::vector<float>& L, const std::vector<float>& R, int from, int to)
{
    float peak = 0.0f;
    for (int i = from; i < to; ++i)
        peak = std::max ({ peak, std::abs (L[(std::size_t) i]), std::abs (R[(std::size_t) i]) });
    return 20.0 * std::log10 (std::max (peak, 1.0e-30f));
}

// The loudest `window`-second RMS in [from, to), stepping by a quarter window.
double loudestWindowDb (const std::vector<float>& L, const std::vector<float>& R, int from, int to, double window)
{
    const int n = (int) (window * kSr);
    double loudest = -300.0;
    for (int i = from; i + n <= to; i += n / 4)
        loudest = std::max (loudest, rmsDb (L, R, i, i + n));
    return loudest;
}

bool allFinite (const std::vector<float>& L, const std::vector<float>& R)
{
    for (std::size_t i = 0; i < L.size(); ++i)
        if (! std::isfinite (L[i]) || ! std::isfinite (R[i])) return false;
    return true;
}

struct Render
{
    double inDb;
    std::vector<double> outDb;   // one per window asked for
};

// `seconds` of the programme at `levelDb` RMS through a fresh stage, in
// kBlock blocks; output RMS over each [from, to) second window.
Render render (const Kind& k, float control, bool autoMakeup, double levelDb, double seconds,
               std::initializer_list<std::pair<double, double>> windows)
{
    const auto r = renderSegments (k, control, { { seconds, levelDb, autoMakeup } });
    Render out;
    out.inDb = rmsDb (r.inL, r.inR, 0, r.starts.back());
    for (const auto& w : windows)
        out.outDb.push_back (rmsDb (r.L, r.R, (int) (w.first * kSr), (int) (w.second * kSr)));
    return out;
}
} // namespace

TEST_CASE ("MultiCompStage: Auto Makeup gives back 20 dB of gain reduction in every kind", "[compressor][multicomp]")
{
    for (const auto& k : kKinds)
    {
        CAPTURE (k.name);
        const auto run = heavyRun (k, k.at20);
        CAPTURE (run.inDb, run.offDb, run.onDb);
        CHECK (run.inDb - run.offDb > 18.0);
        CHECK (run.inDb - run.offDb < 22.0);
        CHECK_THAT (run.onDb, WithinAbs (run.inDb, 0.5));
    }
}

TEST_CASE ("MultiCompStage: past 24 dB of gain reduction Auto Makeup gives back 24 dB", "[compressor][multicomp]")
{
    for (const auto& k : kKinds)
    {
        CAPTURE (k.name);
        const auto run = heavyRun (k, k.at30);
        CAPTURE (run.inDb, run.offDb, run.onDb);
        CHECK (run.inDb - run.offDb > 27.0);
        // The limit MANUAL.md states.
        CHECK_THAT (run.onDb - run.offDb, WithinAbs (24.0, 0.25));
        CHECK (run.onDb < run.inDb);
    }
}

TEST_CASE ("MultiCompStage: a second after a reset Auto Makeup is back within 1 dB in every kind", "[compressor][multicomp]")
{
    // The stage resets on every transport start of a strip without live
    // input, and a bounce prepares it fresh: the makeup has to be back at
    // once, not ten seconds later.
    for (const auto& k : kKinds)
    {
        CAPTURE (k.name);
        const auto r = renderSegments (k, k.at20, { { 0.5, -6.0, true }, { 1.25, -6.0, true, true } });
        const int from = r.starts[1] + (int) (0.9 * kSr), to = r.starts[1] + (int) (1.1 * kSr);
        const double inDb = rmsDb (r.inL, r.inR, from, to), outDb = rmsDb (r.L, r.R, from, to);
        CAPTURE (inDb, outDb);
        CHECK_THAT (outDb, WithinAbs (inDb, 1.0));
    }
}

TEST_CASE ("MultiCompStage auto makeup: what follows heavy reduction", "[.][measure][multicomp]")
{
    std::printf ("(a) loud -6 dBFS at about 24 dB of loss, made up (1 s off, 11 s on), then -40 dBFS for 24 s\n");
    std::printf ("%-10s %8s %8s %9s %9s %8s %8s %8s %8s %8s %9s %9s\n", "kind", "loudIn", "loudOut", "quietMax",
                 "q0-0.5s", "q6s", "q10s", "q14s", "q20s", "q23.5s", "pkQuiet", "pkLoudIn");
    for (const auto& k : kKinds)
    {
        const auto r = renderSegments (k, k.at24, { { 1.0, -6.0, false }, { 11.0, -6.0, true }, { 24.0, -40.0, true } });
        const int q = r.starts[2], end = r.starts[3];
        const auto at = [&] (double s) { return q + (int) (s * kSr); };
        std::printf ("%-10s %8.2f %8.2f %9.2f %9.2f %8.2f %8.2f %8.2f %8.2f %8.2f %9.2f %9.2f%s\n", k.name,
                     rmsDb (r.inL, r.inR, r.starts[1], q), rmsDb (r.L, r.R, at (-1.0), q),
                     loudestWindowDb (r.L, r.R, q, end, 0.05), rmsDb (r.L, r.R, q, at (0.5)),
                     rmsDb (r.L, r.R, at (5.75), at (6.25)), rmsDb (r.L, r.R, at (9.75), at (10.25)),
                     rmsDb (r.L, r.R, at (13.75), at (14.25)), rmsDb (r.L, r.R, at (19.75), at (20.25)),
                     rmsDb (r.L, r.R, at (23.5), end), peakDb (r.L, r.R, q, end),
                     peakDb (r.inL, r.inR, r.starts[1], q), allFinite (r.L, r.R) ? "" : "  NON-FINITE");
    }

    std::printf ("(b) the same loud passage, then -110 dBFS (under the matcher's floor) for 6 s; and -95 dBFS from cold\n");
    std::printf ("%-10s %9s %9s %9s %9s %9s\n", "kind", "-110 1-2s", "-110 5-6s", "-95 in", "-95 1-2s", "-95 5-6s");
    for (const auto& k : kKinds)
    {
        const auto r = renderSegments (k, k.at24, { { 1.0, -6.0, false }, { 11.0, -6.0, true }, { 6.0, -110.0, true } });
        const int q = r.starts[2];
        const auto cold = renderSegments (k, k.at24, { { 6.0, -95.0, true } });
        std::printf ("%-10s %9.2f %9.2f %9.2f %9.2f %9.2f%s\n", k.name,
                     rmsDb (r.L, r.R, q + (int) kSr, q + 2 * (int) kSr), rmsDb (r.L, r.R, q + 5 * (int) kSr, q + 6 * (int) kSr),
                     rmsDb (cold.inL, cold.inR, 0, 6 * (int) kSr),
                     rmsDb (cold.L, cold.R, (int) kSr, 2 * (int) kSr), rmsDb (cold.L, cold.R, 5 * (int) kSr, 6 * (int) kSr),
                     allFinite (r.L, r.R) && allFinite (cold.L, cold.R) ? "" : "  NON-FINITE");
    }

    std::printf ("(c) -6 dBFS at about 24 dB of loss: Auto Makeup on at 1 s, off at 6 s, on at 9 s\n");
    std::printf ("%-10s %8s %8s %8s %11s %11s %11s\n", "kind", "on", "off", "on again", "max step", "step@off", "step@on");
    for (const auto& k : kKinds)
    {
        const auto r = renderSegments (k, k.at24,
                                       { { 1.0, -6.0, false }, { 5.0, -6.0, true }, { 3.0, -6.0, false }, { 5.0, -6.0, true } });
        // The largest sample-to-sample move of the output, steady and at each
        // switch, as a share of the steady output's own largest move.
        const auto maxStep = [&r] (int from, int to)
        {
            float step = 0.0f;
            for (int i = std::max (1, from); i < to; ++i)
                step = std::max ({ step, std::abs (r.L[(std::size_t) i] - r.L[(std::size_t) i - 1]),
                                   std::abs (r.R[(std::size_t) i] - r.R[(std::size_t) i - 1]) });
            return step;
        };
        const int win = (int) (0.2 * kSr);
        const float steady = maxStep (r.starts[2] - (int) kSr, r.starts[2]);
        std::printf ("%-10s %8.2f %8.2f %8.2f %11.4f %11.4f %11.4f\n", k.name,
                     rmsDb (r.L, r.R, r.starts[2] - (int) kSr, r.starts[2]),
                     rmsDb (r.L, r.R, r.starts[3] - (int) kSr, r.starts[3]),
                     rmsDb (r.L, r.R, r.starts[4] - (int) kSr, r.starts[4]), steady,
                     maxStep (r.starts[2] - win, r.starts[2] + win) / steady,
                     maxStep (r.starts[3] - win, r.starts[3] + win) / steady);
    }
}

TEST_CASE ("MultiCompStage auto makeup: level table by kind and loss", "[.][measure][multicomp]")
{
    static constexpr int kGrid = 21;
    const double levels[] { -18.0, -6.0 };
    const double targets[] { 6.0, 12.0, 18.0, 24.0, 30.0 };
    std::printf ("%-10s %6s %7s %8s %8s %9s %9s %9s %9s\n",
                 "kind", "level", "control", "in dB", "off dB", "loss dB", "on@10s", "on@20s", "short dB");
    for (const auto& k : kKinds)
        for (const double level : levels)
        {
            // The level loss with auto makeup off across the control's travel.
            std::array<double, kGrid> control {}, loss {};
            for (int g = 0; g < kGrid; ++g)
            {
                control[(std::size_t) g] = k.from + (k.to - k.from) * g / (kGrid - 1);
                const auto r = render (k, (float) control[(std::size_t) g], false, level, 4.0, { { 3.0, 4.0 } });
                loss[(std::size_t) g] = r.inDb - r.outDb[0];
            }
            for (const double target : targets)
            {
                int g = 0;
                while (g + 1 < kGrid && loss[(std::size_t) (g + 1)] < target) ++g;
                if (g + 1 >= kGrid)
                {
                    std::printf ("%-10s %6.0f   target %4.0f dB out of reach (most %.1f dB)\n",
                                 k.name, level, target, loss[(std::size_t) (kGrid - 1)]);
                    continue;
                }
                const double span = loss[(std::size_t) (g + 1)] - loss[(std::size_t) g];
                const double t = span > 1.0e-9 ? (target - loss[(std::size_t) g]) / span : 0.0;
                const float c = (float) (control[(std::size_t) g] + std::clamp (t, 0.0, 1.0)
                                         * (control[(std::size_t) (g + 1)] - control[(std::size_t) g]));
                const auto off = render (k, c, false, level, 6.0, { { 4.0, 6.0 } });
                const auto on  = render (k, c, true,  level, 20.0, { { 8.0, 10.0 }, { 18.0, 20.0 } });
                std::printf ("%-10s %6.0f %7.2f %8.2f %8.2f %9.2f %9.2f %9.2f %9.2f\n",
                             k.name, level, c, off.inDb, off.outDb[0], off.inDb - off.outDb[0],
                             on.outDb[0], on.outDb[1], on.inDb - on.outDb[1]);
            }
        }
}
