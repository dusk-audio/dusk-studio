#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <FourKEQDSP.hpp>

#include "TestTempDirectory.h"
#include "dsp/ChannelStrip.h"
#include "foundation/Json.h"
#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <vector>

// Format 7 stored a channel EQ band's or filter's frequency as a dial position
// of the 4K EQ core; format 8 stores the Hz it plays and the strips drive the
// core's Hz API. A v7 session loads into the Hz its dials played, for the
// knobs, and keeps each dial, which the strip plays through the core's dial
// API until the frequency moves, so it sounds exactly as it did.

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using EQ   = duskaudio::FourKEQDSP;
using Json = dusk::json::Json;

namespace
{
using duskstudio::ChannelStripParams;
using EqFreq = ChannelStripParams::EqFreq;

constexpr double kSampleRate = 48000.0;
constexpr int    kBlock      = 256;
constexpr int    kBlocks     = 64;

// The HPF and LPF are switched in when set.
struct Bands
{
    float lf, lm, hm, hf;
    float hpf = 0.0f, lpf = 0.0f;
};

// Dial positions inside each band's and filter's measured table, where the Hz
// API plays exactly what the dial API played.
constexpr Bands kDials { 100.0f, 600.0f, 3000.0f, 10000.0f, 80.0f, 9000.0f };

Json v7Session (bool black, const Bands& dials)
{
    Json document {
        { "version", 7 },
        { "tracks", Json::array ({
            { { "name", "Dialled" },
              { "eq", {
                  { "enabled", true },
                  { "type", black ? "black" : "brown" },
                  { "lf", { { "gain",  6.0 }, { "freq", dials.lf } } },
                  { "lm", { { "gain", -4.0 }, { "freq", dials.lm }, { "q", 1.2 } } },
                  { "hm", { { "gain",  5.0 }, { "freq", dials.hm }, { "q", 0.8 } } },
                  { "hf", { { "gain",  4.0 }, { "freq", dials.hf } } } } } }
        }) }
    };
    if (dials.hpf > 0.0f)
        document["tracks"][0]["hpf"] = { { "enabled", true }, { "freq", dials.hpf } };
    if (dials.lpf > 0.0f)
        document["tracks"][0]["lpf"] = { { "enabled", true }, { "freq", dials.lpf } };
    return document;
}

void write (const std::filesystem::path& file, const Json& document)
{
    std::ofstream stream (file);
    stream << document.dump();
    REQUIRE (stream.good());
}

// The format-7 dial a strip plays f through, or 0 when it follows its Hz.
float dialOf (const ChannelStripParams& strip, EqFreq f)
{
    float hz = 0.0f;
    return strip.legacyDial (f).dialFor (strip.eqFreq (f), strip.eqBlackMode.load(), hz);
}

// only: the one band boosted or cut, or -1 for all four.
std::vector<float> render (bool black, const Bands& bands, bool hz, int only = -1)
{
    EQ eq;
    eq.setOversampling (0);
    eq.setMsMode (false);
    eq.setAutoGain (false);
    eq.setBypass (false);
    eq.setEqType (black ? 1 : 0);
    eq.setSaturation (22.0f);
    const auto gain = [only] (int band, float db) { return only < 0 || only == band ? db : 0.0f; };
    eq.setLfGain (gain (0, 6.0f));  eq.setLfBell (false);
    eq.setLmGain (gain (1, -4.0f)); eq.setLmQ (1.2f);
    eq.setHmGain (gain (2, 5.0f));  eq.setHmQ (0.8f);
    eq.setHfGain (gain (3, 4.0f));  eq.setHfBell (false);
    if (hz)
    {
        eq.setLfFreqHz (bands.lf); eq.setLmFreqHz (bands.lm);
        eq.setHmFreqHz (bands.hm); eq.setHfFreqHz (bands.hf);
    }
    else
    {
        eq.setLfFreq (bands.lf); eq.setLmFreq (bands.lm);
        eq.setHmFreq (bands.hm); eq.setHfFreq (bands.hf);
    }
    eq.setHpfEnabled (bands.hpf > 0.0f);
    eq.setLpfEnabled (bands.lpf > 0.0f);
    if (hz)
    {
        eq.setHpfFreqHz (bands.hpf); eq.setLpfFreqHz (bands.lpf);
    }
    else
    {
        eq.setHpfFreq (bands.hpf); eq.setLpfFreq (bands.lpf);
    }
    eq.prepare (kSampleRate, kBlock);
    eq.reset();

    std::vector<float> l ((size_t) (kBlock * kBlocks)), r;
    std::uint32_t seed = 0x2545F491u;
    for (auto& v : l)
    {
        seed = seed * 1664525u + 1013904223u;
        v = 0.4f * ((float) ((seed >> 8) & 0xFFFFFF) / (float) 0x1000000 - 0.5f);
    }
    r = l;
    for (int off = 0; off < (int) l.size(); off += kBlock)
    {
        float* lr[2] = { l.data() + off, r.data() + off };
        eq.processBlock (lr, lr, 2, kBlock);
    }
    return l;
}
} // namespace

