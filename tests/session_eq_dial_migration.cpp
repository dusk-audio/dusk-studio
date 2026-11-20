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
// of the 4K EQ core. Formats 8 to 11 stored the Hz each played and kept the
// dial beside it. Format 12 stores a band's frequency marking, which the strip
// plays through the core's dial law as 4K EQ 2 plays its parameter, and a
// filter's Hz. A v7 band's dial is its marking. A v7 filter loads at the Hz its
// dial played and keeps the dial, which the strip plays through the core's dial
// API until the frequency moves, so it sounds exactly as it did. A format 8 to
// 11 band takes the dial it kept, or the marking that plays its Hz.

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

// Dial positions inside each band's and filter's measured table, where the
// filters' Hz API plays exactly what their dial API played.
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

Json read (const std::filesystem::path& file)
{
    std::ifstream stream (file);
    return Json::parse (stream, nullptr, false);
}

// The format-7 dial a strip plays a filter through, or 0 when it follows its Hz.
float dialOf (const ChannelStripParams& strip, EqFreq filter)
{
    float hz = 0.0f;
    const auto& dial = filter == EqFreq::Hpf ? strip.hpfDial : strip.lpfDial;
    return dial.dialFor (strip.eqFreq (filter), strip.eqBlackMode.load(), hz);
}

Bands stored (const ChannelStripParams& strip)
{
    return { strip.lfFreq.load(), strip.lmFreq.load(), strip.hmFreq.load(), strip.hfFreq.load(),
             strip.hpfFreq.load(), strip.lpfFreq.load() };
}

// The marking a band's dial loads as: the dial, held at the ends of the core's
// anchor table.
float heldMarking (EQ::Band band, float dial)
{
    const float* anchors = EQ::frequencyControlAnchors (band);
    return std::clamp (dial, anchors[0], anchors[EQ::kControlAnchorCount - 1]);
}

bool bitIdentical (const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0;
}

double worstDifference (const std::vector<float>& a, const std::vector<float>& b)
{
    double worst = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (! std::isfinite (a[i]) || ! std::isfinite (b[i]))
            return std::numeric_limits<double>::infinity();
        worst = std::max (worst, (double) std::abs (a[i] - b[i]));
    }
    return worst;
}

// Every band through the dial API, and every filter too or, with filtersInHz,
// through the Hz API. only: the one band boosted or cut, or -1 for all four.
std::vector<float> render (bool black, const Bands& bands, bool filtersInHz, int only = -1)
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
    eq.setLfFreq (bands.lf); eq.setLmFreq (bands.lm);
    eq.setHmFreq (bands.hm); eq.setHfFreq (bands.hf);
    eq.setHpfEnabled (bands.hpf > 0.0f);
    eq.setLpfEnabled (bands.lpf > 0.0f);
    if (filtersInHz)
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

