#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/midi/CoreMidiProtocol.h"
#include "engine/midi/MidiPacketDecoder.h"
#include "foundation/MidiRing.h"

#include <limits>
#include <vector>

using duskstudio::midi::PacketDecoder;
using namespace duskstudio::midi::coremidi;
using Catch::Matchers::WithinAbs;

namespace
{
struct Received
{
    std::vector<std::uint8_t> bytes;
    double timeMs;
};

struct DecoderProbe
{
    PacketDecoder decoder;
    std::vector<Received> messages;

    void push (const std::vector<std::uint8_t>& bytes, double timeMs = 100.0)
    {
        decoder.push (bytes.data(), static_cast<int> (bytes.size()), timeMs,
                      [this] (const std::uint8_t* b, int n, double t)
                      { messages.push_back ({ { b, b + n }, t }); });
    }
};
}

TEST_CASE ("CoreMIDI clocks retain source timestamps across different epochs", "[midi][coremidi][issue-298]")
{
    const ClockAnchor clock { 9000000000000000000ull, 2000.0, 1.0 / 24000.0 };
    REQUIRE_THAT (clock.toMilliseconds (clock.hostTicks - 12000), WithinAbs (1999.5, 1e-9));
    REQUIRE_THAT (clock.toMilliseconds (clock.hostTicks + 48000), WithinAbs (2002.0, 1e-9));
    REQUIRE_THAT (clock.toMilliseconds (0), WithinAbs (2000.0, 1e-9));
    const ClockAnchor afterSleep { clock.hostTicks + 24000, 62001.0, clock.millisecondsPerTick };
    REQUIRE_THAT (afterSleep.toMilliseconds (afterSleep.hostTicks - 12000), WithinAbs (62000.5, 1e-9));
    REQUIRE_THAT (afterSleep.toMilliseconds (0), WithinAbs (62001.0, 1e-9));
}

TEST_CASE ("CoreMIDI endpoint identities preserve signed and compound legacy routes", "[midi][coremidi][issue-298]")
{
    REQUIRE (identifier (42) == "coremidi:42");
    REQUIRE (identifier (-42) == "coremidi:-42");
    REQUIRE (identifier (std::numeric_limits<std::int32_t>::min()) == "coremidi:-2147483648");
    REQUIRE (legacyNameStartsWith (u"", u""));
    REQUIRE (legacyNameStartsWith (u"Device", u""));
    REQUIRE_FALSE (legacyNameStartsWith (u"", u"Device"));
    REQUIRE (legacyNameStartsWith (u"abc Port", u"ABC"));
    REQUIRE_FALSE (legacyNameStartsWith (u"AB", u"ABC"));
    REQUIRE_FALSE (legacyNameStartsWith (u"Stra\u00dfe", u"STRASSE"));
    REQUIRE_FALSE (legacyNameStartsWith (u"\u00e9cho", u"e\u0301cho"));
    REQUIRE (legacyNameStartsWith (u"\U0001f3b9 Port", u"\U0001f3b9"));
    REQUIRE_FALSE (legacyNameStartsWith (u"\U0001f3b9 A", u"\U0001f3b9 B"));
    REQUIRE_FALSE (legacyNameStartsWith (u"\U00010400", u"\U00020400"));
    REQUIRE_FALSE (legacyNameStartsWith (u"\U0001f3b9", u"\xd83c"));

    const duskstudio::midi::BackendDeviceInfo endpoint { "Port", "-21" }, device { "Interface", "-20" };
    auto oldInfo = externalEndpointInfo (endpoint, device, ExternalIdentifier::Device);
    auto newInfo = externalEndpointInfo (endpoint, device, ExternalIdentifier::Endpoint);
    REQUIRE (oldInfo.name == "Interface");
    REQUIRE (oldInfo.identifier == "-20");
    REQUIRE (newInfo.name == "Interface");
    REQUIRE (newInfo.identifier == "-21");
    appendConnectedInfo (oldInfo, { "Bridge", "-30 -31" });
    appendConnectedInfo (newInfo, { "Bridge", "-30 -31" });
    REQUIRE (oldInfo.identifier == "-20, -30 -31");
    REQUIRE (newInfo.identifier == "-21, -30 -31");

    std::vector<EndpointIdentity> identities { { "coremidi:1", oldInfo.identifier, newInfo.identifier } };
    REQUIRE (migrateIdentifier (identities, oldInfo.identifier) == "coremidi:1");
    REQUIRE (migrateIdentifier (identities, newInfo.identifier) == "coremidi:1");
    REQUIRE (migrateIdentifier (identities, "missing").empty());
    REQUIRE (legacyIdentifier (identities, "coremidi:1", { oldInfo }) == oldInfo.identifier);
    REQUIRE (legacyIdentifier (identities, "coremidi:1", { newInfo }) == newInfo.identifier);
    REQUIRE (legacyIdentifier (identities, "coremidi:1", { oldInfo, newInfo }).empty());
    REQUIRE (legacyIdentifier (identities, "coremidi:1", { oldInfo, oldInfo }).empty());
    identities.push_back ({ "coremidi:2", newInfo.identifier, "2" });
    REQUIRE (migrateIdentifier (identities, newInfo.identifier).empty());
    REQUIRE (legacyIdentifier (identities, "coremidi:1", { newInfo }).empty());
    REQUIRE (migrateIdentifier (identities, oldInfo.identifier) == "coremidi:1");
}

