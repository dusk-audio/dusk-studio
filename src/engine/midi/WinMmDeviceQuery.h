#pragma once

#if defined(_WIN32)
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

#include "MidiBackend.h"

#include <optional>
#include <string_view>

namespace duskstudio::midi::winmm
{
enum class Direction { Input, Output };

struct DeviceQueryApi
{
    decltype (&midiInGetNumDevs) inputCount = &midiInGetNumDevs;
    decltype (&midiOutGetNumDevs) outputCount = &midiOutGetNumDevs;
    decltype (&midiInGetDevCapsW) inputCaps = &midiInGetDevCapsW;
    decltype (&midiOutGetDevCapsW) outputCaps = &midiOutGetDevCapsW;
    decltype (&midiInMessage) inputMessage = &midiInMessage;
    decltype (&midiOutMessage) outputMessage = &midiOutMessage;
};

struct Endpoint
{
    unsigned int deviceIndex;
    BackendDeviceInfo info;
};

// Blocking metadata queries, never callback work. Indices belong to this
// enumeration snapshot; re-enumerate and resolve immediately before opening.
std::vector<Endpoint> enumerateDevices (Direction, const DeviceQueryApi& = {});
std::optional<unsigned int> deviceIndexForIdentifier (const std::vector<Endpoint>&,
                                                     std::string_view identifier);
} // namespace duskstudio::midi::winmm
#endif
