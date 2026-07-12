#pragma once

namespace duskstudio::outputpair
{
// Channel-pair encoding shared by the hardware-insert and aux-output-routing
// dropdowns. A pair is encoded as L*1000 + R + 1 so a JUCE ComboBox item id
// (which treats 0 as "nothing selected") round-trips an (L, R) pair without a
// side table. Values <= 0 mean "unrouted".
inline int encodePair (int chL, int chR) noexcept { return (chL * 1000) + chR + 1; }
inline int decodePairL (int id)          noexcept { return (id - 1) / 1000; }
inline int decodePairR (int id)          noexcept { return (id - 1) % 1000; }

// Accumulate a processed stereo buffer (L/R) onto the device output pair named
// by encodedPair. No-op when unrouted (<= 0) or when the pair falls outside the
// device's open outputs - the same bounds discipline as the hardware-insert
// send. Audio-thread safe: no allocation, no lock, plain += onto buffers the
// callback has already cleared, so several taps to the same pair sum cleanly.
inline void tapStereoPairInto (float* const* outs, int numOuts,
                               const float* L, const float* R,
                               int numSamples, int encodedPair) noexcept
{
    if (encodedPair <= 0) return;
    const int chL = decodePairL (encodedPair);
    const int chR = decodePairR (encodedPair);
    if (chL < 0 || chR < 0 || chL >= numOuts || chR >= numOuts) return;
    if (outs[chL] == nullptr || outs[chR] == nullptr)           return;
    for (int i = 0; i < numSamples; ++i)
    {
        outs[chL][i] += L[i];
        outs[chR][i] += R[i];
    }
}
} // namespace duskstudio::outputpair
