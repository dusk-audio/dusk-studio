#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "ui/LargestUniformRect.h"

#include <cstdint>
#include <vector>

using duskstudio::pixels::largestUniformRect;
using duskstudio::pixels::largestUniformShare;

namespace
{
struct Image
{
    int width, height;
    std::vector<unsigned char> rgb;

    Image (int w, int h, std::uint32_t colour) : width (w), height (h), rgb ((std::size_t) (w * h * 3))
    {
        fill (0, 0, w, h, colour);
    }

    void fill (int x0, int y0, int w, int h, std::uint32_t colour)
    {
        for (int y = y0; y < y0 + h; ++y)
            for (int x = x0; x < x0 + w; ++x)
            {
                auto* p = rgb.data() + ((std::size_t) y * (std::size_t) width + (std::size_t) x) * 3;
                p[0] = (unsigned char) (colour >> 16);
                p[1] = (unsigned char) (colour >> 8);
                p[2] = (unsigned char) colour;
            }
    }
};
} // namespace

TEST_CASE ("largestUniformRect: one colour is the whole buffer", "[pixels]")
{
    const Image image (37, 21, 0x20202a);
    const auto r = largestUniformRect (image.rgb.data(), image.width, image.height);
    CHECK (r.x == 0);
    CHECK (r.y == 0);
    CHECK (r.width == 37);
    CHECK (r.height == 21);
    CHECK (r.colour == 0x20202au);
    CHECK_THAT (largestUniformShare (image.rgb.data(), image.width, image.height),
                Catch::Matchers::WithinAbs (1.0, 1.0e-12));
}

TEST_CASE ("largestUniformRect: finds the empty block among drawn controls", "[pixels]")
{
    Image image (100, 80, 0x101010);
    // A checkerboard of controls over the top half leaves no large area there.
    for (int y = 0; y < 40; y += 4)
        for (int x = (y / 4) % 2 * 2; x < 100; x += 4)
            image.fill (x, y, 2, 2, 0xe0e0e0);
    // The bottom half is empty but for a one-pixel line splitting it unevenly.
    image.fill (30, 40, 1, 40, 0x808080);
    const auto r = largestUniformRect (image.rgb.data(), image.width, image.height);
    CHECK (r.colour == 0x101010u);
    CHECK (r.x == 31);
    CHECK (r.width == 69);
    CHECK (r.height == 40 + 2);
    CHECK (r.y == 38);
    CHECK (r.area() == 69LL * 42LL);
}

TEST_CASE ("largestUniformRect: a rectangle never mixes two colours of equal run length", "[pixels]")
{
    // Two side-by-side colour columns of the same height: each is its own
    // rectangle, not one spanning both.
    Image image (10, 6, 0xff0000);
    image.fill (4, 0, 6, 6, 0x00ff00);
    const auto r = largestUniformRect (image.rgb.data(), image.width, image.height);
    CHECK (r.colour == 0x00ff00u);
    CHECK (r.x == 4);
    CHECK (r.width == 6);
    CHECK (r.height == 6);
}

TEST_CASE ("largestUniformRect: an empty buffer has no rectangle", "[pixels]")
{
    CHECK (largestUniformRect (nullptr, 4, 4).area() == 0);
    const unsigned char pixel[3] = { 1, 2, 3 };
    CHECK (largestUniformRect (pixel, 0, 1).area() == 0);
    CHECK (largestUniformRect (pixel, 1, 1).area() == 1);
    CHECK_THAT (largestUniformShare (pixel, 0, 0), Catch::Matchers::WithinAbs (0.0, 0.0));
}
