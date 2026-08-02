#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>

#include <cmath>

using duskstudio::AutomationParam;
using duskstudio::Session;
using duskstudio::SessionSerializer;
using Catch::Matchers::WithinAbs;

namespace
{
juce::File writeSession (const juce::String& json)
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("dusk-studio-clamp-"
                                    + juce::String (juce::Random::getSystemRandom().nextInt()));
    // Fail at the source if setup can't write the fixture, so a later
    // SessionSerializer::load failure can't be misread as the contract breaking.
    REQUIRE (dir.createDirectory().wasOk());
    auto target = dir.getChildFile ("session.json");
    REQUIRE (target.replaceWithText (json));
    return target;
}
} // namespace

// A hand-edited or truncated session.json can carry values the in-app UI never
// produces: negative sample times, out-of-range normalized values, and — via a
// finite double that exceeds float range (1e40) — values that overflow to inf
// when narrowed to float. The loader must sanitize these so they can't break
// the automation lane's binary search or push NaN/inf into a DSP parameter.
// This pins that contract; the matching loader logic lives in
// SessionSerializer's parseAutomationPoint + storeFiniteFloat helpers.
TEST_CASE ("SessionSerializer::load clamps corrupt automation + EQ values",
           "[session][serializer][corruption]")
{
    const auto target = writeSession (R"JSON(
    {
      "version": 3,
      "tracks": [
        {
          "automation": {
            "fader_db": [
              { "t": 96000,  "v": 0.25, "bpm": 120.0 },
              { "t": -48000, "v": 5.0,  "bpm": 1e40 }
            ]
          },
          "eq": {
            "lm": { "gain": 1e40, "freq": 1000.0, "q": 1.0 }
          }
        }
      ]
    }
    )JSON");

    Session s;
    // Seed an explicit, non-default session tempo so the per-point bpm fallback is
    // tested against a known value rather than coupling the assertion to whatever the
    // Session default tempo happens to be. The fixture JSON sets no transport tempo,
    // so this seeded value survives the load and is what the corrupt (+inf) point
    // falls back to.
    s.tempoBpm.store (100.0f);
    REQUIRE (SessionSerializer::load (s, target));

    const auto& pts = s.track (0).automationLanes[(size_t) AutomationParam::FaderDb].pointsConst();
    REQUIRE (pts.size() == 2);

    // Negative time clamped to >= 0 (the -48000 point), and the lane stays
    // sorted so the evaluator's binary search precondition holds.
    REQUIRE (pts[0].timeSamples >= 0);
    REQUIRE (pts[0].timeSamples <= pts[1].timeSamples);
    REQUIRE (pts[0].timeSamples == 0);
    REQUIRE (pts[1].timeSamples == 96000);

    // Out-of-range value clamped to [0, 1] (existing behaviour, pinned here).
    REQUIRE_THAT (pts[0].value, WithinAbs (1.0f, 1e-6f));
    REQUIRE_THAT (pts[1].value, WithinAbs (0.25f, 1e-6f));

    // Non-finite bpm rejected — falls back to the seeded session tempo (100).
    REQUIRE (std::isfinite (pts[0].recordedAtBPM));
    REQUIRE_THAT (pts[0].recordedAtBPM, WithinAbs (100.0f, 1e-3f));

    // Non-finite EQ gain rejected — the in-memory default (0 dB) is kept.
    const float lmGain = s.track (0).strip.lmGainDb.load();
    REQUIRE (std::isfinite (lmGain));
    REQUIRE_THAT (lmGain, WithinAbs (0.0f, 1e-6f));

    target.getParentDirectory().deleteRecursively();
}

