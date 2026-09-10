#pragma once

#include <memory>

namespace duskstudio
{
struct TapeParams;

// Owner for the framework-free TapeMachine core. Its large donor header and
// implementation details stay confined to MasterTape.cpp, leaving MasterBus
// dependent only on this stable application interface.
//
// Every method below forwards straight to the core: prepare allocates,
// everything else is lock-free and safe on the audio thread.
class MasterTape
{
public:
    MasterTape();
    ~MasterTape();

    // The current donor is calibrated and pinned to its tuned 2x path; prepare
    // establishes that DSP and latency without exposing the legacy state choice.
    void prepare (double sampleRate, int blockSize);
    int  latencySamples() const noexcept;

    void pushParameters (const TapeParams& p) noexcept;
    void processInPlace (float* L, float* R, int numSamples) noexcept;

    // Linear peak followers with a 300 ms release, for the tape panel's meters.
    // Relaxed atomic loads - safe from the message thread, at most one block stale.
    struct Vu { float outL, outR; };
    Vu getVu() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace duskstudio
