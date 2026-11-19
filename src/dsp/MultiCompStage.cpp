#include "MultiCompStage.h"
#include "../engine/builtin/DafPlugin.h"
#include "../foundation/Text.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

std::size_t nearestEnum (const builtin::DafParamDesc& desc, float value) noexcept
{
    std::size_t nearest = 0;
    for (std::size_t i = 1; i < desc.enumValues.size(); ++i)
        if (std::abs (desc.enumValues[i] - value) < std::abs (desc.enumValues[nearest] - value))
            nearest = i;
    return nearest;
}

// A value whose text Multi-Comp 2 leaves to its host, printed as the plug-in's
// own settings drawer prints one: a switch or stepped control by its label,
// otherwise no decimals for a bare number or a percentage and one with any
// other unit, the unit after a space.
std::string hostText (const builtin::DafParamDesc& desc, float value)
{
    if (! desc.enumValues.empty())
    {
        const auto i = nearestEnum (desc, value);
        return i < desc.enumLabels.size() ? desc.enumLabels[i] : std::string();
    }
    const int decimals = desc.unit.empty() || desc.unit == "%" ? 0 : 1;
    const float scale = decimals == 0 ? 1.0f : 10.0f;
    const float shown = std::abs (value * scale) < 0.5f ? 0.0f : value;   // no "-0.0"
    char number[32] {};
    std::snprintf (number, sizeof number, "%.*f", decimals, (double) shown);
    return desc.unit.empty() ? std::string (number) : std::string (number) + " " + desc.unit;
}

// What hostText writes, a label or a number with or without the unit.
bool hostValueFromText (const builtin::DafParamDesc& desc, const std::string& raw, float& value)
{
    const auto text = dusk::text::trim (raw);
    if (text.empty()) return false;
    const auto lower = dusk::text::toLowerCase (text);
    for (std::size_t i = 0; i < desc.enumLabels.size() && i < desc.enumValues.size(); ++i)
        if (dusk::text::toLowerCase (desc.enumLabels[i]) == lower)
        {
            value = desc.enumValues[i];
            return true;
        }
    const char* begin = text.c_str();
    char* end = nullptr;
    const double parsed = std::strtod (begin, &end);
    if (end == begin || ! std::isfinite (parsed)) return false;
    const auto unit = dusk::text::toLowerCase (dusk::text::trim (end));
    if (! unit.empty() && unit != dusk::text::toLowerCase (desc.unit)) return false;
    value = desc.enumValues.empty() ? (float) parsed
                                    : desc.enumValues[nearestEnum (desc, (float) parsed)];
    return true;
}
} // namespace

struct MultiCompStage::Impl
{
    explicit Impl (CompStage k)
        : kind (k), plugin (builtin::createMultiComp2 (faceMaskFor (k))),
          instance (builtin::multiComp2Instance (*plugin))
    {
    }

    struct Bound
    {
        std::uint32_t index;
        CompParamId   id;
        float         pushed;
    };

    CompStage kind;
    std::unique_ptr<builtin::DafPlugin> plugin;
    // The plug-in object, resolved here so that reset() on the audio thread
    // is a plain call.
    void* instance = nullptr;

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

    int indexOf (CompParamId id) const noexcept
    {
        for (std::size_t i = 0; i < boundCount; ++i)
            if (bound[i].id == id)
                return (int) bound[i].index;
        return -1;
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
    assert (impl->instance != nullptr);
}

MultiCompStage::~MultiCompStage() = default;

void MultiCompStage::prepare (double sampleRate, int blockSize, int channels)
{
    auto& plugin = *impl->plugin;
    const int bs = std::max (1, blockSize);

    plugin.deactivate();
    impl->channels = channels == 1 ? 1 : 2;
    builtin::setMultiComp2Channels (impl->instance, impl->channels);

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
    builtin::resetMultiComp2 (s.instance);
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

std::string MultiCompStage::valueText (CompParamId id, float value) const
{
    const int index = impl->indexOf (id);
    if (index < 0) return {};
    char text[64] {};
    if (impl->plugin->getParameterValueText ((std::uint32_t) index, value, text, (std::uint32_t) sizeof text))
        return text;
    return hostText (impl->plugin->params()[(std::size_t) index], value);
}

std::string MultiCompStage::stepsText (CompParamId id, const char* separator) const
{
    if (impl->indexOf (id) < 0) return {};
    const auto& spec = compParamSpec (id);
    std::vector<std::string> steps;
    if (spec.integer)
        for (float v = spec.hostMin; v <= spec.hostMax; v += 1.0f)
            steps.push_back (valueText (id, v));
    else
        steps = { valueText (id, spec.hostMin), valueText (id, spec.hostMax) };
    return dusk::text::joinIntoString (steps, separator != nullptr ? separator : "");
}

bool MultiCompStage::valueFromText (CompParamId id, const std::string& text, float& value) const
{
    const int index = impl->indexOf (id);
    if (index < 0) return false;
    const auto& plugin = *impl->plugin;
    const auto& desc = plugin.params()[(std::size_t) index];
    // A parameter the plug-in prints itself reads only as the plug-in reads
    // it: its readout is not its host value, so a bare number cannot stand in.
    char probe[64] {};
    const bool ownText = plugin.getParameterValueText ((std::uint32_t) index, desc.defaultValue,
                                                       probe, (std::uint32_t) sizeof probe);
    float parsed = 0.0f;
    const auto typed = dusk::text::trim (text);
    const bool read = ownText ? plugin.getParameterValueFromText ((std::uint32_t) index, typed.c_str(), parsed)
                              : hostValueFromText (desc, typed, parsed);
    if (! read) return false;
    if (! std::isfinite (parsed)) return false;
    value = clampCompParam (id, parsed);
    return true;
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

bool MultiCompStage::isModeParam (int index) const noexcept
{
    return index >= 0 && index == impl->modeIndex;
}

std::optional<CompParamId> MultiCompStage::sessionParam (int index) const noexcept
{
    if (index < 0 || index >= (int) impl->idByIndex.size()) return std::nullopt;
    const int id = impl->idByIndex[(std::size_t) index];
    if (id < 0) return std::nullopt;
    return (CompParamId) id;
}
} // namespace duskstudio
