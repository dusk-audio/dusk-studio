#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "foundation/VectorOps.h"

#include <algorithm>
#include <array>
#include <vector>

using Catch::Matchers::WithinAbs;

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
