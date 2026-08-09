#pragma once

namespace duskstudio
{
enum class TrackMeterSource
{
    Output,
    Input
};

struct TrackMeterState
{
    bool inputMonitorEnabled = false;
    bool recordArmed = false;
    bool recordingStage = false;
    bool audioTrack = false;
    bool frozen = false;
};

constexpr TrackMeterSource selectTrackMeterSource (TrackMeterState state) noexcept
{
    if (state.inputMonitorEnabled)
        return TrackMeterSource::Input;

    if (state.recordingStage && state.audioTrack && ! state.frozen && state.recordArmed)
        return TrackMeterSource::Input;

    return TrackMeterSource::Output;
}
} // namespace duskstudio