TEST_CASE ("A v7 session's EQ dial positions load as the Hz they played",
           "[session][serializer][migration][eq]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    for (const bool black : { false, true })
    {
        CAPTURE (black);
        duskstudio::test::TempDirectory dir ("dusk-eq-dial-migration-");
        const auto file = dir.path() / "session.json";
        write (file, v7Session (black, kDials));

        auto session = std::make_unique<Session>();
        REQUIRE (SessionSerializer::load (*session, file));
        const auto& strip = session->track (0).strip;
        const Bands loaded { strip.lfFreq.load(), strip.lmFreq.load(),
                             strip.hmFreq.load(), strip.hfFreq.load(),
                             strip.hpfFreq.load(), strip.lpfFreq.load() };

        CHECK_THAT (loaded.lf, WithinRel (EQ::hzForCalibratedEqControl (kDials.lf, EQ::Band::LF, black, false), 1.0e-6f));
        CHECK_THAT (loaded.lm, WithinRel (EQ::hzForCalibratedEqControl (kDials.lm, EQ::Band::LM, black, true),  1.0e-6f));
        CHECK_THAT (loaded.hm, WithinRel (EQ::hzForCalibratedEqControl (kDials.hm, EQ::Band::HM, black, true),  1.0e-6f));
        CHECK_THAT (loaded.hf, WithinRel (EQ::hzForCalibratedEqControl (kDials.hf, EQ::Band::HF, black, false), 1.0e-6f));
        CHECK_THAT (loaded.hpf, WithinRel (EQ::hzForCalibratedFilterControl (kDials.hpf, true, black), 1.0e-6f));
        CHECK_THAT (loaded.lpf, WithinRel (EQ::hzForCalibratedFilterControl (kDials.lpf, false, black), 1.0e-6f));
        CHECK (strip.hpfEnabled.load());
        CHECK (strip.lpfEnabled.load());
        // A migration that stored the dial unchanged would pass the checks
        // above only if the dial law were the identity, which it is not.
        CHECK (std::abs (loaded.hf - kDials.hf) > 1000.0f);
        CHECK (std::abs (loaded.hpf - kDials.hpf) > 30.0f);
        CHECK (std::abs (loaded.lpf - kDials.lpf) > 1500.0f);

        const auto before = render (black, kDials, false);
        const auto after  = render (black, loaded, true);
        double worst = 0.0;
        for (size_t i = 0; i < before.size(); ++i)
            worst = std::max (worst, (double) std::abs (before[i] - after[i]));
        CHECK_THAT (worst, WithinAbs (0.0, 1.0e-6));

        REQUIRE (SessionSerializer::save (*session, file));
        std::ifstream stream (file);
        const auto saved = Json::parse (stream, nullptr, false);
        REQUIRE (saved.is_object());
        CHECK (saved["version"].get<int>() == 8);
        CHECK_THAT (saved["tracks"][0]["eq"]["hf"]["freq"].get<float>(), WithinRel (loaded.hf, 1.0e-6f));
        CHECK_THAT (saved["tracks"][0]["hpf"]["freq"].get<float>(), WithinRel (loaded.hpf, 1.0e-6f));

        // A v8 file is already in Hz: loading it again must not convert twice.
        auto reloaded = std::make_unique<Session>();
        REQUIRE (SessionSerializer::load (*reloaded, file));
        CHECK_THAT (reloaded->track (0).strip.lfFreq.load(), WithinRel (loaded.lf, 1.0e-6f));
        CHECK_THAT (reloaded->track (0).strip.lmFreq.load(), WithinRel (loaded.lm, 1.0e-6f));
        CHECK_THAT (reloaded->track (0).strip.hmFreq.load(), WithinRel (loaded.hm, 1.0e-6f));
        CHECK_THAT (reloaded->track (0).strip.hfFreq.load(), WithinRel (loaded.hf, 1.0e-6f));
        CHECK_THAT (reloaded->track (0).strip.hpfFreq.load(), WithinRel (loaded.hpf, 1.0e-6f));
        CHECK_THAT (reloaded->track (0).strip.lpfFreq.load(), WithinRel (loaded.lpf, 1.0e-6f));
    }
}

// Every Hz a v7 dial could play, over v7's own ranges in either voicing, has
// to fit what the loader holds, or a converted session would move on load.
TEST_CASE ("The held EQ ranges take every Hz a v7 dial played", "[session][migration][eq]")
{
    using duskstudio::ChannelStripParams;
    struct Band { EQ::Band band; bool bell; float v7Lo, v7Hi, heldLo, heldHi, knobLo, knobHi; };
    const Band bands[] {
        { EQ::Band::LF, false,   20.0f,   400.0f, ChannelStripParams::kLfFreqHeldMin, ChannelStripParams::kLfFreqHeldMax,
          ChannelStripParams::kLfFreqMin, ChannelStripParams::kLfFreqMax },
        { EQ::Band::LM, true,   100.0f,  4000.0f, ChannelStripParams::kLmFreqHeldMin, ChannelStripParams::kLmFreqHeldMax,
          ChannelStripParams::kLmFreqMin, ChannelStripParams::kLmFreqMax },
        { EQ::Band::HM, true,   600.0f, 13000.0f, ChannelStripParams::kHmFreqHeldMin, ChannelStripParams::kHmFreqHeldMax,
          ChannelStripParams::kHmFreqMin, ChannelStripParams::kHmFreqMax },
        { EQ::Band::HF, false, 1000.0f, 20000.0f, ChannelStripParams::kHfFreqHeldMin, ChannelStripParams::kHfFreqHeldMax,
          ChannelStripParams::kHfFreqMin, ChannelStripParams::kHfFreqMax },
    };
    for (const auto& b : bands)
    {
        float lo = b.knobLo, hi = b.knobHi;
        for (const bool black : { false, true })
            for (int i = 0; i <= 2000; ++i)
            {
                const float dial = b.v7Lo * std::pow (b.v7Hi / b.v7Lo, (float) i / 2000.0f);
                const float hz = EQ::hzForCalibratedEqControl (dial, b.band, black, b.bell);
                lo = std::min (lo, hz);
                hi = std::max (hi, hz);
            }
        CAPTURE ((int) b.band, lo, hi);
        CHECK (b.heldLo <= lo);
        CHECK (b.heldHi >= hi);
        // An envelope, not a free pass: within 3% of what v7 reached.
        CHECK (b.heldLo > 0.97f * lo);
        CHECK (b.heldHi < 1.03f * hi);
    }

    // The HPF's on side reaches past its knob in Black; its low side and both
    // LPF ends are the OFF positions a filter that was on stops short of.
    float hpfHi = ChannelStripParams::kHpfMaxHz, lpfLo = ChannelStripParams::kLpfMinHz;
    for (const bool black : { false, true })
        for (int i = 0; i <= 2000; ++i)
        {
            const float t = (float) i / 2000.0f;
            hpfHi = std::max (hpfHi, EQ::hzForCalibratedFilterControl (20.0f * std::pow (15.0f, t), true, black));
            lpfLo = std::min (lpfLo, EQ::hzForCalibratedFilterControl (3000.0f * std::pow (20000.0f / 3000.0f, t), false, black));
        }
    CHECK (ChannelStripParams::kHpfHeldMaxHz >= hpfHi);
    CHECK (ChannelStripParams::kHpfHeldMaxHz < 1.03f * hpfHi);
    CHECK (ChannelStripParams::kLpfMinHz <= lpfLo);
}

