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
