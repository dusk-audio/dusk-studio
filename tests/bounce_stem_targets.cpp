// Unit tests for BounceEngine::collectStemTargets and
// BounceEngine::anyHardwareInsertActive - the message-thread planning side
// of the single-pass stems bounce. The render itself is integration scope
// (DUSKSTUDIO_BOUNCE_TEST harness).

#include <catch2/catch_test_macros.hpp>

#include "engine/BounceEngine.h"
#include "session/Session.h"

#include <juce_core/juce_core.h>

using duskstudio::BounceEngine;
using duskstudio::Session;

namespace
{
juce::File baseFile()
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory)
             .getChildFile ("mix.wav");
}
}

TEST_CASE ("collectStemTargets: empty session yields no targets", "[bounce][stems]")
{
    Session s;
    REQUIRE (BounceEngine::collectStemTargets (s, baseFile()).empty());
}

TEST_CASE ("collectStemTargets: tracks then buses then auxes, gated on routing",
           "[bounce][stems]")
{
    Session s;
    s.track (2).name = "drums";
    s.track (2).recordArmed.store (true);

    s.track (5).name = "gtr";
    s.track (5).recordArmed.store (true);
    s.track (5).strip.busAssign[0].store (true);
    s.track (5).strip.auxSendDb[1].store (-12.0f);

    // Routing on a track that is NOT rendered must not activate its units.
    s.track (7).strip.busAssign[3].store (true);
    s.track (7).strip.auxSendDb[3].store (0.0f);

    s.bus (0).name = "Rhythm";
    s.auxLane (1).name = "Plate";

    const auto targets = BounceEngine::collectStemTargets (s, baseFile());
    REQUIRE (targets.size() == 4);

    REQUIRE (targets[0].kind == BounceEngine::StemTarget::Kind::Track);
    REQUIRE (targets[0].index == 2);
    REQUIRE (targets[0].file.getFileName() == "mix_03_drums.wav");

    REQUIRE (targets[1].kind == BounceEngine::StemTarget::Kind::Track);
    REQUIRE (targets[1].index == 5);
    REQUIRE (targets[1].file.getFileName() == "mix_06_gtr.wav");

    REQUIRE (targets[2].kind == BounceEngine::StemTarget::Kind::Bus);
    REQUIRE (targets[2].index == 0);
    REQUIRE (targets[2].file.getFileName() == "mix_bus1_Rhythm.wav");

    REQUIRE (targets[3].kind == BounceEngine::StemTarget::Kind::Aux);
    REQUIRE (targets[3].index == 1);
    REQUIRE (targets[3].file.getFileName() == "mix_aux2_Plate.wav");
}

TEST_CASE ("collectStemTargets: send at the off sentinel stays inactive", "[bounce][stems]")
{
    Session s;
    s.track (0).recordArmed.store (true);
    s.track (0).strip.auxSendDb[0].store (duskstudio::ChannelStripParams::kAuxSendOffDb);

    const auto targets = BounceEngine::collectStemTargets (s, baseFile());
    REQUIRE (targets.size() == 1);
    REQUIRE (targets[0].kind == BounceEngine::StemTarget::Kind::Track);

    // Anything above the sentinel is audible on the audio thread, however
    // faint, so it must produce an aux stem.
    s.track (0).strip.auxSendDb[0].store (-90.0f);
    REQUIRE (BounceEngine::collectStemTargets (s, baseFile()).size() == 2);
}

TEST_CASE ("collectStemTargets: automated send activates its aux even at the off sentinel",
           "[bounce][stems]")
{
    Session s;
    s.track (0).recordArmed.store (true);
    s.track (0).strip.auxSendDb[2].store (duskstudio::ChannelStripParams::kAuxSendOffDb);
    s.track (0).automationLanes[(size_t) duskstudio::AutomationParam::AuxSend3]
        .publishPoints ({ { 0, 0.5f } });

    const auto targets = BounceEngine::collectStemTargets (s, baseFile());
    REQUIRE (targets.size() == 2);
    REQUIRE (targets[1].kind == BounceEngine::StemTarget::Kind::Aux);
    REQUIRE (targets[1].index == 2);
}

TEST_CASE ("anyHardwareInsertActive follows rendered tracks and aux lanes",
           "[bounce][stems]")
{
    Session s;
    REQUIRE_FALSE (BounceEngine::anyHardwareInsertActive (s));

    // An insert on a track that won't render doesn't count.
    s.track (3).hardwareInsert.enabled.store (true);
    REQUIRE_FALSE (BounceEngine::anyHardwareInsertActive (s));

    s.track (3).recordArmed.store (true);
    REQUIRE (BounceEngine::anyHardwareInsertActive (s));

    s.track (3).hardwareInsert.enabled.store (false);
    REQUIRE_FALSE (BounceEngine::anyHardwareInsertActive (s));

    // Aux-lane inserts count regardless of track content.
    s.auxLane (2).hardwareInserts[0].enabled.store (true);
    REQUIRE (BounceEngine::anyHardwareInsertActive (s));
}
