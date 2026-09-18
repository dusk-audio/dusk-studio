#include <catch2/catch_test_macros.hpp>

#include "engine/builtin/BuiltinInstance.h"
#include "foundation/Base64.h"
#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

// A built-in insert is identified by a registry id, not a file path, so the
// session carries builtin_id plus an opaque builtin_state blob on both the
// channel-strip and the aux-slot lanes. These pin that round trip and the
// blob's survival through base64, since a lost blob silently resets a user's
// unit to its defaults on the next load.

namespace
{
juce::File makeTempSessionDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("dusk-studio-builtin-roundtrip-"
                                 + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    return dir;
}

// The blob a loaded Utility unit would hand the session.
std::string encodedUtilityState (float gainDb, float width)
{
    duskstudio::builtin::BuiltinBundle bundle;
    std::string error;
    REQUIRE (bundle.load ("dusk.builtin.utility", error));

    duskstudio::builtin::BuiltinInstance instance;
    REQUIRE (instance.create (bundle, "dusk.builtin.utility", error));
    REQUIRE (instance.activate (48000.0, 128, error));

    for (int i = 0; i < instance.paramCount(); ++i)
    {
        const auto* info = instance.paramInfo (i);
        if (std::string (info->id) == "gain_db") instance.setParamValue (i, gainDb);
        if (std::string (info->id) == "width")   instance.setParamValue (i, width);
    }

    std::vector<std::uint8_t> blob;
    REQUIRE (instance.saveState (blob));
    return dusk::base64::encode (blob.data(), blob.size());
}
} // namespace

TEST_CASE ("SessionSerializer round-trips a built-in insert on a track and an aux slot",
           "[session][serializer][builtin]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");

    const auto trackState = encodedUtilityState (-4.5f, 130.0f);
    const auto auxState   = encodedUtilityState (2.0f, 70.0f);

    auto savedPtr = std::make_unique<Session>();
    Session& saved = *savedPtr;
    saved.track (3).builtinUnitId      = "dusk.builtin.utility";
    saved.track (3).builtinStateBase64 = trackState;
    saved.auxLane (1).builtinUnitId[0]      = "dusk.builtin.utility";
    saved.auxLane (1).builtinStateBase64[0] = auxState;

    REQUIRE (SessionSerializer::save (saved, target));

    auto loadedPtr = std::make_unique<Session>();
    Session& loaded = *loadedPtr;
    REQUIRE (SessionSerializer::load (loaded, target));

    REQUIRE (loaded.track (3).builtinUnitId == "dusk.builtin.utility");
    REQUIRE (loaded.track (3).builtinStateBase64 == trackState);
    REQUIRE (loaded.auxLane (1).builtinUnitId[0] == "dusk.builtin.utility");
    REQUIRE (loaded.auxLane (1).builtinStateBase64[0] == auxState);

    SECTION ("the restored blob still restores the unit's parameters")
    {
        const auto blob = dusk::base64::decode (
            loaded.track (3).builtinStateBase64.data(),
            loaded.track (3).builtinStateBase64.size());

        duskstudio::builtin::BuiltinBundle bundle;
        std::string error;
        REQUIRE (bundle.load ("dusk.builtin.utility", error));
        duskstudio::builtin::BuiltinInstance instance;
        REQUIRE (instance.create (bundle, "dusk.builtin.utility", error));
        REQUIRE (instance.activate (48000.0, 128, error));
        REQUIRE (instance.loadState (blob));

        for (int i = 0; i < instance.paramCount(); ++i)
        {
            const auto* info = instance.paramInfo (i);
            if (std::string (info->id) == "gain_db")
                REQUIRE (instance.getParamValue (i) == -4.5f);
            if (std::string (info->id) == "width")
                REQUIRE (instance.getParamValue (i) == 130.0f);
        }
    }

    SECTION ("an empty slot writes no built-in keys")
    {
        const auto text = target.loadFileAsString();
        const auto parsed = nlohmann::json::parse (text.toStdString(), nullptr, false);
        REQUIRE (parsed.is_object());
        REQUIRE (parsed["tracks"][0].contains ("builtin_id") == false);
        REQUIRE (parsed["tracks"][3]["builtin_id"] == "dusk.builtin.utility");
    }

    dir.deleteRecursively();
}
