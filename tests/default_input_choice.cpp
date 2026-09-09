#include <catch2/catch_test_macros.hpp>

#include "engine/device/DefaultInputChoice.h"

using duskstudio::device::chooseDefaultInputDevice;

TEST_CASE ("No capture devices means the input stays unset")
{
    CHECK (chooseDefaultInputDevice ("Some Output", {}).empty());
    CHECK (chooseDefaultInputDevice ("", {}).empty());
}

// Most interfaces expose both directions under one name, so a first launch
// should stay on one piece of hardware.
TEST_CASE ("The capture device matching the output is preferred")
{
    const std::vector<std::string> inputs {
        "HD Pro Webcam C920, USB Audio",
        "HDA Intel PCH, ALC700 Analog",
        "UMC1820, USB Audio",
    };

    CHECK (chooseDefaultInputDevice ("HDA Intel PCH, ALC700 Analog", inputs)
             == "HDA Intel PCH, ALC700 Analog");
    CHECK (chooseDefaultInputDevice ("UMC1820, USB Audio", inputs)
             == "UMC1820, USB Audio");
}

TEST_CASE ("The match ignores case")
{
    const std::vector<std::string> inputs { "Webcam", "Scarlett 2i2 USB" };
    CHECK (chooseDefaultInputDevice ("scarlett 2i2 usb", inputs) == "Scarlett 2i2 USB");
}

TEST_CASE ("With no matching name the first capture device is taken")
{
    const std::vector<std::string> inputs { "First Input", "Second Input" };
    CHECK (chooseDefaultInputDevice ("Unrelated Output", inputs) == "First Input");
    CHECK (chooseDefaultInputDevice ("", inputs) == "First Input");
}

// An exact match must win even when a case-insensitive one comes earlier.
TEST_CASE ("An exact match outranks a case-insensitive one")
{
    const std::vector<std::string> inputs { "device", "Device" };
    CHECK (chooseDefaultInputDevice ("Device", inputs) == "Device");
}