TEST_CASE ("A v7 session's band dials load as their markings and its filter dials as the Hz they played",
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
        const auto loaded = stored (strip);

        CHECK_THAT (loaded.lf, WithinAbs (kDials.lf, 0.0));
        CHECK_THAT (loaded.lm, WithinAbs (kDials.lm, 0.0));
        CHECK_THAT (loaded.hm, WithinAbs (kDials.hm, 0.0));
        CHECK_THAT (loaded.hf, WithinAbs (kDials.hf, 0.0));
        CHECK_THAT (loaded.hpf, WithinRel (EQ::hzForCalibratedFilterControl (kDials.hpf, true, black), 1.0e-6f));
        CHECK_THAT (loaded.lpf, WithinRel (EQ::hzForCalibratedFilterControl (kDials.lpf, false, black), 1.0e-6f));
        CHECK (strip.hpfEnabled.load());
        CHECK (strip.lpfEnabled.load());
        // A migration that kept the filter dials as their frequencies would
        // pass the checks above only if the dial law were the identity.
        CHECK (std::abs (loaded.hpf - kDials.hpf) > 30.0f);
        CHECK (std::abs (loaded.lpf - kDials.lpf) > 1500.0f);
        CHECK_THAT (dialOf (strip, EqFreq::Hpf), WithinAbs (kDials.hpf, 0.0));
        CHECK_THAT (dialOf (strip, EqFreq::Lpf), WithinAbs (kDials.lpf, 0.0));

        const auto before = render (black, kDials, false);
        const auto after  = render (black, loaded, true);
        CHECK_THAT (worstDifference (before, after), WithinAbs (0.0, 1.0e-6));

        REQUIRE (SessionSerializer::save (*session, file));
        const auto saved = read (file);
        REQUIRE (saved.is_object());
        CHECK (saved["version"].get<int>() == 12);
        CHECK_THAT (saved["tracks"][0]["eq"]["hf"]["freq"].get<float>(), WithinAbs (kDials.hf, 0.0));
        for (const char* band : { "lf", "lm", "hm", "hf" })
            CHECK_FALSE (saved["tracks"][0]["eq"][band].contains ("freq_dial"));
        CHECK_THAT (saved["tracks"][0]["hpf"]["freq"].get<float>(), WithinRel (loaded.hpf, 1.0e-6f));
        CHECK_THAT (saved["tracks"][0]["hpf"]["freq_dial"].get<float>(), WithinAbs (kDials.hpf, 0.0));

        // A format 12 file is converted already: loading it again must not
        // convert twice.
        auto reloaded = std::make_unique<Session>();
        REQUIRE (SessionSerializer::load (*reloaded, file));
        const auto again = stored (reloaded->track (0).strip);
        CHECK_THAT (again.lf, WithinAbs (loaded.lf, 0.0));
        CHECK_THAT (again.lm, WithinAbs (loaded.lm, 0.0));
        CHECK_THAT (again.hm, WithinAbs (loaded.hm, 0.0));
        CHECK_THAT (again.hf, WithinAbs (loaded.hf, 0.0));
        CHECK_THAT (again.hpf, WithinRel (loaded.hpf, 1.0e-6f));
        CHECK_THAT (again.lpf, WithinRel (loaded.lpf, 1.0e-6f));
    }
}

