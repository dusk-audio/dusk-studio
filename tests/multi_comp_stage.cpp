#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/MultiCompStage.h"
#include "engine/builtin/DafPlugin.h"
#include "session/CompParams.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;
using duskstudio::CompParamId;
using duskstudio::CompParams;
using duskstudio::CompStage;
using duskstudio::MultiCompStage;

namespace
{
constexpr double kSr    = 48000.0;
constexpr int    kBlock = 256;
constexpr double kTwoPi = 6.283185307179586;

int indexOf (MultiCompStage& stage, const std::string& symbol)
{
    const auto& descs = stage.plugin().params();
    for (std::size_t i = 0; i < descs.size(); ++i)
        if (descs[i].symbol == symbol)
            return (int) i;
    return -1;
}

std::vector<float> sine (double hz, float amp, int n, double sr = kSr)
{
    std::vector<float> v ((std::size_t) n);
    for (int i = 0; i < n; ++i)
        v[(std::size_t) i] = amp * (float) std::sin (kTwoPi * hz * (double) i / sr);
    return v;
}

// Runs L (and R, when given) through the stage in place, `block` samples a call.
void run (MultiCompStage& stage, std::vector<float>& L, std::vector<float>* R, int block = kBlock)
{
    for (std::size_t offset = 0; offset < L.size(); offset += (std::size_t) block)
    {
        const int n = (int) std::min<std::size_t> ((std::size_t) block, L.size() - offset);
        stage.process (L.data() + offset, R != nullptr ? R->data() + offset : nullptr, n);
    }
}

double rmsDb (const std::vector<float>& v, std::size_t from, std::size_t to)
{
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i)
        sum += (double) v[i] * (double) v[i];
    const double r = std::sqrt (sum / (double) std::max<std::size_t> (1, to - from));
    return 20.0 * std::log10 (std::max (r, 1.0e-12));
}

struct KindCase
{
    CompStage kind;
    int channels;
    const char* name;
};

const KindCase kKinds[] {
    { CompStage::Strip,     1, "strip, mono" },
    { CompStage::Strip,     2, "strip, stereo" },
    { CompStage::Bus,       2, "bus" },
    { CompStage::Multiband, 2, "multiband" },
};
} // namespace

TEST_CASE ("MultiCompStage: every parameter a kind stores resolves to the plug-in's own", "[compressor][multicomp]")
{
    for (const auto kind : { CompStage::Strip, CompStage::Bus, CompStage::Multiband })
    {
        CAPTURE ((int) kind);
        MultiCompStage stage (kind);
        REQUIRE (stage.unresolvedSymbol() == nullptr);
        REQUIRE (stage.kind() == kind);

        // Each stored value reads back through the plug-in index of its symbol.
        CompParams comp;
        std::atomic<bool> engaged { true };
        for (const auto id : duskstudio::compStageParams (kind))
        {
            const auto& spec = duskstudio::compParamSpec (id);
            CAPTURE (spec.symbol);
            const int index = indexOf (stage, spec.symbol);
            REQUIRE (index >= 0);
            comp.set (id, spec.hostMax);
            CHECK_THAT (stage.sessionValue (comp, engaged.load(), 0, index), WithinAbs (comp.get (id), 0.0f));
            comp.set (id, spec.hostMin);
            CHECK_THAT (stage.sessionValue (comp, engaged.load(), 0, index), WithinAbs (comp.get (id), 0.0f));
        }
        for (const char* symbol : { "mode", "daf_bypass", "gr_meter" })
            CHECK (indexOf (stage, symbol) >= 0);
    }
}