TEST_CASE ("CoreMIDI packets separate channel and system messages", "[midi][coremidi][issue-298]")
{
    DecoderProbe probe;
    probe.push ({ 0x90, 60 }, 100.0);
    probe.push ({ 0xf8, 100, 61, 110, 0xc2, 9, 0xd2, 70, 0xe2, 0, 64 }, 101.0);
    probe.push ({ 0xf1, 0x17, 0xf2, 0x01, 0x02, 0xf3, 0x03, 0xf6, 0xfa, 0xfb, 0xfc, 0xfe, 0xff });
    REQUIRE (probe.messages.size() == 15);
    REQUIRE (probe.messages[0].bytes == std::vector<std::uint8_t> { 0xf8 });
    REQUIRE (probe.messages[1].bytes == std::vector<std::uint8_t> { 0x90, 60, 100 });
    REQUIRE_THAT (probe.messages[1].timeMs, WithinAbs (100.0, 1e-9));
    REQUIRE (probe.messages[2].bytes == std::vector<std::uint8_t> { 0x90, 61, 110 });
    REQUIRE (probe.messages[3].bytes == std::vector<std::uint8_t> { 0xc2, 9 });
    REQUIRE (probe.messages[4].bytes == std::vector<std::uint8_t> { 0xd2, 70 });
    REQUIRE (probe.messages[5].bytes == std::vector<std::uint8_t> { 0xe2, 0, 64 });
    REQUIRE (probe.messages[6].bytes == std::vector<std::uint8_t> { 0xf1, 0x17 });
    REQUIRE (probe.messages[7].bytes == std::vector<std::uint8_t> { 0xf2, 1, 2 });
    REQUIRE (probe.messages[8].bytes == std::vector<std::uint8_t> { 0xf3, 3 });
    REQUIRE (probe.messages[9].bytes == std::vector<std::uint8_t> { 0xf6 });
    REQUIRE (probe.messages[10].bytes == std::vector<std::uint8_t> { 0xfa });
    REQUIRE (probe.messages[11].bytes == std::vector<std::uint8_t> { 0xfb });
    REQUIRE (probe.messages[12].bytes == std::vector<std::uint8_t> { 0xfc });
    REQUIRE (probe.messages[13].bytes == std::vector<std::uint8_t> { 0xfe });
    REQUIRE (probe.messages[14].bytes == std::vector<std::uint8_t> { 0xff });
}

TEST_CASE ("CoreMIDI SysEx spans packets and permits interleaved realtime", "[midi][coremidi][issue-298]")
{
    DecoderProbe probe;
    probe.push ({ 0xf0, 0x7f, 0x01 }, 10.0);
    probe.push ({ 0xf8, 0x01, 0x01, 0x60 }, 20.0);
    probe.push ({ 0x00, 0x00, 0x00, 0xf7 }, 30.0);
    REQUIRE (probe.messages.size() == 2);
    REQUIRE (probe.messages[0].bytes == std::vector<std::uint8_t> { 0xf8 });
    REQUIRE (probe.messages[1].bytes == std::vector<std::uint8_t> { 0xf0, 0x7f, 1, 1, 1, 0x60, 0, 0, 0, 0xf7 });
    REQUIRE_THAT (probe.messages[0].timeMs, WithinAbs (20.0, 1e-9));
    REQUIRE_THAT (probe.messages[1].timeMs, WithinAbs (10.0, 1e-9));
}

