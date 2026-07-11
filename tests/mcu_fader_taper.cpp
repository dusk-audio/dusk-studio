#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/McuFaderTaper.h"
#include "engine/McuProtocol.h"

using Catch::Matchers::WithinAbs;
using namespace duskstudio;

// The taper is the shared Mackie/Logic Control fader curve used by both
// McuController (dB -> pitch-bend) and McuReceiver (pitch-bend -> dB). The
// bug it fixes: a linear-in-dB map lands 0 dB at ~89% of throw, so the
// motor position disagreed with the surface's printed scale everywhere but
// the endpoints.

TEST_CASE ("McuFaderTaper: endpoints are exact", "[mcu][taper]")
{
    // Top of throw = kFaderMaxDb (+12) -> full-scale pitch-bend.
    REQUIRE (mcu::faderDbToPitchBend14 (12.0f) == mcu::kPitchBendMaxValue);
    // Bottom of throw = kFaderMinDb (-100, the -inf floor) -> 0.
    REQUIRE (mcu::faderDbToPitchBend14 (-100.0f) == 0);

    REQUIRE_THAT (mcu::pitchBend14ToFaderDb (mcu::kPitchBendMaxValue),
                  WithinAbs (12.0f, 1e-4f));
    REQUIRE_THAT (mcu::pitchBend14ToFaderDb (0),
                  WithinAbs (-100.0f, 1e-4f));
}

TEST_CASE ("McuFaderTaper: 0 dB sits at the printed unity position (~3/4 throw)",
           "[mcu][taper]")
{
    const int unity = mcu::faderDbToPitchBend14 (0.0f);

    // The Mackie/Logic taper anchors 0 dB at position 0.782 of full 14-bit
    // throw, i.e. pitch-bend 12808 — three-quarters up, not 89% as the old
    // linear-in-dB map produced.
    REQUIRE (unity == 12808);
    const float frac = (float) unity / (float) mcu::kPitchBendMaxValue;
    REQUIRE_THAT (frac, WithinAbs (0.782f, 0.01f));

    // The linear-in-dB map that this replaces would have put 0 dB near 0.89.
    REQUIRE (frac < 0.85f);

    // Exact round-trip back to unity.
    REQUIRE_THAT (mcu::pitchBend14ToFaderDb (unity), WithinAbs (0.0f, 1e-4f));
}

TEST_CASE ("McuFaderTaper: pitch-bend -> dB is monotonic over all 16384 codes",
           "[mcu][taper]")
{
    float prev = mcu::pitchBend14ToFaderDb (0);
    for (int pb = 1; pb <= mcu::kPitchBendMaxValue; ++pb)
    {
        const float db = mcu::pitchBend14ToFaderDb (pb);
        REQUIRE (db >= prev - 1e-6f);   // non-decreasing
        prev = db;
    }
}

TEST_CASE ("McuFaderTaper: pitch-bend round-trips through dB exactly for every code",
           "[mcu][taper]")
{
    // pb -> dB -> pb must be the identity: the two maps share breakpoints,
    // so a decoded fader value re-encodes to the same motor position (no
    // fight between incoming touch and outgoing motor feedback).
    for (int pb = 0; pb <= mcu::kPitchBendMaxValue; ++pb)
    {
        const float db  = mcu::pitchBend14ToFaderDb (pb);
        const int   rtp = mcu::faderDbToPitchBend14 (db);
        REQUIRE (rtp == pb);
    }
}

TEST_CASE ("McuFaderTaper: dB round-trips through pitch-bend within one code",
           "[mcu][taper]")
{
    // dB -> pb -> dB is limited only by 14-bit quantisation. Even at the
    // coarsest point (the compressed tail) one pitch-bend step spans a
    // fraction of a dB, so the error stays small.
    for (float db = -100.0f; db <= 12.0f; db += 0.05f)
    {
        const int   pb  = mcu::faderDbToPitchBend14 (db);
        const float rtd = mcu::pitchBend14ToFaderDb (pb);
        REQUIRE_THAT (rtd, WithinAbs (db, 0.2f));
    }
}

TEST_CASE ("McuFaderTaper: values clamp beyond the fader range", "[mcu][taper]")
{
    REQUIRE (mcu::faderDbToPitchBend14 (200.0f)  == mcu::kPitchBendMaxValue);
    REQUIRE (mcu::faderDbToPitchBend14 (-500.0f) == 0);
    REQUIRE (mcu::pitchBend14ToFaderDb (99999) == mcu::pitchBend14ToFaderDb (mcu::kPitchBendMaxValue));
    REQUIRE (mcu::pitchBend14ToFaderDb (-5)    == mcu::pitchBend14ToFaderDb (0));
}
