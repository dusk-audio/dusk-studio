#include <catch2/catch_test_macros.hpp>

#include "engine/ipc/PluginScanProtocol.h"

using namespace duskstudio;

namespace
{
PluginDescriptor makeDescriptor (std::string name, std::string location,
                                 std::string pluginId)
{
    PluginDescriptor descriptor;
    descriptor.name = std::move (name);
    descriptor.formatName = "VST3";
    descriptor.backend = PluginBackend::Native;
    descriptor.location = std::move (location);
    descriptor.pluginId = std::move (pluginId);
    descriptor.uniqueId = 0x123456;
    descriptor.isInstrument = true;
    return descriptor;
}
} // namespace

TEST_CASE ("plugin scan protocol round-trips multiple descriptors")
{
    const std::vector<PluginDescriptor> input {
        makeDescriptor ("Reverb", "/plugins/Reverb.vst3", "com.dusk.reverb"),
        makeDescriptor ("Synth X", "/plugins/Synth X.vst3", "com.dusk.synth")
    };
    const auto framed = scanproto::makePayload (input);
    const auto payload = scanproto::extractPayload (framed);
    REQUIRE_FALSE (payload.empty());
    REQUIRE (scanproto::parsePayload (payload) == input);
}

TEST_CASE ("plugin scan protocol distinguishes empty success from missing framing")
{
    const auto framed = scanproto::makePayload ({});
    const auto payload = scanproto::extractPayload (framed);
    REQUIRE_FALSE (payload.empty());
    REQUIRE (scanproto::parsePayload (payload).empty());
    REQUIRE (scanproto::extractPayload ("plugin output only").empty());
    REQUIRE (scanproto::extractPayload (
        R"({"version":1,"descriptors":[]})==DUSK_SCAN_END==").empty());
    REQUIRE (scanproto::extractPayload (
        std::string (scanproto::kPayloadBegin) + "\n{}").empty());
    REQUIRE (scanproto::extractPayload (
        std::string (scanproto::kPayloadBegin) + "\n"
        R"({"version":1,"descriptors":[]})"
        "\n==DUSK_SCAN_EN").empty());
}

TEST_CASE ("plugin scan protocol ignores output before its begin sentinel")
{
    const auto row = makeDescriptor ("Delay", "/plugins/delay.vst3", "delay.inner");
    const auto payload = scanproto::extractPayload (
        std::string ("plugin wrote this first\n") + scanproto::makePayload ({ row }));
    const auto parsed = scanproto::parsePayload (payload);
    REQUIRE (parsed.size() == 1);
    CHECK (parsed.front().location == "/plugins/delay.vst3");
    CHECK (parsed.front().pluginId == "delay.inner");
}

TEST_CASE ("plugin scan protocol rejects malformed JSON without partial rows")
{
    const auto valid = makeDescriptor ("Valid", "/valid.vst3", "valid");
    auto root = nlohmann::json::parse (scanproto::extractPayload (
        scanproto::makePayload ({ valid })));
    root["descriptors"].push_back ("not an object");
    REQUIRE (scanproto::parsePayload (root.dump()).empty());
    REQUIRE (scanproto::parsePayload ("{not json").empty());
}

TEST_CASE ("plugin scan protocol rejects an out-of-range schema version")
{
    REQUIRE (scanproto::parsePayload (
        R"({"version":18446744073709551615,"descriptors":[]})").empty());
}

TEST_CASE ("plugin scan sandbox policy")
{
    CHECK (scanproto::formatRequiresSandbox ("VST3"));
    CHECK (scanproto::formatRequiresSandbox ("LV2"));
    CHECK (scanproto::formatRequiresSandbox ("AudioUnit"));
    CHECK (scanproto::formatRequiresSandbox ("VST"));
    CHECK_FALSE (scanproto::formatRequiresSandbox ("DuskMultisample"));
    CHECK_FALSE (scanproto::formatRequiresSandbox ("UnknownFutureFormat"));
}
