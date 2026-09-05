#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "foundation/PlanarBuffer.h"

#include <limits>
#include <vector>

using Catch::Matchers::WithinAbs;

TEST_CASE ("dusk::audio::PlanarBuffer sizes and zeroes its channels", "[foundation][audio]")
{
    dusk::audio::PlanarBuffer buffer;
    REQUIRE (buffer.numChannels() == 0);
    REQUIRE (buffer.numSamples() == 0);

    buffer.setSize (3, 517);
    REQUIRE (buffer.numChannels() == 3);
    REQUIRE (buffer.numSamples() == 517);

    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 517; ++i)
            REQUIRE_THAT (buffer.channel (c)[i], WithinAbs (0.0f, 0.0f));

    for (int c = 0; c < 3; ++c)
        REQUIRE (buffer.data()[c] == buffer.channel (c));
}

TEST_CASE ("dusk::audio::PlanarBuffer keeps channels independent", "[foundation][audio]")
{
    dusk::audio::PlanarBuffer buffer;
    buffer.setSize (2, 100);

    for (int i = 0; i < 100; ++i)
    {
        buffer.channel (0)[i] = (float) i;
        buffer.channel (1)[i] = -(float) i;
    }

    for (int i = 0; i < 100; ++i)
    {
        REQUIRE_THAT (buffer.channel (0)[i], WithinAbs ((float) i, 0.0f));
        REQUIRE_THAT (buffer.channel (1)[i], WithinAbs (-(float) i, 0.0f));
    }

    // Padding keeps the next channel clear of a full-length write to this one.
    REQUIRE (buffer.channel (1) - buffer.channel (0) >= 100);

    buffer.clear();
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 100; ++i)
            REQUIRE_THAT (buffer.channel (c)[i], WithinAbs (0.0f, 0.0f));
}

TEST_CASE ("dusk::audio::PlanarBuffer resize republishes its channel pointers",
           "[foundation][audio]")
{
    dusk::audio::PlanarBuffer buffer;
    buffer.setSize (2, 64);
    buffer.channel (0)[0] = 1.0f;

    buffer.setSize (4, 4096);
    REQUIRE (buffer.numChannels() == 4);
    REQUIRE (buffer.numSamples() == 4096);

    for (int c = 0; c < 4; ++c)
    {
        REQUIRE (buffer.data()[c] == buffer.channel (c));
        buffer.channel (c)[4095] = (float) (c + 1);
    }
    for (int c = 0; c < 4; ++c)
    {
        REQUIRE_THAT (buffer.channel (c)[0], WithinAbs (0.0f, 0.0f));
        REQUIRE_THAT (buffer.channel (c)[4095], WithinAbs ((float) (c + 1), 0.0f));
    }

    buffer.setSize (0, 0);
    REQUIRE (buffer.numChannels() == 0);
    REQUIRE (buffer.numSamples() == 0);
    buffer.clear();
}

TEST_CASE ("dusk::audio::PlanarBuffer stride arithmetic holds at huge sample counts",
           "[foundation][audio]")
{
    using Buffer = dusk::audio::PlanarBuffer;
    REQUIRE (Buffer::strideFor (0) == 0);
    REQUIRE (Buffer::strideFor (-5) == 0);
    REQUIRE (Buffer::strideFor (1) == 16);
    REQUIRE (Buffer::strideFor (16) == 16);
    REQUIRE (Buffer::strideFor (17) == 32);

    constexpr int intMax = std::numeric_limits<int>::max();
    REQUIRE (Buffer::strideFor (intMax) == 2147483648LL);
    REQUIRE (Buffer::storageFor (2, intMax) == 4294967296LL);
    REQUIRE (Buffer::storageFor (2, intMax) > Buffer::kMaxTotalSamples);
}

TEST_CASE ("dusk::audio::PlanarBuffer refuses a size it cannot hold", "[foundation][audio]")
{
    dusk::audio::PlanarBuffer buffer;
    REQUIRE (buffer.setSize (2, 64));
    REQUIRE (buffer.numSamples() == 64);

    REQUIRE_FALSE (buffer.setSize (2, std::numeric_limits<int>::max()));
    REQUIRE (buffer.numChannels() == 0);
    REQUIRE (buffer.numSamples() == 0);
    buffer.clear();

    REQUIRE (buffer.setSize (1, 32));
    REQUIRE (buffer.numChannels() == 1);
    REQUIRE (buffer.numSamples() == 32);
}
