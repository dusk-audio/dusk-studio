#include <catch2/catch_test_macros.hpp>

#include "engine/device/ClockChoice.h"

#include <vector>

using duskstudio::device::savedClockEntry;

// Settings > Audio selects what is saved in its rate and block-size lists,
// which on PipeWire is not always what runs.
TEST_CASE ("The saved entry of a device clock list is the chosen value, else the running one",
           "[audio][device]")
{
    const std::vector<double> rates { 44100.0, 48000.0, 96000.0 };
    CHECK (savedClockEntry (rates, 48000.0, 44100.0) == 1);
    CHECK (savedClockEntry (rates, 44100.0, 44100.0) == 0);
    // A chosen rate this device does not list: what runs is shown.
    CHECK (savedClockEntry (rates, 88200.0, 96000.0) == 2);
    CHECK (savedClockEntry (rates, 0.0, 48000.0) == 1);
    CHECK (savedClockEntry (rates, 88200.0, 192000.0) == -1);
    CHECK (savedClockEntry (std::vector<double> {}, 48000.0, 48000.0) == -1);

    const std::vector<int> sizes { 64, 128, 256, 512, 1024 };
    CHECK (savedClockEntry (sizes, 512, 1024) == 3);
    CHECK (savedClockEntry (sizes, 400, 1024) == 4);
    CHECK (savedClockEntry (sizes, 0, 100) == -1);
}
