#pragma once

#include <cstdint>

// The engine's transport currency at the plugin-hosting boundary, mirroring the
// slice of JUCE's AudioPlayHead::PositionInfo a tempo-synced plugin needs. Each
// native host (CLAP note events, VST3 ProcessContext, LV2 time atoms) translates
// this into its own format. A null PortBuffers::transport pointer means "not
// supplied"; the fields are only read when a caller wires a live transport.
namespace dusk
{
struct TransportPosition
{
    double       bpm                     = 120.0;
    double       ppqPosition             = 0.0;
    double       timeInSeconds           = 0.0;
    std::int64_t timeInSamples           = 0;
    int          timeSignatureNumerator   = 4;
    int          timeSignatureDenominator = 4;
    bool         isPlaying               = false;
    bool         isRecording             = false;
    bool         isLooping               = false;
};
} // namespace dusk
