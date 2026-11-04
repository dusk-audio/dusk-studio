// A plug-in scan on a build with the native LV2 host. JUCE's LV2 format stays
// registered for bundles picked by hand and for sessions that saved a plug-in
// it hosted, but the scan leaves LV2 discovery to the native host.

#include <catch2/catch_test_macros.hpp>

#include "engine/PluginManager.h"
#include "engine/PluginSearchPaths.h"
#include "TestProcessEnvironment.h"
#include "TestTempDirectory.h"

#include <filesystem>
#include <string>
#include <vector>

#if DUSKSTUDIO_HAS_NATIVE_LV2 && DUSKSTUDIO_HAS_OOP_PLUGINS
namespace
{
namespace fs = std::filesystem;
using duskstudio::PluginBackend;
using duskstudio::PluginDescriptor;
using duskstudio::PluginManager;

void copyBundle (const fs::path& from, const fs::path& to)
{
    fs::create_directories (to.parent_path());
    fs::copy (from, to, fs::copy_options::recursive);
}

std::vector<std::string> juceLv2Rows (const PluginManager& manager)
{
    auto descriptors = manager.getEffectDescriptions();
    const auto instruments = manager.getInstrumentDescriptions();
    descriptors.insert (descriptors.end(), instruments.begin(), instruments.end());
    std::vector<std::string> rows;
    for (const auto& descriptor : descriptors)
        if (descriptor.backend == PluginBackend::JuceLegacy && descriptor.formatName == "LV2")
            rows.push_back (descriptor.location);
    return rows;
}

bool hasNativeLv2Row (const PluginManager& manager, const std::string& uri)
{
    auto descriptors = manager.getLv2EffectDescriptions();
    const auto instruments = manager.getLv2InstrumentDescriptions();
    descriptors.insert (descriptors.end(), instruments.begin(), instruments.end());
    for (const auto& descriptor : descriptors)
        if (descriptor.pluginId == uri) return true;
    return false;
}
} // namespace

TEST_CASE ("A plug-in scan leaves LV2 discovery to the native host",
           "[plugin][lv2][scanner][ipc][regression][issue-947]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const duskstudio::test::TempDirectory temp ("dusk-lv2-scan-");
    const auto root = fs::canonical (temp.path());
    const auto home = root / "home";
    const auto customRoot = root / "custom";
    const auto pickedBundle = home / ".lv2" / "StateProbe.lv2";
    copyBundle (DUSKSTUDIO_STATE_PROBE_LV2_FIXTURE_PATH, pickedBundle);
    copyBundle (DUSKSTUDIO_FILE_STATE_LV2_FIXTURE_PATH, customRoot / "Vendor" / "FileState.lv2");

    const duskstudio::test::ScopedEnvironmentVariable homeVariable ("HOME", home.u8string());
    const duskstudio::test::ScopedEnvironmentVariable lv2Path ("LV2_PATH", (home / ".lv2").u8string());
    const duskstudio::test::ScopedEnvironmentVariable config ("DUSKSTUDIO_CONFIG_DIR", (root / "config").u8string());
    std::string error;
    REQUIRE (duskstudio::pluginpaths::save ({ customRoot }, error));

    // A saved plug-in list holding a JUCE LV2 row for a plug-in that is still installed.
    {
        juce::PluginDescription earlier;
        earlier.name = "Dusk control-state fixture";
        earlier.pluginFormatName = "LV2";
        earlier.fileOrIdentifier = "urn:duskstudio:test:control-state";
        earlier.numInputChannels = earlier.numOutputChannels = 2;
        juce::KnownPluginList list;
        list.addType (earlier);
        const auto xml = list.createXml();
        REQUIRE (xml != nullptr);
        REQUIRE (xml->writeTo (juce::File (juce::String ((root / "config" / "plugin-cache.xml").u8string()))));
    }

    {
        PluginManager manager;
        REQUIRE (juceLv2Rows (manager) == std::vector<std::string> { "urn:duskstudio:test:control-state" });
        // The real sandbox child, so a scan of JUCE's LV2 format would add rows.
        manager.setHostExecutableOverride (DUSKSTUDIO_PLUGIN_HOST_PATH, "--ipc-host");
        for (const char* pass : { "first scan", "rescan" })
        {
            INFO (pass);
            manager.scanInstalledPlugins();
            CHECK (juceLv2Rows (manager).empty());
            CHECK (hasNativeLv2Row (manager, "urn:duskstudio:test:state-probe"));
            CHECK (hasNativeLv2Row (manager, "urn:duskstudio:test:control-state"));
        }

        juce::String failure;
        const auto picked = manager.createPluginInstance (
            juce::File (juce::String (pickedBundle.u8string())), 48000.0, 64, failure);
        INFO (failure);
        REQUIRE (picked != nullptr);
        CHECK (picked->getPluginDescription().pluginFormatName == "LV2");
    }

    PluginDescriptor saved;
    saved.name = "Dusk control-state fixture";
    saved.formatName = "LV2";
    saved.backend = PluginBackend::JuceLegacy;
    saved.location = "urn:duskstudio:test:control-state";
    PluginManager restarted;
    juce::String failure;
    const auto restored = restarted.createPluginInstance (saved, 48000.0, 64, failure);
    INFO (failure);
    CHECK (restored != nullptr);
}
#endif