TEST_CASE ("CoreMIDI drops oversized SysEx whole and recovers at the next message", "[midi][coremidi][issue-298]")
{
    DecoderProbe probe;
    probe.push ({ 0xf0 });
    probe.push (std::vector<std::uint8_t> (dusk::kMidiBlockBytes, 1));
    probe.push ({ 0xf8, 0xf7, 0x90, 60, 100 });
    REQUIRE (probe.messages.size() == 2);
    REQUIRE (probe.messages[0].bytes == std::vector<std::uint8_t> { 0xf8 });
    REQUIRE (probe.messages[1].bytes == std::vector<std::uint8_t> { 0x90, 60, 100 });
}

TEST_CASE ("CoreMIDI largest accepted SysEx fits the collector ring", "[midi][coremidi][issue-298]")
{
    DecoderProbe probe;
    std::vector<std::uint8_t> sysex (dusk::kMidiBlockBytes - sizeof (double) - sizeof (std::int32_t), 1);
    sysex.front() = 0xf0;
    sysex.back() = 0xf7;
    probe.push (sysex);
    REQUIRE (probe.messages.size() == 1);
    dusk::MidiRing ring (dusk::kMidiBlockBytes);
    REQUIRE (ring.push (probe.messages[0].bytes.data(), static_cast<int> (probe.messages[0].bytes.size()), 100.0));
    std::vector<std::uint8_t> received;
    const auto count = ring.drain ([&] (const std::uint8_t* bytes, int size, double)
                                 { received.assign (bytes, bytes + size); });
    REQUIRE (count == 1);
    REQUIRE (received == sysex);

    probe.messages.clear();
    sysex.insert (sysex.end() - 1, 1);
    probe.push (sysex);
    REQUIRE (probe.messages.empty());
    probe.push ({ 0x90, 60, 100 });
    REQUIRE (probe.messages.size() == 1);
    REQUIRE (probe.messages[0].bytes == std::vector<std::uint8_t> { 0x90, 60, 100 });
}

TEST_CASE ("CoreMIDI ignores orphan data and discards interrupted messages", "[midi][coremidi][issue-298]")
{
    DecoderProbe probe;
    probe.push ({ 1, 2, 0x90, 60, 0xc0, 10, 0xf1, 1, 60, 100, 0xf0, 1, 0x80, 60, 0 });
    REQUIRE (probe.messages.size() == 3);
    REQUIRE (probe.messages[0].bytes == std::vector<std::uint8_t> { 0xc0, 10 });
    REQUIRE (probe.messages[1].bytes == std::vector<std::uint8_t> { 0xf1, 1 });
    REQUIRE (probe.messages[2].bytes == std::vector<std::uint8_t> { 0x80, 60, 0 });
    probe.push ({ 0x90, 61 });
    probe.decoder.reset();
    probe.push ({ 100 });
    REQUIRE (probe.messages.size() == 3);
}

TEST_CASE ("CoreMIDI orphan end of SysEx cancels running status without becoming a message", "[midi][coremidi][issue-298]")
{
    DecoderProbe probe;
    probe.push ({ 0x90, 60, 100, 0xf7, 61, 100, 0xf0, 0xf7 });
    REQUIRE (probe.messages.size() == 2);
    REQUIRE (probe.messages[0].bytes == std::vector<std::uint8_t> { 0x90, 60, 100 });
    REQUIRE (probe.messages[1].bytes == std::vector<std::uint8_t> { 0xf0, 0xf7 });
    probe.push ({ 0xf4, 0xf5 });
    REQUIRE (probe.messages.size() == 4);
    REQUIRE (probe.messages[2].bytes == std::vector<std::uint8_t> { 0xf4 });
    REQUIRE (probe.messages[3].bytes == std::vector<std::uint8_t> { 0xf5 });
}
