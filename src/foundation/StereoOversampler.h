#pragma once

#include <dsp/DuskOversampler.hpp>   // duskaudio::HalfbandFIR + hbtaps (donor)

#include <cstddef>
#include <vector>

// Stereo block oversampler built from the donor's halfband-FIR kernels. Where
// duskaudio::Oversampler fuses up + process + down behind a single-channel
// per-sample functor, this splits the round trip into a block UP phase and a
// block DOWN phase so a stereo, look-ahead, sample-buffered algorithm (the
// brickwall limiter, true-peak metering) can run its own pass over BOTH
// channels' oversampled samples in between — the functor form can't, because it
// hands back each processed sample before the next channel is seen.
//
// The FIR operations, tap sets and per-stage ordering are exactly the donor's
// (2x = stage A 47-tap, 4x = stage A then stage B 15-tap), so up→down with no
// intervening processing is the same linear-phase round trip the donor performs
// — verified sample-for-sample against duskaudio::Oversampler in the tests.
namespace dusk::audio
{
class StereoOversampler
{
public:
    // factor must be 1, 2, or 4 (1 = passthrough). Set before prepare().
    void setFactor (int f) noexcept { factor = (f == 4) ? 4 : (f == 2 ? 2 : 1); }
    int  getFactor() const noexcept { return factor; }

    // Sizes the oversampled scratch for the largest block/factor. Idempotent.
    void prepare (int maxBlockSize) noexcept
    {
        const std::size_t n = (std::size_t) (maxBlockSize > 0 ? maxBlockSize : 0) * 4;
        scratchL.assign (n, 0.0f);
        scratchR.assign (n, 0.0f);
        reset();
    }

    void reset() noexcept
    {
        for (int c = 0; c < 2; ++c)
        {
            upA[c].reset();  downA[c].reset();
            upB[c].reset();  downB[c].reset();
        }
    }

    // Fixed group delay of the up+down FIR round trip, in base-rate samples
    // (mirrors duskaudio::Oversampler::latency): 2x stage 23, 4x adds 3.5.
    float latency() const noexcept
    {
        if (factor == 4) return 23.0f + 3.5f;
        if (factor == 2) return 23.0f;
        return 0.0f;
    }

    // Writable view of one channel's oversampled samples, returned by up().
    struct Block { float* L; float* R; int numSamples; };

    // Upsamples L/R (numSamples each) into the internal scratch and returns
    // pointers to it. The caller processes the samples in place, then calls
    // processSamplesDown to fold them back. Pointers stay valid until the next
    // up/down call.
    Block processSamplesUp (const float* L, const float* R, int numSamples) noexcept
    {
        upChannel (0, L, scratchL.data(), numSamples);
        upChannel (1, R, scratchR.data(), numSamples);
        return { scratchL.data(), scratchR.data(), numSamples * factor };
    }

    // Downsamples the (caller-modified) scratch back into L/R.
    void processSamplesDown (float* L, float* R, int numSamples) noexcept
    {
        downChannel (0, scratchL.data(), L, numSamples);
        downChannel (1, scratchR.data(), R, numSamples);
    }

private:
    void upChannel (int c, const float* in, float* out, int numSamples) noexcept
    {
        int oi = 0;
        for (int n = 0; n < numSamples; ++n)
        {
            const float x = in[n];
            if (factor == 1) { out[oi++] = x; continue; }

            upA[c].push (x);    const float a0 = 2.0f * upA[c].out (duskaudio::hbtaps::kA);
            upA[c].push (0.0f); const float a1 = 2.0f * upA[c].out (duskaudio::hbtaps::kA);
            if (factor == 2) { out[oi++] = a0; out[oi++] = a1; continue; }

            for (float a : { a0, a1 })
            {
                upB[c].push (a);    out[oi++] = 2.0f * upB[c].out (duskaudio::hbtaps::kB);
                upB[c].push (0.0f); out[oi++] = 2.0f * upB[c].out (duskaudio::hbtaps::kB);
            }
        }
    }

    void downChannel (int c, const float* os, float* out, int numSamples) noexcept
    {
        int oi = 0;
        for (int n = 0; n < numSamples; ++n)
        {
            if (factor == 1) { out[n] = os[oi++]; continue; }

            if (factor == 2)
            {
                downA[c].push (os[oi++]); downA[c].push (os[oi++]);
                out[n] = downA[c].out (duskaudio::hbtaps::kA);
                continue;
            }

            downB[c].push (os[oi++]); downB[c].push (os[oi++]);
            const float d0 = downB[c].out (duskaudio::hbtaps::kB);
            downB[c].push (os[oi++]); downB[c].push (os[oi++]);
            const float d1 = downB[c].out (duskaudio::hbtaps::kB);
            downA[c].push (d0); downA[c].push (d1);
            out[n] = downA[c].out (duskaudio::hbtaps::kA);
        }
    }

    int factor = 4;
    duskaudio::HalfbandFIR<47, 12> upA[2], downA[2];   // base <-> 2x
    duskaudio::HalfbandFIR<15, 4>  upB[2], downB[2];   // 2x  <-> 4x
    std::vector<float> scratchL, scratchR;
};
} // namespace dusk::audio
