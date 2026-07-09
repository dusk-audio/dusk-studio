#include "Metronome.h"
#include "../foundation/Decibels.h"
#include <algorithm>
#include <cmath>

namespace duskstudio
{
constexpr float kPi = 3.14159265358979323846f;

void Metronome::prepare (double sampleRate)
{
    sr = sampleRate;
    clickLength = (int) (sampleRate * 0.005);  // 5 ms click
    if (clickLength < 4) clickLength = 4;
    reset();
}

void Metronome::reset() noexcept
{
    clickPos = -1;
    for (auto& v : voices) v.pos = -1;
    nextVoice = 0;
    lastBeatIdx = std::numeric_limits<std::int64_t>::min();
    lastBeatSeeded = false;
}

void Metronome::process (std::int64_t playheadStart, bool transportRolling,
                          float* L, float* R, int numSamples,
                          bool forceEnable) noexcept
{
    if (sr <= 0.0 || L == nullptr || R == nullptr) return;
    const bool effectiveEnabled = forceEnable
                                  || enabled.load (std::memory_order_relaxed);
    const bool poly = polyphonic.load (std::memory_order_relaxed);

    if (! effectiveEnabled)
    {
        // Disabled - but if a click was already sounding, finish it so we
        // don't hard-cut audio. New clicks are NOT triggered. Check ALL
        // voices in polyphonic mode + the legacy mono slot.
        bool anyInFlight = (clickPos >= 0);
        for (auto& v : voices) if (v.pos >= 0) { anyInFlight = true; break; }
        if (! anyInFlight) return;
    }

    if (! transportRolling) lastBeatSeeded = false;

    const float bpm = bpm_.load (std::memory_order_relaxed);
    if (bpm <= 0.0f) return;

    const double samplesPerBeat = sr * 60.0 / (double) bpm;
    if (samplesPerBeat < 1.0) return;

    const int   bpb  = std::max (1, beatsPerBar.load (std::memory_order_relaxed));
    const float vol  = dusk::audio::decibelsToGain (
                         volumeDb.load (std::memory_order_relaxed));

    auto triggerClick = [this, poly] (float freq)
    {
        if (poly)
        {
            // Round-robin voice grab — overwrite the oldest voice (the
            // slot we last assigned). With kVoices=4 and click bodies
            // ~80 ms at 5 ms sine + 4× envelope ramp, four overlapping
            // voices covers any realistic polyrhythm. Worst case = a
            // 5th click steals voice 0 mid-fade-out (negligible click
            // tail loss).
            auto& v = voices[(size_t) nextVoice];
            v.pos = 0;
            v.length = clickLength;
            v.freq = freq;
            nextVoice = (nextVoice + 1) % kVoices;
            // Keep the legacy mono path silent so we don't double-
            // trigger when toggling poly mid-play.
            clickPos = -1;
        }
        else
        {
            clickPos = 0;
            clickFreq = freq;
            // Idle all polyphonic voices so leftover ones don't ring on.
            for (auto& v : voices) v.pos = -1;
        }
    };

    auto renderVoice = [this, vol] (int& pos, int length, float freq,
                                     float* L_, float* R_, int i)
    {
        if (pos < 0 || pos >= length) return;
        const float t = (float) pos / (float) sr;
        const int rampLen = std::max (1, length / 4);
        float env = 1.0f;
        if (pos < rampLen)
            env = (float) pos / (float) rampLen;
        else if (pos > length - rampLen)
            env = (float) (length - pos) / (float) rampLen;

        const float accent = (freq > 1000.0f) ? 1.4f : 1.0f;
        const float s = std::sin (2.0f * kPi * freq * t) * env * vol * accent;
        L_[i] += s;
        R_[i] += s;
        ++pos;
        if (pos >= length) pos = -1;
    };

    for (int i = 0; i < numSamples; ++i)
    {
        const std::int64_t absSample = playheadStart + i;

        if (transportRolling && effectiveEnabled)
        {
            // floor() rounds toward -inf, so the beat index is correct for both
            // forward playback and a negative playhead (count-in pre-roll sets
            // the playhead to startSample - countInSamples). The old ceil()-1
            // form mis-indexed exact-beat negative positions, double/skipping
            // clicks across sample 0.
            const std::int64_t beatIdx =
                (std::int64_t) std::floor ((double) absSample / samplesPerBeat);

            if (! lastBeatSeeded)
            {
                lastBeatIdx = beatIdx;
                lastBeatSeeded = true;
            }
            else if (beatIdx != lastBeatIdx)
            {
                const int beatInBar = (int) (((beatIdx % bpb) + bpb) % bpb);
                // Downbeat (beat 0) higher pitch + the accent in
                // renderVoice; other beats lower pitch. User explicitly
                // requested this contour ("bar higher, beats lower").
                const float freq = (beatInBar == 0) ? 1200.0f : 880.0f;
                triggerClick (freq);
                lastBeatIdx = beatIdx;
            }
        }

        // Render BOTH paths every sample so an in-flight click tail
        // finishes after a poly toggle. triggerClick still owns mutual
        // exclusion at click-START time (poly trigger silences legacy
        // mono slot + vice versa), so we never double-fire. Tails just
        // ring out cleanly on whichever path was active when they
        // started. Inactive slots are no-ops (pos < 0 early-returns
        // inside renderVoice).
        for (auto& v : voices) renderVoice (v.pos, v.length, v.freq, L, R, i);
        renderVoice (clickPos, clickLength, clickFreq, L, R, i);
    }
}
} // namespace duskstudio