TEST_CASE ("MultiCompStage: a bypassed stage is the input delayed by its reported latency", "[compressor][multicomp]")
{
    for (const double sr : { 44100.0, 48000.0, 96000.0 })
        for (const auto& k : kKinds)
        {
            CAPTURE (sr, k.name);
            MultiCompStage stage (k.kind);
            stage.prepare (sr, kBlock, k.channels);
            const int latency = stage.latencySamples();
            REQUIRE (latency > 0);

            CompParams comp;
            stage.pushParameters (comp, false, 0);

            // Past the 30 ms bypass crossfade, then an impulse, then a tone.
            const int settle = (int) (0.1 * sr);
            const int impulseAt = settle + 17;
            const int toneFrom = impulseAt + 4 * latency;
            const int total = toneFrom + 8 * kBlock;
            std::vector<float> in ((std::size_t) total, 0.0f);
            in[(std::size_t) impulseAt] = 0.5f;
            const auto tone = sine (997.0, 0.25f, total - toneFrom, sr);
            std::copy (tone.begin(), tone.end(), in.begin() + toneFrom);

            std::vector<float> L = in, R = in;
            run (stage, L, k.channels == 2 ? &R : nullptr);

            int peakAt = 0;
            for (int i = settle; i < toneFrom; ++i)
                if (std::abs (L[(std::size_t) i]) > std::abs (L[(std::size_t) peakAt]))
                    peakAt = i;
            CHECK (peakAt - impulseAt == latency);

            float worst = 0.0f;
            for (int i = settle + latency; i < total; ++i)
            {
                worst = std::max (worst, std::abs (L[(std::size_t) i] - in[(std::size_t) (i - latency)]));
                if (k.channels == 2)
                    worst = std::max (worst, std::abs (R[(std::size_t) i] - in[(std::size_t) (i - latency)]));
            }
            CHECK (worst < 1.0e-6f);
        }
}

TEST_CASE ("MultiCompStage: the delay is 73 samples at 44.1 and 48 kHz and 79 at 96 kHz", "[compressor][multicomp]")
{
    struct RateCase { double sr; int latency; };
    for (const auto rate : { RateCase { 44100.0, 73 }, RateCase { 48000.0, 73 }, RateCase { 96000.0, 79 } })
        for (const auto& k : kKinds)
        {
            CAPTURE (rate.sr, k.name);
            MultiCompStage stage (k.kind);
            stage.prepare (rate.sr, kBlock, k.channels);
            CHECK (stage.latencySamples() == rate.latency);
        }
}

TEST_CASE ("MultiCompStage: an engaged stage reduces a hot sine and reports it", "[compressor][multicomp]")
{
    struct Case
    {
        const char* name;
        CompStage kind;
        int mode;
        CompParamId control;
        float value;
    };
    const Case cases[] {
        { "Opto", CompStage::Strip, 0, CompParamId::OptoPeakReduction, 80.0f },
        { "FET",  CompStage::Strip, 1, CompParamId::FetThreshold, -30.0f },
        { "VCA",  CompStage::Strip, 2, CompParamId::VcaThreshold, -40.0f },
        { "Bus",  CompStage::Bus,   0, CompParamId::BusThreshold, -20.0f },
    };
    for (const auto& c : cases)
    {
        CAPTURE (c.name);
        MultiCompStage stage (c.kind);
        stage.prepare (kSr, kBlock, 2);
        CompParams comp;
        comp.set (c.control, c.value);
        stage.pushParameters (comp, true, c.mode);

        const int total = (int) kSr;
        const auto in = sine (1000.0, 0.5f, total);
        std::vector<float> L = in, R = in;
        run (stage, L, &R);

        const auto from = (std::size_t) (total * 3 / 4);
        const double inDb  = rmsDb (in, from, (std::size_t) total);
        const double outDb = rmsDb (L, from, (std::size_t) total);
        const float gr = stage.gainReductionDb();
        CAPTURE (inDb, outDb, gr);
        CHECK (gr < -1.0f);
        CHECK (outDb < inDb - 1.0);
    }
}

TEST_CASE ("MultiCompStage: a Multiband stage reports each band's reduction, and only that band's", "[compressor][multicomp]")
{
    MultiCompStage stage (CompStage::Multiband);
    stage.prepare (kSr, kBlock, 2);
    CompParams comp;
    // A low tone over a threshold only the low band has.
    for (int band = 0; band < duskstudio::kCompBandCount; ++band)
        comp.set (duskstudio::compBandParam (band, duskstudio::CompBandField::Threshold), band == 0 ? -40.0f : 0.0f);
    stage.pushParameters (comp, true, 0);
    for (int band = 0; band < duskstudio::kCompBandCount; ++band)
        CHECK_THAT (stage.bandGainReductionDb (band), WithinAbs (0.0f, 0.0f));

    const int total = (int) kSr;
    auto L = sine (60.0, 0.5f, total), R = L;
    run (stage, L, &R);
    CHECK (stage.bandGainReductionDb (0) < -3.0f);
    for (int band = 1; band < duskstudio::kCompBandCount; ++band)
    {
        CAPTURE (band);
        CHECK (stage.bandGainReductionDb (band) > -0.5f);
        CHECK (stage.bandGainReductionDb (band) <= 0.0f);
    }
    CHECK_THAT (stage.bandGainReductionDb (-1), WithinAbs (0.0f, 0.0f));
    CHECK_THAT (stage.bandGainReductionDb (duskstudio::kCompBandCount), WithinAbs (0.0f, 0.0f));

    MultiCompStage bus (CompStage::Bus);
    for (int band = 0; band < duskstudio::kCompBandCount; ++band)
        CHECK_THAT (bus.bandGainReductionDb (band), WithinAbs (0.0f, 0.0f));
}

