#include <catch2/catch_test_macros.hpp>

#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>

namespace
{
juce::File makeTempSessionDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("dusk-studio-transport-bounds-"
                                    + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    return dir;
}
} // namespace

// Regression guard for the transport / master load-validation gap. A hand-edited
// or corrupt session.json can carry tempo_bpm / beats_per_bar / beat_unit == 0,
// which divide-by-zero in the beat/bar math, plus out-of-range master/aux gains.
// The loader must clamp them — mirroring the audio-region clamp. We round-trip
// through save (which writes the raw model values) so the real load path runs.
TEST_CASE ("SessionSerializer clamps out-of-range transport + master fields on load",
           "[session][serializer][bounds]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    const auto dir    = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");

    Session a;
    a.tempoBpm.store       (0.0f);     // divide-by-zero source
    a.beatsPerBar.store    (0);        // divide-by-zero source
    a.beatUnit.store       (0);        // divide-by-zero source
    a.metronomeVolDb.store (999.0f);   // absurd gain
    a.uiStage.store        (99);       // past the 4-value Stage enum
    a.master().faderDb.store (1.0e9f); // absurd master gain

    REQUIRE (SessionSerializer::save (a, target));

    Session b;
    REQUIRE (SessionSerializer::load (b, target));

    // The crash-class fields: every divisor must be strictly positive.
    REQUIRE (b.tempoBpm.load()    >= 30.0f);
    REQUIRE (b.beatsPerBar.load() >= 1);
    REQUIRE (b.beatUnit.load()    >= 1);

    REQUIRE (b.metronomeVolDb.load() <= 12.0f);
    REQUIRE (b.uiStage.load() <= 3);
    REQUIRE (b.master().faderDb.load() <= 12.0f);

    dir.deleteRecursively();
}
