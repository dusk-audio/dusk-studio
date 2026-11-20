#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace duskstudio::pixels
{
struct UniformRect
{
    int x = 0, y = 0, width = 0, height = 0;
    // 0xRRGGBB.
    std::uint32_t colour = 0;

    long long area() const noexcept { return (long long) width * (long long) height; }
};

// The largest axis-aligned rectangle whose pixels are all one colour, in an RGB
// buffer of `width` x `height` pixels, three bytes each, rows top to bottom. Each
// row keeps, per column, how far up the pixel's colour runs; along a run of one
// colour in the row those heights form a histogram, whose largest rectangle is
// found with a stack. Linear in the pixel count.
inline UniformRect largestUniformRect (const unsigned char* rgb, int width, int height)
{
    UniformRect best;
    if (rgb == nullptr || width <= 0 || height <= 0)
        return best;
    const auto at = [rgb, width] (int x, int y)
    {
        const auto* p = rgb + ((std::size_t) y * (std::size_t) width + (std::size_t) x) * 3;
        return ((std::uint32_t) p[0] << 16) | ((std::uint32_t) p[1] << 8) | (std::uint32_t) p[2];
    };
    std::vector<int> run ((std::size_t) width, 0);
    std::vector<int> stack;
    stack.reserve ((std::size_t) width + 1);
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
            run[(std::size_t) x] = y > 0 && at (x, y) == at (x, y - 1) ? run[(std::size_t) x] + 1 : 1;

        int start = 0;
        while (start < width)
        {
            const std::uint32_t colour = at (start, y);
            int end = start + 1;
            while (end < width && at (end, y) == colour)
                ++end;
            stack.clear();
            for (int x = start; x <= end; ++x)
            {
                const int h = x < end ? run[(std::size_t) x] : 0;
                while (! stack.empty() && run[(std::size_t) stack.back()] >= h)
                {
                    const int top = run[(std::size_t) stack.back()];
                    stack.pop_back();
                    const int left = stack.empty() ? start : stack.back() + 1;
                    const long long area = (long long) top * (long long) (x - left);
                    if (area > best.area())
                        best = { left, y - top + 1, x - left, top, colour };
                }
                stack.push_back (x);
            }
            start = end;
        }
    }
    return best;
}

// The share of the buffer's area that rectangle covers, 0 to 1.
inline double largestUniformShare (const unsigned char* rgb, int width, int height)
{
    if (width <= 0 || height <= 0)
        return 0.0;
    return (double) largestUniformRect (rgb, width, height).area() / ((double) width * (double) height);
}
} // namespace duskstudio::pixels
