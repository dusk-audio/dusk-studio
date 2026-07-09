#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "foundation/Decibels.h"
#include "foundation/ScopedNoDenormals.h"
#include "foundation/SmoothedValue.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <limits>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE ("dusk::audio decibels match juce::Decibels", "[foundation][audio]")
{
    for (float db : { -120.f, -100.f, -80.f, -60.f, -12.f, -6.f, 0.f, 6.f, 12.f, 24.f })
        REQUIRE_THAT (dusk::audio::decibelsToGain (db),
                      WithinAbs (juce::Decibels::decibelsToGain (db), 0.0f));

    for (float g : { 0.f, 1e-6f, 0.001f, 0.5f, 1.f, 2.f, 4.f })
        REQUIRE_THAT (dusk::audio::gainToDecibels (g),
                      WithinAbs (juce::Decibels::gainToDecibels (g), 0.0f));

    // Custom -infinity floor, and the below-floor / zero-gain edges.
    REQUIRE_THAT (dusk::audio::decibelsToGain (-90.f, -80.f),
                  WithinAbs (juce::Decibels::decibelsToGain (-90.f, -80.f), 0.0f));
    REQUIRE_THAT (dusk::audio::gainToDecibels (0.f, -80.f),
                  WithinAbs (juce::Decibels::gainToDecibels (0.f, -80.f), 0.0f));
}

TEST_CASE ("dusk::audio::SmoothedValue matches juce (linear)", "[foundation][audio]")
{
    dusk::audio::SmoothedValue<float> d;
    juce::SmoothedValue<float>        j;

    d.reset (48000.0, 0.02);
    j.reset (48000.0, 0.02);
    d.setCurrentAndTargetValue (0.f);
    j.setCurrentAndTargetValue (0.f);

    d.setTargetValue (1.f);
    j.setTargetValue (1.f);
    REQUIRE (d.isSmoothing() == j.isSmoothing());

    for (int i = 0; i < 2000; ++i)
        REQUIRE_THAT (d.getNextValue(), WithinAbs (j.getNextValue(), 0.0f));

    REQUIRE_THAT (d.getCurrentValue(), WithinAbs (j.getCurrentValue(), 0.0f));
    REQUIRE_THAT (d.getTargetValue(),  WithinAbs (j.getTargetValue(),  0.0f));

    SECTION ("retarget mid-ramp")
    {
        d.setTargetValue (1.f);  j.setTargetValue (1.f);
        for (int i = 0; i < 300; ++i) { d.getNextValue(); j.getNextValue(); }
        d.setTargetValue (0.3f); j.setTargetValue (0.3f);
        for (int i = 0; i < 500; ++i)
            REQUIRE_THAT (d.getNextValue(), WithinAbs (j.getNextValue(), 0.0f));
    }

    SECTION ("skip matches")
    {
        d.setTargetValue (0.9f); j.setTargetValue (0.9f);
        REQUIRE_THAT (d.skip (100), WithinAbs (j.skip (100), 0.0f));
    }
}

TEST_CASE ("dusk::audio::ScopedNoDenormals flushes subnormals", "[foundation][audio]")
{
#if defined(DUSK_AUDIO_HAS_FTZ_SSE) || defined(DUSK_AUDIO_HAS_FTZ_ARM64)
    volatile float subnormal = std::numeric_limits<float>::min() / 4.0f;
    REQUIRE (subnormal > 0.0f);   // a genuine nonzero subnormal without the guard

    {
        dusk::audio::ScopedNoDenormals guard;
        volatile float x = subnormal;
        volatile float y = x * 0.5f;   // subnormal result is flushed to zero
        REQUIRE_THAT ((float) y, WithinAbs (0.0f, 0.0f));
    }

    // Restored on scope exit: subnormal arithmetic is representable again.
    volatile float z = subnormal * 0.5f;
    REQUIRE (z > 0.0f);
#else
    SUCCEED ("no flush-to-zero support on this platform");
#endif
}
