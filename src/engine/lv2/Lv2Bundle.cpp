#include "Lv2Bundle.h"

#include <lilv/lilv.h>

#include <cstdint>

namespace duskstudio::lv2
{
Lv2Bundle::~Lv2Bundle() { unload(); }

bool Lv2Bundle::load (const std::string& path, std::string& errorOut)
{
    unload();

    LilvWorld* world = lilv_world_new();
    if (world == nullptr) { errorOut = "lilv_world_new failed"; return false; }

    // A bundle URI must end in '/', or lilv resolves the manifest's relative
    // includes (dsp.ttl, ui.ttl) against the PARENT directory and silently loses
    // the port/DSP metadata. Normalise the trailing slash before making the URI.
    std::string dir = path;
    if (! dir.empty() && dir.back() != '/') dir += '/';

    // Load only this bundle, not the whole LV2_PATH - fast and side-effect-free.
    LilvNode* bundleUri = lilv_new_file_uri (world, nullptr, dir.c_str());
    if (bundleUri == nullptr)
    { errorOut = "invalid bundle path: " + path; lilv_world_free (world); return false; }
    lilv_world_load_bundle (world, bundleUri);
    lilv_node_free (bundleUri);

    descriptors.clear();
    const LilvPlugins* allPlugins = lilv_world_get_all_plugins (world);
    LILV_FOREACH (plugins, it, allPlugins)
        descriptors.push_back (describePlugin (world, lilv_plugins_get (allPlugins, it)));

    if (descriptors.empty())
    { errorOut = "no plugins advertised by bundle: " + path; lilv_world_free (world); return false; }

    worldHandle = world;
    bundlePath  = path;
    return true;
}

void Lv2Bundle::unload()
{
    if (worldHandle != nullptr)
    {
        lilv_world_free (static_cast<LilvWorld*> (worldHandle));
        worldHandle = nullptr;
    }
    descriptors.clear();
    bundlePath.clear();
}

PluginDesc Lv2Bundle::describePlugin (void* worldHandle, const void* pluginHandle)
{
    auto* world = static_cast<LilvWorld*> (worldHandle);
    const auto* p = static_cast<const LilvPlugin*> (pluginHandle);

    LilvNode* audioClass = lilv_new_uri (world, LILV_URI_AUDIO_PORT);
    LilvNode* inputClass = lilv_new_uri (world, LILV_URI_INPUT_PORT);
    LilvNode* atomClass  = lilv_new_uri (world, LILV_URI_ATOM_PORT);

    PluginDesc d;
    d.uri = lilv_node_as_uri (lilv_plugin_get_uri (p));
    if (LilvNode* nameNode = lilv_plugin_get_name (p))
    {
        d.name = lilv_node_as_string (nameNode);
        lilv_node_free (nameNode);
    }

    int atomInputs = 0;
    const uint32_t numPorts = lilv_plugin_get_num_ports (p);
    for (uint32_t i = 0; i < numPorts; ++i)
    {
        const LilvPort* port = lilv_plugin_get_port_by_index (p, i);
        const bool isInput = lilv_port_is_a (p, port, inputClass);
        if (lilv_port_is_a (p, port, audioClass))
            (isInput ? d.audioInputs : d.audioOutputs) += 1;
        else if (isInput && lilv_port_is_a (p, port, atomClass))
            ++atomInputs;
    }
    // Instrument: takes MIDI/atom in, produces audio, but has no audio input.
    d.isInstrument = (d.audioInputs == 0 && atomInputs > 0 && d.audioOutputs > 0);

    lilv_node_free (audioClass);
    lilv_node_free (inputClass);
    lilv_node_free (atomClass);
    return d;
}

const void* Lv2Bundle::pluginByUri (const std::string& uri) const
{
    if (worldHandle == nullptr) return nullptr;
    auto* world = static_cast<LilvWorld*> (worldHandle);
    const LilvPlugins* allPlugins = lilv_world_get_all_plugins (world);
    LilvNode* uriNode = lilv_new_uri (world, uri.c_str());
    const LilvPlugin* p = lilv_plugins_get_by_uri (allPlugins, uriNode);
    lilv_node_free (uriNode);
    return p;
}
} // namespace duskstudio::lv2