TEST_CASE ("MultiCompStage: a mono layout reads one channel and matches a stereo run's left", "[compressor][multicomp]")
{
    for (int mode = 0; mode < 3; ++mode)
    {
        CAPTURE (mode);
        MultiCompStage mono (CompStage::Strip), stereo (CompStage::Strip);
        mono.prepare (kSr, kBlock, 1);
        stereo.prepare (kSr, kBlock, 2);
        CompParams comp;
        comp.set (CompParamId::OptoPeakReduction, 70.0f);
        comp.set (CompParamId::FetThreshold, -30.0f);
        comp.set (CompParamId::VcaThreshold, -40.0f);
        mono.pushParameters (comp, true, mode);
        stereo.pushParameters (comp, true, mode);

        const int total = (int) (kSr / 2);
        auto in = sine (440.0, 0.4f, total);
        const auto overtone = sine (3100.0, 0.1f, total);
        for (std::size_t i = 0; i < in.size(); ++i)
            in[i] += overtone[i];

        std::vector<float> monoL = in;
        run (mono, monoL, nullptr);
        std::vector<float> stereoL = in, stereoR = in;
        run (stereo, stereoL, &stereoR);

        float worst = 0.0f, level = 0.0f;
        for (std::size_t i = 0; i < in.size(); ++i)
        {
            worst = std::max (worst, std::abs (monoL[i] - stereoL[i]));
            level = std::max (level, std::abs (stereoL[i]));
        }
        CAPTURE (worst, level);
        REQUIRE (level > 0.05f);
        // Two identical channels make a linked detector read what one channel
        // does, so the only difference left is the order the stereo path sums
        // its channels in: rounding, a few float steps at this level. (GCC on
        // x86-64 renders the two bit for bit.)
        CHECK (worst < 1.0e-6f);
    }
}

TEST_CASE ("MultiCompStage: pushed values reach the plug-in", "[compressor][multicomp]")
{
    for (const auto kind : { CompStage::Strip, CompStage::Bus, CompStage::Multiband })
    {
        CAPTURE ((int) kind);
        MultiCompStage stage (kind);
        stage.prepare (kSr, kBlock, 2);
        auto& plugin = stage.plugin();

        // Each value the plug-in does not start at, and one it keeps exactly:
        // the range end furthest from its default.
        CompParams comp;
        for (const auto id : duskstudio::compStageParams (kind))
        {
            const auto& spec = duskstudio::compParamSpec (id);
            const bool nearTop = spec.hostMax - spec.hostDefault < spec.hostDefault - spec.hostMin;
            comp.set (id, nearTop ? spec.hostMin : spec.hostMax);
        }
        stage.pushParameters (comp, false, 2);

        for (const auto id : duskstudio::compStageParams (kind))
        {
            const auto& spec = duskstudio::compParamSpec (id);
            CAPTURE (spec.symbol);
            CHECK_THAT (plugin.getParameterValue ((std::uint32_t) indexOf (stage, spec.symbol)), WithinAbs (comp.get (id), 0.0f));
        }
        const float mode = kind == CompStage::Strip ? 2.0f : (kind == CompStage::Bus ? 3.0f : 7.0f);
        CHECK_THAT (plugin.getParameterValue ((std::uint32_t) indexOf (stage, "mode")), WithinAbs (mode, 0.0f));
        CHECK_THAT (plugin.getParameterValue ((std::uint32_t) indexOf (stage, "daf_bypass")), WithinAbs (1.0f, 0.0f));

        // A strip's mode follows compMode, clamped to its three faces.
        if (kind == CompStage::Strip)
        {
            stage.pushParameters (comp, true, 9);
            CHECK_THAT (plugin.getParameterValue ((std::uint32_t) indexOf (stage, "mode")), WithinAbs (2.0f, 0.0f));
            stage.pushParameters (comp, true, 0);
            CHECK_THAT (plugin.getParameterValue ((std::uint32_t) indexOf (stage, "mode")), WithinAbs (0.0f, 0.0f));
            CHECK_THAT (plugin.getParameterValue ((std::uint32_t) indexOf (stage, "daf_bypass")), WithinAbs (0.0f, 0.0f));
        }
    }
}