TEST_CASE ("A v7 EQ dial that played outside today's band range keeps its exact Hz",
           "[session][serializer][migration][eq]")
{
    using duskstudio::ChannelStripParams;
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    // Black LF at the old 400 top played about 660 Hz, black LM at the old 100
    // bottom about 139 Hz, black HM at 13000 about 8.8 kHz and black HF at the
    // old 1 kHz bottom about 637 Hz, all past today's knobs. Each loads at that
    // Hz and plays what it played; its knob rests on the end stop until moved.
    // A dial past v7's own range was clamped by the v7 loader before it played,
    // so it converts from that clamp.
    duskstudio::test::TempDirectory dir ("dusk-eq-dial-edges-");
    const auto file = dir.path() / "session.json";
    const Bands edges { 400.0f, 100.0f, 13000.0f, 1000.0f };
    auto document = v7Session (true, edges);
    document["tracks"].push_back ({ { "eq", { { "type", "brown" },
                                              { "lf", { { "freq", 5.0 } } },
                                              { "hf", { { "freq", 1.0e9 } } } } } });
    write (file, document);

    auto session = std::make_unique<Session>();
    REQUIRE (SessionSerializer::load (*session, file));

    const auto& black = session->track (0).strip;
    const Bands played { EQ::hzForCalibratedEqControl (edges.lf, EQ::Band::LF, true, false),
                         EQ::hzForCalibratedEqControl (edges.lm, EQ::Band::LM, true, true),
                         EQ::hzForCalibratedEqControl (edges.hm, EQ::Band::HM, true, true),
                         EQ::hzForCalibratedEqControl (edges.hf, EQ::Band::HF, true, false) };
    CHECK (played.lf > ChannelStripParams::kLfFreqMax);
    CHECK (played.lm < ChannelStripParams::kLmFreqMin);
    CHECK (played.hm > ChannelStripParams::kHmFreqMax);
    CHECK (played.hf < ChannelStripParams::kHfFreqMin);
    const Bands loaded { black.lfFreq.load(), black.lmFreq.load(), black.hmFreq.load(), black.hfFreq.load() };
    CHECK_THAT (loaded.lf, WithinRel (played.lf, 1.0e-6f));
    CHECK_THAT (loaded.lm, WithinRel (played.lm, 1.0e-6f));
    CHECK_THAT (loaded.hm, WithinRel (played.hm, 1.0e-6f));
    CHECK_THAT (loaded.hf, WithinRel (played.hf, 1.0e-6f));

    // Each band plays what its dial played. Past the ends of the core's
    // measured tables the Hz API holds the LF/LM and HM/HF pair corrections
    // where the table ends, so two such bands boosted together are not
    // bit-identical to the dial API; FourKEQDSP.hpp setLfFreqHz.
    for (int band = 0; band < 4; ++band)
    {
        CAPTURE (band);
        const auto before = render (true, edges, false, band);
        const auto after  = render (true, loaded, true, band);
        double worst = 0.0;
        for (size_t i = 0; i < before.size(); ++i)
            worst = std::max (worst, (double) std::abs (before[i] - after[i]));
        CHECK_THAT (worst, WithinAbs (0.0, 1.0e-6));
    }

    const auto& brown = session->track (1).strip;
    CHECK_THAT (brown.lfFreq.load(), WithinRel (EQ::hzForCalibratedEqControl (20.0f, EQ::Band::LF, false, false), 1.0e-6f));
    CHECK_THAT (brown.hfFreq.load(), WithinRel (EQ::hzForCalibratedEqControl (20000.0f, EQ::Band::HF, false, false), 1.0e-6f));

    // Saved as held, and a v8 reload holds it again, with its dial.
    REQUIRE (SessionSerializer::save (*session, file));
    auto reloaded = std::make_unique<Session>();
    REQUIRE (SessionSerializer::load (*reloaded, file));
    const auto& again = reloaded->track (0).strip;
    CHECK_THAT (again.lfFreq.load(), WithinRel (played.lf, 1.0e-6f));
    CHECK_THAT (again.lmFreq.load(), WithinRel (played.lm, 1.0e-6f));
    CHECK_THAT (again.hmFreq.load(), WithinRel (played.hm, 1.0e-6f));
    CHECK_THAT (again.hfFreq.load(), WithinRel (played.hf, 1.0e-6f));
    CHECK_THAT (dialOf (again, EqFreq::Lf), WithinAbs (edges.lf, 0.0));
    CHECK_THAT (dialOf (again, EqFreq::Lm), WithinAbs (edges.lm, 0.0));
    CHECK_THAT (dialOf (again, EqFreq::Hm), WithinAbs (edges.hm, 0.0));
    CHECK_THAT (dialOf (again, EqFreq::Hf), WithinAbs (edges.hf, 0.0));
    CHECK_THAT (dialOf (reloaded->track (1).strip, EqFreq::Lf), WithinAbs (20.0, 0.0));
    CHECK_THAT (dialOf (reloaded->track (1).strip, EqFreq::Hf), WithinAbs (20000.0, 0.0));
}

