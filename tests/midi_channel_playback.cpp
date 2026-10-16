#include <catch2/catch_test_macros.hpp>

#include "engine/MidiTimelineSchedule.h"
#include "foundation/MidiBuffer.h"

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace duskstudio;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr float kBpm = 120.0f;

struct WireEvent
{
    std::vector<std::uint8_t> bytes;
    int sample = 0;
};

std::vector<WireEvent> playControls (const std::vector<MidiCc>& controls,
                                     std::int64_t start, std::int64_t end, bool chased)
{
    MidiRegion region;
    region.lengthInTicks = 100;
    region.lengthInSamples = ticksToSamples (region.lengthInTicks, kSampleRate, kBpm);
    region.ccs = controls;
    const auto timeline = buildMidiTimeline ({ region });
    midischedule::ControllerChase chase;
    dusk::MidiBuffer buffer;
    buffer.reserveBytes (4096);
    int scans = 100000;
    const auto outcome = midischedule::scheduleSpan (
        *timeline, nullptr, kSampleRate, kBpm, { start, end, chased }, scans, chase,
        [&buffer] (std::uint8_t status, std::uint8_t data1, std::uint8_t data2, std::int64_t sample)
        {
            const std::uint8_t bytes[] { status, data1, data2 };
            const auto kind = status & 0xf0;
            return buffer.addEvent (bytes, kind == 0xc0 || kind == 0xd0 ? 2 : 3, (int) sample);
        });
    REQUIRE (outcome == midischedule::Outcome::complete);

    std::vector<WireEvent> result;
    for (const auto metadata : buffer)
        result.push_back ({ { metadata.data, metadata.data + metadata.numBytes }, metadata.samplePosition });
    return result;
}

bool contains (const std::vector<WireEvent>& events, const std::vector<std::uint8_t>& bytes)
{
    return std::any_of (events.begin(), events.end(), [&bytes] (const auto& event)
    {
        return event.bytes == bytes;
    });
}
} // namespace

TEST_CASE ("MIDI playback preserves the full pitch-bend range", "[midi][schedule][channel-controls]")
{
    const std::vector<MidiCc> controls {
        { 6, 0, 0, 0, MidiCc::Kind::PitchBend },
        { 6, 0, 8192, 1, MidiCc::Kind::PitchBend },
        { 6, 0, 16383, 2, MidiCc::Kind::PitchBend }
    };
    SECTION ("scheduled events")
    {
        const auto events = playControls (controls, 0, 151, false);
        REQUIRE (events.size() == 3);
        CHECK (events[0].bytes == std::vector<std::uint8_t> { 0xe5, 0, 0 });
        CHECK (events[1].bytes == std::vector<std::uint8_t> { 0xe5, 0, 64 });
        CHECK (events[2].bytes == std::vector<std::uint8_t> { 0xe5, 127, 127 });
        CHECK (events[0].sample == 0);
        CHECK (events[1].sample == 50);
        CHECK (events[2].sample == 100);
    }
    SECTION ("chased event")
    {
        const auto events = playControls (controls, 125, 126, true);
        REQUIRE (events.size() == 1);
        CHECK (events[0].bytes == std::vector<std::uint8_t> { 0xe5, 127, 127 });
        CHECK (events[0].sample == 0);
    }
}

TEST_CASE ("MIDI chase keeps control kinds and channels independent", "[midi][schedule][channel-controls]")
{
    const std::vector<MidiCc> controls {
        { 1, 7, 30, 0 },
        { 1, 7, 80, 0, MidiCc::Kind::PolyPressure },
        { 1, 0, 8193, 0, MidiCc::Kind::PitchBend },
        { 1, 0, 64, 0, MidiCc::Kind::ChannelPressure },
        { 1, 0, 12, 0, MidiCc::Kind::ProgramChange },
        { 16, 0, 98, 0, MidiCc::Kind::ProgramChange },
        { 1, 7, 50, 4 }
    };
    const auto events = playControls (controls, 200, 201, true);
    REQUIRE (events.size() == 6);
    CHECK (contains (events, { 0xb0, 7, 50 }));
    CHECK_FALSE (contains (events, { 0xb0, 7, 30 }));
    CHECK (contains (events, { 0xa0, 7, 80 }));
    CHECK (contains (events, { 0xe0, 1, 64 }));
    CHECK (contains (events, { 0xd0, 64 }));
    CHECK (contains (events, { 0xc0, 12 }));
    CHECK (contains (events, { 0xcf, 98 }));
    for (const auto& event : events)
        CHECK (event.sample == 0);
}

TEST_CASE ("MIDI chase retains polyphonic pressure independently for each key",
           "[midi][schedule][channel-controls]")
{
    const std::vector<MidiCc> controls {
        { 2, 60, 30, 0, MidiCc::Kind::PolyPressure },
        { 2, 61, 40, 0, MidiCc::Kind::PolyPressure },
        { 2, 60, 90, 2, MidiCc::Kind::PolyPressure }
    };
    const auto events = playControls (controls, 150, 151, true);
    REQUIRE (events.size() == 2);
    CHECK (contains (events, { 0xa1, 60, 90 }));
    CHECK (contains (events, { 0xa1, 61, 40 }));
}

TEST_CASE ("MIDI program and channel-pressure playback use two-byte messages",
           "[midi][schedule][channel-controls]")
{
    const std::vector<MidiCc> controls {
        { 4, 0, 127, 0, MidiCc::Kind::ProgramChange },
        { 4, 0, 73, 1, MidiCc::Kind::ChannelPressure }
    };
    SECTION ("scheduled events")
    {
        const auto events = playControls (controls, 0, 101, false);
        REQUIRE (events.size() == 2);
        CHECK (events[0].bytes == std::vector<std::uint8_t> { 0xc3, 127 });
        CHECK (events[1].bytes == std::vector<std::uint8_t> { 0xd3, 73 });
    }
    SECTION ("chased events")
    {
        const auto events = playControls (controls, 100, 101, true);
        REQUIRE (events.size() == 2);
        CHECK (contains (events, { 0xc3, 127 }));
        CHECK (contains (events, { 0xd3, 73 }));
    }
}

TEST_CASE ("MIDI bank selection precedes a program at a playback or chase seam",
           "[midi][schedule][channel-controls]")
{
    std::vector<MidiCc> controls;
    bool chased = true;
    SECTION ("equal-tick input order")
    {
        controls = {
            { 1, 0, 4, 2 },
            { 1, 32, 6, 2 },
            { 1, 0, 9, 2, MidiCc::Kind::ProgramChange }
        };
        chased = false;
    }
    SECTION ("chased bank before explicit program")
    {
        controls = {
            { 1, 0, 4, 0 },
            { 1, 32, 6, 0 },
            { 1, 0, 9, 2, MidiCc::Kind::ProgramChange }
        };
    }
    SECTION ("explicit bank before chased program")
    {
        controls = {
            { 1, 0, 9, 0, MidiCc::Kind::ProgramChange },
            { 1, 0, 4, 2 },
            { 1, 32, 6, 2 }
        };
    }
    const auto events = playControls (controls, 100, 101, chased);
    REQUIRE (events.size() == 3);
    CHECK (events[0].bytes == std::vector<std::uint8_t> { 0xb0, 0, 4 });
    CHECK (events[1].bytes == std::vector<std::uint8_t> { 0xb0, 32, 6 });
    CHECK (events[2].bytes == std::vector<std::uint8_t> { 0xc0, 9 });
}
