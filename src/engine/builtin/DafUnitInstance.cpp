#include "DafUnitInstance.h"

#include "../../foundation/Json.h"
#include "../../foundation/ScopedNoDenormals.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace duskstudio::builtin
{
namespace
{
constexpr const char* kSection = "Controls";

// An integer parameter with at most this many positions is a switch. A wider one
// stays a knob, whose value the plug-in rounds.
constexpr int kMaxSwitchPositions = 32;

hosting::PortLayout makeLayout (int inputs, int outputs)
{
    hosting::PortLayout layout;
    if (inputs > 0)
    {
        hosting::BusInfo in;
        in.dir = hosting::BusInfo::Direction::Input;
        in.channelCount = inputs;
        in.active = true;
        in.name = "Input";
        layout.inputs.push_back (std::move (in));
        layout.mainInIndex = 0;
    }

    hosting::BusInfo out;
    out.dir = hosting::BusInfo::Direction::Output;
    out.channelCount = outputs;
    out.active = true;
    out.name = "Output";
    layout.outputs.push_back (std::move (out));
    layout.mainOutIndex = 0;
    return layout;
}

// The value the plug-in itself would hold after a write of `value`, so the mirror
// and a saved session agree with the DSP. Ties between enumeration values go to
// the higher one, as the plug-ins' own quantisers do.
float conform (const DafParamDesc& p, float value) noexcept
{
    const float v = std::clamp (value, p.minValue, p.maxValue);
    if (! p.enumValues.empty())
    {
        float nearest = p.enumValues.front();
        for (const float e : p.enumValues)
            if (std::abs (e - v) <= std::abs (nearest - v))
                nearest = e;
        return nearest;
    }
    if (p.isBoolean)
        return v - p.minValue >= 0.5f * (p.maxValue - p.minValue) ? p.maxValue : p.minValue;
    if (p.isInteger)
        return std::round (v);
    return v;
}
} // namespace

DafUnitInstance::DafUnitInstance (std::string stateId, std::unique_ptr<DafPlugin> hosted)
    : id (std::move (stateId)),
      plugin (std::move (hosted)),
      values (plugin->params().size())
{
    const auto& params = plugin->params();
    infos.resize (params.size());
    choiceText.resize (params.size());
    choicePointers.resize (params.size());

    for (std::size_t i = 0; i < params.size(); ++i)
    {
        const auto& p = params[i];
        auto& info = infos[i];
        info.id           = p.symbol.c_str();
        info.name         = p.name.c_str();
        info.section      = kSection;
        info.suffix       = p.unit.c_str();
        info.minValue     = p.minValue;
        info.maxValue     = p.maxValue;
        info.defaultValue = p.defaultValue;
        info.hidden       = p.isOutput || p.isHidden;

        const int positions = (int) std::lround (p.maxValue - p.minValue) + 1;
        if (p.isBoolean)
        {
            info.kind = ParamKind::Toggle;
        }
        else if (p.isInteger && positions >= 2 && positions <= kMaxSwitchPositions)
        {
            auto& text = choiceText[i];
            for (int k = 0; k < positions; ++k)
            {
                const float position = p.minValue + (float) k;
                std::string label = std::to_string ((int) std::lround (position));
                for (std::size_t e = 0; e < p.enumValues.size(); ++e)
                    if (std::abs (p.enumValues[e] - position) < 0.5f)
                        label = p.enumLabels[e];
                text.push_back (std::move (label));
            }
            for (const auto& label : text)
                choicePointers[i].push_back (label.c_str());
            info.kind = ParamKind::Choice;
            info.choices = choicePointers[i].data();
            info.choiceCount = positions;
        }

        values[i].store (p.defaultValue, std::memory_order_relaxed);
        if (p.isOutput)
            outputIndices.push_back ((std::uint32_t) i);
    }

    layout = makeLayout (plugin->numInputs(), plugin->numOutputs());
}

DafUnitInstance::~DafUnitInstance() = default;

bool DafUnitInstance::activate (double sampleRate, int maxBlockFrames, std::string& errorOut)
{
    if (sampleRate <= 0.0 || maxBlockFrames <= 0)
    {
        errorOut = "invalid audio spec";
        return false;
    }

    active.store (false, std::memory_order_release);
    plugin->deactivate();

    // The audio thread is fenced or not yet running, so the message thread stands
    // in as the ring's consumer. The mirror is at least as new as anything queued,
    // and it goes in whole.
    writes.drain ([] (const ParamWrite&) {});
    resyncAll.store (false, std::memory_order_relaxed);
    pushAllParams();

    plugin->activate (sampleRate, maxBlockFrames);
    preparedFrames = maxBlockFrames;
    active.store (true, std::memory_order_release);
    return true;
}

void DafUnitInstance::deactivate()
{
    active.store (false, std::memory_order_release);
    plugin->deactivate();
}

bool DafUnitInstance::reactivate (double sampleRate, int maxBlockFrames, std::string& errorOut)
{
    // The plug-in object survives, and with it every parameter.
    return activate (sampleRate, maxBlockFrames, errorOut);
}

void DafUnitInstance::processBlock (const hosting::PortBuffers& io) noexcept
{
    dusk::audio::ScopedNoDenormals noDenormals;

    if (! active.load (std::memory_order_acquire))
        return;
    if (io.numFrames <= 0 || io.numFrames > preparedFrames)
        return;
    if (io.mainOut == nullptr || io.mainOutChannels < plugin->numOutputs())
        return;
    if (plugin->numInputs() > 0
        && (io.mainIn == nullptr || io.mainInChannels < plugin->numInputs()))
        return;

    writes.drain ([this] (const ParamWrite& w) { plugin->setParameterValue (w.index, w.value); });
    if (resyncAll.exchange (false, std::memory_order_acq_rel))
        pushAllParams();

    if (io.transport != nullptr)
        plugin->setTimePosition (*io.transport);

    plugin->run (io.mainIn, io.mainOut, (std::uint32_t) io.numFrames);

    for (const auto index : outputIndices)
        values[index].store (plugin->getParameterValue (index), std::memory_order_relaxed);
}

void DafUnitInstance::pushAllParams() noexcept
{
    // Ascending, as every DAF format replays a session. A plug-in may carry two
    // parameters for one control and let the later write win (Tape Echo 2's
    // legacy sync division), so the order is part of the restore.
    const auto& params = plugin->params();
    for (std::uint32_t i = 0; i < (std::uint32_t) params.size(); ++i)
        if (! params[i].isOutput)
            plugin->setParameterValue (i, values[i].load (std::memory_order_relaxed));
}

bool DafUnitInstance::saveState (std::vector<std::uint8_t>& out) const
{
    out.clear();

    dusk::json::Json params = dusk::json::Json::object();
    const auto& descs = plugin->params();
    for (std::size_t i = 0; i < descs.size(); ++i)
        if (! descs[i].isOutput)
            params[descs[i].symbol] = values[i].load (std::memory_order_relaxed);

    dusk::json::Json root = dusk::json::Json::object();
    root["id"] = id;
    root["version"] = kStateVersion;
    root["params"] = std::move (params);

    const auto text = root.dump();
    out.assign (text.begin(), text.end());
    return true;
}

bool DafUnitInstance::loadState (const std::vector<std::uint8_t>& in)
{
    if (in.empty()) return false;

    const auto root = dusk::json::Json::parse (
        std::string (in.begin(), in.end()), nullptr, /*allow_exceptions*/ false);
    if (! root.is_object() || dusk::json::getString (root, "id") != id)
        return false;

    // A version-1 blob under this id holds a knob unit's controls, not this
    // plug-in's, so it restores as the plug-in's defaults rather than as a
    // plausible-looking wrong patch.
    const int version = dusk::json::getInt (root, "version", 0);
    if (version != 1 && version != kStateVersion)
        return false;

    const auto& saved = dusk::json::child (root, "params");
    const auto& descs = plugin->params();
    for (std::size_t i = 0; i < descs.size(); ++i)
    {
        const auto& p = descs[i];
        if (p.isOutput) continue;
        setParamValue ((int) i, version == kStateVersion
                                    ? dusk::json::getFiniteFloat (saved, p.symbol.c_str(),
                                                                  p.defaultValue)
                                    : p.defaultValue);
    }
    return true;
}

int DafUnitInstance::getLatencySamples() const noexcept
{
    if (! active.load (std::memory_order_acquire)) return 0;
    return std::max (0, plugin->latencySamples());
}

const ParamInfo* DafUnitInstance::paramInfo (int index) const noexcept
{
    if (index < 0 || index >= paramCount()) return nullptr;
    return &infos[(std::size_t) index];
}

float DafUnitInstance::getParamValue (int index) const noexcept
{
    if (index < 0 || index >= paramCount()) return 0.0f;
    return values[(std::size_t) index].load (std::memory_order_relaxed);
}

void DafUnitInstance::setParamValue (int index, float value) noexcept
{
    if (index < 0 || index >= paramCount() || ! std::isfinite (value)) return;
    const auto& p = plugin->params()[(std::size_t) index];
    if (p.isOutput) return;

    const float v = conform (p, value);
    values[(std::size_t) index].store (v, std::memory_order_relaxed);
    // A full ring means the audio thread has not drained for a long while. The
    // mirror already holds this value, so ask for all of it to be pushed instead.
    if (! writes.push ({ (std::uint32_t) index, v }))
        resyncAll.store (true, std::memory_order_release);
}
} // namespace duskstudio::builtin