TEST_CASE ("A v7 filter at OFF stays off and one past today's range keeps its sound",
           "[session][serializer][migration][eq]")
{
    using duskstudio::ChannelStripParams;
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    // Brown HPF 40 played about 16 Hz and Brown LPF 12800 about 22.5 kHz, past
    // the OFF end of today's ranges: they land one hertz short, so a filter
    // that was on stays on and its knob still shows a frequency. Black HPF 300
    // played about 316 Hz, past the knob's top, and keeps it.
    const auto track = [] (const char* type, Json hpf, Json lpf)
    {
        return Json { { "eq", { { "type", type } } }, { "hpf", std::move (hpf) }, { "lpf", std::move (lpf) } };
    };
    Json document {
        { "version", 7 },
        { "tracks", Json::array ({
            track ("brown", { { "enabled", false }, { "freq", 20.0 } },
                            { { "enabled", false }, { "freq", 20000.0 } }),
            track ("brown", { { "enabled", true }, { "freq", 40.0 } },
                            { { "enabled", true }, { "freq", 12800.0 } }),
            track ("black", { { "enabled", true }, { "freq", 300.0 } },
                            { { "enabled", true }, { "freq", 3000.0 } }),
            track ("brown", { { "enabled", false }, { "freq", "loud" } },
                            { { "enabled", true }, { "freq", -5.0 } }),
        }) }
    };
    duskstudio::test::TempDirectory dir ("dusk-eq-filter-edges-");
    const auto file = dir.path() / "session.json";
    write (file, document);

    auto session = std::make_unique<Session>();
    REQUIRE (SessionSerializer::load (*session, file));

    const auto& off = session->track (0).strip;
    CHECK_FALSE (off.hpfEnabled.load());
    CHECK_FALSE (off.lpfEnabled.load());
    CHECK_THAT (off.hpfFreq.load(), WithinRel (ChannelStripParams::kHpfOffHz, 1.0e-6f));
    CHECK_THAT (off.lpfFreq.load(), WithinRel (ChannelStripParams::kLpfOffHz, 1.0e-6f));

    const auto& brown = session->track (1).strip;
    CHECK (EQ::hzForCalibratedFilterControl (40.0f, true, false) < ChannelStripParams::kHpfOffHz);
    CHECK (EQ::hzForCalibratedFilterControl (12800.0f, false, false) > ChannelStripParams::kLpfOffHz);
    CHECK (brown.hpfEnabled.load());
    CHECK (brown.lpfEnabled.load());
    CHECK_THAT (brown.hpfFreq.load(), WithinRel (ChannelStripParams::kHpfOffHz + 1.0f, 1.0e-6f));
    CHECK_THAT (brown.lpfFreq.load(), WithinRel (ChannelStripParams::kLpfOffHz - 1.0f, 1.0e-6f));
    // Its knob stops a hertz short of OFF, and it plays its dial, exactly where it played.
    CHECK_THAT (dialOf (brown, EqFreq::Hpf), WithinAbs (40.0, 0.0));
    CHECK_THAT (dialOf (brown, EqFreq::Lpf), WithinAbs (12800.0, 0.0));
    CHECK_THAT (dialOf (off, EqFreq::Hpf), WithinAbs (20.0, 0.0));

    const auto& black = session->track (2).strip;
    CHECK (EQ::hzForCalibratedFilterControl (300.0f, true, true) > ChannelStripParams::kHpfMaxHz);
    CHECK_THAT (black.hpfFreq.load(), WithinRel (EQ::hzForCalibratedFilterControl (300.0f, true, true), 1.0e-6f));
    CHECK_THAT (black.lpfFreq.load(), WithinRel (EQ::hzForCalibratedFilterControl (3000.0f, false, true), 1.0e-6f));

    // Unreadable is OFF, as the v7 loader read it; a value past v7's range
    // converts from the v7 loader's clamp.
    const auto& clamped = session->track (3).strip;
    CHECK_THAT (clamped.hpfFreq.load(), WithinRel (ChannelStripParams::kHpfOffHz, 1.0e-6f));
    CHECK_THAT (clamped.lpfFreq.load(), WithinRel (EQ::hzForCalibratedFilterControl (3000.0f, false, false), 1.0e-6f));

    // Saved and reloaded, every filter keeps its frequency and its dial, the
    // ones at OFF and past it included.
    REQUIRE (SessionSerializer::save (*session, file));
    auto reloaded = std::make_unique<Session>();
    REQUIRE (SessionSerializer::load (*reloaded, file));
    CHECK_THAT (reloaded->track (2).strip.hpfFreq.load(), WithinRel (black.hpfFreq.load(), 1.0e-6f));
    CHECK_THAT (reloaded->track (1).strip.lpfFreq.load(), WithinRel (ChannelStripParams::kLpfOffHz - 1.0f, 1.0e-6f));
    const float keptDials[][2] { { 20.0f, 20000.0f }, { 40.0f, 12800.0f }, { 300.0f, 3000.0f }, { 20.0f, 3000.0f } };
    for (int t = 0; t < 4; ++t)
    {
        CAPTURE (t);
        CHECK_THAT (dialOf (reloaded->track (t).strip, EqFreq::Hpf), WithinAbs (keptDials[t][0], 0.0));
        CHECK_THAT (dialOf (reloaded->track (t).strip, EqFreq::Lpf), WithinAbs (keptDials[t][1], 0.0));
    }
}