// Two contracts on the master path: its automation lane must be time-sorted
// like the track / bus / aux paths, and a corrupt transport tempo (a literal
// that overflows to +inf on parse) must not flow into recordedAtBPM via the
// per-point bpm fallback.
TEST_CASE ("SessionSerializer::load sorts master automation + survives non-finite tempo",
           "[session][serializer][corruption]")
{
    const auto target = writeSession (R"JSON(
    {
      "version": 3,
      "transport": { "tempo_bpm": 1e40 },
      "master": {
        "automation": {
          "fader_db": [
            { "t": 96000, "v": 0.2 },
            { "t": 0,     "v": 0.8, "bpm": 100.0 }
          ]
        }
      }
    }
    )JSON");

    Session s;
    s.tempoBpm.store (90.0f);   // non-default finite tempo, seeded before load
    REQUIRE (SessionSerializer::load (s, target));

    // The corrupt (+inf) transport tempo must NOT overwrite the session tempo
    // (it would otherwise clamp to 300); the seeded 90 is preserved.
    REQUIRE_THAT (s.tempoBpm.load(), WithinAbs (90.0f, 1e-3f));

    const auto& pts = s.master().automationLanes[(size_t) AutomationParam::FaderDb].pointsConst();
    REQUIRE (pts.size() == 2);

    // Input was time-reversed; the master path must sort ascending.
    REQUIRE (pts[0].timeSamples == 0);
    REQUIRE (pts[1].timeSamples == 96000);

    // Non-finite session tempo never reaches recordedAtBPM. The point with a
    // per-point bpm keeps it (100); the one without inherits the preserved
    // session tempo (90), not the corrupt +inf transport value.
    REQUIRE (std::isfinite (pts[0].recordedAtBPM));
    REQUIRE (std::isfinite (pts[1].recordedAtBPM));
    REQUIRE_THAT (pts[0].recordedAtBPM, WithinAbs (100.0f, 1e-3f));
    REQUIRE_THAT (pts[1].recordedAtBPM, WithinAbs (90.0f, 1e-3f));

    target.getParentDirectory().deleteRecursively();
}

TEST_CASE ("SessionSerializer::load defaults present invalid master floats",
           "[session][serializer][corruption]")
{
    const auto target = writeSession (R"JSON(
    {
      "version": 3,
      "master": {
        "eq_lf_boost": "not-a-number",
        "eq_lf_atten": 3.5,
        "comp_thresh_db": 1e40
      }
    }
    )JSON");

    Session s;
    s.master().eqLfBoost.store (7.0f);
    s.master().eqLfAtten.store (8.0f);
    s.master().compThreshDb.store (-12.0f);
    s.master().compRatio.store (2.0f);

    REQUIRE (SessionSerializer::load (s, target));
    const duskstudio::MasterBusParams def;
    REQUIRE_THAT (s.master().eqLfBoost.load(), WithinAbs (def.eqLfBoost.load(), 1e-6f));
    REQUIRE_THAT (s.master().eqLfAtten.load(), WithinAbs (3.5f, 1e-6f));
    REQUIRE_THAT (s.master().compThreshDb.load(), WithinAbs (def.compThreshDb.load(), 1e-6f));
    // A missing key in a present master section retains the live value.
    REQUIRE_THAT (s.master().compRatio.load(), WithinAbs (2.0f, 1e-6f));

    target.getParentDirectory().deleteRecursively();
}

TEST_CASE ("SessionSerializer::load defaults present invalid master bools",
           "[session][serializer][corruption]")
{
    const auto target = writeSession (R"JSON(
    {
      "version": 3,
      "master": {
        "eq_enabled": "maybe",
        "comp_enabled": true,
        "comp_release_auto": "maybe"
      }
    }
    )JSON");

    Session s;
    s.master().eqEnabled.store (true);
    s.master().compEnabled.store (false);
    s.master().compReleaseAuto.store (false);

    REQUIRE (SessionSerializer::load (s, target));
    // Model defaults, not a blanket false: auto-release defaults to ON, so a
    // corrupt value must not silently switch the bus comp to manual release.
    REQUIRE (s.master().eqEnabled.load() == false);
    REQUIRE (s.master().compEnabled.load() == true);
    REQUIRE (s.master().compReleaseAuto.load() == true);

    target.getParentDirectory().deleteRecursively();
}

TEST_CASE ("SessionSerializer::load treats a non-object master as absent",
           "[session][serializer][corruption]")
{
    const auto target = writeSession (R"JSON(
    {
      "version": 3,
      "master": 5
    }
    )JSON");

    Session s;
    s.master().eqLfBoost.store (7.0f);
    s.master().compRatio.store (9.0f);
    s.master().compReleaseAuto.store (false);

    REQUIRE (SessionSerializer::load (s, target));
    // "master" is not an object, so the section never parsed - every field has
    // to fall back to the model default rather than inherit what the previously
    // loaded session left in the live Session.
    const duskstudio::MasterBusParams def;
    REQUIRE_THAT (s.master().eqLfBoost.load(), WithinAbs (def.eqLfBoost.load(), 1e-6f));
    REQUIRE_THAT (s.master().compRatio.load(), WithinAbs (def.compRatio.load(), 1e-6f));
    REQUIRE (s.master().compReleaseAuto.load() == def.compReleaseAuto.load());

    target.getParentDirectory().deleteRecursively();
}

