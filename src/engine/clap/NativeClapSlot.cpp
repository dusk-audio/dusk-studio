#include "NativeClapSlot.h"

#include <cstring>

namespace duskstudio::clap
{
bool NativeClapSlot::load (const juce::File& path, double sampleRate, int maxBlock,
                           std::string& errorOut)
{
    unload();

    auto b = std::make_unique<ClapBundle>();
    std::string err;
    if (! b->load (path.getFullPathName().toStdString(), err))
    { errorOut = "bundle: " + err; return false; }
    if (b->plugins().empty())
    { errorOut = "no plugins in bundle"; return false; }

    auto inst = std::make_unique<ClapInstance>();
    if (! inst->create (*b, b->plugins().front().id, err))
    { errorOut = "create: " + err; return false; }
    if (! inst->activate (sampleRate, maxBlock, err))
    { errorOut = "activate: " + err; return false; }

    // Publish only after both are fully built; the audio thread's acquire-load of
    // `ready` pairs with this release so it never sees a half-built instance.
    bundle     = std::move (b);
    instance   = std::move (inst);
    loadedPath = path.getFullPathName();
    ready.store (true, std::memory_order_release);
    return true;
}

bool NativeClapSlot::reactivate (double sampleRate, int maxBlock, std::string& errorOut)
{
    if (instance == nullptr) { errorOut = "no instance"; return false; }
    // `ready`/`instance` are unchanged across the call; the audio thread (fenced by the
    // engine gate) sees active==false mid-swap and no-ops, then resumes after activate.
    return instance->reactivate (sampleRate, maxBlock, errorOut);
}

void NativeClapSlot::unload()
{
    ready.store (false, std::memory_order_release);
    instance.reset();   // destroys the plugin first (deactivate → destroy)
    bundle.reset();     // then unloads the .so
    loadedPath.clear();
}

void NativeClapSlot::leakForShutdown() noexcept
{
    ready.store (false, std::memory_order_release);
    (void) instance.release();   // leak: skip deactivate/plugin->destroy
    (void) bundle.release();     //       and dlclose — u-he hangs there
    loadedPath.clear();
}

bool NativeClapSlot::saveState (std::vector<uint8_t>& out) const
{
    return instance != nullptr && instance->saveState (out);
}

bool NativeClapSlot::loadState (const std::vector<uint8_t>& in)
{
    return instance != nullptr && instance->loadState (in);
}

void NativeClapSlot::processStereo (const float* inL, const float* inR,
                                    float* outL, float* outR, int numFrames) noexcept
{
    if (! ready.load (std::memory_order_acquire) || instance == nullptr)
    {
        if (numFrames > 0)
        {
            if (outL != nullptr) std::memset (outL, 0, sizeof (float) * (size_t) numFrames);
            if (outR != nullptr) std::memset (outR, 0, sizeof (float) * (size_t) numFrames);
        }
        return;
    }
    if (bypassed.load (std::memory_order_relaxed))
    {
        // Passthrough — copy in→out (a no-op when called in-place).
        if (numFrames > 0)
        {
            if (inL == nullptr)
            {
                if (outL != nullptr) std::memset (outL, 0, sizeof (float) * (size_t) numFrames);
                if (outR != nullptr) std::memset (outR, 0, sizeof (float) * (size_t) numFrames);
                return;
            }
            const size_t n = sizeof (float) * (size_t) numFrames;
            if (outL != nullptr && outL != inL) std::memcpy (outL, inL, n);
            const float* r = (inR != nullptr) ? inR : inL;
            if (outR != nullptr && outR != r)   std::memcpy (outR, r, n);
        }
        return;
    }
    instance->processStereo (inL, inR, outL, outR, numFrames);
}
} // namespace duskstudio::clap
