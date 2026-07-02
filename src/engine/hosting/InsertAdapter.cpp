#include "InsertAdapter.h"

#include <cstring>

namespace duskstudio::hosting
{
void InsertAdapter::repoint()
{
    inPtrs.resize ((size_t) inChans);
    for (int c = 0; c < inChans; ++c)
        inPtrs[(size_t) c] = inStore.data() + (size_t) c * (size_t) maxFrames;

    outPtrs.resize ((size_t) outChans);
    for (int c = 0; c < outChans; ++c)
        outPtrs[(size_t) c] = outStore.data() + (size_t) c * (size_t) maxFrames;

    scPtrs.resize ((size_t) scChans);
    for (int c = 0; c < scChans; ++c)
        scPtrs[(size_t) c] = scStore.data() + (size_t) c * (size_t) maxFrames;
}

void InsertAdapter::prepare (const PortLayout& layout, int maxBlockFrames)
{
    maxFrames = juce::jmax (0, maxBlockFrames);
    inChans  = layout.mainInChannels();
    outChans = layout.mainOutChannels();
    scChans  = layout.hasSidechain()
                 ? layout.inputs[(size_t) layout.sidechainInIndex].channelCount
                 : 0;

    inStore .assign ((size_t) inChans  * (size_t) maxFrames, 0.0f);
    outStore.assign ((size_t) outChans * (size_t) maxFrames, 0.0f);
    scStore .assign ((size_t) scChans  * (size_t) maxFrames, 0.0f);
    repoint();
}

void InsertAdapter::process (INativeInstance& inst,
                             float* L, float* R, int numFrames,
                             const float* sidechainL,
                             const float* sidechainR,
                             const juce::MidiBuffer* midiIn,
                             const juce::AudioPlayHead::PositionInfo* transport) noexcept
{
    if (! inst.isActive() || numFrames <= 0 || numFrames > maxFrames)
        return;   // dry passthrough — leave L/R untouched

    const auto n = (size_t) numFrames;

    // ── INPUT FOLD: stereo L/R → the plugin's main-in channel count ──
    if (inChans == 1)
    {
        // Sum to mono so a mono plugin hears both sides.
        float* d = inStore.data();
        for (int i = 0; i < numFrames; ++i)
            d[i] = 0.5f * (L[i] + R[i]);
    }
    else if (inChans >= 2)
    {
        std::memcpy (inPtrs[0], L, n * sizeof (float));
        std::memcpy (inPtrs[1], R, n * sizeof (float));
        for (int c = 2; c < inChans; ++c)   // extra main-in channels (rare) get silence
            juce::FloatVectorOperations::clear (inPtrs[(size_t) c], numFrames);
    }
    // inChans == 0 → instrument: no audio input to build.

    // ── SIDECHAIN: fill the bus from the tap, or silence (never null) ──
    if (scChans >= 1)
    {
        if (sidechainL != nullptr) std::memcpy (scPtrs[0], sidechainL, n * sizeof (float));
        else                       juce::FloatVectorOperations::clear (scPtrs[0], numFrames);

        if (scChans >= 2)
        {
            const float* r = sidechainR != nullptr ? sidechainR : sidechainL;   // mono source → dup
            if (r != nullptr) std::memcpy (scPtrs[1], r, n * sizeof (float));
            else              juce::FloatVectorOperations::clear (scPtrs[1], numFrames);

            for (int c = 2; c < scChans; ++c)
                juce::FloatVectorOperations::clear (scPtrs[(size_t) c], numFrames);
        }
    }

    // Clear the output scratch so any main-out channel the plugin leaves
    // unwritten reads as silence, not last block's data.
    for (int c = 0; c < outChans; ++c)
        juce::FloatVectorOperations::clear (outPtrs[(size_t) c], numFrames);

    PortBuffers io;
    io.mainIn              = inChans  > 0 ? inPtrs.data() : nullptr;
    io.mainInChannels      = inChans;
    io.mainOut             = outChans > 0 ? outPtrs.data() : nullptr;
    io.mainOutChannels     = outChans;
    io.sidechainIn         = scChans  > 0 ? scPtrs.data() : nullptr;
    io.sidechainInChannels = scChans;
    io.numFrames           = numFrames;
    io.midiIn              = midiIn;
    io.transport           = transport;

    inst.processBlock (io);

    // ── OUTPUT FOLD: the plugin's main-out → stereo L/R ──
    if (outChans == 1)
    {
        const float* s = outPtrs[0];   // broadcast mono to both sides
        std::memcpy (L, s, n * sizeof (float));
        std::memcpy (R, s, n * sizeof (float));
    }
    else if (outChans >= 2)
    {
        std::memcpy (L, outPtrs[0], n * sizeof (float));
        std::memcpy (R, outPtrs[1], n * sizeof (float));
        // main-out channels >= 2 (multi-out) are dropped: an insert is stereo.
    }
    // outChans == 0 → nothing to fold; L/R pass through unchanged.
}
} // namespace duskstudio::hosting
