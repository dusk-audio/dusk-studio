#include "MasterTape.h"
#include "../engine/builtin/DafPlugin.h"
#include "../session/Session.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace duskstudio
{
namespace
{
// The TapeParams field behind a Tape Machine 2 parameter, by the plug-in's
// symbol. Exactly one member pointer is set.
struct Binding
{
    const char* symbol;
    std::atomic<int>   TapeParams::* asInt;
    std::atomic<float> TapeParams::* asFloat;
    std::atomic<bool>  TapeParams::* asBool;
};

constexpr Binding intParam (const char* s, std::atomic<int> TapeParams::* m)     { return { s, m, nullptr, nullptr }; }
constexpr Binding floatParam (const char* s, std::atomic<float> TapeParams::* m) { return { s, nullptr, m, nullptr }; }
constexpr Binding boolParam (const char* s, std::atomic<bool> TapeParams::* m)   { return { s, nullptr, nullptr, m }; }

// Every parameter but the bypass, the meters and the oversampling choice, which
// the plug-in keeps only so its indices stay fixed.
constexpr Binding kBindings[]
{
    intParam   ("tapeMachine",   &TapeParams::machine),
    intParam   ("tapeSpeed",     &TapeParams::speed),
    intParam   ("tapeType",      &TapeParams::type),
    intParam   ("signalPath",    &TapeParams::signalPath),
    intParam   ("eqStandard",    &TapeParams::eqStandard),
    floatParam ("inputGain",     &TapeParams::inputGainDb),
    floatParam ("bias",          &TapeParams::bias),
    intParam   ("calibration",   &TapeParams::calibration),
    boolParam  ("autoCal",       &TapeParams::autoCal),
    floatParam ("highpassFreq",  &TapeParams::highpassHz),
    floatParam ("lowpassFreq",   &TapeParams::lowpassHz),
    floatParam ("noiseAmount",   &TapeParams::noiseAmount),
    boolParam  ("noiseEnabled",  &TapeParams::noiseEnabled),
    floatParam ("wowAmount",     &TapeParams::wow),
    floatParam ("flutterAmount", &TapeParams::flutter),
    floatParam ("outputGain",    &TapeParams::outputGainDb),
    boolParam  ("autoComp",      &TapeParams::autoComp),
    intParam   ("headWidth",     &TapeParams::headWidth),
    boolParam  ("crosstalk",     &TapeParams::crosstalk),
    boolParam  ("wowFlutterOn",  &TapeParams::wowFlutterOn),
    boolParam  ("transformer",   &TapeParams::transformer),
    floatParam ("reproLF",       &TapeParams::reproLfDb),
    floatParam ("reproLMF",      &TapeParams::reproLmfDb),
    floatParam ("reproHMF",      &TapeParams::reproHmfDb),
    floatParam ("reproHF",       &TapeParams::reproHfDb),
    floatParam ("levelHmfTrim",  &TapeParams::levelHmfTrimDb),
    floatParam ("levelHfTrim",   &TapeParams::levelHfTrimDb),
    floatParam ("lpQ",           &TapeParams::lowpassQ),
    floatParam ("progHmfTrim",   &TapeParams::progHmfTrimDb),
    floatParam ("progHfTrim",    &TapeParams::progHfTrimDb),
    floatParam ("reproSubBell",  &TapeParams::reproSubBellDb),
    floatParam ("progLfTrim",    &TapeParams::progLfTrimDb),
};

// DAF's symbol for a parameter designated as the plug-in's bypass.
constexpr const char* kBypassSymbol = "daf_bypass";

float read (const Binding& b, const TapeParams& p) noexcept
{
    if (b.asInt != nullptr)   return (float) (p.*b.asInt).load (std::memory_order_relaxed);
    if (b.asFloat != nullptr) return (p.*b.asFloat).load (std::memory_order_relaxed);
    return (p.*b.asBool).load (std::memory_order_relaxed) ? 1.0f : 0.0f;
}

bool sameBits (float a, float b) noexcept
{
    std::uint32_t x = 0, y = 0;
    std::memcpy (&x, &a, sizeof x);
    std::memcpy (&y, &b, sizeof y);
    return x == y;
}

void write (const Binding& b, TapeParams& p, float v) noexcept
{
    if (b.asInt != nullptr)        (p.*b.asInt).store ((int) std::lround (v), std::memory_order_relaxed);
    else if (b.asFloat != nullptr) (p.*b.asFloat).store (v, std::memory_order_relaxed);
    else                           (p.*b.asBool).store (v >= 0.5f, std::memory_order_relaxed);
}
} // namespace

struct MasterTape::Impl
{
    std::unique_ptr<builtin::DafPlugin> plugin = builtin::createTapeMachine2();

    // By plug-in parameter index; null where the session holds nothing.
    std::vector<const Binding*> bound;

    // Audio thread: the value each bound index last received. NaN forces the
    // next push.
    std::vector<float> pushed;

    int engageIndex     = -1;
    int signalPathIndex = -1;
    int thruPath        = -1;
    int processingLatency = 0;
};

MasterTape::MasterTape() : impl (std::make_unique<Impl>())
{
    const auto& descs = impl->plugin->params();
    impl->bound.assign (descs.size(), nullptr);
    impl->pushed.assign (descs.size(), std::numeric_limits<float>::quiet_NaN());

    for (std::size_t i = 0; i < descs.size(); ++i)
    {
        const auto& d = descs[i];
        if (d.symbol == kBypassSymbol)
            impl->engageIndex = (int) i;

        for (const auto& b : kBindings)
            if (d.symbol == b.symbol)
                impl->bound[i] = &b;

        if (d.symbol == "signalPath")
        {
            impl->signalPathIndex = (int) i;
            for (std::size_t e = 0; e < d.enumLabels.size(); ++e)
                if (d.enumLabels[e] == "Thru")
                    impl->thruPath = (int) std::lround (d.enumValues[e]);
        }
    }
}

MasterTape::~MasterTape() = default;

void MasterTape::prepare (double sampleRate, int blockSize)
{
    auto& plugin = *impl->plugin;

    // The plug-in's report is path-dependent and reads zero on Thru, so take the
    // processing figure with the path forced to Repro. The first push restores
    // the session's path before any audio reaches the plug-in.
    if (impl->signalPathIndex >= 0)
        plugin.setParameterValue ((std::uint32_t) impl->signalPathIndex, 0.0f);
    plugin.deactivate();
    plugin.activate (sampleRate, std::max (1, blockSize));
    impl->processingLatency = std::max (0, plugin.latencySamples());

    std::fill (impl->pushed.begin(), impl->pushed.end(),
               std::numeric_limits<float>::quiet_NaN());
}

int MasterTape::latencySamples() const noexcept
{
    return impl->processingLatency;
}

bool MasterTape::isPassthroughPath() const noexcept
{
    if (impl->signalPathIndex < 0)
        return false;
    const float path = impl->pushed[(std::size_t) impl->signalPathIndex];
    return ! std::isnan (path) && (int) std::lround (path) == impl->thruPath;
}

void MasterTape::pushParameters (const TapeParams& p) noexcept
{
    for (std::size_t i = 0; i < impl->bound.size(); ++i)
    {
        const auto* b = impl->bound[i];
        if (b == nullptr) continue;

        const float v = read (*b, p);
        if (! sameBits (v, impl->pushed[i]))
        {
            impl->pushed[i] = v;
            impl->plugin->setParameterValue ((std::uint32_t) i, v);
        }
    }
}

void MasterTape::processInPlace (float* L, float* R, int numSamples) noexcept
{
    // The plug-in's run hands the buffers straight to its core, which reads
    // inputs[ch][n] before writing outputs[ch][n], so in-place is contractual.
    float* lr[2] = { L, R };
    impl->plugin->run (lr, lr, (std::uint32_t) numSamples);
}

builtin::DafPlugin& MasterTape::plugin() noexcept
{
    return *impl->plugin;
}

float MasterTape::sessionValue (const MasterBusParams& params, int index) const noexcept
{
    if (index < 0 || index >= (int) impl->bound.size())
        return 0.0f;
    if (index == impl->engageIndex)
        return params.tapeEnabled.load (std::memory_order_relaxed) ? 0.0f : 1.0f;
    if (const auto* b = impl->bound[(std::size_t) index])
        return read (*b, params.tape);
    return impl->plugin->getParameterValue ((std::uint32_t) index);
}

void MasterTape::setSessionValue (MasterBusParams& params, int index, float value) const noexcept
{
    if (index < 0 || index >= (int) impl->bound.size())
        return;
    if (index == impl->engageIndex)
    {
        params.tapeEnabled.store (value < 0.5f, std::memory_order_relaxed);
        return;
    }
    if (const auto* b = impl->bound[(std::size_t) index])
    {
        const auto& d = impl->plugin->params()[(std::size_t) index];
        write (*b, params.tape, std::clamp (value, d.minValue, d.maxValue));
    }
}

bool MasterTape::isEngageParam (int index) const noexcept
{
    return index >= 0 && index == impl->engageIndex;
}
} // namespace duskstudio
