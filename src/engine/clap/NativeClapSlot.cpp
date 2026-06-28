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

void NativeClapSlot::unload()
{
    ready.store (false, std::memory_order_release);
    instance.reset();   // destroys the plugin first (deactivate → destroy)
    bundle.reset();     // then unloads the .so
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
    instance->processStereo (inL, inR, outL, outR, numFrames);
}
} // namespace duskstudio::clap
