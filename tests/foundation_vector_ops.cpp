#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "foundation/VectorOps.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace
{
dusk::audio::FloatMinMax referenceMinMax (const float* data, int count)
{
    if (count <= 0) return { 0.0f, 0.0f };
    float minValue = data[0];
    float maxValue = data[0];
    for (int i = 1; i < count; ++i)
    {
        minValue = std::min (minValue, data[i]);
        maxValue = std::max (maxValue, data[i]);
    }
    return { minValue, maxValue };
}

void requireSameMinMax (dusk::audio::FloatMinMax actual, dusk::audio::FloatMinMax expected)
{
    if (std::isnan (expected.min)) REQUIRE (std::isnan (actual.min));
    else                           REQUIRE_THAT (actual.min, WithinAbs (expected.min, 0.0f));

    if (std::isnan (expected.max)) REQUIRE (std::isnan (actual.max));
    else                           REQUIRE_THAT (actual.max, WithinAbs (expected.max, 0.0f));
}
} // namespace

TEST_CASE ("dusk::audio vector operations match scalar references", "[foundation][audio]")
{
    for (const int count : { 1, 3, 4, 7, 8, 64, 257 })
    {
        std::vector<float> source ((size_t) count);
        std::vector<float> copy ((size_t) count, -99.0f);
        std::vector<float> added ((size_t) count, 0.25f);
        std::vector<float> reference ((size_t) count);

        for (int i = 0; i < count; ++i)
        {
            source[(size_t) i] = 0.125f * (float) ((i * 17) % 23 - 11);
            reference[(size_t) i] = source[(size_t) i];
        }

        dusk::audio::vecClear (copy.data(), count);
        for (float value : copy)
            REQUIRE_THAT (value, WithinAbs (0.0f, 0.0f));

        dusk::audio::vecCopy (copy.data(), source.data(), count);
        for (int i = 0; i < count; ++i)
            REQUIRE_THAT (copy[(size_t) i], WithinAbs (source[(size_t) i], 0.0f));

        for (int i = 0; i < count; ++i)
            reference[(size_t) i] += added[(size_t) i];
        dusk::audio::vecAdd (added.data(), source.data(), count);
        for (int i = 0; i < count; ++i)
            REQUIRE_THAT (added[(size_t) i], WithinAbs (reference[(size_t) i], 0.0f));

        for (int i = 0; i < count; ++i)
            reference[(size_t) i] = -source[(size_t) i];
        dusk::audio::vecNegate (copy.data(), source.data(), count);
        for (int i = 0; i < count; ++i)
            REQUIRE_THAT (copy[(size_t) i], WithinAbs (reference[(size_t) i], 0.0f));
    }
}

TEST_CASE ("dusk::audio vecNegate supports in-place buffers", "[foundation][audio]")
{
    std::array<float, 8> values { -3.0f, -0.5f, 0.0f, 0.25f, 1.0f, 2.0f, -4.0f, 8.0f };
    const auto reference = values;
    dusk::audio::vecNegate (values.data(), values.data(), (int) values.size());

    for (size_t i = 0; i < values.size(); ++i)
        REQUIRE_THAT (values[i], WithinAbs (-reference[i], 0.0f));
}

TEST_CASE ("dusk::audio findSignedMinMax matches std::minmax_element", "[foundation][audio]")
{
    std::vector<float> values (257);
    for (int i = 0; i < (int) values.size(); ++i)
        values[(size_t) i] = 0.03125f * (float) (((i * 37) % 101) - 50);
    values[7] = -12.5f;
    values[193] = 13.75f;

    for (const int count : { 1, 3, 4, 7, 8, 64, 257 })
    {
        const auto expected = std::minmax_element (values.begin(), values.begin() + count);
        const auto actual = dusk::audio::findSignedMinMax (values.data(), count);
        REQUIRE_THAT (actual.min, WithinAbs (*expected.first, 0.0f));
        REQUIRE_THAT (actual.max, WithinAbs (*expected.second, 0.0f));
    }
}

TEST_CASE ("dusk::audio findSignedMinMax reports an empty range as zero", "[foundation][audio]")
{
    const auto empty = dusk::audio::findSignedMinMax (nullptr, 0);
    REQUIRE_THAT (empty.min, WithinAbs (0.0f, 0.0f));
    REQUIRE_THAT (empty.max, WithinAbs (0.0f, 0.0f));

    const auto negative = dusk::audio::findSignedMinMax (nullptr, -4);
    REQUIRE_THAT (negative.min, WithinAbs (0.0f, 0.0f));
    REQUIRE_THAT (negative.max, WithinAbs (0.0f, 0.0f));
}

TEST_CASE ("dusk::audio findSignedMinMax sees a NaN in any lane or tail slot",
           "[foundation][audio]")
{
    for (const int count : { 8, 9, 11, 16, 33, 64 })
    {
        for (int nanAt = 0; nanAt < count; ++nanAt)
        {
            std::vector<float> values ((size_t) count);
            for (int i = 0; i < count; ++i)
                values[(size_t) i] = 0.25f * (float) ((i * 13) % 7 - 3);

            // The extreme shares a SIMD lane with the NaN and lands after it,
            // so a vector pass that let the NaN stick in that lane's
            // accumulator would miss it.
            const int sameLane = nanAt + 4 < count ? nanAt + 4 : nanAt - 4;
            values[(size_t) sameLane] = -9.5f;
            values[(size_t) ((nanAt + 2) % count)] = 9.5f;
            values[(size_t) nanAt] = std::numeric_limits<float>::quiet_NaN();

            INFO ("count " << count << " nan at " << nanAt);
            requireSameMinMax (dusk::audio::findSignedMinMax (values.data(), count),
                               referenceMinMax (values.data(), count));
        }
    }
}