namespace
{
// Dials on a flat stretch of each measured table, where the Hz API reads the
// band's Q and the pair interaction at the table's edge rather than at the
// dial, and dials off them. The filters differ between the two APIs anywhere.
constexpr Bands kFlatDials    {   25.0f, 3000.0f, 10000.0f, 17653.0f,  28.0f, 17000.0f };
constexpr Bands kInsideDials  {  100.0f,  600.0f,  3000.0f, 10000.0f, 300.0f,  9000.0f };

// Every band boosted or cut hard, so both pair interactions are in play.
Json v7Strip (bool black, const Bands& d)
{
    return {
        { "eq", {
            { "enabled", true },
            { "type", black ? "black" : "brown" },
            { "lf", { { "gain", 12.0 }, { "freq", d.lf } } },
            { "lm", { { "gain", 10.0 }, { "freq", d.lm }, { "q", 1.4 } } },
            { "hm", { { "gain",  7.5 }, { "freq", d.hm }, { "q", 0.9 } } },
            { "hf", { { "gain", 15.0 }, { "freq", d.hf } } } } },
        { "hpf", { { "enabled", true }, { "freq", d.hpf } } },
        { "lpf", { { "enabled", true }, { "freq", d.lpf } } },
    };
}

struct DialTrack { bool black; Bands dials; };
constexpr DialTrack kDialTracks[] {
    { false, kFlatDials }, { true, kFlatDials }, { false, kInsideDials }, { true, kInsideDials },
};

Json v7DialSession()
{
    Json document { { "version", 7 }, { "tracks", Json::array() } };
    for (const auto& t : kDialTracks)
        document["tracks"].push_back (v7Strip (t.black, t.dials));
    return document;
}

float originalDial (const Bands& d, EqFreq f)
{
    switch (f)
    {
        case EqFreq::Hpf: return d.hpf;
        case EqFreq::Lpf: return d.lpf;
        case EqFreq::Lf:  return d.lf;
        case EqFreq::Lm:  return d.lm;
        case EqFreq::Hm:  return d.hm;
        case EqFreq::Hf:  break;
    }
    return d.hf;
}

constexpr EqFreq kAllFreqs[] { EqFreq::Hpf, EqFreq::Lpf, EqFreq::Lf, EqFreq::Lm, EqFreq::Hm, EqFreq::Hf };

// A core set up as ChannelStrip::prepare sets up its own, at the rate the strip
// runs it (the base rate times the oversampling factor), and handed the strip's
// settings the way the audio thread hands them over. adjust runs after that,
// last setter winning, to build the reference.
template <typename Adjust>
std::vector<float> renderAsStrip (const ChannelStripParams& params, double rate, Adjust&& adjust)
{
    EQ eq;
    eq.setOversampling (0);
    eq.setMsMode (false);
    eq.setAutoGain (false);
    eq.setBypass (false);
    eq.prepare (rate, kBlock);
    eq.reset();
    duskstudio::ChannelStrip::pushEqParameters (eq, params);
    adjust (eq);

    std::vector<float> l ((size_t) (kBlock * kBlocks)), r;
    std::uint32_t seed = 0x9E3779B9u;
    for (auto& v : l)
    {
        seed = seed * 1664525u + 1013904223u;
        v = 0.4f * ((float) ((seed >> 8) & 0xFFFFFF) / (float) 0x1000000 - 0.5f);
    }
    r = l;
    for (int off = 0; off < (int) l.size(); off += kBlock)
    {
        float* lr[2] = { l.data() + off, r.data() + off };
        eq.processBlock (lr, lr, 2, kBlock);
    }
    return l;
}

std::vector<float> renderAsStrip (const ChannelStripParams& params, double rate)
{
    return renderAsStrip (params, rate, [] (EQ&) {});
}

// The original v7 dials through the dial API, but for the one index skip, which
// keeps what the strip set.
void setDials (EQ& eq, const Bands& d, int skip = -1)
{
    if (skip != (int) EqFreq::Hpf) eq.setHpfFreq (d.hpf);
    if (skip != (int) EqFreq::Lpf) eq.setLpfFreq (d.lpf);
    if (skip != (int) EqFreq::Lf)  eq.setLfFreq (d.lf);
    if (skip != (int) EqFreq::Lm)  eq.setLmFreq (d.lm);
    if (skip != (int) EqFreq::Hm)  eq.setHmFreq (d.hm);
    if (skip != (int) EqFreq::Hf)  eq.setHfFreq (d.hf);
}

bool bitIdentical (const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0;
}

double worstDifference (const std::vector<float>& a, const std::vector<float>& b)
{
    double worst = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        worst = std::max (worst, (double) std::abs (a[i] - b[i]));
    return worst;
}

// The strip's rates at 48 kHz: 1x and 4x Effect oversampling.
constexpr double kStripRates[] { 48000.0, 192000.0 };

// Every strip of a session loaded from v7DialSession plays its original dials.
void checkPlaysOriginalDials (const duskstudio::Session& session, const char* what)
{
    for (size_t t = 0; t < std::size (kDialTracks); ++t)
    {
        const auto& track = kDialTracks[t];
        const auto& strip = session.track ((int) t).strip;
        CAPTURE (what, t, track.black);
        for (const auto f : kAllFreqs)
            CHECK_THAT (dialOf (strip, f), WithinAbs (originalDial (track.dials, f), 0.0));
        for (const double rate : kStripRates)
        {
            CAPTURE (rate);
            const auto reference = renderAsStrip (strip, rate, [&track] (EQ& eq) { setDials (eq, track.dials); });
            CHECK (bitIdentical (renderAsStrip (strip, rate), reference));
        }
    }
}
} // namespace

