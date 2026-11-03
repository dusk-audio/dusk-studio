#include <catch2/catch_test_macros.hpp>

#include "ui/DspReadout.h"

using duskstudio::DspReadings;
using duskstudio::DspSeverity;
using duskstudio::dspSeverity;

TEST_CASE ("the DSP readout keeps red for backend xruns and amber for risks", "[ui][dsp-readout]")
{
    SECTION ("quiet readings are the normal colour")
    {
        REQUIRE (dspSeverity ({ 0.10, 0, 0, 0 }) == DspSeverity::normal);
        REQUIRE (dspSeverity ({ 0.85, 0, 0, 0 }) == DspSeverity::normal);
    }
    SECTION ("an engine overrun alone is amber")
    {
        REQUIRE (dspSeverity ({ 0.10, 1, 0, 0 }) == DspSeverity::risk);
    }
    SECTION ("load above 85% alone is amber")
    {
        REQUIRE (dspSeverity ({ 0.86, 0, 0, 0 }) == DspSeverity::risk);
    }
    SECTION ("a realtime warning alone is amber")
    {
        REQUIRE (dspSeverity ({ 0.10, 0, 0, 1 }) == DspSeverity::risk);
    }
    SECTION ("a backend xrun is red alone and beside every risk")
    {
        REQUIRE (dspSeverity ({ 0.10, 0, 1, 0 }) == DspSeverity::disruption);
        REQUIRE (dspSeverity ({ 0.10, 3, 2, 0 }) == DspSeverity::disruption);
        REQUIRE (dspSeverity ({ 0.95, 3, 1, 2 }) == DspSeverity::disruption);
    }
    SECTION ("each severity paints its own colour")
    {
        const auto normal = duskstudio::dspSeverityColour (DspSeverity::normal);
        const auto risk = duskstudio::dspSeverityColour (DspSeverity::risk);
        const auto disruption = duskstudio::dspSeverityColour (DspSeverity::disruption);
        REQUIRE (normal != risk);
        REQUIRE (risk != disruption);
        REQUIRE (normal != disruption);
    }
}
