#pragma once

#include <string>
#include <vector>

namespace duskstudio::hosting
{
// The negotiated bus/port shape of a native plugin instance, produced ONCE at
// load on the message thread and stable until the instance deactivates. This is
// the single host-agnostic description that VST3 (setBusArrangements / getBusInfo),
// LV2 (lilv port classification) and CLAP (audio_ports) all populate, so the
// InsertAdapter and the audio-thread call sites never branch per format. It
// replaces the two bare inCh/outCh ints the CLAP host carried.
//
// Message-thread only: the audio thread reads a cached copy held by the slot,
// never walks this vector.
struct BusInfo
{
    enum class Kind      { Audio, Event };
    enum class Direction { Input, Output };
    enum class Role      { Main, Aux, Sidechain };

    Kind      kind        = Kind::Audio;
    Direction dir         = Direction::Input;
    Role      role        = Role::Main;
    int       channelCount = 0;      // audio channels; 0 for event buses
    bool      active       = false;  // did we activate / connect this bus?
    bool      carriesMidi  = false;  // event bus that carries note / MIDI data
    std::string name;
};

struct PortLayout
{
    std::vector<BusInfo> inputs;
    std::vector<BusInfo> outputs;

    // Indices into inputs / outputs, or -1 when absent. The main audio buses are
    // what the mixer insert feeds; sidechain + event buses are optional.
    int  mainInIndex      = -1;
    int  mainOutIndex     = -1;
    int  sidechainInIndex = -1;
    int  eventInIndex     = -1;
    int  eventOutIndex    = -1;

    // MIDI-in + audio-out with no main audio input: the plugin IS the source
    // (see the instrument branch in ChannelStrip), not an in-line processor.
    bool isInstrument = false;

    int  mainInChannels() const noexcept
        { return mainInIndex  >= 0 ? inputs [(size_t) mainInIndex ].channelCount : 0; }
    int  mainOutChannels() const noexcept
        { return mainOutIndex >= 0 ? outputs[(size_t) mainOutIndex].channelCount : 0; }
    bool hasSidechain() const noexcept { return sidechainInIndex >= 0; }
    bool acceptsMidi()  const noexcept { return eventInIndex     >= 0; }
    bool emitsMidi()    const noexcept { return eventOutIndex    >= 0; }
};
} // namespace duskstudio::hosting
