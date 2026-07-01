#include "NativeLv2Slot.h"

#include <cstring>

namespace duskstudio::lv2
{
bool NativeLv2Slot::load (const juce::File& path, double sampleRate, int maxBlock,
                          std::string& errorOut)
{
    unload();

    auto b = std::make_unique<Lv2Bundle>();
    std::string err;
    if (! b->load (path.getFullPathName().toStdString(), err))
    { errorOut = "bundle: " + err; return false; }
    if (b->plugins().empty())
    { errorOut = "no plugins in bundle"; return false; }

    // First audio effect (audio in + out); fall back to the first advertised plugin.
    std::string uri = b->plugins().front().uri;
    for (const auto& d : b->plugins())
        if (d.audioInputs > 0 && d.audioOutputs > 0) { uri = d.uri; break; }

    auto inst = std::make_unique<Lv2Instance>();
    if (! inst->create (*b, uri, err))
    { errorOut = "create: " + err; return false; }
    if (! inst->activate (sampleRate, maxBlock, err))
    { errorOut = "activate: " + err; return false; }

    // Publish only after both are fully built; the audio thread's acquire-load of
    // `ready` pairs with this release so it never sees a half-built instance.
    bundle     = std::move (b);
    instance   = std::move (inst);
    adapter.prepare (instance->portLayout(), maxBlock);   // size the fold scratch
    loadedPath = path.getFullPathName();
    ready.store (true, std::memory_order_release);
    return true;
}

bool NativeLv2Slot::reactivate (double sampleRate, int maxBlock, std::string& errorOut)
{
    if (instance == nullptr) { errorOut = "no instance"; return false; }
    if (! instance->reactivate (sampleRate, maxBlock, errorOut)) return false;
    adapter.prepare (instance->portLayout(), maxBlock);
    return true;
}

void NativeLv2Slot::unload()
{
    ready.store (false, std::memory_order_release);
    instance.reset();   // free the LilvInstance first
    bundle.reset();     // then the world backing its plugin
    loadedPath.clear();
}

void NativeLv2Slot::leakForShutdown() noexcept
{
    ready.store (false, std::memory_order_release);
    (void) instance.release();
    (void) bundle.release();
    loadedPath.clear();
}

bool NativeLv2Slot::saveState (std::vector<uint8_t>& out) const
{
    return instance != nullptr && instance->saveState (out);
}

bool NativeLv2Slot::loadState (const std::vector<uint8_t>& in)
{
    return instance != nullptr && instance->loadState (in);
}

void NativeLv2Slot::processStereo (const float* inL, const float* inR,
                                   float* outL, float* outR, int numFrames) noexcept
{
    auto clearOutputs = [&]
    {
        if (numFrames <= 0) return;
        const size_t n = sizeof (float) * (size_t) numFrames;
        if (outL != nullptr) std::memset (outL, 0, n);
        if (outR != nullptr) std::memset (outR, 0, n);
    };

    // Clear when empty or momentarily inactive (mid-reactivate, fenced by the
    // engine gate) — matches the pre-adapter behaviour of a no-op silent pass.
    if (! ready.load (std::memory_order_acquire) || instance == nullptr
        || ! instance->isActive() || inL == nullptr)
    {
        clearOutputs();
        return;
    }

    if (numFrames <= 0) return;
    const size_t n = sizeof (float) * (size_t) numFrames;

    if (bypassed.load (std::memory_order_relaxed))
    {
        // Passthrough — copy in→out (a no-op when called in-place).
        if (outL != nullptr && outL != inL) std::memcpy (outL, inL, n);
        const float* r = (inR != nullptr) ? inR : inL;
        if (outR != nullptr && outR != r)   std::memcpy (outR, r, n);
        return;
    }

    // The adapter needs both output buffers to process in place; a missing one is
    // the same silent no-op the pre-adapter path gave it.
    if (outL == nullptr || outR == nullptr)
    {
        clearOutputs();
        return;
    }

    // The InsertAdapter processes in place, so stage the input into the output
    // buffers first (a no-op for the in-place call sites where out == in).
    if (outL != inL) std::memcpy (outL, inL, n);
    const float* r = (inR != nullptr) ? inR : inL;
    if (outR != r)  std::memcpy (outR, r, n);

    adapter.process (*instance, outL, outR, numFrames);
}
} // namespace duskstudio::lv2
