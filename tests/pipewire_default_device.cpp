#include <catch2/catch_test_macros.hpp>

#include "engine/pipewire/PipeWireDefaultDevice.h"

#include <string>
#include <vector>

using duskstudio::pipewire::indexOfMetadataDefault;

// PipeWire publishes the desktop's chosen devices as JSON in its "default"
// metadata object. Resolving one of those values against a scan is pure, so it
// carries the coverage; binding the metadata object needs a live graph.

namespace
{
const std::vector<std::string> kIds {
    "alsa_output.usb-BEHRINGER_UMC1820-00.multichannel-output",
    "alsa_output.pci-0000_00_1f.3.analog-stereo",
    "alsa_output.pci-0000_00_1f.3.analog-stereo.monitor",
};
const std::vector<std::string> kNames {
    "UMC1820 Multichannel",
    "Built-in Audio Analog Stereo",
    "Monitor of Built-in Audio Analog Stereo",
};
} // namespace

TEST_CASE ("indexOfMetadataDefault: names a device the scan saw", "[audio][pipewire]")
{
    REQUIRE (indexOfMetadataDefault (
                 R"({"name":"alsa_output.pci-0000_00_1f.3.analog-stereo"})", kIds, kNames)
             == 1);
}

TEST_CASE ("indexOfMetadataDefault: names a device the scan did not see",
           "[audio][pipewire]")
{
    // A default that is configured but absent, such as a Bluetooth sink that is
    // switched off. The caller's own order decides instead.
    REQUIRE (indexOfMetadataDefault (
                 R"({"name":"bluez_output.41_42_3E_4D_61_3B.1"})", kIds, kNames)
             == -1);
}

TEST_CASE ("indexOfMetadataDefault: a monitor default falls through", "[audio][pipewire]")
{
    // Recording the loopback of your own output by default produces takes full
    // of whatever else was playing, with nothing on screen to say why.
    REQUIRE (indexOfMetadataDefault (
                 R"({"name":"alsa_output.pci-0000_00_1f.3.analog-stereo.monitor"})",
                 kIds, kNames)
             == -1);
}

TEST_CASE ("indexOfMetadataDefault: no metadata, or metadata that is not usable",
           "[audio][pipewire]")
{
    SECTION ("absent")        { REQUIRE (indexOfMetadataDefault ("", kIds, kNames) == -1); }
    SECTION ("not json")      { REQUIRE (indexOfMetadataDefault ("alsa_output.x", kIds, kNames) == -1); }
    SECTION ("truncated")     { REQUIRE (indexOfMetadataDefault (R"({"name":)", kIds, kNames) == -1); }
    SECTION ("no name key")   { REQUIRE (indexOfMetadataDefault (R"({"id":7})", kIds, kNames) == -1); }
    SECTION ("empty name")    { REQUIRE (indexOfMetadataDefault (R"({"name":""})", kIds, kNames) == -1); }
    SECTION ("name not text") { REQUIRE (indexOfMetadataDefault (R"({"name":7})", kIds, kNames) == -1); }
    SECTION ("json null")     { REQUIRE (indexOfMetadataDefault ("null", kIds, kNames) == -1); }
    SECTION ("empty scan")    { REQUIRE (indexOfMetadataDefault (R"({"name":"x"})", {}, {}) == -1); }
}