TEST_CASE ("A v7 session plays every band and filter through its original dial bit for bit",
           "[session][serializer][migration][eq]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    duskstudio::test::TempDirectory dir ("dusk-eq-dial-identity-");
    const auto file = dir.path() / "session.json";
    write (file, v7DialSession());

    auto session = std::make_unique<Session>();
    REQUIRE (SessionSerializer::load (*session, file));
    checkPlaysOriginalDials (*session, "loaded");

    // The knobs still read the Hz each dial played.
    const auto& flatBrown = session->track (0).strip;
    CHECK_THAT (flatBrown.hmFreq.load(), WithinRel (EQ::hzForCalibratedEqControl (kFlatDials.hm, EQ::Band::HM, false, true), 1.0e-6f));
    CHECK_THAT (flatBrown.hfFreq.load(), WithinRel (EQ::hzForCalibratedEqControl (kFlatDials.hf, EQ::Band::HF, false, false), 1.0e-6f));

    // What the dials are kept for: with the bands on flat stretches, their Hz
    // through the Hz API is not what the dials played, by a lot at these gains.
    for (const size_t t : { size_t (0), size_t (1) })
    {
        CAPTURE (t);
        const auto& strip = session->track ((int) t).strip;
        const auto reference = renderAsStrip (strip, 48000.0, [t] (EQ& eq) { setDials (eq, kDialTracks[t].dials); });
        const auto inHz = renderAsStrip (strip, 48000.0, [&strip] (EQ& eq)
        {
            eq.setLfFreqHz (strip.lfFreq.load());   eq.setLmFreqHz (strip.lmFreq.load());
            eq.setHmFreqHz (strip.hmFreq.load());   eq.setHfFreqHz (strip.hfFreq.load());
        });
        CHECK (worstDifference (inHz, reference) > 1.0e-3);
    }

    // Saved beside each Hz, reloaded, and still bit for bit.
    REQUIRE (SessionSerializer::save (*session, file));
    {
        std::ifstream stream (file);
        const auto saved = Json::parse (stream, nullptr, false);
        REQUIRE (saved.is_object());
        const auto& flat = saved["tracks"][0];
        CHECK_THAT (flat["eq"]["hm"]["freq_dial"].get<double>(), WithinAbs (kFlatDials.hm, 0.0));
        CHECK_THAT (flat["hpf"]["freq_dial"].get<double>(), WithinAbs (kFlatDials.hpf, 0.0));
        CHECK_THAT (flat["eq"]["hm"]["freq"].get<float>(), WithinRel (flatBrown.hmFreq.load(), 1.0e-6f));
        CHECK_FALSE (saved["tracks"][4]["eq"]["lf"].contains ("freq_dial"));
    }
    auto reloaded = std::make_unique<Session>();
    REQUIRE (SessionSerializer::load (*reloaded, file));
    checkPlaysOriginalDials (*reloaded, "reloaded");
}

TEST_CASE ("Moving a converted band's frequency hands only that band to its Hz",
           "[session][migration][eq]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    duskstudio::test::TempDirectory dir ("dusk-eq-dial-move-");
    const auto file = dir.path() / "session.json";
    write (file, v7DialSession());
    auto session = std::make_unique<Session>();
    REQUIRE (SessionSerializer::load (*session, file));

    for (const auto moved : kAllFreqs)
    {
        CAPTURE ((int) moved);
        auto& strip = session->track (1).strip;
        const auto& dials = kDialTracks[1].dials;
        const auto word = strip.legacyDial (moved).raw();
        const float held = strip.eqFreq (moved).load();
        const float hz = moved == EqFreq::Hpf ? 150.0f : moved == EqFreq::Lpf ? 12000.0f
                       : moved == EqFreq::Lf  ? 120.0f : moved == EqFreq::Lm  ? 800.0f
                       : moved == EqFreq::Hm  ? 4000.0f : 9000.0f;
        strip.setEqFreq (moved, hz);

        for (const auto f : kAllFreqs)
            CHECK_THAT (dialOf (strip, f), WithinAbs (f == moved ? 0.0f : originalDial (dials, f), 0.0));
        CHECK (strip.legacyDial (moved).raw() == 0);
        for (const double rate : kStripRates)
        {
            CAPTURE (rate);
            const auto reference = renderAsStrip (strip, rate, [&dials, moved, hz] (EQ& eq)
            {
                setDials (eq, dials, (int) moved);
                switch (moved)
                {
                    case EqFreq::Hpf: eq.setHpfFreqHz (hz); break;
                    case EqFreq::Lpf: eq.setLpfFreqHz (hz); break;
                    case EqFreq::Lf:  eq.setLfFreqHz (hz); break;
                    case EqFreq::Lm:  eq.setLmFreqHz (hz); break;
                    case EqFreq::Hm:  eq.setHmFreqHz (hz); break;
                    case EqFreq::Hf:  eq.setHfFreqHz (hz); break;
                }
            });
            CHECK (bitIdentical (renderAsStrip (strip, rate), reference));
        }

        // Moved back onto the converted Hz, it stays on its Hz.
        strip.setEqFreq (moved, held);
        CHECK_THAT (dialOf (strip, moved), WithinAbs (0.0, 0.0));
        strip.legacyDial (moved).setRaw (word);
    }
}

