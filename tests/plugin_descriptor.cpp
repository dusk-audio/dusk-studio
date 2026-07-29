#include <catch2/catch_test_macros.hpp>

#include "engine/PluginDescriptor.h"

#include <nlohmann/json.hpp>

using namespace duskstudio;

TEST_CASE ("plugin descriptor supplies defaults for missing optional fields")
{
    PluginDescriptor descriptor;
    REQUIRE (PluginDescriptor::fromJson (
        R"({"version":1,"backend":"juce_legacy","format_name":"Future"})",
        descriptor));
    CHECK (descriptor.formatName == "Future");
    CHECK (descriptor.name.empty());
    CHECK (descriptor.uniqueId == 0);
    CHECK_FALSE (descriptor.isInstrument);
}

TEST_CASE ("plugin descriptor rejects malformed input without mutation")
{
    PluginDescriptor descriptor;
    descriptor.name = "keep";
    SECTION ("top-level array")
    {
        CHECK_FALSE (PluginDescriptor::fromJson ("[]", descriptor));
        CHECK (descriptor.name == "keep");
    }
    SECTION ("unknown backend")
    {
        CHECK_FALSE (PluginDescriptor::fromJson (
            R"({"version":1,"backend":"alien"})", descriptor));
        CHECK (descriptor.name == "keep");
    }
    SECTION ("string integer field")
    {
        CHECK_FALSE (PluginDescriptor::fromJson (
            R"({"version":1,"backend":"native","unique_id":"bad"})", descriptor));
        CHECK (descriptor.name == "keep");
    }
    SECTION ("fractional integer field")
    {
        CHECK_FALSE (PluginDescriptor::fromJson (
            R"({"version":1,"backend":"native","unique_id":1.5})", descriptor));
        CHECK (descriptor.name == "keep");
    }
    SECTION ("positive 32-bit overflow")
    {
        CHECK_FALSE (PluginDescriptor::fromJson (
            R"({"version":1,"backend":"native","unique_id":2147483648})", descriptor));
        CHECK (descriptor.name == "keep");
    }
    SECTION ("negative 32-bit overflow")
    {
        CHECK_FALSE (PluginDescriptor::fromJson (
            R"({"version":1,"backend":"native","deprecated_uid":-2147483649})",
            descriptor));
        CHECK (descriptor.name == "keep");
    }
    SECTION ("64-bit overflow")
    {
        CHECK_FALSE (PluginDescriptor::fromJson (
            R"({"version":1,"backend":"native","last_info_update_ms":9223372036854775808})",
            descriptor));
        CHECK (descriptor.name == "keep");
    }
    SECTION ("boolean integer field")
    {
        CHECK_FALSE (PluginDescriptor::fromJson (
            R"({"version":1,"backend":"native","num_input_channels":true})",
            descriptor));
        CHECK (descriptor.name == "keep");
    }
    SECTION ("integer boolean field")
    {
        CHECK_FALSE (PluginDescriptor::fromJson (
            R"({"version":1,"backend":"native","is_instrument":1})", descriptor));
        CHECK (descriptor.name == "keep");
    }
}

TEST_CASE ("plugin descriptor preserves every field and unknown format names")
{
    PluginDescriptor descriptor;
    descriptor.name = "Name";
    descriptor.descriptiveName = "Descriptive";
    descriptor.manufacturer = "Maker";
    descriptor.category = "Category";
    descriptor.version = "9.8.7";
    descriptor.formatName = "FutureFormat";
    descriptor.backend = PluginBackend::Native;
    descriptor.location = "/bundle";
    descriptor.pluginId = "inner.id";
    descriptor.uniqueId = 123;
    descriptor.deprecatedUid = -456;
    descriptor.numInputChannels = 7;
    descriptor.numOutputChannels = 8;
    descriptor.lastFileModificationMs = 1234567890123;
    descriptor.lastInfoUpdateMs = 2234567890123;
    descriptor.isInstrument = true;
    descriptor.hasSharedContainer = true;
    descriptor.hasAraExtension = true;

    SECTION ("object API")
    {
        const auto object = descriptor.toJsonObject();
        REQUIRE (object.is_object());
        REQUIRE_FALSE (object.empty());
        CHECK (object.begin().key() == "version");
        CHECK (object.rbegin().key() == "has_ara_extension");

        PluginDescriptor restored;
        const nlohmann::json objectForRead = object;
        REQUIRE (PluginDescriptor::fromJsonObject (objectForRead, restored));
        CHECK (restored == descriptor);
    }

    SECTION ("string compatibility API")
    {
        PluginDescriptor restored;
        REQUIRE (PluginDescriptor::fromJson (descriptor.toJson(), restored));
        CHECK (restored == descriptor);
    }
}

TEST_CASE ("plugin descriptor object API rejects malformed data without mutation")
{
    PluginDescriptor descriptor;
    descriptor.name = "keep";
    const nlohmann::json malformed {
        { "version", 1 },
        { "backend", "native" },
        { "is_instrument", 1 }
    };

    CHECK_FALSE (PluginDescriptor::fromJsonObject (malformed, descriptor));
    CHECK (descriptor.name == "keep");
}

TEST_CASE ("plugin descriptor replaces invalid UTF-8 while serializing")
{
    PluginDescriptor descriptor;
    descriptor.name = "Invalid ";
    descriptor.name.push_back (static_cast<char> (0xff));

    std::string serialized;
    REQUIRE_NOTHROW (serialized = descriptor.toJson());

    PluginDescriptor restored;
    REQUIRE (PluginDescriptor::fromJson (serialized, restored));
    CHECK (restored.name == std::string ("Invalid \xef\xbf\xbd"));
}

TEST_CASE ("loaded descriptor trusts instantiated classification but preserves reload identity")
{
    PluginDescriptor scanned;
    scanned.name = "Scanned Effect";
    scanned.formatName = "VST3";
    scanned.backend = PluginBackend::JuceLegacy;
    scanned.location = "/plugins/Shell.vst3";
    scanned.pluginId = "shell-child";
    scanned.isInstrument = false;

    PluginDescriptor instantiated;
    instantiated.name = "Actual Instrument";
    instantiated.formatName = "VST3";
    instantiated.location =
        "/plugins/Shell.vst3/Contents/x86_64-linux/Shell.so";
    instantiated.isInstrument = true;
    instantiated.numInputChannels = 0;
    instantiated.numOutputChannels = 2;

    const auto merged = mergeLoadedPluginDescriptor (scanned, instantiated);
    CHECK (merged.name == "Actual Instrument");
    CHECK (merged.isInstrument);
    CHECK (merged.numInputChannels == 0);
    CHECK (merged.numOutputChannels == 2);
    CHECK (merged.location == "/plugins/Shell.vst3");
    CHECK (merged.pluginId == "shell-child");
    CHECK (merged.backend == PluginBackend::JuceLegacy);
}
