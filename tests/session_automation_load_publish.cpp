#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>

#include <array>
#include <cstdint>
#include <optional>

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
// lane's read() pointer across the load. The load publishes each lane once,
// so the audio thread never reads a lane cleared but not yet loaded, and every
// pre-load vector a callback read stays alive until its read scope closes.
// ASan builds catch a free under the scope even when a plain build passes.
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

    const auto generationOf = [&]
    {
        return std::array<std::uint64_t, 5> { trackLaneLoaded.snapshot.generation(),
                                              trackLaneAbsent.snapshot.generation(),
                                              masterLane.snapshot.generation(),
                                              auxLaneAbsent.snapshot.generation(),
                                              busLaneAbsent.snapshot.generation() };
    };
    const auto preGenerations = generationOf();

    // A callback in flight across the load.
    std::optional<duskstudio::SnapshotReadScope> callback;
    callback.emplace();
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

    // Every touched lane published once...
    const auto postGenerations = generationOf();
    for (size_t i = 0; i < preGenerations.size(); ++i)
        REQUIRE (postGenerations[i] == preGenerations[i] + 1);

    // ...and the pre-load vectors the callback read are still alive, with
    // what was published before the load.
    REQUIRE (preTrackLoaded->size() == 2);
    REQUIRE (preTrackAbsent->size() == 1);
    REQUIRE (preMaster->size() == 4);
    REQUIRE (preAux->size() == 1);
    REQUIRE (preBus->size() == 2);

    // Once the callback returns, the next publish frees them.
    callback.reset();
    trackLaneLoaded.publishPoints ({});
    REQUIRE (trackLaneLoaded.snapshot.retiredCount() == 0);

    dir.deleteRecursively();
}
