#include <catch2/catch_test_macros.hpp>

#include "ui/TrackMeterSource.h"

using namespace duskstudio;

TEST_CASE ("Track meter source follows tracking controls without changing mix metering",
           "[console][meter]")
{
    SECTION ("Recording-stage audio ARM selects live input with IN off")
    {
        TrackMeterState state;
        state.recordingStage = true;
        state.audioTrack = true;
        state.recordArmed = true;
        REQUIRE (selectTrackMeterSource (state) == TrackMeterSource::Input);

        state.recordArmed = false;
        REQUIRE (selectTrackMeterSource (state) == TrackMeterSource::Output);
    }

    SECTION ("IN remains the explicit input-meter override")
    {
        TrackMeterState state;
        state.inputMonitorEnabled = true;
        REQUIRE (selectTrackMeterSource (state) == TrackMeterSource::Input);

        state.recordingStage = true;
        state.audioTrack = true;
        state.frozen = true;
        REQUIRE (selectTrackMeterSource (state) == TrackMeterSource::Input);
    }

    SECTION ("Mixing ignores a retained ARM state")
    {
        TrackMeterState state;
        state.audioTrack = true;
        state.recordArmed = true;
        REQUIRE (selectTrackMeterSource (state) == TrackMeterSource::Output);
    }

    SECTION ("MIDI and frozen ARM states do not select an audio-device meter")
    {
        TrackMeterState state;
        state.recordingStage = true;
        state.recordArmed = true;
        REQUIRE (selectTrackMeterSource (state) == TrackMeterSource::Output);

        state.audioTrack = true;
        state.frozen = true;
        REQUIRE (selectTrackMeterSource (state) == TrackMeterSource::Output);
    }
}