TEST_CASE ("MultiCompStage: the plug-in keeps its own Oversampling", "[compressor][multicomp]")
{
    // Multi-Comp 2's reference parity was established at its default 2x
    // (parameter value 1), and no stage moves it.
    for (const auto kind : { CompStage::Strip, CompStage::Bus, CompStage::Multiband })
    {
        CAPTURE ((int) kind);
        MultiCompStage stage (kind);
        const int oversampling = indexOf (stage, "oversampling");
        REQUIRE (oversampling >= 0);
        stage.prepare (kSr, kBlock, 2);
        CompParams comp;
        for (const auto id : duskstudio::compStageParams (kind))
            comp.set (id, duskstudio::compParamSpec (id).hostMax);
        stage.pushParameters (comp, true, 2);
        stage.pushParameters (comp, false, 1);
        const float setting = stage.plugin().getParameterValue ((std::uint32_t) oversampling);
        CHECK_THAT (setting, WithinAbs (stage.plugin().params()[(std::size_t) oversampling].defaultValue, 0.0f));
        CHECK_THAT (setting, WithinAbs (1.0f, 0.0f));
    }
}

TEST_CASE ("MultiCompStage: a reset stage plays silence as a fresh one does", "[compressor][multicomp]")
{
    // A twin that is not reset plays its last latency's worth of the
    // compressed sine, and its filters' decay after it, when silence follows.
    // Reset, the plug-in is back at the rest its activation leaves, so
    // silence comes out as it does from a fresh instance: exactly zero in
    // FET, VCA and Bus, and in Opto the floor its modelled cell settles to,
    // under 1e-12.
    struct Case
    {
        const char* name;
        CompStage kind;
        int mode;
        int channels;
        float floor;
    };
    const Case cases[] {
        { "Opto, mono",   CompStage::Strip, 0, 1, 1.0e-12f },
        { "Opto, stereo", CompStage::Strip, 0, 2, 1.0e-12f },
        { "FET, mono",    CompStage::Strip, 1, 1, 0.0f },
        { "FET, stereo",  CompStage::Strip, 1, 2, 0.0f },
        { "VCA, mono",    CompStage::Strip, 2, 1, 0.0f },
        { "VCA, stereo",  CompStage::Strip, 2, 2, 0.0f },
        { "Bus",          CompStage::Bus,   0, 2, 0.0f },
    };
    const auto silence = [] (MultiCompStage& stage, int channels)
    {
        std::vector<float> L (4096, 0.0f), R (4096, 0.0f);
        run (stage, L, channels == 2 ? &R : nullptr, 64);
        float peak = 0.0f;
        for (std::size_t i = 0; i < L.size(); ++i)
            peak = std::max ({ peak, std::abs (L[i]), std::abs (R[i]) });
        return peak;
    };
    for (const auto& c : cases)
    {
        CAPTURE (c.name);
        MultiCompStage reset (c.kind), held (c.kind), fresh (c.kind);
        CompParams comp;
        comp.set (CompParamId::OptoPeakReduction, 60.0f);
        comp.set (CompParamId::FetThreshold, -30.0f);
        comp.set (CompParamId::VcaThreshold, -40.0f);
        comp.set (CompParamId::BusThreshold, -20.0f);
        for (auto* stage : { &reset, &held, &fresh })
        {
            stage->prepare (kSr, 64, c.channels);
            stage->pushParameters (comp, true, c.mode);
        }
        for (auto* stage : { &reset, &held })
        {
            auto L = sine (150.0, 0.7f, 4096);
            auto R = L;
            run (*stage, L, c.channels == 2 ? &R : nullptr, 64);
        }
        reset.reset();

        const float replayed = silence (held, c.channels);
        const float afterReset = silence (reset, c.channels);
        const float fromFresh = silence (fresh, c.channels);
        CAPTURE (replayed, afterReset, fromFresh);
        // The held twin replays at least 0.016 in every case here.
        CHECK (replayed > 1.0e-3f);
        CHECK (afterReset <= c.floor);
        CHECK (fromFresh <= c.floor);
    }
}

