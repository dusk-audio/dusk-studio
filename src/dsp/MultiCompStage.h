#pragma once

#include "../session/CompParams.h"

#include <atomic>
#include <memory>

namespace duskstudio
{
namespace builtin { class DafPlugin; }

// One compressor of the mixer: Multi-Comp 2 run in process as one of Dusk's own
// DAF plug-ins, in the mode its kind plays. The session's CompParams hold the
// plug-in's parameters and the audio thread pushes whatever changed into it at
// the top of each block, so the session stays the one owner of every value an
// editor shows.
//
// prepare allocates and must run with the audio thread fenced; pushParameters,
// process, clearHistory and gainReductionDb are audio-thread and lock-free.
class MultiCompStage
{
public:
    explicit MultiCompStage (CompStage kind);
    ~MultiCompStage();

    // channels is the layout the plug-in runs: 1 (mono) or 2 (stereo). The
    // plug-in keeps its own Oversampling, 2x, whatever the engine's factor:
    // its reference parity was established there. Latency is resolved here, so
    // latencySamples() is correct once this returns.
    void prepare (double sampleRate, int blockSize, int channels);

    // The delay the stage adds while it runs, in every mode.
    int latencySamples() const noexcept;

    // The mode is stripMode clamped to 0..2 on a Strip and the kind's own on
    // the others; the plug-in's bypass is !enabled.
    void pushParameters (const CompParams& comp, bool enabled, int stripMode) noexcept;

    // In place. R is null on a mono layout and required on a stereo one.
    void process (float* L, float* R, int numSamples) noexcept;

    // For a stage that stopped running: runs silence through its delay so the
    // audio held there never plays when it next runs. The plug-in's
    // analog-modelled filters keep their state and decay from it then. Does
    // nothing when the stage has not run since it last cleared.
    void clearHistory() noexcept;

    // The plug-in's GR output, in dB at or below 0.
    float gainReductionDb() const noexcept;

    // The plug-in, for building its editor. Message thread.
    builtin::DafPlugin& plugin() noexcept;

    CompStage kind() const noexcept;

    // The first symbol among the kind's parameters, mode, bypass and GR that
    // the plug-in lacks, or null when it has them all.
    const char* unresolvedSymbol() const noexcept;

    // The session value behind each of the plug-in's parameter indices, which
    // is what its editor reads and writes. The bypass parameter is the engage
    // switch inverted. The mode parameter is the strip's compMode on a Strip
    // (a write outside 0..2 is ignored) and the kind's fixed mode elsewhere,
    // which writes leave alone. An output, and a parameter outside the kind's
    // list, reads the plug-in and ignores writes. Message thread.
    float sessionValue (const CompParams& comp, bool engaged, int stripMode,
                        int index) const noexcept;
    void  setSessionValue (CompParams& comp, std::atomic<bool>& engaged,
                           std::atomic<int>* stripMode, int index, float value) const noexcept;
    bool  isEngageParam (int index) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace duskstudio
