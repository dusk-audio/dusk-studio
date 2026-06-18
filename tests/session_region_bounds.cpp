#include <catch2/catch_test_macros.hpp>

#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>

namespace
{
juce::File makeTempSessionDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("dusk-studio-region-bounds-"
                                    + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    return dir;
}
} // namespace

// Regression guard for the session-load validation gap: a hand-edited or
// truncated session.json can carry negative sample-domain region fields, which
// underflow PlaybackEngine's read-pointer math
// (readStart = sourceOffset + (firstWithin - timelineStart)) and silently lose
// audio / read out of bounds. The loader must clamp every such field to a sane
// range, mirroring the MIDI-note loader. We round-trip through save (which
// writes the raw model values) so the assertions exercise the real load path.
TEST_CASE ("SessionSerializer clamps out-of-range audio-region fields on load",
           "[session][serializer][bounds]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;
    using duskstudio::AudioRegion;
    using duskstudio::TakeRef;

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");

    Session a;

    AudioRegion r;
    r.file            = dir.getChildFile ("take.wav");   // need not exist on disk
    r.timelineStart   = -1000;
    r.lengthInSamples = -5000;
    r.sourceOffset    = -200;
    r.numChannels     = 99;                              // out of the [1,2] range

    TakeRef take;
    take.file            = dir.getChildFile ("take_prev.wav");
    take.sourceOffset    = -50;
    take.lengthInSamples = -10;
    r.previousTakes.push_back (take);

    a.track (0).regions.push_back (std::move (r));

    REQUIRE (SessionSerializer::save (a, target));

    Session b;
    REQUIRE (SessionSerializer::load (b, target));
    REQUIRE (b.track (0).regions.size() == 1);

    const auto& loaded = b.track (0).regions[0];
    REQUIRE (loaded.timelineStart   >= 0);
    REQUIRE (loaded.lengthInSamples >= 0);
    REQUIRE (loaded.sourceOffset    >= 0);
    REQUIRE (loaded.numChannels >= 1);
    REQUIRE (loaded.numChannels <= 2);

    REQUIRE (loaded.previousTakes.size() == 1);
    REQUIRE (loaded.previousTakes[0].sourceOffset    >= 0);
    REQUIRE (loaded.previousTakes[0].lengthInSamples >= 0);

    dir.deleteRecursively();
}