TEST_CASE ("MultiCompStage: a block over the prepared size runs as prepared-size parts", "[compressor][multicomp]")
{
    MultiCompStage whole (CompStage::Bus), parts (CompStage::Bus);
    whole.prepare (kSr, 64, 2);
    parts.prepare (kSr, 64, 2);
    CompParams comp;
    comp.set (CompParamId::BusThreshold, -20.0f);
    whole.pushParameters (comp, true, 0);
    parts.pushParameters (comp, true, 0);

    const auto in = sine (500.0, 0.5f, 64 * 40);
    std::vector<float> wholeL = in, wholeR = in, partsL = in, partsR = in;
    run (whole, wholeL, &wholeR, 64 * 5);
    run (parts, partsL, &partsR, 64);
    REQUIRE (wholeL == partsL);
    REQUIRE (wholeR == partsR);

    // An empty block changes nothing.
    std::vector<float> one { 0.25f };
    whole.process (one.data(), one.data(), 0);
    CHECK_THAT (one[0], WithinAbs (0.25f, 0.0f));
}

TEST_CASE ("MultiCompStage: the editor reads and writes the session behind each parameter", "[compressor][multicomp]")
{
    CompParams comp;
    std::atomic<bool> engaged { true };
    std::atomic<int> stripMode { 1 };

    SECTION ("strip")
    {
        MultiCompStage stage (CompStage::Strip);
        stage.prepare (kSr, kBlock, 1);
        const int mode = indexOf (stage, "mode");
        const int bypass = indexOf (stage, "daf_bypass");
        const int threshold = indexOf (stage, "vca_threshold");

        CHECK_THAT (stage.sessionValue (comp, true, stripMode.load(), mode), WithinAbs (1.0f, 0.0f));
        stage.setSessionValue (comp, engaged, &stripMode, mode, 2.0f);
        CHECK (stripMode.load() == 2);
        stage.setSessionValue (comp, engaged, &stripMode, mode, 3.0f);   // Bus is no strip face
        CHECK (stripMode.load() == 2);

        CHECK (stage.isEngageParam (bypass));
        CHECK_FALSE (stage.isEngageParam (mode));
        CHECK_THAT (stage.sessionValue (comp, true, 0, bypass), WithinAbs (0.0f, 0.0f));
        CHECK_THAT (stage.sessionValue (comp, false, 0, bypass), WithinAbs (1.0f, 0.0f));
        stage.setSessionValue (comp, engaged, &stripMode, bypass, 1.0f);
        CHECK_FALSE (engaged.load());
        stage.setSessionValue (comp, engaged, &stripMode, bypass, 0.0f);
        CHECK (engaged.load());

        // Stored values clamp to their range.
        stage.setSessionValue (comp, engaged, &stripMode, threshold, -99.0f);
        CHECK_THAT (comp.get (CompParamId::VcaThreshold), WithinAbs (-55.0f, 0.0f));
        CHECK_THAT (stage.sessionValue (comp, true, 0, threshold), WithinAbs (-55.0f, 0.0f));

        // Outside the strip's list: the bus threshold and stereo link read the
        // plug-in and keep the session as it was.
        const auto before = comp.values();
        for (const char* symbol : { "bus_threshold", "stereo_link", "gr_meter" })
        {
            CAPTURE (symbol);
            const int index = indexOf (stage, symbol);
            CHECK_THAT (stage.sessionValue (comp, true, 0, index),
                        WithinAbs (stage.plugin().getParameterValue ((std::uint32_t) index), 0.0f));
            stage.setSessionValue (comp, engaged, &stripMode, index, 1.0f);
        }
        CHECK (comp.values() == before);
    }

    SECTION ("bus")
    {
        MultiCompStage stage (CompStage::Bus);
        const int mode = indexOf (stage, "mode");
        CHECK_THAT (stage.sessionValue (comp, true, 1, mode), WithinAbs (3.0f, 0.0f));
        stage.setSessionValue (comp, engaged, &stripMode, mode, 0.0f);
        CHECK (stripMode.load() == 1);

        const int threshold = indexOf (stage, "bus_threshold");
        stage.setSessionValue (comp, engaged, nullptr, threshold, -12.5f);
        CHECK_THAT (comp.get (CompParamId::BusThreshold), WithinAbs (-12.5f, 0.0f));
    }

    SECTION ("multiband")
    {
        MultiCompStage stage (CompStage::Multiband);
        CHECK_THAT (stage.sessionValue (comp, true, 0, indexOf (stage, "mode")), WithinAbs (7.0f, 0.0f));
        const int bandBypass = indexOf (stage, "mb_bypass_2");
        stage.setSessionValue (comp, engaged, nullptr, bandBypass, 0.8f);
        CHECK_THAT (comp.get (duskstudio::compBandParam (2, duskstudio::CompBandField::Bypass)), WithinAbs (1.0f, 0.0f));
    }
}


