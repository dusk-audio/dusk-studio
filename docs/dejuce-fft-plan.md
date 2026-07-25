# De-JUCE — FFT tower (executable spec)

Status: **F0–F2 pending.** Branch `dejuce/fft`. One PR. Read
`docs/dejuce-campaign.md` + the memory ledger first, per ritual.

## Goal

DpAligner and MasteringEqEditor off JUCE's dsp::FFT and WindowingFunction,
onto a vendored pffft behind a `dusk::audio::Fft` wrapper with
JUCE-compatible call semantics. `juce_dsp` stays linked (ProcessSpec /
DelayLine / Oversampling consumers remain); no gate movement expected —
both files keep other JUCE tokens. tests/console_saturation.cpp keeps its
JUCE FFT (tests may link JUCE freely).

## Library decision

Original single-file pffft (Julien Pommier, FFTPACK license, BSD-style,
GPL-compatible). Already fetched, smoke-compiled. Radix 2/3/5, min N 32
real / 16 complex — all call sites are pow2, orders 10–~19.

## F0 — vendor + wrapper + A/B tests

1. `external/pffft/pffft.c` + `pffft.h` verbatim (from scratchpad copy);
   LICENSES.txt entry (hand-maintained file — mirror the existing entry
   format). Compile pffft.c into the app target and the test target
   (C file; suppress third-party warnings the way other vendored code does,
   check how CMakeLists treats external/).
2. `src/foundation/Fft.{h,cpp}`, `namespace dusk::audio`, `class Fft`:
   - `explicit Fft (int order)` — pow2 size = 1<<order.
   - `void performRealOnlyForwardTransform (float* buf, bool onlyNonNegative) const`
     — JUCE layout contract: input reals in buf[0..N), output N/2+1
     interleaved re/im pairs from buf[0] (DC and Nyquist imag = 0).
     Implemented as pffft real forward (ordered) + unpack: pffft packs
     DC.re in slot 0 and Nyquist.re in slot 1.
   - `void performFrequencyOnlyForwardTransform (float* buf) const` —
     real forward then magnitudes sqrt(re^2+im^2) into buf[0..N/2],
     match JUCE's zeroing of the remainder (verify JUCE's exact behavior
     in its source before writing the A/B assertion).
   - `void perform (const std::complex<float>* in, std::complex<float>* out, bool inverse) const`
     — out-of-place ONLY (assert in != out); forward unscaled, inverse
     scaled by 1/N to match JUCE.
   - `static void fillHannWindow (float* table, int numSamples)` —
     0.5 - 0.5*cos(2*pi*i/(N-1)), symmetric, non-normalised — must match
     JUCE WindowingFunction hann(normalise=false) exactly.
   - Wrapper owns pffft setup + internal 16-byte-aligned work/staging
     buffers (pffft_aligned_malloc); copies caller data through staging so
     call-site buffers need no alignment. All call sites are worker/message
     thread — copies are fine, no RT constraint. Non-copyable; header must
     stay free of JUCE tokens.
3. Narrow-link Catch2 A/B tests `tests/fft_pffft_parity.cpp` (JUCE linked
   in the test target already): real-forward parity order 10 vs
   juce::dsp::FFT on random + impulse + DC + Nyquist-rate signals
   (WithinAbs 1e-4 scaled); complex forward+inverse round-trip and parity
   order 12 incl. out-of-place aliasing guard; frequency-only parity order
   11; Hann table parity vs juce::dsp::WindowingFunction (1e-6); inverse
   scaling matches JUCE (1/N).

## F1 — call-site flips

- `src/engine/DpAligner.cpp` — onsetEnvelope: `dusk::audio::Fft` real
  forward, keep hand-rolled Hann or switch to `fillHannWindow` (identical
  formula — switching is fine); crossCorrelate: complex forward x2 +
  inverse, `std::complex<float>` vectors replace juce::dsp::Complex, keep
  the out-of-place discipline and the comment explaining WHY (rewrite it to
  reference the wrapper's out-of-place contract, not JUCE FFTFallback).
  Drop the `<juce_dsp/juce_dsp.h>` include; `juce::MathConstants` -> local
  constant if it was only for the window. juce::File/AudioBuffer stay.
- `src/ui/MasteringEqEditor.{h,cpp}` — member `dusk::audio::Fft fft
  { kFftOrder }`; WindowingFunction member -> `std::array<float, kFftSize>`
  filled once with `fillHannWindow`; multiplyWithWindowingTable -> plain
  loop or FloatVectorOperations::multiply (file keeps JUCE anyway);
  `performFrequencyOnlyForwardTransform` call shape unchanged. Drop the
  `<juce_dsp/juce_dsp.h>` include from the header. kRef scaling math
  unchanged.

## F2 — verify

App build zero new warnings; full ctest incl. the 4 dp_aligner_align
end-to-end cases (real WAV alignment — the integration A/B); gate must
stay <= 182; screenshot harness run + visual check of the MasteringEqEditor
spectrum figure (analyzer must still draw a sane spectrum); selftest under
Xvfb. AI-slop sweep. MANUAL.md: no user-visible change expected — confirm.

## Owed to Marc's bench

- Ear check: DP import alignment on a real session (headless parity only
  proves math, not musical result).

## Resume phrase

"FFT tower, branch dejuce/fft — check spec status line, continue at first
unchecked phase."
