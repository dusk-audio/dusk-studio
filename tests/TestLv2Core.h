#pragma once

#include <filesystem>
#include <fstream>

namespace duskstudio::test
{
// Real LV2 installs carry lv2core, which labels lv2:Plugin. lilv (0.26 and 0.28)
// leaks the class node lilv_world_load_all creates for an unlabelled class, so a
// sandboxed LV2_PATH that a scan loads needs the label as well.
inline bool writeLv2CoreLabel (const std::filesystem::path& lv2Directory)
{
    const auto bundle = lv2Directory / "lv2core-label.lv2";
    std::filesystem::create_directories (bundle);
    std::ofstream manifest (bundle / "manifest.ttl");
    manifest << "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n"
                "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
                "lv2:Plugin a rdfs:Class ; rdfs:label \"Plugin\" .\n";
    return manifest.good();
}
} // namespace duskstudio::test
