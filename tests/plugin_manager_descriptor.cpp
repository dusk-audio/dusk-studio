#include <catch2/catch_test_macros.hpp>

#include "engine/PluginManager.h"

using namespace duskstudio;

namespace
{
PluginDescriptor completeDescriptor()
{
    PluginDescriptor descriptor;
    descriptor.name = "Adapter";
    descriptor.descriptiveName = "Adapter Long Name";
    descriptor.manufacturer = "Dusk";
    descriptor.category = "Fx|Delay";
    descriptor.version = "4.2.1";
    descriptor.formatName = "VST3";
    descriptor.backend = PluginBackend::JuceLegacy;
    descriptor.location = "/plugins/Adapter.vst3";
    descriptor.uniqueId = 1234567;
    descriptor.deprecatedUid = -7654321;
    descriptor.numInputChannels = 2;
    descriptor.numOutputChannels = 4;
    descriptor.lastFileModificationMs = 1700000000123;
    descriptor.lastInfoUpdateMs = 1700000000456;
    descriptor.isInstrument = true;
    descriptor.hasSharedContainer = true;
    descriptor.hasAraExtension = true;
    return descriptor;
}
} // namespace

TEST_CASE ("PluginManager JUCE adapter round-trips every representable field")
{
    const auto expected = completeDescriptor();
    const auto juceDescriptor = PluginManager::descriptorToJuceForTest (expected);
    const auto restored = PluginManager::descriptorFromJuceForTest (juceDescriptor);
    CHECK (restored == expected);
}

TEST_CASE ("PluginManager JUCE adapter preserves native inner plugin identity")
{
    auto first = completeDescriptor();
    first.backend = PluginBackend::Native;
    first.location = "/plugins/Shell.vst3";
    first.pluginId = "shell.first";

    auto second = first;
    second.pluginId = "shell.second";

    const auto firstJuce = PluginManager::descriptorToJuceForTest (first);
    const auto secondJuce = PluginManager::descriptorToJuceForTest (second);
    CHECK (firstJuce.fileOrIdentifier == "/plugins/Shell.vst3\nshell.first");
    CHECK (secondJuce.fileOrIdentifier == "/plugins/Shell.vst3\nshell.second");
    CHECK (firstJuce.fileOrIdentifier != secondJuce.fileOrIdentifier);
    CHECK (PluginManager::descriptorFromJuceForTest (firstJuce) == first);
    CHECK (PluginManager::descriptorFromJuceForTest (secondJuce) == second);
}

TEST_CASE ("PluginManager JUCE adapter leaves legacy identifiers unchanged")
{
    auto legacy = completeDescriptor();
    legacy.backend = PluginBackend::JuceLegacy;
    legacy.location = "legacy-format-specific-identifier";

    const auto juceDescriptor = PluginManager::descriptorToJuceForTest (legacy);
    CHECK (juceDescriptor.fileOrIdentifier.toStdString() == legacy.location);

    const auto restored = PluginManager::descriptorFromJuceForTest (juceDescriptor);
    CHECK (restored.backend == PluginBackend::JuceLegacy);
    CHECK (restored.location == legacy.location);
    CHECK (restored.pluginId.empty());
}

TEST_CASE ("PluginManager imports a legacy native XML cache once and prunes stale rows")
{
    auto present = PluginManager::descriptorToJuceForTest (completeDescriptor());
    present.name = "Present";
    present.pluginFormatName = "LV2-Native";
    present.fileOrIdentifier = "/plugins/present.lv2\nhttps://dusk.audio/present";

    auto stale = present;
    stale.name = "Stale";
    stale.fileOrIdentifier = "/plugins/stale.lv2\nhttps://dusk.audio/stale";

    juce::XmlElement root ("KNOWNPLUGINS");
    root.addChildElement (present.createXml().release());
    root.addChildElement (stale.createXml().release());

    const auto imported = PluginManager::importLegacyNativeCacheForTest (
        root.toString (juce::XmlElement::TextFormat().singleLine()),
        [] (const juce::File& location)
        {
            return location.getFullPathName() == "/plugins/present.lv2";
        });

    REQUIRE (imported.size() == 1);
    CHECK (imported.front().name == "Present");
    CHECK (imported.front().backend == PluginBackend::Native);
    CHECK (imported.front().formatName == "LV2");
    CHECK (imported.front().location == "/plugins/present.lv2");
    CHECK (imported.front().pluginId == "https://dusk.audio/present");
}

TEST_CASE ("PluginManager falls back from malformed native JSON without erasing cache")
{
    auto legacy = PluginManager::descriptorToJuceForTest (completeDescriptor());
    legacy.name = "Legacy fallback";
    legacy.pluginFormatName = "LV2-Native";
    legacy.fileOrIdentifier = "/plugins/fallback.lv2\nhttps://dusk.audio/fallback";

    juce::XmlElement root ("KNOWNPLUGINS");
    root.addChildElement (legacy.createXml().release());
    const auto legacyXml = root.toString (
        juce::XmlElement::TextFormat().singleLine());
    const auto locationExists = [] (const juce::File&) { return true; };

    std::vector<PluginDescriptor> loaded { completeDescriptor() };
    REQUIRE (PluginManager::loadNativeCacheSourcesForTest (
        std::string ("{not json"), legacyXml, locationExists, loaded));
    REQUIRE (loaded.size() == 1);
    CHECK (loaded.front().name == "Legacy fallback");
    CHECK (loaded.front().pluginId == "https://dusk.audio/fallback");

    loaded = { completeDescriptor() };
    CHECK_FALSE (PluginManager::loadNativeCacheSourcesForTest (
        std::string ("{not json"), juce::String ("<broken"),
        locationExists, loaded));
    REQUIRE (loaded.size() == 1);
    CHECK (loaded.front() == completeDescriptor());

    loaded = { completeDescriptor() };
    REQUIRE (PluginManager::loadNativeCacheSourcesForTest (
        std::string (R"({"version":1,"descriptors":[]})"),
        legacyXml, locationExists, loaded));
    CHECK (loaded.empty());
}
