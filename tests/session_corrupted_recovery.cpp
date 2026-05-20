#include <catch2/catch_test_macros.hpp>

#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>

namespace
{
juce::File makeTempSessionDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("focal-corrupted-session-"
                                    + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    return dir;
}

void writeRaw (const juce::File& target, const juce::String& contents)
{
    target.deleteFile();
    target.create();
    target.replaceWithText (contents);
}
} // namespace

// The most painful Patreon support ticket is "Focal crashed on session
// load and now I can't open my project." These tests pin the contract
// that a corrupt session.json fails LOADING (returns false or leaves
// defaults) rather than crashing the host process. A few seconds of
// regression cover for one of the worst-case user experiences.

TEST_CASE ("SessionSerializer::load survives truncated JSON",
           "[session][serializer][corruption]")
{
    using focal::Session;
    using focal::SessionSerializer;

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");

    // Plausible-looking partial file — header + first track, no closing
    // brace. JSON parser must reject; we must not crash.
    writeRaw (target,
        R"({"version":1,"tempo":120.0,"tracks":[{"name":"truncated"}])");

    Session s;
    (void) SessionSerializer::load (s, target);
    // No specific return-value contract here — best-effort is fine.
    // Crash is the failure mode we're guarding against.

    dir.deleteRecursively();
}

TEST_CASE ("SessionSerializer::load survives empty / non-JSON garbage",
           "[session][serializer][corruption]")
{
    using focal::Session;
    using focal::SessionSerializer;

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");

    SECTION ("Empty file")
    {
        writeRaw (target, "");
        Session s;
        (void) SessionSerializer::load (s, target);
    }

    SECTION ("Binary garbage")
    {
        // Raw write past the embedded NUL — juce::String truncates at
        // \x00 so going through replaceWithText would make this test
        // identical to "Empty file" above.
        target.deleteFile();
        target.create();
        juce::FileOutputStream os (target);
        REQUIRE (os.openedOk());
        const juce::uint8 bytes[] = { 0x00, 0x01, 0x02, 0xff, 0xfe };
        os.write (bytes, sizeof (bytes));
        os.flush();
        Session s;
        (void) SessionSerializer::load (s, target);
    }

    SECTION ("Bracket-only")
    {
        writeRaw (target, "{}");
        Session s;
        REQUIRE (SessionSerializer::load (s, target));
        // Empty object is valid JSON — every field falls back to default.
        // No specific assertion on track names; the absence of a crash IS
        // the contract.
    }

    SECTION ("Missing file")
    {
        target.deleteFile();
        Session s;
        REQUIRE_FALSE (SessionSerializer::load (s, target));
    }

    dir.deleteRecursively();
}

// Future-version rejection is enforced by SessionSerializer's
// version-gate (see tests/session_format_version.cpp). This file's
// scope is "load survives garbage without crashing"; the contract for
// version mismatches lives in the dedicated test.
