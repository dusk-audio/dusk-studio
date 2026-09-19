#pragma once

#include <functional>

namespace duskstudio::shutdown
{
using PhaseSink = std::function<void (const char* phase)>;

// Installs the observer that also sees each phase; a default-constructed sink
// clears it.
void setPhaseSink (PhaseSink sink);

// Writes "[Dusk Studio/shutdown] <phase>" to stderr, flushed, then feeds the
// sink if one is installed. The stderr line is a contract: the Windows
// regression phase and a contract test grep it verbatim.
void emitPhase (const char* phase);
} // namespace duskstudio::shutdown
