#include "MultiCompStage.h"
#include "../engine/builtin/DafPlugin.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace duskstudio
{
namespace
{
// DAF's symbol for a parameter designated as the plug-in's bypass.
constexpr const char* kBypassSymbol        = "daf_bypass";
constexpr const char* kModeSymbol          = "mode";
constexpr const char* kGainReductionSymbol = "gr_meter";

// Multi-Comp 2's modes (its duskaudio::MultiCompMode): Opto, FET Black and VCA
// are the strip's three, Bus is the console bus compressor, 7 the multiband.
constexpr int kStripModeCount = 3;
constexpr int kBusMode        = 3;
constexpr int kMultibandMode  = 7;

std::uint32_t faceMaskFor (CompStage kind) noexcept
{
    switch (kind)
    {
        case CompStage::Strip:     return 0x07u;
        case CompStage::Bus:       return 1u << kBusMode;
        case CompStage::Multiband: break;
    }
    return 1u << kMultibandMode;
}

bool sameBits (float a, float b) noexcept
{
    std::uint32_t x = 0, y = 0;
    std::memcpy (&x, &a, sizeof x);
    std::memcpy (&y, &b, sizeof y);
    return x == y;
}

constexpr float kUnsent = std::numeric_limits<float>::quiet_NaN();
} // namespace

struct MultiCompStage::Impl
{
    explicit Impl (CompStage k) : kind (k), plugin (builtin::createMultiComp2 (faceMaskFor (k))) {}

    struct Bound
    {
        std::uint32_t index;
        CompParamId   id;
        float         pushed;
    };

    CompStage kind;
    std::unique_ptr<builtin::DafPlugin> plugin;

    // The kind's parameters, each with its plug-in index and the value it
    // last received (NaN forces the next push).
    std::array<Bound, kCompParamCount> bound {};
    std::size_t boundCount = 0;

    // By plug-in index: the CompParams field behind it, or -1.
    std::vector<int> idByIndex;

    int modeIndex = -1, bypassIndex = -1, grIndex = -1;
    float pushedMode = kUnsent, pushedBypass = kUnsent;
    const char* unresolved = nullptr;

    int channels = 2;
    int maxBlock = 0;
    int latency = 0;
    bool prepared = false;
    // The stage has run since it was last at rest.
    bool held = false;

    float modeFor (int stripMode) const noexcept
    {
        switch (kind)
        {
            case CompStage::Strip:     return (float) std::clamp (stripMode, 0, kStripModeCount - 1);
            case CompStage::Bus:       return (float) kBusMode;
            case CompStage::Multiband: break;
        }
        return (float) kMultibandMode;
    }

    void send (int index, float value, float& last) noexcept
    {
        if (index >= 0 && ! sameBits (value, last))
        {
            last = value;
            plugin->setParameterValue ((std::uint32_t) index, value);
        }
    }

    void forgetPushed() noexcept
    {
        for (std::size_t i = 0; i < boundCount; ++i)
            bound[i].pushed = kUnsent;
        pushedMode = pushedBypass = kUnsent;
    }

    void run (const float* L, const float* R, float* outL, float* outR, int numSamples) noexcept
    {
        const float* inputs[4] = { L, R, nullptr, nullptr };
        float* outputs[2] = { outL, outR };
        plugin->run (inputs, outputs, (std::uint32_t) numSamples, nullptr, 0);
    }
};

MultiCompStage::MultiCompStage (CompStage kind) : impl (std::make_unique<Impl> (kind))
{
    const auto& descs = impl->plugin->params();
    impl->idByIndex.assign (descs.size(), -1);

    const auto find = [&descs] (const char* symbol) -> int
    {
        for (std::size_t i = 0; i < descs.size(); ++i)
            if (descs[i].symbol == symbol)
                return (int) i;
        return -1;
    };
    const auto resolve = [this, &find] (const char* symbol) -> int
    {
        const int index = find (symbol);
        if (index < 0 && impl->unresolved == nullptr)
            impl->unresolved = symbol;
        return index;
    };

    for (const auto id : compStageParams (kind))
    {
        const int index = resolve (compParamSpec (id).symbol);
        if (index < 0) continue;
        impl->bound[impl->boundCount++] = { (std::uint32_t) index, id, kUnsent };
        impl->idByIndex[(std::size_t) index] = (int) id;
    }
    impl->modeIndex   = resolve (kModeSymbol);
    impl->bypassIndex = resolve (kBypassSymbol);
    impl->grIndex     = resolve (kGainReductionSymbol);

    // A symbol the plug-in lacks would leave its session value unplayed.
    assert (impl->unresolved == nullptr);
}

MultiCompStage::~MultiCompStage() = default;

void MultiCompStage::prepare (double sampleRate, int blockSize, int channels)
{
    auto& plugin = *impl->plugin;
    const int bs = std::max (1, blockSize);

    plugin.deactivate();
    impl->channels = channels == 1 ? 1 : 2;
    [[maybe_unused]] const bool laidOut = builtin::setMultiComp2Channels (plugin, impl->channels);
    assert (laidOut);

    // A fixed mode is in place before the plug-in prepares, so its first block
    // runs it from rest.
    if (impl->modeIndex >= 0 && impl->kind != CompStage::Strip)
        plugin.setParameterValue ((std::uint32_t) impl->modeIndex, impl->modeFor (0));

    plugin.activate (sampleRate, bs);
    impl->latency = std::max (0, plugin.latencySamples());
    impl->maxBlock = bs;
    impl->held = false;
    impl->prepared = true;
    impl->forgetPushed();
}

int MultiCompStage::latencySamples() const noexcept
{
    return impl->latency;
}

void MultiCompStage::pushParameters (const CompParams& comp, bool enabled, int stripMode) noexcept
{
    auto& s = *impl;
    for (std::size_t i = 0; i < s.boundCount; ++i)
    {
        auto& b = s.bound[i];
        s.send ((int) b.index, comp.get (b.id), b.pushed);
    }
    s.send (s.modeIndex, s.modeFor (stripMode), s.pushedMode);
    s.send (s.bypassIndex, enabled ? 0.0f : 1.0f, s.pushedBypass);
}

void MultiCompStage::process (float* L, float* R, int numSamples) noexcept
{
    auto& s = *impl;
    if (numSamples <= 0 || ! s.prepared || L == nullptr) return;
    assert (s.channels == 1 || R != nullptr);
    if (s.channels == 2 && R == nullptr) return;

    s.held = true;
    // The plug-in reads every input sample before it writes that output
    // sample, so in place is contractual. A block over the prepared size runs
    // in prepared-size parts.
    for (int offset = 0; offset < numSamples; offset += s.maxBlock)
    {
        const int n = std::min (s.maxBlock, numSamples - offset);
        float* l = L + offset;
        float* r = s.channels == 2 ? R + offset : nullptr;
        s.run (l, r, l, r, n);
    }
}

void MultiCompStage::reset() noexcept
{
    auto& s = *impl;
    if (! s.held || ! s.prepared) return;
    s.held = false;
    [[maybe_unused]] const bool reset = builtin::resetMultiComp2 (*s.plugin);
    assert (reset);
}

float MultiCompStage::gainReductionDb() const noexcept
{
    if (impl->grIndex < 0) return 0.0f;
    return impl->plugin->getParameterValue ((std::uint32_t) impl->grIndex);
}

builtin::DafPlugin& MultiCompStage::plugin() noexcept
{
    return *impl->plugin;
}

CompStage MultiCompStage::kind() const noexcept
{
    return impl->kind;
}

const char* MultiCompStage::unresolvedSymbol() const noexcept
{
    return impl->unresolved;
}

float MultiCompStage::sessionValue (const CompParams& comp, bool engaged, int stripMode,
                                    int index) const noexcept
{
    const auto& s = *impl;
    if (index < 0 || index >= (int) s.idByIndex.size())
        return 0.0f;
    if (index == s.bypassIndex)
        return engaged ? 0.0f : 1.0f;
    if (index == s.modeIndex)
        return s.modeFor (stripMode);
    if (const int id = s.idByIndex[(std::size_t) index]; id >= 0)
        return comp.get ((CompParamId) id);
    return s.plugin->getParameterValue ((std::uint32_t) index);
}

void MultiCompStage::setSessionValue (CompParams& comp, std::atomic<bool>& engaged,
                                      std::atomic<int>* stripMode, int index, float value) const noexcept
{
    const auto& s = *impl;
    if (index < 0 || index >= (int) s.idByIndex.size() || ! std::isfinite (value))
        return;
    if (index == s.bypassIndex)
    {
        engaged.store (value < 0.5f, std::memory_order_relaxed);
        return;
    }
    if (index == s.modeIndex)
    {
        const long mode = std::lround (value);
        if (s.kind == CompStage::Strip && stripMode != nullptr && mode >= 0 && mode < kStripModeCount)
            stripMode->store ((int) mode, std::memory_order_relaxed);
        return;
    }
    if (const int id = s.idByIndex[(std::size_t) index]; id >= 0)
        comp.set ((CompParamId) id, value);
}

bool MultiCompStage::isEngageParam (int index) const noexcept
{
    return index >= 0 && index == impl->bypassIndex;
}
} // namespace duskstudio
