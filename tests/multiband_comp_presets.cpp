#include <catch2/catch_test_macros.hpp>

#include "dsp/MultibandCompPresets.h"

using namespace duskstudio::mbpresets;

// Guards the transcribed DP-24 preset table against data-entry errors: every
// value must land inside the donor multiband compressor's parameter ranges, or
// it would clamp silently on apply.
TEST_CASE ("multiband comp presets stay within donor parameter ranges")
{
    REQUIRE (kNumPresets == 9);

    const auto checkBand = [] (const Band& b)
    {
        REQUIRE (b.thresholdDb >= -60.0f); REQUIRE (b.thresholdDb <= 0.0f);
        REQUIRE (b.ratio       >=   1.0f); REQUIRE (b.ratio       <= 20.0f);
        REQUIRE (b.makeupDb    >= -12.0f); REQUIRE (b.makeupDb    <= 12.0f);
        REQUIRE (b.attackMs    >=  0.1f);  REQUIRE (b.attackMs    <= 100.0f);
        REQUIRE (b.releaseMs   >=  10.0f); REQUIRE (b.releaseMs   <= 1000.0f);
    };

    for (int i = 0; i < kNumPresets; ++i)
    {
        const auto& p = kPresets[(size_t) i];
        REQUIRE (p.name != nullptr);
        checkBand (p.low);
        checkBand (p.mid);
        checkBand (p.high);

        // crossover_1 range [20, 500]; hi-xover feeds crossover_3 [2000, 16000].
        REQUIRE (p.lowXoverHz >= 20.0f);   REQUIRE (p.lowXoverHz <= 500.0f);
        REQUIRE (p.hiXoverHz  >= 2000.0f); REQUIRE (p.hiXoverHz  <= 16000.0f);
        REQUIRE (p.hiXoverHz  >  p.lowXoverHz);
    }
}
