#include "Lv2Scanner.h"
#include "../PluginSearchPaths.h"

#include <lilv/lilv.h>

namespace duskstudio::lv2
{
std::vector<ScannedLv2> Lv2Scanner::scan (const std::atomic<bool>* abort)
{
    std::vector<ScannedLv2> found;
    if (abort != nullptr && abort->load (std::memory_order_relaxed)) return found;

    LilvWorld* world = lilv_world_new();
    if (world == nullptr) return found;
    lilv_world_load_all (world);   // $LV2_PATH, else the spec default directories
    for (const auto& bundle : pluginpaths::findBundles (pluginpaths::load(), ".lv2", abort))
    {
        if (abort != nullptr && abort->load (std::memory_order_relaxed)) break;
        const auto path = bundle.u8string() + "/";
        LilvNode* uri = lilv_new_file_uri (world, nullptr, path.c_str());
        if (uri != nullptr) lilv_world_load_bundle (world, uri);
        lilv_node_free (uri);
    }

    const LilvPlugins* all = lilv_world_get_all_plugins (world);
    LILV_FOREACH (plugins, it, all)
    {
        if (abort != nullptr && abort->load (std::memory_order_relaxed)) break;
        const LilvPlugin* p = lilv_plugins_get (all, it);

        // The bundle directory is what NativeLv2Slot::load takes. A malformed
        // manifest can yield a plugin with no bundle URI - skip, don't crash.
        const LilvNode* bundleUri = lilv_plugin_get_bundle_uri (p);
        if (bundleUri == nullptr) continue;
        char* bundleDir = lilv_file_uri_parse (lilv_node_as_uri (bundleUri), nullptr);
        if (bundleDir == nullptr) continue;

        found.push_back ({ std::string (bundleDir),
                           Lv2Bundle::describePlugin (world, p) });
        lilv_free (bundleDir);
    }

    lilv_world_free (world);
    return found;
}
} // namespace duskstudio::lv2
