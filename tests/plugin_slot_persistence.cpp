#include <catch2/catch_test_macros.hpp>

#include "engine/PluginManager.h"
#include "engine/PluginSlot.h"

using namespace duskstudio;

TEST_CASE ("PluginSlot capture placeholder is visible but never persisted")
{
    PluginSlot slot;
    slot.setOfflineForCapture ("Screenshot Only");

    CHECK (slot.isOffline());
    CHECK (slot.getOfflineName() == "Screenshot Only");
    CHECK_FALSE (slot.getDescriptorForSave (0).has_value());
    CHECK (slot.getLegacyDescriptionXmlForSave().isEmpty());
    CHECK (slot.getStateBase64ForSave (0).isEmpty());
}

TEST_CASE ("PluginSlot preserves identity and last-known state while temporarily inactive")
{
    PluginDescriptor descriptor;
    descriptor.name = "Temporarily inactive";
    descriptor.formatName = "VST3";
    descriptor.location = "/plugins/Inactive.vst3";
    descriptor.uniqueId = 42;

    PluginSlot slot;
    slot.setTemporarilyInactivePersistenceForTest (descriptor, "c3RhdGU=");

    CHECK_FALSE (slot.isLoaded());
    CHECK_FALSE (slot.isOffline());
    REQUIRE (slot.getDescriptorForSave (0).has_value());
    CHECK (*slot.getDescriptorForSave (0) == descriptor);
    CHECK (slot.getStateBase64ForSave (0) == "c3RhdGU=");
}

TEST_CASE ("PluginSlot crash-like inactive persistence survives until explicit unload")
{
    PluginDescriptor descriptor;
    descriptor.name = "Crashed child";
    descriptor.formatName = "VST3";
    descriptor.location = "/plugins/Crashed.vst3";

    PluginSlot slot;
    slot.setTemporarilyInactivePersistenceForTest (descriptor, "bGFzdC1rbm93bg==");
    REQUIRE (slot.getDescriptorForSave (0).has_value());
    CHECK (slot.getStateBase64ForSave (0) == "bGFzdC1rbm93bg==");

    slot.unload();
    CHECK_FALSE (slot.getDescriptorForSave (0).has_value());
    CHECK (slot.getStateBase64ForSave (0).isEmpty());
}

TEST_CASE ("PluginSlot failed restore preserves the exact unnormalised reference and state")
{
    PluginDescriptor descriptor;
    descriptor.name = "Missing VST3";
    descriptor.formatName = "VST3";
    descriptor.location =
        "/definitely/missing/Missing.vst3/Contents/x86_64-linux/Missing.so";
    descriptor.uniqueId = 9876;
    const juce::String state = "ZXhhY3Qtc3RhdGU=";

    PluginManager manager;
    PluginSlot slot;
    slot.setManager (manager);
    juce::String error;
    CHECK_FALSE (slot.restoreFromSavedState (descriptor, {}, state, error));
    REQUIRE (slot.getDescriptorForSave (0).has_value());
    CHECK (*slot.getDescriptorForSave (0) == descriptor);
    CHECK (slot.getStateBase64ForSave (0) == state);
    CHECK (slot.isOffline());
}

TEST_CASE ("PluginSlot migrates valid legacy XML to a structured offline descriptor")
{
    PluginDescriptor legacy;
    legacy.name = "Legacy Missing";
    legacy.manufacturer = "Dusk";
    legacy.formatName = "VST3";
    legacy.location = "/definitely/missing/Legacy.vst3";
    legacy.uniqueId = 12345;
    legacy.numInputChannels = 2;
    legacy.numOutputChannels = 2;
    const auto xml = PluginManager::descriptorToJuceForTest (legacy).createXml();
    REQUIRE (xml != nullptr);

    PluginManager manager;
    PluginSlot slot;
    slot.setManager (manager);
    juce::String error;
    CHECK_FALSE (slot.restoreFromSavedState (
        std::nullopt,
        xml->toString (juce::XmlElement::TextFormat().singleLine()),
        "bGVnYWN5LXN0YXRl",
        error));

    REQUIRE (slot.getDescriptorForSave (0).has_value());
    CHECK (*slot.getDescriptorForSave (0) == legacy);
    CHECK (slot.getLegacyDescriptionXmlForSave().isEmpty());
    CHECK (slot.getStateBase64ForSave (0) == "bGVnYWN5LXN0YXRl");
}
