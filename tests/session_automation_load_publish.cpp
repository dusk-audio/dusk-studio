#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>

using Catch::Matchers::WithinAbs;
using duskstudio::AutomationParam;
using duskstudio::AutomationPoint;
using duskstudio::Session;
using duskstudio::SessionSerializer;

namespace
{
juce::File makeTempSessionDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("dusk-studio-automation-load-"
                                    + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    return dir;
}

AutomationPoint pt (juce::int64 t, float v)
{
    AutomationPoint p;
    p.timeSamples   = t;
    p.value         = v;
    p.recordedAtBPM = 120.0f;
    return p;
}
} // namespace

// Loading a session into a Session whose lanes already hold points is the
// mid-run "open another session" path: the audio thread can be holding a
// lane's read() pointer across the load. AtomicSnapshot retires exactly one
// previous value, so the load must publish each lane exactly ONCE - a
// clear-then-publish pair on the same lane frees the pre-load vector while
// it may still be read. This pins the observable half of that contract:
// after a load, the pre-load vector must still be alive (retired, not
// destroyed). Under the old double-publish load this dereference is
// use-after-free - ASAN/TSan builds catch it even when a plain build
// happens to pass.
TEST_CASE ("Session load publishes each automation lane exactly once",
           "[session][serializer][automation]")
{
    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");

    Session a;
    a.track (0).automationLanes[(size_t) AutomationParam::FaderDb]
        .publishPoints ({ pt (0, 0.1f), pt (48000, 0.9f), pt (96000, 0.5f) });
    a.master().automationLanes[(size_t) AutomationParam::FaderDb]
        .publishPoints ({ pt (0, 0.7f) });
    REQUIRE (SessionSerializer::save (a, target));

    Session b;
    auto& trackLaneLoaded = b.track (0).automationLanes[(size_t) AutomationParam::FaderDb];
    auto& trackLaneAbsent = b.track (1).automationLanes[(size_t) AutomationParam::Pan];
    auto& masterLane      = b.master().automationLanes[(size_t) AutomationParam::FaderDb];
    auto& auxLaneAbsent   = b.auxLane (0).params.automationLanes[(size_t) AutomationParam::FaderDb];
    auto& busLaneAbsent   = b.bus (0).strip.automationLanes[(size_t) AutomationParam::FaderDb];

    trackLaneLoaded.publishPoints ({ pt (10, 0.2f), pt (20, 0.3f) });
    trackLaneAbsent.publishPoints ({ pt (30, 0.4f) });
    masterLane     .publishPoints ({ pt (40, 0.5f), pt (50, 0.6f), pt (60, 0.7f), pt (70, 0.8f) });
    auxLaneAbsent  .publishPoints ({ pt (80, 0.9f) });
    busLaneAbsent  .publishPoints ({ pt (90, 1.0f), pt (95, 0.0f) });

    const auto* preTrackLoaded = trackLaneLoaded.snapshot.read();
    const auto* preTrackAbsent = trackLaneAbsent.snapshot.read();
    const auto* preMaster      = masterLane.snapshot.read();
    const auto* preAux         = auxLaneAbsent.snapshot.read();
    const auto* preBus         = busLaneAbsent.snapshot.read();

    REQUIRE (SessionSerializer::load (b, target));

    // Loaded contents are correct.
    REQUIRE (trackLaneLoaded.pointsConst().size() == 3);
    REQUIRE (trackLaneLoaded.pointsConst()[0].timeSamples == 0);
    REQUIRE_THAT (trackLaneLoaded.pointsConst()[2].value, WithinAbs (0.5f, 1e-4f));
    REQUIRE (masterLane.pointsConst().size() == 1);

    // Lanes absent from the JSON come back empty - the load must still
    // overwrite whatever the previous session left in them.
    REQUIRE (trackLaneAbsent.pointsConst().empty());
    REQUIRE (auxLaneAbsent.pointsConst().empty());
    REQUIRE (busLaneAbsent.pointsConst().empty());

    // Every touched lane swapped its audio-visible pointer once...
    REQUIRE (trackLaneLoaded.snapshot.read() != preTrackLoaded);
    REQUIRE (trackLaneAbsent.snapshot.read() != preTrackAbsent);
    REQUIRE (masterLane.snapshot.read()      != preMaster);
    REQUIRE (auxLaneAbsent.snapshot.read()   != preAux);
    REQUIRE (busLaneAbsent.snapshot.read()   != preBus);

    // ...and the pre-load vectors are still alive as the retired value:
    // exactly one publish per lane. Sizes must match what was published
    // before the load.
    REQUIRE (preTrackLoaded->size() == 2);
    REQUIRE (preTrackAbsent->size() == 1);
    REQUIRE (preMaster->size() == 4);
    REQUIRE (preAux->size() == 1);
    REQUIRE (preBus->size() == 2);

    dir.deleteRecursively();
}
