#pragma once

#include <memory>

namespace duskstudio
{
struct TapeParams;

// Owner for the framework-free TapeMachine core. The core's header and
// multi-comp's UniversalCompressor core both define the same duskaudio:: math
// helpers (kPiD, kTwoPiF, dbToGain, ...), so the two headers cannot appear in
// one translation unit. MasterBus needs both stages, so the tape core stays
// confined to MasterTape.cpp behind this interface.
//
// Every method below forwards straight to the core: prepare allocates,
// everything else is lock-free and safe on the audio thread.
class MasterTape
{
public:
    MasterTape();
    ~MasterTape();

    // oversamplingFactor: 1, 2 or 4. Applied before the core's prepare so
    // latencySamples() is correct immediately after this returns.
    void prepare (double sampleRate, int blockSize, int oversamplingFactor);
    int  latencySamples() const noexcept;

    void pushParameters (const TapeParams& p) noexcept;
    void setBypass (bool shouldBypass) noexcept;
    void processInPlace (float* L, float* R, int numSamples) noexcept;

    // Linear peak followers with a 300 ms release, for the tape panel's meters.
    // Relaxed atomic loads - safe from the message thread, at most one block stale.
    struct Vu { float outL, outR, inL, inR; };
    Vu getVu() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace duskstudio
