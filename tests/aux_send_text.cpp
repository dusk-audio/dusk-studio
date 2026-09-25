#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "session/Session.h"
#include "ui/AuxSendText.h"

using Catch::Matchers::WithinAbs;
using duskstudio::ChannelStripParams;

TEST_CASE ("an aux send at its bottom end stop reads OFF, as the strip shows it", "[ui][aux]")
{
    REQUIRE (duskstudio::auxSendValueText (ChannelStripParams::kAuxSendMinDb) == "OFF");
    REQUIRE (duskstudio::auxSendValueText (ChannelStripParams::kAuxSendOffDb) == "OFF");
    REQUIRE (duskstudio::auxSendCaption (ChannelStripParams::kAuxSendMinDb, false) == "\xe2\x88\x92");
    REQUIRE (duskstudio::auxSendCaption (ChannelStripParams::kAuxSendOffDb, true) == "\xe2\x88\x92 PRE");
}

TEST_CASE ("an audible aux send reads its level in dB", "[ui][aux]")
{
    REQUIRE (duskstudio::auxSendValueText (-59.9) == "-59.9 dB");
    REQUIRE (duskstudio::auxSendValueText (-12.0) == "-12.0 dB");
    REQUIRE (duskstudio::auxSendValueText (-0.01) == "0.0 dB");
    REQUIRE (duskstudio::auxSendValueText (ChannelStripParams::kAuxSendMaxDb) == "6.0 dB");
    REQUIRE (duskstudio::auxSendCaption (-59.9, false) == "-60");
    REQUIRE (duskstudio::auxSendCaption (-12.0, false) == "-12");
    REQUIRE (duskstudio::auxSendCaption (-6.1, true) == "-6.1 PRE");
    REQUIRE (duskstudio::auxSendCaption (-0.01, false) == "0.0");
}

TEST_CASE ("aux send text parses back to a knob position", "[ui][aux]")
{
    REQUIRE_THAT (duskstudio::auxSendFromText ("OFF"), WithinAbs (ChannelStripParams::kAuxSendMinDb, 1e-9));
    REQUIRE_THAT (duskstudio::auxSendFromText (" off "), WithinAbs (ChannelStripParams::kAuxSendMinDb, 1e-9));
    REQUIRE_THAT (duskstudio::auxSendFromText ("-12.0 dB"), WithinAbs (-12.0, 1e-9));
    REQUIRE_THAT (duskstudio::auxSendFromText ("+3 dB"), WithinAbs (3.0, 1e-9));
    REQUIRE_THAT (duskstudio::auxSendFromText ("-100"), WithinAbs (ChannelStripParams::kAuxSendMinDb, 1e-9));
    REQUIRE_THAT (duskstudio::auxSendFromText ("nan"), WithinAbs (ChannelStripParams::kAuxSendMinDb, 1e-9));
    REQUIRE (duskstudio::auxSendFromText (duskstudio::auxSendValueText (-59.9)) > ChannelStripParams::kAuxSendMinDb);
}
