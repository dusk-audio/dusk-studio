#include "Lv2Scanner.h"
#include "../PluginSearchPaths.h"

#include <lilv/lilv.h>

#include <filesystem>
#include <set>
#include <string>

namespace duskstudio::lv2
{
namespace
{
// The bundle directory is what NativeLv2Slot::load takes. A malformed manifest
// can yield a plugin with no bundle URI, which gives an empty string.
std::string bundleDirectory (const LilvPlugin* plugin)
{
    const LilvNode* bundleUri = lilv_plugin_get_bundle_uri (plugin);
    if (bundleUri == nullptr) return {};
    char* parsed = lilv_file_uri_parse (lilv_node_as_uri (bundleUri), nullptr);
    if (parsed == nullptr) return {};
    std::string directory (parsed);
    lilv_free (parsed);
    return directory;
}

// Two spellings that resolve to the same folder are one bundle. A path that
// cannot be resolved keeps its own spelling, so at worst it loads twice.
std::filesystem::path bundleIdentity (const std::filesystem::path& bundle)
{
    std::error_code error;
    auto identity = std::filesystem::canonical (bundle, error);
    if (error) return bundle.lexically_normal();
    return identity;
}
} // namespace

std::vector<ScannedLv2> Lv2Scanner::scan (const std::atomic<bool>* abort, bool* walkError)
{
    std::vector<ScannedLv2> found;
    if (abort != nullptr && abort->load (std::memory_order_relaxed)) return found;

    LilvWorld* world = lilv_world_new();
    if (world == nullptr) return found;
    lilv_world_load_all (world);   // $LV2_PATH, else the spec default directories

    const LilvPlugins* fromPath = lilv_world_get_all_plugins (world);
    std::set<std::filesystem::path> loaded;
    LILV_FOREACH (plugins, it, fromPath)
        if (const auto directory = bundleDirectory (lilv_plugins_get (fromPath, it)); ! directory.empty())
            loaded.insert (bundleIdentity (std::filesystem::u8path (directory)));

    for (const auto& bundle : pluginpaths::findBundles (pluginpaths::load(), ".lv2", abort, walkError))
    {
        if (abort != nullptr && abort->load (std::memory_order_relaxed)) break;
        if (loaded.count (bundleIdentity (bundle)) != 0) continue;
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
        auto directory = bundleDirectory (p);
        if (directory.empty()) continue;
        found.push_back ({ std::move (directory), Lv2Bundle::describePlugin (world, p) });
    }

    lilv_world_free (world);
    return found;
}
} // namespace duskstudio::lv2
