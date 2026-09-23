#pragma once

#include <memory>

namespace duskstudio
{
struct TapeParams;
struct MasterBusParams;

namespace builtin { class DafPlugin; }

// The master tape: Tape Machine 2, run in process as one of Dusk's own DAF
// plug-ins. The session's TapeParams hold the plug-in's parameters and the audio
// thread pushes whatever changed into it at the top of each block, so the
// session stays the one owner of every value the editor shows.
//
// prepare allocates and must run with the audio thread fenced; pushParameters
// and processInPlace are audio-thread and lock-free.
class MasterTape
{
public:
    MasterTape();
    ~MasterTape();

    // Latency is resolved here, so latencySamples() is correct once this returns.
    void prepare (double sampleRate, int blockSize);

    // The processing paths' delay, resolved in prepare and constant for its
    // life. The plug-in reports zero while its signal path is a passthrough; the
    // master aligns to the processing figure at all times so a path change
    // never moves the mix, and isPassthroughPath() says when the plug-in is
    // handing the input straight back.
    int  latencySamples() const noexcept;
    bool isPassthroughPath() const noexcept;

    void pushParameters (const TapeParams& p) noexcept;
    void processInPlace (float* L, float* R, int numSamples) noexcept;

    // The plug-in, for building its editor. Message thread.
    builtin::DafPlugin& plugin() noexcept;

    // The session value behind each of the plug-in's parameter indices, which is
    // what its editor reads and writes. The bypass parameter is the editor's
    // power switch and stands for the master's tape engage, inverted; an output
    // reads the plug-in's meter. Message thread.
    float sessionValue (const MasterBusParams& params, int index) const noexcept;
    void  setSessionValue (MasterBusParams& params, int index, float value) const noexcept;
    bool  isEngageParam (int index) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace duskstudio