// Strip, comp and hardware-insert values feed DSP directly. A hand-edited file
// can carry a fader of 1e39 (inf once narrowed), a pan of 3.0, or an aux send
// that is not a number at all. None of them may reach the mix, and a junk aux
// send in particular must resolve to OFF: 0 dB is a unity send, so the corrupt
// file would come back feedback-loud.
TEST_CASE ("SessionSerializer::load clamps corrupt strip and hardware values",
           "[session][serializer][corruption]")
{
    const auto target = writeSession (R"JSON(
    {
      "version": 3,
      "tracks": [
        {
          "fader_db": 1e40,
          "pan": 3.0,
          "bus_assign": [true],
          "aux_send_db": ["bad", -12.0, 1e40, -60.0],
          "hpf": { "freq": -5.0 },
          "lpf": { "freq": 1e40 },
          "comp": { "mode": 99, "vca_ratio": 500.0, "fet_attack": 0.0 },
          "hardware_insert": { "output_gain_db": 100.0, "dry_wet": -1.0 }
        }
      ],
      "buses": [ { "fader_db": -200.0, "comp_ratio": 99.0, "eq_lf_db": 50.0 } ]
    }
    )JSON");

    Session s;
    auto& track = s.track (0);
    for (auto& assigned : track.strip.busAssign) assigned.store (true);
    for (auto& level : track.strip.auxSendDb)    level.store (-6.0f);

    REQUIRE (SessionSerializer::load (s, target));

    REQUIRE_THAT (track.strip.faderDb.load(), WithinAbs (0.0f, 1e-6f));
    REQUIRE_THAT (track.strip.pan.load(), WithinAbs (1.0f, 1e-6f));
    REQUIRE (track.strip.busAssign[0].load());
    for (size_t i = 1; i < track.strip.busAssign.size(); ++i)
        REQUIRE_FALSE (track.strip.busAssign[i].load());

    // "bad" and the overflowing literal collapse to OFF, not unity. -60 sits AT
    // the knob floor, which the strip itself stores as the OFF sentinel.
    REQUIRE_THAT (track.strip.auxSendDb[0].load(), WithinAbs (-100.0f, 1e-6f));
    REQUIRE_THAT (track.strip.auxSendDb[1].load(), WithinAbs (-12.0f, 1e-6f));
    REQUIRE_THAT (track.strip.auxSendDb[2].load(), WithinAbs (-100.0f, 1e-6f));
    REQUIRE_THAT (track.strip.auxSendDb[3].load(), WithinAbs (-100.0f, 1e-6f));

    REQUIRE_THAT (track.strip.hpfFreq.load(), WithinAbs (20.0f, 1e-6f));
    REQUIRE_THAT (track.strip.lpfFreq.load(), WithinAbs (20000.0f, 1e-3f));
    REQUIRE (track.strip.compMode.load() == 2);
    REQUIRE_THAT (track.strip.compVcaRatio.load(), WithinAbs (120.0f, 1e-6f));
    REQUIRE_THAT (track.strip.compFetAttack.load(), WithinAbs (0.02f, 1e-6f));
    REQUIRE_THAT (track.hardwareInsert.outputGainDb.load(), WithinAbs (12.0f, 1e-6f));
    REQUIRE_THAT (track.hardwareInsert.dryWet.load(), WithinAbs (0.0f, 1e-6f));

    const auto& bus = s.bus (0).strip;
    REQUIRE_THAT (bus.faderDb.load(), WithinAbs (-100.0f, 1e-6f));
    REQUIRE_THAT (bus.compRatio.load(), WithinAbs (10.0f, 1e-6f));
    REQUIRE_THAT (bus.eqLfGainDb.load(), WithinAbs (9.0f, 1e-6f));

    target.getParentDirectory().deleteRecursively();
}
