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
