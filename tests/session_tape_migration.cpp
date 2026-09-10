#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <cstring>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace
{
juce::File makeTempSessionDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("dusk-studio-tape-migration-"
                                    + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    return dir;
}

// Rebuild the container JUCE's AudioProcessor::copyXmlToBinary produces:
// uint32 LE magic, uint32 LE text length, single-line XML text, NUL.
juce::String makeLegacyTapeStateBlob (const juce::String& xml)
{
    const auto utf8 = xml.toRawUTF8();
    const auto len  = (std::uint32_t) std::strlen (utf8);

    std::vector<std::uint8_t> bytes;
    const auto pushLe32 = [&bytes] (std::uint32_t v)
    {
        bytes.push_back ((std::uint8_t) (v & 0xff));
        bytes.push_back ((std::uint8_t) ((v >> 8) & 0xff));
        bytes.push_back ((std::uint8_t) ((v >> 16) & 0xff));
        bytes.push_back ((std::uint8_t) ((v >> 24) & 0xff));
    };
    pushLe32 (0x21324356u);
    pushLe32 (len);
    bytes.insert (bytes.end(), utf8, utf8 + len);
    bytes.push_back (0);

    return juce::MemoryBlock (bytes.data(), bytes.size()).toBase64Encoding();
}
} // namespace

