#pragma once

#include "MidiBackend.h"

#include <memory>

namespace duskstudio::midi
{
// Construct/control on the message thread; CoreMIDI notifications use its run
// loop. stop() fences input delivery and must not be called by the receiver.
std::unique_ptr<IMidiInputBackend> makeCoreMidiInputBackend();
std::unique_ptr<IMidiOutputBackend> makeCoreMidiOutputBackend();
std::string coreMidiLegacyInputIdentifier (const std::string& identifier);
std::string coreMidiLegacyOutputIdentifier (const std::string& identifier);
} // namespace duskstudio::midi
