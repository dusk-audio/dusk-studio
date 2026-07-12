#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/McuFaderTaper.h"
#include "engine/McuProtocol.h"
#include "engine/FaderBindingMap.h"

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

// The MIDI-bindings apply/pickup path (AudioEngine) shares these two helpers
// for its fader-dB targets: TrackFader, BusFader, AuxLaneFader, MasterFader.
// A PitchBend-triggered binding rides the Mackie taper; a CC/Note binding
// stays linear (generic 7-bit knob, no printed scale).

TEST_CASE ("FaderBindingMap: PitchBend fader binds ride the Mackie taper",
           "[mcu][taper][bindings]")
{
    // Unity (pb 12808) is what a Model-12-style motor fader sends at its
    // printed 0 dB — the taper must land it on 0 dB, not the ~-10 dB the old
    // linear-in-dB binding map produced. Consumed through the shared helper.
    const float unityFrac = 12808.0f / (float) mcu::kPitchBendMaxValue;
    REQUIRE_THAT (faderBindingFracToDb (unityFrac, /*pitchBend*/ true),
                  WithinAbs (0.0f, 1e-3f));

    // Top of throw = +12 dB; bottom = -100 dB, which sits below the
    // ChannelStripParams hard-mute floor (-90) so a zeroed motor fader still
    // silences the strip.
    REQUIRE_THAT (faderBindingFracToDb (1.0f, true), WithinAbs (12.0f, 1e-3f));
    REQUIRE_THAT (faderBindingFracToDb (0.0f, true), WithinAbs (-100.0f, 1e-3f));
    REQUIRE (faderBindingFracToDb (0.0f, true) < -90.0f);
}

TEST_CASE ("FaderBindingMap: CC/Note fader binds stay linear -90..+12 dB",
           "[mcu][taper][bindings]")
{
    REQUIRE_THAT (faderBindingFracToDb (0.0f, /*pitchBend*/ false), WithinAbs (-90.0f, 1e-4f));
    REQUIRE_THAT (faderBindingFracToDb (1.0f, false), WithinAbs (12.0f, 1e-4f));
    REQUIRE_THAT (faderBindingFracToDb (0.5f, false), WithinAbs (-39.0f, 1e-4f));

    // A 7-bit CC lands 0 dB at ~0.882 of throw — the deliberately different
    // behaviour from the PB taper (0 dB at ~0.782).
    REQUIRE_THAT (faderBindingFracToDb (90.0f / 102.0f, false), WithinAbs (0.0f, 1e-3f));
}

TEST_CASE ("FaderBindingMap: apply and pickup inverse agree (soft-takeover symmetry)",
           "[mcu][taper][bindings]")
{
    // faderBindingDbToFrac is the pickup read-back; it must invert
    // faderBindingFracToDb for the SAME trigger or takeover latches at the
    // wrong spot. Quantisation-limited on the PB side (14-bit codes).
    for (bool pb : { false, true })
    {
        for (int i = 0; i <= 100; ++i)
        {
            const float frac = (float) i / 100.0f;
            const float db   = faderBindingFracToDb (frac, pb);
            const float back = faderBindingDbToFrac (db, pb);
            REQUIRE_THAT (back, WithinAbs (frac, 2e-3f));
        }
    }
}

TEST_CASE ("McuFaderTaper: values clamp beyond the fader range", "[mcu][taper]")
{
    REQUIRE (mcu::faderDbToPitchBend14 (200.0f)  == mcu::kPitchBendMaxValue);
    REQUIRE (mcu::faderDbToPitchBend14 (-500.0f) == 0);
    REQUIRE (mcu::pitchBend14ToFaderDb (99999) == mcu::pitchBend14ToFaderDb (mcu::kPitchBendMaxValue));
    REQUIRE (mcu::pitchBend14ToFaderDb (-5)    == mcu::pitchBend14ToFaderDb (0));
}