// Tape settings moved out of the hosted processor's APVTS blob and into plain
// JSON. Sessions saved before that carry only the blob, so the loader has to
// decode it once - if this breaks, every pre-existing session silently reverts
// to default tape settings on open, which is the kind of data loss nobody
// notices until they have already saved over it.
TEST_CASE ("SessionSerializer migrates the legacy tape_state blob", "[session][serializer][tape]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    const auto dir    = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");

    const juce::String xml =
        "<Parameters>"
          "<PARAM id=\"tapeMachine\" value=\"1.0\"/>"
          "<PARAM id=\"tapeSpeed\" value=\"3.0\"/>"
          "<PARAM id=\"tapeType\" value=\"3.0\"/>"
          "<PARAM id=\"signalPath\" value=\"1.0\"/>"
          "<PARAM id=\"eqStandard\" value=\"2.0\"/>"
          "<PARAM id=\"calibration\" value=\"2.0\"/>"
          "<PARAM id=\"inputGain\" value=\"5.5\"/>"
          "<PARAM id=\"saturation\" value=\"42.0\"/>"
          "<PARAM id=\"bias\" value=\"71.5\"/>"
          "<PARAM id=\"autoCal\" value=\"0.0\"/>"
          "<PARAM id=\"highpassFreq\" value=\"85.0\"/>"
          "<PARAM id=\"lowpassFreq\" value=\"11000.0\"/>"
          "<PARAM id=\"noiseAmount\" value=\"12.5\"/>"
          "<PARAM id=\"noiseEnabled\" value=\"1.0\"/>"
          "<PARAM id=\"wowAmount\" value=\"18.0\"/>"
          "<PARAM id=\"flutterAmount\" value=\"9.0\"/>"
          "<PARAM id=\"outputGain\" value=\"-2.5\"/>"
          "<PARAM id=\"autoComp\" value=\"0.0\"/>"
          "<PARAM id=\"oversampling\" value=\"2.0\"/>"
          "<PARAM id=\"headWidth\" value=\"2.0\"/>"
          "<PARAM id=\"crosstalk\" value=\"0.0\"/>"
          "<PARAM id=\"wowFlutterOn\" value=\"0.0\"/>"
          "<PARAM id=\"transformer\" value=\"0.0\"/>"
          "<PARAM id=\"bypass\" value=\"0.0\"/>"
          "<PARAM id=\"reproLF\" value=\"2.5\"/>"
          "<PARAM id=\"reproLMF\" value=\"-1.5\"/>"
          "<PARAM id=\"reproHMF\" value=\"3.0\"/>"
          "<PARAM id=\"reproHF\" value=\"-2.0\"/>"
          "<PARAM id=\"levelHmfTrim\" value=\"4.0\"/>"
          "<PARAM id=\"levelHfTrim\" value=\"-3.0\"/>"
          "<PARAM id=\"lpQ\" value=\"1.35\"/>"
          "<PARAM id=\"progHmfTrim\" value=\"2.0\"/>"
          "<PARAM id=\"progHfTrim\" value=\"-1.0\"/>"
          "<PARAM id=\"reproSubBell\" value=\"1.75\"/>"
          "<PARAM id=\"progLfTrim\" value=\"3.25\"/>"
        "</Parameters>";

    const juce::String legacy =
        "{\"version\":3,\"master\":{\"tape_enabled\":true,\"tape_state\":\""
        + makeLegacyTapeStateBlob (xml) + "\"}}";
    REQUIRE (target.replaceWithText (legacy));

    Session s;
    REQUIRE (SessionSerializer::load (s, target));

    const auto& t = s.master().tape;
    REQUIRE (t.machine.load() == 1);
    REQUIRE (t.speed.load() == 3);
    REQUIRE (t.type.load() == 3);
    REQUIRE (t.signalPath.load() == 1);
    // The retired editor exposed AES at index 2; current TM2 has NAB/CCIR, so
    // stale index 2 canonicalizes to the donor's highest valid choice (CCIR).
    REQUIRE (t.eqStandard.load() == 1);
    REQUIRE (t.calibration.load() == 2);
    REQUIRE (t.headWidth.load() == 2);
    REQUIRE_THAT (t.inputGainDb.load(),  WithinAbs (5.5f, 1.0e-5f));
    REQUIRE_THAT (t.bias.load(),         WithinAbs (71.5f, 1.0e-5f));
    REQUIRE_THAT (t.highpassHz.load(),   WithinAbs (85.0f, 1.0e-5f));
    REQUIRE_THAT (t.lowpassHz.load(),    WithinAbs (11000.0f, 1.0e-3f));
    REQUIRE_THAT (t.noiseAmount.load(),  WithinAbs (12.5f, 1.0e-5f));
    REQUIRE_THAT (t.wow.load(),          WithinAbs (18.0f, 1.0e-5f));
    REQUIRE_THAT (t.flutter.load(),      WithinAbs (9.0f, 1.0e-5f));
    REQUIRE_THAT (t.outputGainDb.load(), WithinAbs (-2.5f, 1.0e-5f));
    REQUIRE_FALSE (t.autoCal.load());
    REQUIRE (t.noiseEnabled.load());
    REQUIRE_FALSE (t.autoComp.load());
    REQUIRE_FALSE (t.crosstalk.load());
    REQUIRE_FALSE (t.wowFlutterEnabled.load());
    REQUIRE_FALSE (t.transformer.load());
    REQUIRE_THAT (t.reproLfDb.load(),          WithinAbs (2.5f, 1.0e-5f));
    REQUIRE_THAT (t.reproLmfDb.load(),         WithinAbs (-1.5f, 1.0e-5f));
    REQUIRE_THAT (t.reproHmfDb.load(),         WithinAbs (3.0f, 1.0e-5f));
    REQUIRE_THAT (t.reproHfDb.load(),          WithinAbs (-2.0f, 1.0e-5f));
    REQUIRE_THAT (t.levelHmfTrimDb.load(),     WithinAbs (4.0f, 1.0e-5f));
    REQUIRE_THAT (t.levelHfTrimDb.load(),      WithinAbs (-3.0f, 1.0e-5f));
    REQUIRE_THAT (t.lpQ.load(),                WithinAbs (1.35f, 1.0e-5f));
    REQUIRE_THAT (t.progHmfTrimDb.load(),      WithinAbs (2.0f, 1.0e-5f));
    REQUIRE_THAT (t.progHfTrimDb.load(),       WithinAbs (-1.0f, 1.0e-5f));
    REQUIRE_THAT (t.reproSubBellDb.load(),     WithinAbs (1.75f, 1.0e-5f));
    REQUIRE_THAT (t.progLfTrimDb.load(),       WithinAbs (3.25f, 1.0e-5f));
    REQUIRE (s.master().tapeEnabled.load());

    SECTION ("saving drops the blob and the values survive a plain round-trip")
    {
        REQUIRE (SessionSerializer::save (s, target));
        REQUIRE_FALSE (target.loadFileAsString().contains ("tape_state"));

        Session b;
        REQUIRE (SessionSerializer::load (b, target));
        REQUIRE (b.master().tape.machine.load() == 1);
        REQUIRE (b.master().tape.speed.load() == 3);
        REQUIRE (b.master().tape.signalPath.load() == 1);
        REQUIRE (b.master().tape.eqStandard.load() == 1);
        REQUIRE (b.master().tape.headWidth.load() == 2);
        REQUIRE_THAT (b.master().tape.inputGainDb.load(), WithinAbs (5.5f, 1.0e-5f));
        REQUIRE_THAT (b.master().tape.lowpassHz.load(),   WithinAbs (11000.0f, 1.0e-3f));
        REQUIRE (b.master().tape.noiseEnabled.load());
        REQUIRE_FALSE (b.master().tape.autoComp.load());
        REQUIRE_FALSE (b.master().tape.crosstalk.load());
        REQUIRE_FALSE (b.master().tape.wowFlutterEnabled.load());
        REQUIRE_FALSE (b.master().tape.transformer.load());
        REQUIRE_THAT (b.master().tape.reproLfDb.load(),
                      WithinAbs (2.5f, 1.0e-5f));
        REQUIRE_THAT (b.master().tape.levelHmfTrimDb.load(),
                      WithinAbs (4.0f, 1.0e-5f));
        REQUIRE_THAT (b.master().tape.lpQ.load(),
                      WithinAbs (1.35f, 1.0e-5f));
        REQUIRE_THAT (b.master().tape.reproSubBellDb.load(),
                      WithinAbs (1.75f, 1.0e-5f));
        REQUIRE_THAT (b.master().tape.progLfTrimDb.load(),
                      WithinAbs (3.25f, 1.0e-5f));
        REQUIRE (b.master().tapeEnabled.load());
    }

    SECTION ("a partial tape object resets the absent fields and ignores a stale blob")
    {
        const juce::String partial =
            "{\"version\":3,\"master\":{\"tape\":{\"machine\":1},\"tape_state\":\""
            + makeLegacyTapeStateBlob (xml) + "\"}}";
        REQUIRE (target.replaceWithText (partial));

        Session d;
        d.master().tape.wow.store (99.0f);
        d.master().tape.signalPath.store (3);
        REQUIRE (SessionSerializer::load (d, target));

        REQUIRE (d.master().tape.machine.load() == 1);
        // Present "tape" object wins outright - the legacy blob must not be
        // replayed on top of it, and absent keys fall back to the defaults
        // rather than keeping whatever the reused Session held.
        REQUIRE (d.master().tape.signalPath.load() == 0);
        REQUIRE (d.master().tape.speed.load() == 1);
        REQUIRE (d.master().tape.headWidth.load() == 1);
        REQUIRE_THAT (d.master().tape.wow.load(), WithinAbs (7.0f, 1.0e-5f));
        REQUIRE_THAT (d.master().tape.inputGainDb.load(), WithinAbs (0.0f, 1.0e-5f));
        REQUIRE (d.master().tape.autoCal.load());
        REQUIRE_FALSE (d.master().tape.noiseEnabled.load());
        REQUIRE (d.master().tape.crosstalk.load());
        REQUIRE (d.master().tape.wowFlutterEnabled.load());
        REQUIRE (d.master().tape.transformer.load());
        REQUIRE_THAT (d.master().tape.lpQ.load(), WithinAbs (0.707f, 1.0e-5f));
    }

    SECTION ("a corrupt blob leaves the defaults intact")
    {
        const juce::String bad =
            "{\"version\":3,\"master\":{\"tape_state\":\"bm90LWEtY29udGFpbmVy\"}}";
        REQUIRE (target.replaceWithText (bad));

        Session c;
        REQUIRE (SessionSerializer::load (c, target));
        REQUIRE (c.master().tape.machine.load() == 0);
        REQUIRE (c.master().tape.speed.load() == 1);
        REQUIRE (c.master().tape.autoComp.load());
        REQUIRE_THAT (c.master().tape.wow.load(), WithinAbs (7.0f, 1.0e-5f));
    }

    dir.deleteRecursively();
}
