#include "ShutdownPhases.h"

#include <cstdio>
#include <utility>

namespace duskstudio::shutdown
{
namespace
{
// Message thread only - shutdown runs there from first phase to last, so the
// sink needs no synchronisation.
PhaseSink& phaseSink()
{
    static PhaseSink sink;
    return sink;
}
} // namespace

void setPhaseSink (PhaseSink sink)
{
    phaseSink() = std::move (sink);
}

void emitPhase (const char* phase)
{
    std::fprintf (stderr, "[Dusk Studio/shutdown] %s\n", phase);
    std::fflush (stderr);
    if (auto& sink = phaseSink())
        sink (phase);
}
} // namespace duskstudio::shutdown
