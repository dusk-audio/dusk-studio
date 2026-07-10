#include "Lv2Scanner.h"

#include <lilv/lilv.h>

namespace duskstudio::lv2
{
std::vector<ScannedLv2> Lv2Scanner::scan()
{
    std::vector<ScannedLv2> found;
    LilvWorld* world = lilv_world_new();
    if (world == nullptr) return found;
    lilv_world_load_all (world);   // $LV2_PATH, else the spec default directories

    const LilvPlugins* all = lilv_world_get_all_plugins (world);
    LILV_FOREACH (plugins, it, all)
    {
        const LilvPlugin* p = lilv_plugins_get (all, it);

        // The bundle directory is what NativeLv2Slot::load takes. A malformed
        // manifest can yield a plugin with no bundle URI — skip, don't crash.
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