// A control on a mixer face shows what Multi-Comp 2 itself prints for its
// value: the plug-in's own readouts (the Opto dials, the FET 1-7 timing dials,
// VCA compression as a ratio), its stepped controls' labels, and otherwise the
// value with the unit it declares, never a law of Dusk's own.
TEST_CASE ("MultiCompStage: values read as Multi-Comp 2 prints them", "[compressor][multicomp]")
{
    const MultiCompStage strip (CompStage::Strip);
    const CompParams fresh;

    CHECK (strip.valueText (CompParamId::OptoGain, fresh.get (CompParamId::OptoGain)) == "20");
    CHECK (strip.valueText (CompParamId::OptoPeakReduction, 0.0f) == "0");
    CHECK (strip.valueText (CompParamId::OptoPeakReduction, 100.0f) == "100");
    CHECK (strip.valueText (CompParamId::FetAttack, fresh.get (CompParamId::FetAttack)) == "2.0");
    CHECK (strip.valueText (CompParamId::FetAttack, 1.0f) == "7.0");
    CHECK (strip.valueText (CompParamId::FetRelease, 400.0f) == "3.0");
    CHECK (strip.valueText (CompParamId::FetRelease, 50.0f) == "1.0");
    CHECK (strip.valueText (CompParamId::FetRatio, 0.0f) == "4:1");
    CHECK (strip.valueText (CompParamId::FetRatio, 4.0f) == "All");
    CHECK (strip.valueText (CompParamId::VcaCompression, 100.0f) == "INF");
    const auto ratio = strip.valueText (CompParamId::VcaCompression, fresh.get (CompParamId::VcaCompression));
    CHECK (ratio.size() > 2);
    CHECK (ratio.substr (ratio.size() - 2) == ":1");
    CHECK (strip.valueText (CompParamId::VcaThreshold, -27.0f) == "-27.0 dB");
    CHECK (strip.valueText (CompParamId::FetOutput, -0.04f) == "0.0 dB");
    CHECK (strip.valueText (CompParamId::OptoLimit, 1.0f) == "On");
    CHECK (strip.valueText (CompParamId::BusRatio, 0.0f).empty());

    const MultiCompStage bus (CompStage::Bus);
    CHECK (bus.valueText (CompParamId::BusRatio, 2.0f) == "10:1");
    CHECK (bus.valueText (CompParamId::BusAttack, 0.0f) == "0.1ms");
    CHECK (bus.valueText (CompParamId::BusRelease, 4.0f) == "Auto");
    CHECK (bus.valueText (CompParamId::BusMakeup, 20.0f) == "20.0 dB");
    CHECK (bus.valueText (CompParamId::BusThreshold, 15.0f) == "15.0 dB");
    CHECK (bus.stepsText (CompParamId::BusRatio, " / ") == "2:1 / 4:1 / 10:1");
    CHECK (bus.stepsText (CompParamId::BusMakeup, " to ") == "0.0 dB to 20.0 dB");
    CHECK (bus.valueText (CompParamId::FetRatio, 0.0f).empty());
}

// What a user types into a readout reads as the plug-in reads it, so typing
// back a readout lands on a value that prints the same.
TEST_CASE ("MultiCompStage: typed text reads back as the plug-in reads it", "[compressor][multicomp]")
{
    const MultiCompStage strip (CompStage::Strip);
    const MultiCompStage bus (CompStage::Bus);

    for (const auto id : duskstudio::compStageParams (CompStage::Strip))
    {
        const auto& spec = duskstudio::compParamSpec (id);
        for (const float t : { 0.0f, 0.3f, 1.0f })
        {
            const float v = duskstudio::clampCompParam (id, spec.hostMin + t * (spec.hostMax - spec.hostMin));
            const auto text = strip.valueText (id, v);
            CAPTURE (spec.symbol, v, text);
            float read = std::nanf ("");
            REQUIRE (strip.valueFromText (id, text, read));
            CHECK (strip.valueText (id, read) == text);
        }
    }

    float value = 0.0f;
    CHECK (strip.valueFromText (CompParamId::FetRatio, "all", value));
    CHECK_THAT (value, WithinAbs (4.0f, 0.0f));
    CHECK (strip.valueFromText (CompParamId::FetOutput, " -12 dB ", value));
    CHECK_THAT (value, WithinAbs (-12.0f, 1e-6f));
    CHECK (strip.valueFromText (CompParamId::FetOutput, "35", value));
    CHECK_THAT (value, WithinAbs (20.0f, 0.0f));
    CHECK (strip.valueFromText (CompParamId::VcaCompression, "INF", value));
    CHECK_THAT (value, WithinAbs (100.0f, 0.0f));
    CHECK (strip.valueFromText (CompParamId::FetAttack, "4", value));
    CHECK_THAT (value, WithinAbs (0.5f, 1e-6f));

    value = -1.0f;
    CHECK_FALSE (strip.valueFromText (CompParamId::FetOutput, "12 ms", value));
    CHECK_FALSE (strip.valueFromText (CompParamId::FetAttack, "4 ms", value));
    CHECK_FALSE (strip.valueFromText (CompParamId::BusRatio, "2:1", value));
    CHECK_THAT (value, WithinAbs (-1.0f, 0.0f));

    CHECK (bus.valueFromText (CompParamId::BusRelease, "auto", value));
    CHECK_THAT (value, WithinAbs (4.0f, 0.0f));
}

