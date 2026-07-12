#pragma once

#include "INativeInstance.h"
#include "PortBuffers.h"
#include "PortLayout.h"

#include <vector>

namespace duskstudio::hosting
{
// Reconciles a plugin's arbitrary negotiated PortLayout to the mixer's fixed
// stereo-in / stereo-out insert contract. This is the ONE place the "an insert
// is stereo" invariant lives:
//
//   * mono plugin  -> the stereo feed is summed to a mono input, and the mono
//                    output is broadcast back to L and R;
//   * stereo plugin-> straight L/R through;
//   * multi-out    -> only main-out channels 0/1 reach the mixer; aux output
//                    buses are dropped (an insert has nowhere to send them);
//   * sidechain    -> fed from the tap when routed, otherwise pre-sized silence
//                    (never a null pointer - plugins may dereference it);
//   * instrument   -> no audio input; the plugin's output IS the signal.
//
// Owns every process-scratch buffer, sized once in prepare(); process() runs on
// the audio thread and allocates nothing.
class InsertAdapter
{
public:
    // Message thread. Size scratch for `layout` at `maxBlockFrames`. Idempotent -
    // safe to call again when the layout or block size changes.
    void prepare (const PortLayout& layout, int maxBlockFrames);

    // Audio thread. Run `inst` as a stereo insert over L/R in place. sidechainL/R
    // feed the plugin's sidechain bus when it has one (null -> silence). midiIn /
    // transport are forwarded for instrument / tempo-synced plugins (null ok).
    // Leaves L/R untouched (dry passthrough) if `inst` is inactive or numFrames
    // exceeds the prepared maximum.
    void process (INativeInstance& inst,
                  float* L, float* R, int numFrames,
                  const float* sidechainL = nullptr,
                  const float* sidechainR = nullptr,
                  const juce::MidiBuffer* midiIn = nullptr,
                  const juce::AudioPlayHead::PositionInfo* transport = nullptr) noexcept;

    int inputChannels()     const noexcept { return inChans; }
    int outputChannels()    const noexcept { return outChans; }
    int sidechainChannels() const noexcept { return scChans; }

private:
    void repoint();   // rebuild the per-channel pointer arrays into the stores

    int maxFrames = 0;
    int inChans = 0, outChans = 0, scChans = 0;

    // Contiguous per-channel scratch; the pointer arrays below index into these.
    // Kept mutable (float*) so we can fill them; float** converts implicitly to
    // PortBuffers' const float* const* for the input / sidechain buses.
    std::vector<float>  inStore, outStore, scStore;
    std::vector<float*> inPtrs, outPtrs, scPtrs;
};
} // namespace duskstudio::hosting
