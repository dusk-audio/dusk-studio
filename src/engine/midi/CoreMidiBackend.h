#pragma once

#include "MidiBackend.h"

#include <memory>

namespace duskstudio::midi
{
// Construct/control on the message thread; CoreMIDI notifications use its run
// loop. stop() fences input delivery and must not be called by the receiver.
std::unique_ptr<IMidiInputBackend> makeCoreMidiInputBackend();
std::unique_ptr<IMidiOutputBackend> makeCoreMidiOutputBackend();
// Resolve against the fallback's actual enumeration, since its external-device
// identifier representation differs between supported framework versions.
std::string coreMidiLegacyInputIdentifier (const std::string& identifier,
                                          const std::vector<BackendDeviceInfo>& available);
std::string coreMidiLegacyOutputIdentifier (const std::string& identifier,
                                           const std::vector<BackendDeviceInfo>& available);
} // namespace duskstudio::midi
