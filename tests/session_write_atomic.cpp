#include <catch2/catch_test_macros.hpp>

#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>

namespace
{
juce::File makeTempDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("dusk-studio-write-atomic-"
                                    + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    return dir;
}
} // namespace

TEST_CASE ("writeAtomic creates a new target and leaves no tmp behind",
           "[session][serializer][writeAtomic]")
{
    using duskstudio::SessionSerializer;

    const auto dir = makeTempDir();
    const auto target = dir.getChildFile ("session.json");

    REQUIRE (SessionSerializer::writeAtomic (target, "{\"a\":1}"));
    REQUIRE (target.existsAsFile());
    REQUIRE (target.loadFileAsString().contains ("\"a\":1"));
    REQUIRE_FALSE (dir.getChildFile ("session.json.tmp").existsAsFile());

    dir.deleteRecursively();
}

TEST_CASE ("writeAtomic replaces existing content in place",
           "[session][serializer][writeAtomic]")
{
    using duskstudio::SessionSerializer;

    const auto dir = makeTempDir();
    const auto target = dir.getChildFile ("session.json");

    REQUIRE (SessionSerializer::writeAtomic (target, "{\"gen\":1}"));
    REQUIRE (SessionSerializer::writeAtomic (target, "{\"gen\":2}"));

    const auto contents = target.loadFileAsString();
    REQUIRE (contents.contains ("\"gen\":2"));
    REQUIRE_FALSE (contents.contains ("\"gen\":1"));
    REQUIRE_FALSE (dir.getChildFile ("session.json.tmp").existsAsFile());

    dir.deleteRecursively();
}

TEST_CASE ("writeAtomic survives a stale tmp from a prior crash",
           "[session][serializer][writeAtomic]")
{
    using duskstudio::SessionSerializer;

    const auto dir = makeTempDir();
    const auto target = dir.getChildFile ("session.json");

    // A crash between tmp-write and replace leaves session.json.tmp on
    // disk. The next save must overwrite it and complete normally.
    dir.getChildFile ("session.json.tmp").replaceWithText ("{\"stale\":true}");

    REQUIRE (SessionSerializer::writeAtomic (target, "{\"fresh\":true}"));
    REQUIRE (target.loadFileAsString().contains ("\"fresh\":true"));
    REQUIRE_FALSE (dir.getChildFile ("session.json.tmp").existsAsFile());

    dir.deleteRecursively();
}