TEST_CASE ("A converted dial plays only in the voicing it was converted in",
           "[session][migration][eq]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    duskstudio::test::TempDirectory dir ("dusk-eq-dial-voicing-");
    const auto file = dir.path() / "session.json";
    write (file, v7DialSession());
    auto session = std::make_unique<Session>();
    REQUIRE (SessionSerializer::load (*session, file));

    // Brown's dial through Black would play neither the old sound nor what the
    // knob shows; the switch plays the knob's Hz, and switching back the dial.
    auto& strip = session->track (0).strip;
    strip.eqBlackMode.store (true);
    for (const auto f : kAllFreqs)
        CHECK_THAT (dialOf (strip, f), WithinAbs (0.0, 0.0));
    strip.eqBlackMode.store (false);
    for (const auto f : kAllFreqs)
        CHECK_THAT (dialOf (strip, f), WithinAbs (originalDial (kFlatDials, f), 0.0));

    // A save while switched keeps each dial with the voicing it plays in: the
    // reopened track plays its knobs' Hz in Black, and switched back to Brown
    // plays its original dials bit for bit.
    strip.eqBlackMode.store (true);
    REQUIRE (SessionSerializer::save (*session, file));
    {
        std::ifstream stream (file);
        const auto saved = Json::parse (stream, nullptr, false);
        REQUIRE (saved.is_object());
        CHECK (saved["tracks"][0]["eq"]["type"] == "black");
        CHECK (saved["tracks"][0]["eq"]["hm"]["freq_dial_type"] == "brown");
        CHECK (saved["tracks"][0]["hpf"]["freq_dial_type"] == "brown");
        CHECK (saved["tracks"][1]["eq"]["hm"]["freq_dial_type"] == "black");
    }
    auto reloaded = std::make_unique<Session>();
    REQUIRE (SessionSerializer::load (*reloaded, file));
    auto& reopened = reloaded->track (0).strip;
    REQUIRE (reopened.eqBlackMode.load());
    for (const auto f : kAllFreqs)
    {
        CAPTURE ((int) f);
        CHECK_THAT (dialOf (reopened, f), WithinAbs (0.0, 0.0));
        CHECK (reopened.legacyDial (f).raw() != 0);
    }
    reopened.eqBlackMode.store (false);
    checkPlaysOriginalDials (*reloaded, "saved switched, reopened and switched back");
}

TEST_CASE ("A v8 file's EQ dial plays only beside the frequency the migrator kept it for",
           "[session][serializer][migration][eq]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    duskstudio::test::TempDirectory dir ("dusk-eq-dial-v8-");
    const auto legacy = dir.path() / "legacy.json";
    write (legacy, v7DialSession());
    auto session = std::make_unique<Session>();
    REQUIRE (SessionSerializer::load (*session, legacy));
    REQUIRE (dialOf (session->track (3).strip, EqFreq::Lf) > 0.0f);

    // What the migrator writes beside a Brown LF dial at 100, a Brown LM dial
    // at 600, a Black HM dial at 3000 and a Black HPF dial at 80.
    const float lf = EQ::hzForCalibratedEqControl (100.0f, EQ::Band::LF, false, false);
    const float lm = EQ::hzForCalibratedEqControl (600.0f, EQ::Band::LM, false, true);
    const float hmBlack = EQ::hzForCalibratedEqControl (3000.0f, EQ::Band::HM, true, true);
    const float hmBrown = EQ::hzForCalibratedEqControl (3000.0f, EQ::Band::HM, false, true);
    const float hpfBlack = EQ::hzForCalibratedFilterControl (80.0f, true, true);
    REQUIRE (std::abs (lm - 700.0f) > 100.0f);
    REQUIRE (std::abs (hmBlack - hmBrown) > 1.0e-3f * hmBrown);
    REQUIRE (hpfBlack > ChannelStripParams::kHpfOffHz + 1.0f);

    Json document {
        { "version", 8 },
        { "tracks", Json::array ({
            // Kept dials: in the track's voicing; kept by a switched track in
            // the other; a filter at OFF with its own; and a frequency a few
            // millionths off, as another system's libm can compute it.
            { { "eq", { { "type", "brown" },
                        { "lf", { { "freq", lf * (1.0f + 4.0e-6f) }, { "freq_dial", 100.0 } } },
                        { "lm", { { "freq", lm }, { "freq_dial", 600.0 } } },
                        { "hm", { { "freq", hmBlack }, { "freq_dial", 3000.0 }, { "freq_dial_type", "black" } } } } },
              { "hpf", { { "enabled", true }, { "freq", hpfBlack }, { "freq_dial", 80.0 }, { "freq_dial_type", "black" } } },
              { "lpf", { { "enabled", true }, { "freq", 19999.6 }, { "freq_dial", 19999.6 } } } },
            // Dials no migrator kept: past v7's range, negative, zero, too
            // big for a float, unreadable, and missing (NaN is written null).
            { { "eq", { { "type", "brown" },
                        { "lf", { { "freq", 300.0 }, { "freq_dial", 99999.0 } } },
                        { "lm", { { "freq", 700.0 }, { "freq_dial", -5.0 } } },
                        { "hm", { { "freq", 3000.0 }, { "freq_dial", "loud" } } },
                        { "hf", { { "freq", 9000.0 }, { "freq_dial", 1.0e300 } } } } },
              { "hpf", { { "enabled", true }, { "freq", 90.0 }, { "freq_dial", 0.0 } } },
              { "lpf", { { "enabled", true }, { "freq", 9000.0 }, { "freq_dial", nullptr } } } },
            // Dials in range beside a frequency they did not play: 600 under
            // a 700 Hz knob, which would play about 412 Hz; Black's HM dial
            // beside Brown's Hz; a dial in neither voicing; an HPF dial at OFF
            // beside another OFF frequency; an LPF dial beside another's Hz.
            { { "eq", { { "type", "brown" },
                        { "lf", { { "freq", lf }, { "freq_dial", 100.0 }, { "freq_dial_type", "grey" } } },
                        { "lm", { { "freq", 700.0 }, { "freq_dial", 600.0 } } },
                        { "hm", { { "freq", hmBrown }, { "freq_dial", 3000.0 }, { "freq_dial_type", "black" } } } } },
              { "hpf", { { "enabled", true }, { "freq", 20.0 }, { "freq_dial", 20.4 } } },
              { "lpf", { { "enabled", true }, { "freq", 9000.0 }, { "freq_dial", 12000.0 } } } },
        }) }
    };
    const auto file = dir.path() / "session.json";
    write (file, document);
    REQUIRE (SessionSerializer::load (*session, file));

    const auto keptDial = [] (const ChannelStripParams& strip, EqFreq f, bool& black)
    {
        float hz = 0.0f;
        return strip.legacyDial (f).kept (strip.eqFreq (f), hz, black);
    };
    const auto& kept = session->track (0).strip;
    CHECK_THAT (dialOf (kept, EqFreq::Lf), WithinAbs (100.0, 0.0));
    CHECK_THAT (dialOf (kept, EqFreq::Lm), WithinAbs (600.0, 0.0));
    CHECK_THAT (dialOf (kept, EqFreq::Lpf), WithinAbs (19999.6f, 0.0));
    for (const auto f : { EqFreq::Hm, EqFreq::Hpf })
    {
        CAPTURE ((int) f);
        bool black = false;
        CHECK_THAT (dialOf (kept, f), WithinAbs (0.0, 0.0));
        CHECK_THAT (keptDial (kept, f, black), WithinAbs (f == EqFreq::Hm ? 3000.0 : 80.0, 0.0));
        CHECK (black);
    }

    for (const int t : { 1, 2 })
        for (const auto f : kAllFreqs)
        {
            CAPTURE (t, (int) f);
            CHECK (session->track (t).strip.legacyDial (f).raw() == 0);
        }
    CHECK_THAT (session->track (1).strip.lfFreq.load(), WithinAbs (300.0, 0.0));
    CHECK_THAT (session->track (2).strip.lmFreq.load(), WithinAbs (700.0, 0.0));

    // A track the file does not describe, and a band without the key, follow
    // their Hz: nothing survives from the session loaded before.
    for (const auto f : kAllFreqs)
    {
        CAPTURE ((int) f);
        CHECK (session->track (3).strip.legacyDial (f).raw() == 0);
    }
    CHECK (kept.legacyDial (EqFreq::Hf).raw() == 0);
}