// Every Hz a v7 HPF dial could play, over v7's range in either voicing, has to
// fit what the loader holds, or a converted session would move on load.
TEST_CASE ("The held HPF range takes every Hz a v7 filter dial played", "[session][migration][eq]")
{
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

TEST_CASE ("A v7 band dial past today's knob loads at the knob's end and plays as it did",
           "[session][serializer][migration][eq]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    // Black LM at the old 100 bottom, HM at the old 13000 top and HF at the old
    // 1 kHz bottom sit past today's knobs, where the core's dial law has
    // already gone flat, so each loads at its knob's end and plays exactly
    // what it played. LF at the old 400 top is on its knob. A dial past v7's
    // own range was clamped by the v7 loader before it played, so it converts
    // from that clamp.
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

    const auto held = stored (session->track (0).strip);
    const Bands loaded { held.lf, held.lm, held.hm, held.hf };
    CHECK_THAT (loaded.lf, WithinAbs (400.0f, 0.0));
    CHECK_THAT (loaded.lm, WithinAbs (ChannelStripParams::kLmFreqMin, 0.0));
    CHECK_THAT (loaded.hm, WithinAbs (ChannelStripParams::kHmFreqMax, 0.0));
    CHECK_THAT (loaded.hf, WithinAbs (ChannelStripParams::kHfFreqMin, 0.0));
    for (int band = -1; band < 4; ++band)
    {
        CAPTURE (band);
        CHECK (bitIdentical (render (true, edges, false, band), render (true, loaded, false, band)));
    }

    const auto& brown = session->track (1).strip;
    CHECK_THAT (brown.lfFreq.load(), WithinAbs (ChannelStripParams::kLfFreqMin, 0.0));
    CHECK_THAT (brown.hfFreq.load(), WithinAbs (ChannelStripParams::kHfFreqMax, 0.0));

    REQUIRE (SessionSerializer::save (*session, file));
    auto reloaded = std::make_unique<Session>();
    REQUIRE (SessionSerializer::load (*reloaded, file));
    const auto again = stored (reloaded->track (0).strip);
    CHECK_THAT (again.lf, WithinAbs (loaded.lf, 0.0));
    CHECK_THAT (again.lm, WithinAbs (loaded.lm, 0.0));
    CHECK_THAT (again.hm, WithinAbs (loaded.hm, 0.0));
    CHECK_THAT (again.hf, WithinAbs (loaded.hf, 0.0));
}

TEST_CASE ("A v7 filter at OFF stays off and one past today's range keeps its sound",
           "[session][serializer][migration][eq]")
{
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
// Band dials past or at the ends of each band's measured table, which load at
// their knob's end, and dials inside them. The filters' two APIs differ
// anywhere, so every filter keeps its dial.
constexpr Bands kEdgeDials   {   25.0f, 3000.0f, 10000.0f, 17653.0f,  28.0f, 17000.0f };
constexpr Bands kInsideDials {  100.0f,  600.0f,  3000.0f, 10000.0f, 300.0f,  9000.0f };

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
    { false, kEdgeDials }, { true, kEdgeDials }, { false, kInsideDials }, { true, kInsideDials },
};

Json v7DialSession()
{
    Json document { { "version", 7 }, { "tracks", Json::array() } };
    for (const auto& t : kDialTracks)
        document["tracks"].push_back (v7Strip (t.black, t.dials));
    return document;
}

constexpr EqFreq kAllFreqs[] { EqFreq::Hpf, EqFreq::Lpf, EqFreq::Lf, EqFreq::Lm, EqFreq::Hm, EqFreq::Hf };
constexpr EqFreq kFilters[] { EqFreq::Hpf, EqFreq::Lpf };

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

// The strip's rates at 48 kHz: 1x and 4x Effect oversampling.
constexpr double kStripRates[] { 48000.0, 192000.0 };

// Every strip of a session loaded from v7DialSession plays its original dials:
// each band holds its dial as its marking, and each filter keeps its dial.
void checkPlaysOriginalDials (const duskstudio::Session& session, const char* what)
{
    for (size_t t = 0; t < std::size (kDialTracks); ++t)
    {
        const auto& track = kDialTracks[t];
        const auto& strip = session.track ((int) t).strip;
        CAPTURE (what, t, track.black);
        CHECK_THAT (dialOf (strip, EqFreq::Hpf), WithinAbs (track.dials.hpf, 0.0));
        CHECK_THAT (dialOf (strip, EqFreq::Lpf), WithinAbs (track.dials.lpf, 0.0));
        CHECK_THAT (strip.lfFreq.load(), WithinAbs (heldMarking (EQ::Band::LF, track.dials.lf), 0.0));
        CHECK_THAT (strip.lmFreq.load(), WithinAbs (heldMarking (EQ::Band::LM, track.dials.lm), 0.0));
        CHECK_THAT (strip.hmFreq.load(), WithinAbs (heldMarking (EQ::Band::HM, track.dials.hm), 0.0));
        CHECK_THAT (strip.hfFreq.load(), WithinAbs (heldMarking (EQ::Band::HF, track.dials.hf), 0.0));
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

    // The filters' knobs read the Hz each dial played, or stop a hertz short
    // of OFF where it played past it.
    const auto& insideBrown = session->track (2).strip;
    CHECK_THAT (insideBrown.hpfFreq.load(), WithinRel (EQ::hzForCalibratedFilterControl (kInsideDials.hpf, true, false), 1.0e-6f));
    CHECK_THAT (insideBrown.lpfFreq.load(), WithinRel (EQ::hzForCalibratedFilterControl (kInsideDials.lpf, false, false), 1.0e-6f));
    const auto& edgeBrown = session->track (0).strip;
    CHECK_THAT (edgeBrown.hpfFreq.load(), WithinAbs (ChannelStripParams::kHpfOffHz + 1.0f, 0.0));
    CHECK_THAT (edgeBrown.lpfFreq.load(), WithinAbs (ChannelStripParams::kLpfOffHz - 1.0f, 0.0));

    // Saved with each filter's dial beside its Hz and each band's marking
    // alone, reloaded, and still bit for bit.
    REQUIRE (SessionSerializer::save (*session, file));
    {
        const auto saved = read (file);
        REQUIRE (saved.is_object());
        const auto& edge = saved["tracks"][0];
        CHECK_THAT (edge["eq"]["hm"]["freq"].get<float>(), WithinAbs (ChannelStripParams::kHmFreqMax, 0.0));
        CHECK_FALSE (edge["eq"]["hm"].contains ("freq_dial"));
        CHECK_THAT (edge["hpf"]["freq_dial"].get<double>(), WithinAbs (kEdgeDials.hpf, 0.0));
        CHECK_THAT (edge["hpf"]["freq"].get<float>(), WithinRel (edgeBrown.hpfFreq.load(), 1.0e-6f));
        CHECK_FALSE (saved["tracks"][4]["hpf"].contains ("freq_dial"));
    }
    auto reloaded = std::make_unique<Session>();
    REQUIRE (SessionSerializer::load (*reloaded, file));
    checkPlaysOriginalDials (*reloaded, "reloaded");
}

TEST_CASE ("Moving a band or a converted filter changes only what it plays",
           "[session][migration][eq]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    duskstudio::test::TempDirectory dir ("dusk-eq-dial-move-");
    const auto file = dir.path() / "session.json";
    write (file, v7DialSession());
    auto session = std::make_unique<Session>();
    REQUIRE (SessionSerializer::load (*session, file));

    // A moved filter leaves its dial for its Hz; a moved band plays its new
    // marking through the dial law. Nothing else changes.
    for (const auto moved : kAllFreqs)
    {
        CAPTURE ((int) moved);
        auto& strip = session->track (1).strip;
        const auto& dials = kDialTracks[1].dials;
        const auto hpfWord = strip.hpfDial.raw(), lpfWord = strip.lpfDial.raw();
        const float held = strip.eqFreq (moved).load();
        const float to = moved == EqFreq::Hpf ? 150.0f : moved == EqFreq::Lpf ? 12000.0f
                       : moved == EqFreq::Lf  ? 120.0f : moved == EqFreq::Lm  ? 800.0f
                       : moved == EqFreq::Hm  ? 4000.0f : 9000.0f;
        strip.setEqFreq (moved, to);

        for (const auto f : kFilters)
            CHECK_THAT (dialOf (strip, f), WithinAbs (f == moved ? 0.0f : f == EqFreq::Hpf ? dials.hpf : dials.lpf, 0.0));
        for (const double rate : kStripRates)
        {
            CAPTURE (rate);
            const auto reference = renderAsStrip (strip, rate, [&dials, moved, to] (EQ& eq)
            {
                setDials (eq, dials, (int) moved);
                switch (moved)
                {
                    case EqFreq::Hpf: eq.setHpfFreqHz (to); break;
                    case EqFreq::Lpf: eq.setLpfFreqHz (to); break;
                    case EqFreq::Lf:  eq.setLfFreq (to); break;
                    case EqFreq::Lm:  eq.setLmFreq (to); break;
                    case EqFreq::Hm:  eq.setHmFreq (to); break;
                    case EqFreq::Hf:  eq.setHfFreq (to); break;
                }
            });
            CHECK (bitIdentical (renderAsStrip (strip, rate), reference));
        }

        // A filter moved back onto its converted Hz stays on its Hz.
        strip.setEqFreq (moved, held);
        if (moved == EqFreq::Hpf || moved == EqFreq::Lpf)
            CHECK_THAT (dialOf (strip, moved), WithinAbs (0.0, 0.0));
        strip.hpfDial.setRaw (hpfWord);
        strip.lpfDial.setRaw (lpfWord);
    }
}

TEST_CASE ("A converted filter dial plays only in the voicing it was converted in, and a band keeps its marking",
           "[session][migration][eq]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    duskstudio::test::TempDirectory dir ("dusk-eq-dial-voicing-");
    const auto file = dir.path() / "session.json";
    write (file, v7DialSession());
    auto session = std::make_unique<Session>();
    REQUIRE (SessionSerializer::load (*session, file));

    // A Brown filter dial through Black would play neither the old sound nor
    // what the knob shows; the switch plays the knob's Hz, and switching back
    // the dial. A band keeps its marking, which plays where Black's dial law
    // puts it, as 4K EQ 2's knob does.
    auto& strip = session->track (0).strip;
    const auto markings = stored (strip);
    strip.eqBlackMode.store (true);
    for (const auto f : kFilters)
        CHECK_THAT (dialOf (strip, f), WithinAbs (0.0, 0.0));
    const auto switched = stored (strip);
    CHECK_THAT (switched.lf, WithinAbs (markings.lf, 0.0));
    CHECK_THAT (switched.lm, WithinAbs (markings.lm, 0.0));
    CHECK_THAT (switched.hm, WithinAbs (markings.hm, 0.0));
    CHECK_THAT (switched.hf, WithinAbs (markings.hf, 0.0));
    const auto black = renderAsStrip (strip, 48000.0);
    const auto blackReference = renderAsStrip (strip, 48000.0, [&markings] (EQ& eq)
    {
        eq.setLfFreq (markings.lf); eq.setLmFreq (markings.lm);
        eq.setHmFreq (markings.hm); eq.setHfFreq (markings.hf);
        eq.setHpfFreqHz (markings.hpf); eq.setLpfFreqHz (markings.lpf);
    });
    CHECK (bitIdentical (black, blackReference));
    strip.eqBlackMode.store (false);
    CHECK_THAT (dialOf (strip, EqFreq::Hpf), WithinAbs (kEdgeDials.hpf, 0.0));
    CHECK_THAT (dialOf (strip, EqFreq::Lpf), WithinAbs (kEdgeDials.lpf, 0.0));

    // A save while switched keeps each filter dial with the voicing it plays
    // in: the reopened track plays its filters' Hz in Black, and switched back
    // to Brown plays its original dials bit for bit.
    strip.eqBlackMode.store (true);
    REQUIRE (SessionSerializer::save (*session, file));
    {
        const auto saved = read (file);
        REQUIRE (saved.is_object());
        CHECK (saved["tracks"][0]["eq"]["type"] == "black");
        CHECK (saved["tracks"][0]["hpf"]["freq_dial_type"] == "brown");
        CHECK (saved["tracks"][1]["hpf"]["freq_dial_type"] == "black");
        CHECK_FALSE (saved["tracks"][0]["eq"]["hm"].contains ("freq_dial_type"));
    }
    auto reloaded = std::make_unique<Session>();
    REQUIRE (SessionSerializer::load (*reloaded, file));
    auto& reopened = reloaded->track (0).strip;
    REQUIRE (reopened.eqBlackMode.load());
    for (const auto f : kFilters)
    {
        CAPTURE ((int) f);
        CHECK_THAT (dialOf (reopened, f), WithinAbs (0.0, 0.0));
        CHECK ((f == EqFreq::Hpf ? reopened.hpfDial : reopened.lpfDial).raw() != 0);
    }
    reopened.eqBlackMode.store (false);
    checkPlaysOriginalDials (*reloaded, "saved switched, reopened and switched back");
}

// Formats 8 to 11 stored each band's Hz, with the dial the v7 -> v8 step kept
// beside it while the band still played it. A band carrying a dial the
// migrator kept for its Hz in the track's voicing takes that dial as its
// marking. Any other band, one whose dial a voicing switch suspended
// included, takes the marking whose band the dial law plays where the Hz API
// played the Hz in the track's voicing, held at the ends of the table. A filter keeps its Hz, and a dial only beside the
// frequency the migrator kept it for.
TEST_CASE ("A format 8 to 11 band takes its kept dial as its marking, or the marking that plays its Hz",
           "[session][serializer][migration][eq]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    duskstudio::test::TempDirectory dir ("dusk-eq-dial-v8-");
    const auto legacy = dir.path() / "legacy.json";
    write (legacy, v7DialSession());
    auto session = std::make_unique<Session>();
    REQUIRE (SessionSerializer::load (*session, legacy));
    REQUIRE (dialOf (session->track (3).strip, EqFreq::Hpf) > 0.0f);

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
            // HF at 9 kHz is past where Brown's dial law reaches.
            { { "eq", { { "type", "brown" },
                        { "lf", { { "freq", 300.0 }, { "freq_dial", 99999.0 } } },
                        { "lm", { { "freq", 700.0 }, { "freq_dial", -5.0 } } },
                        { "hm", { { "freq", 3000.0 }, { "freq_dial", "loud" } } },
                        { "hf", { { "freq", 9000.0 }, { "freq_dial", 1.0e300 } } } } },
              { "hpf", { { "enabled", true }, { "freq", 90.0 }, { "freq_dial", 0.0 } } },
              { "lpf", { { "enabled", true }, { "freq", 9000.0 }, { "freq_dial", nullptr } } } },
            // Dials in range beside a frequency they did not play: 600 under
            // a 700 Hz LM, which it would play near 412 Hz; Black's HM dial
            // beside Brown's Hz; a dial in neither voicing; an HPF dial at OFF
            // beside another OFF frequency; an LPF dial beside another's Hz.
            // In Black, as the track's own voicing.
            { { "eq", { { "type", "black" },
                        { "lf", { { "freq", 150.0 }, { "freq_dial", 100.0 }, { "freq_dial_type", "grey" } } },
                        { "lm", { { "freq", 700.0 }, { "freq_dial", 600.0 } } },
                        { "hm", { { "freq", hmBrown }, { "freq_dial", 3000.0 }, { "freq_dial_type", "black" } } } } },
              { "hpf", { { "enabled", true }, { "freq", 20.0 }, { "freq_dial", 20.4 } } },
              { "lpf", { { "enabled", true }, { "freq", 9000.0 }, { "freq_dial", 12000.0 } } } },
        }) }
    };
    const auto file = dir.path() / "session.json";
    write (file, document);
    REQUIRE (SessionSerializer::load (*session, file));

    const auto& kept = session->track (0).strip;
    CHECK_THAT (kept.lfFreq.load(), WithinAbs (100.0, 0.0));
    CHECK_THAT (kept.lmFreq.load(), WithinAbs (600.0, 0.0));
    CHECK (kept.hmFreq.load() != 3000.0f);
    CHECK_THAT (kept.hfFreq.load(), WithinAbs (ChannelStripParams::kHfFreqDefault, 0.0));
    CHECK_THAT (dialOf (kept, EqFreq::Lpf), WithinAbs (19999.6f, 0.0));
    {
        // Kept for Black on a Brown track: suspended until it switches.
        float hz = 0.0f;
        bool black = false;
        CHECK_THAT (dialOf (kept, EqFreq::Hpf), WithinAbs (0.0, 0.0));
        CHECK_THAT (kept.hpfDial.kept (kept.hpfFreq, hz, black), WithinAbs (80.0, 0.0));
        CHECK (black);
    }

    // The marking whose band sits where the Hz API put a band asked for hz: a
    // bell's design frequency is hz, and a shelf's the one the Hz API derives
    // from its corner. Gain moves both alike, so the reference gain stands for
    // all.
    const auto playsHz = [] (float marking, float hz, EQ::Band band, bool black)
    {
        const bool bell = band == EQ::Band::LM || band == EQ::Band::HM;
        const float played = EQ::calibratedEqFrequency (marking, EQ::kEqReferenceGainDb, band, black, bell);
        const float asked = EQ::calibratedEqFrequencyForHz (hz, EQ::kEqReferenceGainDb, band, black, bell);
        return std::abs (played - asked) <= 1.0e-4f * asked;
    };
    CHECK (playsHz (kept.hmFreq.load(), hmBlack, EQ::Band::HM, false));
    const auto& unkept = session->track (1).strip;
    CHECK (playsHz (unkept.lfFreq.load(), 300.0f, EQ::Band::LF, false));
    CHECK (playsHz (unkept.lmFreq.load(), 700.0f, EQ::Band::LM, false));
    CHECK (playsHz (unkept.hmFreq.load(), 3000.0f, EQ::Band::HM, false));
    CHECK (EQ::hzForCalibratedEqControl (ChannelStripParams::kHfFreqMax, EQ::Band::HF, false, false) < 9000.0f);
    CHECK_THAT (unkept.hfFreq.load(), WithinAbs (ChannelStripParams::kHfFreqMax, 0.0));
    // Not the Hz kept as a marking: the law is not the identity.
    CHECK (std::abs (unkept.lmFreq.load() - 700.0f) > 50.0f);

    const auto& mismatched = session->track (2).strip;
    CHECK (playsHz (mismatched.lfFreq.load(), 150.0f, EQ::Band::LF, true));
    CHECK (playsHz (mismatched.lmFreq.load(), 700.0f, EQ::Band::LM, true));
    CHECK (playsHz (mismatched.hmFreq.load(), hmBrown, EQ::Band::HM, true));
    CHECK (std::abs (mismatched.lmFreq.load() - 600.0f) > 50.0f);

    for (const int t : { 1, 2 })
    {
        CAPTURE (t);
        CHECK (session->track (t).strip.hpfDial.raw() == 0);
        CHECK (session->track (t).strip.lpfDial.raw() == 0);
    }
    CHECK_THAT (session->track (1).strip.hpfFreq.load(), WithinAbs (90.0, 0.0));

    // A track the file does not describe follows its Hz: nothing survives from
    // the session loaded before.
    CHECK (session->track (3).strip.hpfDial.raw() == 0);
    CHECK (session->track (3).strip.lpfDial.raw() == 0);

    // Saved as format 12: markings alone, with no dial beside any band.
    REQUIRE (SessionSerializer::save (*session, file));
    const auto saved = read (file);
    REQUIRE (saved.is_object());
    for (std::size_t t = 0; t < 3; ++t)
        for (const char* band : { "lf", "lm", "hm", "hf" })
        {
            CAPTURE (t, band);
            CHECK_FALSE (saved["tracks"][t]["eq"][band].contains ("freq_dial"));
            CHECK_FALSE (saved["tracks"][t]["eq"][band].contains ("freq_dial_type"));
        }
    CHECK_THAT (saved["tracks"][0]["eq"]["lm"]["freq"].get<float>(), WithinAbs (600.0, 0.0));
}