namespace
{
// Every value a kind stores, moved a good way off `from`, the crossovers in
// order so the plug-in reads them back as given.
void moveEveryValue (CompStage kind, const CompParams& from, CompParams& to)
{
    to.copyFrom (from);
    for (const auto id : duskstudio::compStageParams (kind))
    {
        const auto& spec = duskstudio::compParamSpec (id);
        const float range = spec.hostMax - spec.hostMin;
        to.set (id, from.get (id) < spec.hostMin + 0.5f * range ? spec.hostMax - 0.1f * range
                                                                 : spec.hostMin + 0.1f * range);
    }
    if (kind == CompStage::Multiband)
    {
        to.set (CompParamId::MbCrossover1, 0.3f);
        to.set (CompParamId::MbCrossover2, 0.5f);
        to.set (CompParamId::MbCrossover3, 0.8f);
    }
}

std::vector<float> hotBlocks (MultiCompStage& stage, int channels, int blocks, double& phase)
{
    std::vector<float> L ((std::size_t) (blocks * kBlock)), R;
    for (auto& x : L)
    {
        x = 0.7f * (float) std::sin (phase);
        phase += kTwoPi * 220.0 / kSr;
    }
    R = L;
    run (stage, L, channels == 2 ? &R : nullptr);
    if (channels == 2) L.insert (L.end(), R.begin(), R.end());
    return L;
}
} // namespace

// The editor reads every control back from the plug-in as it opens. The seed
// an editor's host gives as it builds one hands the plug-in what a push would,
// whether or not the owner has run a block since, with BYPASS showing the
// engage switch, which the plug-in's own bypass otherwise never does.
TEST_CASE ("MultiCompStage: an editor's seed hands the plug-in what a push would, BYPASS showing the engage", "[compressor][multicomp][editor]")
{
    for (const auto& k : kKinds)
    {
        CAPTURE (k.name);
        MultiCompStage seeded (k.kind), pushed (k.kind);
        CompParams comp, moved;
        comp.resetToDefaults (k.kind);
        for (auto* stage : { &seeded, &pushed })
        {
            stage->prepare (kSr, kBlock, k.channels);
            stage->pushParameters (comp, true, 0);
        }
        moveEveryValue (k.kind, comp, moved);
        const int mode = k.kind == CompStage::Strip ? 2 : 0;
        pushed.pushParameters (moved, true, mode);
        seeded.seedEditor (moved, false, mode);

        const auto& params = seeded.plugin().params();
        const int bypass = indexOf (seeded, "daf_bypass");
        int compared = 0;
        for (std::size_t i = 0; i < params.size(); ++i)
        {
            if (params[i].isOutput) continue;
            CAPTURE (params[i].symbol);
            const float expected = (int) i == bypass ? 1.0f : pushed.editorReading ((int) i);
            CHECK_THAT (seeded.editorReading ((int) i), WithinAbs (expected, 0.0f));
            ++compared;
        }
        CHECK (compared > 20);
        for (const auto id : duskstudio::compStageParams (k.kind))
            CHECK_THAT (seeded.editorReading (indexOf (seeded, duskstudio::compParamSpec (id).symbol)),
                        WithinAbs (moved.get (id), 2.0e-3f));

        seeded.seedEditor (moved, true, mode);
        CHECK_THAT (seeded.editorReading (bypass), WithinAbs (0.0f, 0.0f));
    }
}