// Every move of a band or filter, from its knob, a MIDI binding or a control
// surface, goes through ChannelStripParams' move helpers: it drops the dial as
// setEqFreq does and engages a bypassed EQ. A filter engages it once it leaves
// OFF, and turned back to OFF it switches off and leaves the EQ as it is. A
// load stores its values directly, so a session saved bypassed stays bypassed.
TEST_CASE ("Moving a band or filter engages the channel EQ, and loading a session does not",
           "[session][eq]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    for (const auto f : kAllFreqs)
    {
        CAPTURE ((int) f);
        ChannelStripParams strip;
        strip.legacyDial (f).set (100.0f, strip.eqFreq (f).load(), false);
        const float hz = f == EqFreq::Hpf ? 150.0f : f == EqFreq::Lpf ? 12000.0f
                       : f == EqFreq::Lf  ? 120.0f : f == EqFreq::Lm  ? 800.0f
                       : f == EqFreq::Hm  ? 4000.0f : 9000.0f;
        CHECK (strip.moveEqFreq (f, hz));
        CHECK_THAT (strip.eqFreq (f).load(), WithinAbs (hz, 0.0f));
        CHECK (strip.legacyDial (f).raw() == 0);
        CHECK (strip.eqEnabled.load());
    }

    for (const auto control : { &ChannelStripParams::lfGainDb, &ChannelStripParams::lmGainDb,
                                &ChannelStripParams::hmGainDb, &ChannelStripParams::hfGainDb,
                                &ChannelStripParams::lmQ, &ChannelStripParams::hmQ })
    {
        ChannelStripParams strip;
        strip.moveEqBand (strip.*control, 2.5f);
        CHECK_THAT ((strip.*control).load(), WithinAbs (2.5f, 0.0f));
        CHECK (strip.eqEnabled.load());
    }

    ChannelStripParams filters;
    CHECK_FALSE (filters.moveEqFreq (EqFreq::Hpf, ChannelStripParams::kHpfOffHz));
    CHECK_FALSE (filters.moveEqFreq (EqFreq::Lpf, ChannelStripParams::kLpfOffHz));
    CHECK_FALSE (filters.eqEnabled.load());
    CHECK (filters.moveEqFreq (EqFreq::Lpf, 8000.0f));
    CHECK (filters.lpfEnabled.load());
    CHECK (filters.eqEnabled.load());
    CHECK_FALSE (filters.moveEqFreq (EqFreq::Lpf, ChannelStripParams::kLpfOffHz));
    CHECK_FALSE (filters.lpfEnabled.load());
    CHECK (filters.eqEnabled.load());

    duskstudio::test::TempDirectory dir ("dusk-eq-move-load-");
    const auto file = dir.path() / "session.json";
    auto document = v7Session (false, kDials);
    document["tracks"][0]["eq"]["enabled"] = false;
    write (file, document);
    auto session = std::make_unique<Session>();
    REQUIRE (SessionSerializer::load (*session, file));
    const auto& loaded = session->track (0).strip;
    CHECK_THAT (loaded.lfGainDb.load(), WithinAbs (6.0f, 0.0f));
    CHECK (loaded.hpfEnabled.load());
    CHECK_FALSE (loaded.eqEnabled.load());
}
