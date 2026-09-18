#include "BuiltinInstance.h"

#include "../../foundation/Json.h"

#include <algorithm>
#include <cstring>

namespace duskstudio::builtin
{
namespace
{
constexpr int kStateVersion = 1;

hosting::PortLayout makeLayout (bool isInstrument)
{
    hosting::PortLayout layout;
    if (! isInstrument)
    {
        hosting::BusInfo in;
        in.dir = hosting::BusInfo::Direction::Input;
        in.channelCount = 2;
        in.active = true;
        in.name = "Input";
        layout.inputs.push_back (std::move (in));
        layout.mainInIndex = 0;
    }
    else
    {
        hosting::BusInfo events;
        events.kind = hosting::BusInfo::Kind::Event;
        events.dir = hosting::BusInfo::Direction::Input;
        events.carriesMidi = true;
        events.active = true;
        events.name = "MIDI In";
        layout.inputs.push_back (std::move (events));
        layout.eventInIndex = 0;
    }

    hosting::BusInfo out;
    out.dir = hosting::BusInfo::Direction::Output;
    out.channelCount = 2;
    out.active = true;
    out.name = "Output";
    layout.outputs.push_back (std::move (out));
    layout.mainOutIndex = 0;
    layout.isInstrument = isInstrument;
    return layout;
}
} // namespace

bool BuiltinInstance::create (const BuiltinBundle& bundle, const std::string& pluginId,
                              std::string& errorOut)
{
    info = bundle.unit();
    if (info == nullptr)
    {
        errorOut = "bundle carries no unit";
        return false;
    }
    if (! pluginId.empty() && pluginId != info->id)
    {
        errorOut = "requested unit '" + pluginId + "' is not '" + info->id + "'";
        return false;
    }

    id = info->id;
    if (info->createPlugin != nullptr)
    {
        auto plugin = info->createPlugin();
        if (plugin == nullptr)
        {
            errorOut = "no plug-in for built-in unit '" + id + "'";
            return false;
        }
        dafUnit = std::make_unique<DafUnitInstance> (id, std::move (plugin));
        return true;
    }

    unit = createUnit (info->id);
    if (unit == nullptr)
    {
        errorOut = "no factory for built-in unit '" + id + "'";
        return false;
    }

    layout = makeLayout (info->isInstrument);
    return true;
}

bool BuiltinInstance::activate (double sampleRate, int maxBlockFrames, std::string& errorOut)
{
    if (dafUnit != nullptr)
        return dafUnit->activate (sampleRate, maxBlockFrames, errorOut);
    if (unit == nullptr)
    {
        errorOut = "no unit";
        return false;
    }
    if (sampleRate <= 0.0 || maxBlockFrames <= 0)
    {
        errorOut = "invalid audio spec";
        return false;
    }
    unit->prepare (sampleRate, maxBlockFrames);
    preparedBlockFrames = maxBlockFrames;
    active.store (true, std::memory_order_release);
    return true;
}

void BuiltinInstance::deactivate()
{
    if (dafUnit != nullptr)
    {
        dafUnit->deactivate();
        return;
    }
    active.store (false, std::memory_order_release);
    preparedBlockFrames = 0;
}

bool BuiltinInstance::reactivate (double sampleRate, int maxBlockFrames, std::string& errorOut)
{
    if (dafUnit != nullptr)
        return dafUnit->reactivate (sampleRate, maxBlockFrames, errorOut);
    // The unit keeps its parameters across a re-prepare, so a rate change costs
    // nothing but the smoother re-seed inside prepare().
    active.store (false, std::memory_order_release);
    return activate (sampleRate, maxBlockFrames, errorOut);
}

void BuiltinInstance::processBlock (const hosting::PortBuffers& io) noexcept
{
    if (dafUnit != nullptr)
    {
        dafUnit->processBlock (io);
        return;
    }
    if (! active.load (std::memory_order_acquire) || unit == nullptr)
        return;
    if (io.numFrames <= 0 || io.numFrames > preparedBlockFrames)
        return;
    if (io.mainOut == nullptr || io.mainOutChannels < 2)
        return;

    float* outL = io.mainOut[0];
    float* outR = io.mainOut[1];
    if (outL == nullptr || outR == nullptr)
        return;

    const auto bytes = sizeof (float) * (size_t) io.numFrames;
    if (io.mainIn != nullptr && io.mainInChannels >= 2)
    {
        std::memcpy (outL, io.mainIn[0], bytes);
        std::memcpy (outR, io.mainIn[1], bytes);
    }

    unit->process (outL, outR, io.numFrames, io.midiIn);
}

bool BuiltinInstance::saveState (std::vector<std::uint8_t>& out) const
{
    if (dafUnit != nullptr)
        return dafUnit->saveState (out);
    out.clear();
    if (unit == nullptr) return false;

    dusk::json::Json params = dusk::json::Json::object();
    for (int i = 0; i < unit->paramCount(); ++i)
        params[unit->paramInfo (i).id] = unit->getParam (i);

    dusk::json::Json root = dusk::json::Json::object();
    root["id"] = id;
    root["version"] = kStateVersion;
    root["params"] = std::move (params);

    const auto text = root.dump();
    out.assign (text.begin(), text.end());
    return true;
}

bool BuiltinInstance::loadState (const std::vector<std::uint8_t>& in)
{
    if (dafUnit != nullptr)
        return dafUnit->loadState (in);
    if (unit == nullptr || in.empty()) return false;

    const auto root = dusk::json::Json::parse (
        std::string (in.begin(), in.end()), nullptr, /*allow_exceptions*/ false);
    if (! root.is_object()) return false;
    // A blob belongs to exactly one unit; applying another unit's parameter set
    // by name would restore a plausible-looking wrong patch.
    if (dusk::json::getString (root, "id") != id) return false;

    const auto& params = dusk::json::child (root, "params");
    for (int i = 0; i < unit->paramCount(); ++i)
    {
        const auto& p = unit->paramInfo (i);
        unit->setParam (i, (float) dusk::json::getDouble (params, p.id,
                                                          (double) p.defaultValue));
    }
    return true;
}

int BuiltinInstance::getLatencySamples() const noexcept
{
    if (dafUnit != nullptr)
        return dafUnit->getLatencySamples();
    if (! active.load (std::memory_order_acquire) || unit == nullptr) return 0;
    return std::max (0, unit->latencySamples());
}

int BuiltinInstance::paramCount() const noexcept
{
    if (dafUnit != nullptr) return dafUnit->paramCount();
    return unit != nullptr ? unit->paramCount() : 0;
}

const ParamInfo* BuiltinInstance::paramInfo (int index) const noexcept
{
    if (dafUnit != nullptr) return dafUnit->paramInfo (index);
    if (unit == nullptr || index < 0 || index >= unit->paramCount()) return nullptr;
    return &unit->paramInfo (index);
}

float BuiltinInstance::getParamValue (int index) const noexcept
{
    if (dafUnit != nullptr) return dafUnit->getParamValue (index);
    return unit != nullptr ? unit->getParam (index) : 0.0f;
}

void BuiltinInstance::setParamValue (int index, float value) noexcept
{
    if (dafUnit != nullptr) dafUnit->setParamValue (index, value);
    else if (unit != nullptr) unit->setParam (index, value);
}
} // namespace duskstudio::builtin
