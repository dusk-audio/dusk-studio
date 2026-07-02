#include "NativeVst3Slot.h"

#include <cstring>

namespace duskstudio::vst3
{
bool NativeVst3Slot::load (const juce::File& path, double sampleRate, int maxBlock,
                           std::string& errorOut)
{
    unload();

    auto b = std::make_unique<Vst3Bundle>();
    std::string err;
    if (! b->load (path.getFullPathName().toStdString(), err))
    { errorOut = "module: " + err; return false; }

    // First audio effect class; instruments need a source, not an insert.
    std::string classId;
    for (const auto& d : b->plugins())
        if (! d.isInstrument) { classId = d.id; break; }
    if (classId.empty())
    { errorOut = "no effect class in module"; return false; }

    auto inst = std::make_unique<Vst3Instance>();
    if (! inst->create (*b, classId, err))
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

bool NativeVst3Slot::reactivate (double sampleRate, int maxBlock, std::string& errorOut)
{
    if (instance == nullptr) { errorOut = "no instance"; return false; }
    if (! instance->reactivate (sampleRate, maxBlock, errorOut)) return false;
    adapter.prepare (instance->portLayout(), maxBlock);
    return true;
}

void NativeVst3Slot::unload()
{
    ready.store (false, std::memory_order_release);
    instance.reset();   // release the component/controller first
    bundle.reset();     // then the module backing their vtables
    loadedPath.clear();
}

void NativeVst3Slot::leakForShutdown() noexcept
{
    ready.store (false, std::memory_order_release);
    (void) instance.release();
    (void) bundle.release();
    loadedPath.clear();
}

bool NativeVst3Slot::saveState (std::vector<uint8_t>& out) const
{
    return instance != nullptr && instance->saveState (out);
}

bool NativeVst3Slot::loadState (const std::vector<uint8_t>& in)
{
    return instance != nullptr && instance->loadState (in);
}

void NativeVst3Slot::processStereo (const float* inL, const float* inR,
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
    // engine gate) — matches CLAP/LV2.
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
    // the same silent no-op as an empty slot.
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
} // namespace duskstudio::vst3
