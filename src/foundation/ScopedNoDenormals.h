#pragma once

#include <cstdint>

// RAII flush-to-zero / denormals-are-zero guard, matching JUCE ScopedNoDenormals:
// snapshot the FP control register, enable FTZ+DAZ for the scope, restore on exit.
// Prevents the CPU stalls that subnormal floats cause in audio DSP loops.
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
 #include <xmmintrin.h>
 #define DUSK_AUDIO_HAS_FTZ_SSE 1
#elif defined(__aarch64__) || defined(_M_ARM64)
 #define DUSK_AUDIO_HAS_FTZ_ARM64 1
#endif

namespace dusk::audio
{
class ScopedNoDenormals
{
public:
    ScopedNoDenormals() noexcept
    {
#if defined(DUSK_AUDIO_HAS_FTZ_SSE)
        saved = _mm_getcsr();
        _mm_setcsr ((unsigned int) saved | 0x8040u);   // FTZ (0x8000) | DAZ (0x0040)
#elif defined(DUSK_AUDIO_HAS_FTZ_ARM64)
        std::uint64_t fpcr;
        asm volatile ("mrs %0, fpcr" : "=r" (fpcr));
        saved = fpcr;
        asm volatile ("msr fpcr, %0" : : "r" (fpcr | (std::uint64_t (1) << 24)));   // FZ
#endif
    }

    ~ScopedNoDenormals() noexcept
    {
#if defined(DUSK_AUDIO_HAS_FTZ_SSE)
        _mm_setcsr ((unsigned int) saved);
#elif defined(DUSK_AUDIO_HAS_FTZ_ARM64)
        asm volatile ("msr fpcr, %0" : : "r" (saved));
#endif
    }

    ScopedNoDenormals (const ScopedNoDenormals&)            = delete;
    ScopedNoDenormals& operator= (const ScopedNoDenormals&) = delete;

private:
#if defined(DUSK_AUDIO_HAS_FTZ_SSE) || defined(DUSK_AUDIO_HAS_FTZ_ARM64)
    std::uint64_t saved = 0;   // MXCSR (x86) / FPCR (arm64) snapshot
#endif
};
} // namespace dusk::audio
