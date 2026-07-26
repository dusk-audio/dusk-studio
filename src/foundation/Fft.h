#pragma once

#include <complex>

struct PFFFT_Setup;

namespace dusk::audio
{
// Power-of-two FFT over vendored pffft (external/pffft), with the buffer
// layouts and scaling conventions of the framework FFT it replaces, so the
// analysis call sites keep their existing unpacking code.
//
// Not thread-safe: one instance owns the staging buffers every transform runs
// through, so a single instance must not be driven from two threads at once.
// Every call site is worker- or message-thread and owns its instance.
class Fft
{
public:
    // pffft's minimum is 32 points real / 16 complex; the ceiling keeps a
    // mistaken order from asking for a gigabyte of twiddle table.
    static constexpr int kMinOrder = 5;
    static constexpr int kMaxOrder = 24;

    // order is clamped to [kMinOrder, kMaxOrder].
    explicit Fft (int order);
    ~Fft();

    Fft (const Fft&) = delete;
    Fft& operator= (const Fft&) = delete;

    int getSize() const noexcept { return size; }

    // buf holds size real samples on entry and 2*size floats of capacity. On
    // return it holds interleaved re/im pairs: bins 0..size/2 always (DC and
    // Nyquist have zero imaginary part), plus the conjugate-mirrored bins
    // size/2+1..size-1 when onlyNonNegative is false.
    void performRealOnlyForwardTransform (float* buf, bool onlyNonNegative = false) const noexcept;

    // buf holds size real samples on entry and 2*size floats of capacity. On
    // return buf[0..size/2] are the bin magnitudes and the rest is zeroed.
    void performFrequencyOnlyForwardTransform (float* buf) const noexcept;

    // size complex values in and out; in and out may alias. Forward is
    // unscaled, inverse is scaled by 1/size.
    void perform (const std::complex<float>* in, std::complex<float>* out, bool inverse) const noexcept;

    // Symmetric, non-normalised Hann: 0.5 - 0.5*cos(2*pi*i/(numSamples-1)).
    static void fillHannWindow (float* table, int numSamples) noexcept;

private:
    PFFFT_Setup* getSetup (bool complexTransform) const noexcept;

    bool stagingReady() const noexcept
    {
        return stagingIn != nullptr && stagingOut != nullptr && work != nullptr;
    }

    int size = 0;
    mutable PFFFT_Setup* realSetup = nullptr;
    mutable PFFFT_Setup* complexSetup = nullptr;
    float* stagingIn = nullptr;
    float* stagingOut = nullptr;
    float* work = nullptr;
};
} // namespace dusk::audio
