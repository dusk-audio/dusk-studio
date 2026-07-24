#pragma once

#include <juce_core/juce_core.h>
#include <vector>

// Recovers a DP fragment's timeline position by aligning it against the song's
// master mixdown. The DP stores no usable placement data in its .sys/firmware
// (verified), but the mixdown is the device's own render of the arrangement, so
// each fragment's audio sits at its true position inside it. Raw-sample / phase
// correlation fails (master-bus EQ/comp/limiting alters the waveform), but the
// ONSET-TIMING envelope survives that processing, so a spectral-flux onset
// envelope cross-correlation locks onto the right offset.
//
// The reliable confidence signal is PEAK DOMINANCE (best peak vs the best peak
// far away), not raw correlation height: a used take dominates; a discarded
// alternate take or a fragment not present in the mix produces a flat,
// non-dominant peak and is left unplaced. Message-thread only (decodes files).

namespace duskstudio::dp
{
struct Alignment
{
    bool        placed = false;        // confident enough to use
    std::int64_t timelineStartSamples = 0;  // in mixdown sample-rate
    double      positionSeconds = 0.0;
    float       sigma = 0.0f;          // peak height in std-devs (informational)
    float       dominance = 0.0f;      // peak / next-best-far-away peak (the real gate)
    bool        fullLength = false;    // fragment >= mixdown length -> take at song start
};

// Decode the mixdown once, then align each fragment source against it. Returns
// one Alignment per source (same order). Sources that are unreadable, longer
// than the mixdown (full-length takes -> placed at 0), or below the dominance
// gate come back placed=false (caller leaves them at song start) except the
// fullLength case which is placed=true at 0. Never throws.
//
// `dominanceGate` defaults to the empirically-validated 1.5 (used solo take
// scored 2.17; rejected alternate takes ~1.05).
std::vector<Alignment> alignToMixdown (const juce::File& mixdown,
                                       const std::vector<juce::File>& fragmentSources,
                                       float dominanceGate = 1.5f);

// Exposed for unit testing: spectral-flux onset envelope (hop 240, win 1024 at
// any SR) and FFT cross-correlation peak search. Returns {bestLagFrames,
// sigma, dominance}; lagFrames * hop = sample offset of `shortEnv` within
// `longEnv`.
struct CorrResult { int lagFrames; float sigma; float dominance; };
std::vector<float> onsetEnvelope (const float* samples, int numSamples,
                                  int hop = 240, int fftOrder = 10);
CorrResult crossCorrelate (const std::vector<float>& longEnv,
                           const std::vector<float>& shortEnv);
} // namespace duskstudio::dp
