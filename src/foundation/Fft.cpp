#include "Fft.h"

#include "pffft.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dusk::audio
{
namespace
{
constexpr float kPi = 3.14159265358979323846f;

float* allocFloats (int n) noexcept
{
    return static_cast<float*> (pffft_aligned_malloc (sizeof (float) * (size_t) n));
}
} // namespace

Fft::Fft (int order)
    : size (1 << std::min (std::max (order, kMinOrder), kMaxOrder))
{
    stagingIn  = allocFloats (2 * size);
    stagingOut = allocFloats (2 * size);
    work       = allocFloats (2 * size);
}

Fft::~Fft()
{
    if (realSetup    != nullptr) pffft_destroy_setup (realSetup);
    if (complexSetup != nullptr) pffft_destroy_setup (complexSetup);
    pffft_aligned_free (stagingIn);
    pffft_aligned_free (stagingOut);
    pffft_aligned_free (work);
}

// The two pffft setups carry a twiddle table each, and no call site uses both
// kinds: build each on first use so a complex-only correlation at order ~20
// never allocates the real table (and vice versa).
PFFFT_Setup* Fft::getSetup (bool complexTransform) const noexcept
{
    auto*& setup = complexTransform ? complexSetup : realSetup;
    if (setup == nullptr)
        setup = pffft_new_setup (size, complexTransform ? PFFFT_COMPLEX : PFFFT_REAL);
    return setup;
}

void Fft::performRealOnlyForwardTransform (float* buf, bool onlyNonNegative) const noexcept
{
    auto* setup = getSetup (false);
    if (setup == nullptr || buf == nullptr || ! stagingReady()) return;

    std::memcpy (stagingIn, buf, sizeof (float) * (size_t) size);
    pffft_transform_ordered (setup, stagingIn, stagingOut, work, PFFFT_FORWARD);

    // pffft packs the two purely-real bins into the first complex slot:
    // stagingOut[0] is DC, stagingOut[1] is Nyquist. Bins 1..size/2-1 follow
    // as ordinary interleaved re/im pairs.
    const int half = size / 2;
    std::memcpy (buf + 2, stagingOut + 2, sizeof (float) * (size_t) (size - 2));
    buf[0] = stagingOut[0];
    buf[1] = 0.0f;
    buf[2 * half]     = stagingOut[1];
    buf[2 * half + 1] = 0.0f;

    if (! onlyNonNegative)
    {
        for (int i = half + 1; i < size; ++i)
        {
            buf[2 * i]     =  buf[2 * (size - i)];
            buf[2 * i + 1] = -buf[2 * (size - i) + 1];
        }
    }
}

void Fft::performFrequencyOnlyForwardTransform (float* buf) const noexcept
{
    if (getSetup (false) == nullptr || buf == nullptr || ! stagingReady()) return;

    performRealOnlyForwardTransform (buf, true);

    const int limit = size / 2 + 1;
    for (int i = 0; i < limit; ++i)
    {
        const float re = buf[2 * i];
        const float im = buf[2 * i + 1];
        buf[i] = std::sqrt (re * re + im * im);
    }

    std::memset (buf + limit, 0, sizeof (float) * (size_t) (2 * size - limit));
}

void Fft::perform (const std::complex<float>* in, std::complex<float>* out, bool inverse) const noexcept
{
    auto* setup = getSetup (true);
    if (setup == nullptr || in == nullptr || out == nullptr || ! stagingReady()) return;

    // in and out may alias: the input is fully copied into staging before any
    // output is written, and pffft reads only from staging.
    std::memcpy (stagingIn, reinterpret_cast<const float*> (in),
                 sizeof (float) * (size_t) (2 * size));
    pffft_transform_ordered (setup, stagingIn, stagingOut, work,
                             inverse ? PFFFT_BACKWARD : PFFFT_FORWARD);

    auto* dst = reinterpret_cast<float*> (out);
    if (inverse)
    {
        const float scale = 1.0f / (float) size;
        for (int i = 0; i < 2 * size; ++i)
            dst[i] = stagingOut[i] * scale;
    }
    else
    {
        std::memcpy (dst, stagingOut, sizeof (float) * (size_t) (2 * size));
    }
}

void Fft::fillHannWindow (float* table, int numSamples) noexcept
{
    if (table == nullptr || numSamples < 2) return;

    for (int i = 0; i < numSamples; ++i)
    {
        const float c = std::cos ((float) (2 * i) * kPi / (float) (numSamples - 1));
        table[i] = (float) (0.5 - 0.5 * (double) c);
    }
}
} // namespace dusk::audio
