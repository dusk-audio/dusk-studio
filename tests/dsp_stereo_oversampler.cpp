#include <catch2/catch_test_macros.hpp>

#include "foundation/StereoOversampler.h"

#include <dsp/DuskOversampler.hpp>

#include <random>
#include <vector>

// The split block up/down must reproduce the donor's fused per-sample round
// trip exactly: with no processing in between, StereoOversampler up→down on a
// channel is bit-identical to duskaudio::Oversampler::processSample(x, identity)
// fed the same samples. Same FIR kernels, same stage order, so the only thing
// this proves is that splitting up from down (and running both channels through
// independent FIR state) didn't perturb the arithmetic.
TEST_CASE ("StereoOversampler up/down matches the donor round trip", "[dsp][oversampler]")
{
    constexpr int N = 2048;

    std::mt19937 rng (0x0537u);
    std::uniform_real_distribution<float> dist (-1.0f, 1.0f);
    std::vector<float> in (N);
    for (auto& s : in) s = dist (rng);

    for (int factor : { 1, 2, 4 })
    {
        // Donor reference: fused, single channel, identity functor.
        duskaudio::Oversampler ref;
        ref.setFactor (factor);
        ref.reset();
        std::vector<float> refOut (N);
        for (int n = 0; n < N; ++n)
            refOut[(size_t) n] = ref.processSample (in[(size_t) n], [] (float s) noexcept { return s; });

        // Split, stereo: feed the same signal to L; feed a scaled copy to R to
        // confirm the channels are independent (no cross-talk).
        dusk::audio::StereoOversampler os;
        os.setFactor (factor);
        os.prepare (N);

        std::vector<float> L (in), R (N);
        for (int n = 0; n < N; ++n) R[(size_t) n] = -0.5f * in[(size_t) n];

        os.processSamplesUp (L.data(), R.data(), N);      // fills internal scratch
        os.processSamplesDown (L.data(), R.data(), N);    // no processing between

        for (int n = 0; n < N; ++n)
        {
            REQUIRE (L[(size_t) n] == refOut[(size_t) n]);
            REQUIRE (R[(size_t) n] == -0.5f * refOut[(size_t) n]);   // linear ⇒ scales exactly
        }
    }
}