// The seed goes into the plug-in behind the back of the stage's own record of
// what it pushed, so the next push resends everything. Until the plug-in next
// runs it keeps showing the editor's BYPASS; it runs with its own bypass off.
TEST_CASE ("MultiCompStage: the push after an editor's seed plays the session, whatever the seed left", "[compressor][multicomp][editor]")
{
    MultiCompStage stage (CompStage::Bus);
    stage.prepare (kSr, kBlock, 2);
    const int makeup = indexOf (stage, "bus_makeup");
    const int bypass = indexOf (stage, "daf_bypass");
    CompParams session, seen;
    session.resetToDefaults (CompStage::Bus);
    session.set (CompParamId::BusMakeup, 2.0f);
    stage.pushParameters (session, true, 0);

    // The session moves and comes back between two pushes while an editor
    // opens on the moved value.
    seen.copyFrom (session);
    seen.set (CompParamId::BusMakeup, 9.0f);
    stage.seedEditor (seen, false, 0);
    CHECK_THAT (stage.editorReading (makeup), WithinAbs (9.0f, 0.0f));
    CHECK_THAT (stage.editorReading (bypass), WithinAbs (1.0f, 0.0f));

    stage.pushParameters (session, true, 0);
    CHECK_THAT (stage.editorReading (makeup), WithinAbs (2.0f, 0.0f));
    CHECK_THAT (stage.editorReading (bypass), WithinAbs (1.0f, 0.0f));

    std::vector<float> L ((std::size_t) kBlock, 0.0f), R ((std::size_t) kBlock, 0.0f);
    stage.process (L.data(), R.data(), kBlock);
    CHECK_THAT (stage.editorReading (bypass), WithinAbs (0.0f, 0.0f));
    stage.pushParameters (session, true, 0);
    CHECK_THAT (stage.editorReading (bypass), WithinAbs (0.0f, 0.0f));
}

// A compressor that is out sits at rest, and an editor opened on it seeds its
// plug-in, BYPASS showing it out. Engaged again, prepared again or reset after
// the seed, it plays exactly what a twin that took the same values by a push
// plays.
TEST_CASE ("MultiCompStage: a stage an editor seeded while out plays as one no editor touched", "[compressor][multicomp][editor]")
{
    struct Case { CompStage kind; int channels; int mode; const char* name; };
    const Case cases[] { { CompStage::Strip, 1, 1, "FET, mono" },
                         { CompStage::Strip, 2, 2, "VCA, stereo" },
                         { CompStage::Bus, 2, 0, "bus" } };
    enum class After { engaged, prepared, reset };
    for (const auto& c : cases)
        for (const auto after : { After::engaged, After::prepared, After::reset })
        {
            CAPTURE (c.name, (int) after);
            MultiCompStage seeded (c.kind), plain (c.kind);
            CompParams first, second;
            first.resetToDefaults (c.kind);
            first.set (CompParamId::FetInput, 25.0f);
            first.set (CompParamId::VcaThreshold, -40.0f);
            first.set (CompParamId::BusThreshold, -20.0f);
            moveEveryValue (c.kind, first, second);
            double seededPhase = 0.0, plainPhase = 0.0;
            const auto both = [&] (const std::function<void (MultiCompStage&)>& step, int blocks)
            {
                step (seeded);
                step (plain);
                const auto a = hotBlocks (seeded, c.channels, blocks, seededPhase);
                const auto b = hotBlocks (plain, c.channels, blocks, plainPhase);
                REQUIRE (a.size() == b.size());
                CHECK (std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0);
            };
            for (auto* stage : { &seeded, &plain })
                stage->prepare (kSr, kBlock, c.channels);
            both ([&] (MultiCompStage& s) { s.pushParameters (first, true, c.mode); }, 20);

            // Switched out: the fade ends and the plug-in goes back to rest,
            // here before the editor opens or, last, after it.
            if (after != After::reset)
                for (auto* stage : { &seeded, &plain }) stage->reset();
            seeded.seedEditor (second, false, c.mode);
            plain.pushParameters (second, true, c.mode);
            if (after == After::reset)
                for (auto* stage : { &seeded, &plain }) stage->reset();
            both ([&] (MultiCompStage& s)
            {
                if (after == After::prepared) s.prepare (kSr, kBlock, c.channels);
                s.pushParameters (second, true, c.mode);
            }, 20);
        }
}