// Every move of a band or filter, from its knob, a MIDI binding or a control
// surface, goes through ChannelStripParams' move helpers: it drops a filter's
// dial as setEqFreq does and engages a bypassed EQ. A filter engages it once
// it leaves OFF, and turned back to OFF it switches off and leaves the EQ as
// it is. A load stores its values directly, so a session saved bypassed stays
// bypassed.
TEST_CASE ("Moving a band or filter engages the channel EQ, and loading a session does not",
           "[session][eq]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    for (const auto f : kAllFreqs)
    {
        CAPTURE ((int) f);
        ChannelStripParams strip;
        strip.hpfDial.set (100.0f, strip.hpfFreq.load(), false);
        strip.lpfDial.set (9000.0f, strip.lpfFreq.load(), false);
        const float to = f == EqFreq::Hpf ? 150.0f : f == EqFreq::Lpf ? 12000.0f
                       : f == EqFreq::Lf  ? 120.0f : f == EqFreq::Lm  ? 800.0f
                       : f == EqFreq::Hm  ? 4000.0f : 9000.0f;
        CHECK (strip.moveEqFreq (f, to));
        CHECK_THAT (strip.eqFreq (f).load(), WithinAbs (to, 0.0f));
        CHECK ((strip.hpfDial.raw() == 0) == (f == EqFreq::Hpf));
        CHECK ((strip.lpfDial.raw() == 0) == (f == EqFreq::Lpf));
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
