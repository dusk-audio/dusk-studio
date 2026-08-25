#pragma once

#include <DuskWidgets.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>

namespace duskspike
{

// The kit owns the skewed range and the meter ballistics; the spike only names them.
using Range = DuskWidgets::Range;

// The shipping parameters are plain atomics on ChannelStripParams; the spike mirrors the
// subset the strip draws, at the same ranges and defaults, so the knob feel is comparable.
struct StripParams
{
    std::string name = "KICK IN";

    std::atomic<float> faderDb { 0.0f };
    std::atomic<float> pan     { 0.0f };
    std::atomic<bool>  mute    { false };
    std::atomic<bool>  solo    { false };
    std::atomic<bool>  phaseInvert { false };
    std::atomic<bool>  inputMonitor { false };
    std::atomic<bool>  recordArmed  { true };
    std::atomic<bool>  printEffects { false };

    std::array<std::atomic<bool>, 4> busAssign {};

    std::atomic<float> hpfFreq { 20.0f };
    std::atomic<float> lpfFreq { 20000.0f };
    std::atomic<bool>  eqEnabled   { true };
    std::atomic<bool>  eqBlackMode { false };
    std::atomic<float> hfGainDb { 2.5f };  std::atomic<float> hfFreq { 8000.0f };
    std::atomic<float> hmGainDb { -1.5f }; std::atomic<float> hmFreq { 2000.0f }; std::atomic<float> hmQ { 0.7f };
    std::atomic<float> lmGainDb { 0.0f };  std::atomic<float> lmFreq { 600.0f };  std::atomic<float> lmQ { 1.4f };
    std::atomic<float> lfGainDb { 3.0f };  std::atomic<float> lfFreq { 100.0f };

    std::atomic<bool>  compEnabled { true };
    std::atomic<float> compFetRatioIndex { 0.0f };
    std::atomic<float> compFetAttack  { 0.2f };
    std::atomic<float> compFetRelease { 400.0f };
    std::atomic<float> compFetOutput  { 0.0f };
    std::atomic<float> compFetThresholdDb { -14.0f };

    std::array<std::atomic<float>, 4> auxSendDb { { { -12.0f }, { -100.0f }, { -24.0f }, { -100.0f } } };
    std::array<std::atomic<bool>, 4>  auxSendPreFader {};

    // Written by the stub source thread, read by the frame. Same contract as the engine's
    // meter atomics: dBFS, absolute per-block peak, no ballistics on the writer side.
    std::atomic<float> meterInputDb { -100.0f };
    std::atomic<float> meterGrDb   { 0.0f };

    static constexpr float kAuxSendOffDb = -100.0f;

    static const Range& faderRange()  { static const Range r = Range::withMidPoint (-90.0f, 6.0f, -12.0f); return r; }
    static const Range& hpfRange()    { static const Range r = Range::withMidPoint (20.0f, 300.0f, 80.0f); return r; }
    static const Range& lpfRange()    { static const Range r = Range::withMidPoint (3000.0f, 20000.0f, 8000.0f); return r; }
    static const Range& hfFreqRange() { static const Range r = Range::withMidPoint (1000.0f, 20000.0f, 8000.0f); return r; }
    static const Range& hmFreqRange() { static const Range r = Range::withMidPoint (600.0f, 13000.0f, 2000.0f); return r; }
    static const Range& lmFreqRange() { static const Range r = Range::withMidPoint (100.0f, 4000.0f, 600.0f); return r; }
    static const Range& lfFreqRange() { static const Range r = Range::withMidPoint (20.0f, 400.0f, 100.0f); return r; }
    static const Range& qRange()      { static const Range r = Range::withMidPoint (0.4f, 4.0f, 1.0f); return r; }
    static const Range& gainRange()   { static const Range r { -15.0f, 15.0f, 1.0f }; return r; }
    static const Range& atkRange()    { static const Range r = Range::withMidPoint (0.02f, 80.0f, 0.5f); return r; }
    static const Range& relRange()    { static const Range r = Range::withMidPoint (50.0f, 1100.0f, 300.0f); return r; }
    static const Range& makRange()    { static const Range r { -20.0f, 20.0f, 1.0f }; return r; }
    static const Range& auxRange()    { static const Range r = Range::withMidPoint (-60.0f, 6.0f, -12.0f); return r; }
    static const Range& panRange()    { static const Range r { -1.0f, 1.0f, 1.0f }; return r; }
};

// Stands in for the audio thread: a detached 30 Hz writer, which is the rate the UI polls
// meters at today. Its only job is to make the meters move like real programme material so
// the frame cost of the meter path is measured rather than guessed.
class StubMeterSource
{
public:
    explicit StubMeterSource (StripParams& p) : params (p) {}

    ~StubMeterSource() { stop(); }

    void start()
    {
        if (running.exchange (true))
            return;

        thread = std::thread ([this]
        {
            using clock = std::chrono::steady_clock;
            auto next = clock::now();
            float phase = 0.0f;

            while (running.load (std::memory_order_relaxed))
            {
                next += std::chrono::microseconds (33333);
                phase += 0.0333f;

                // A kick-ish envelope at 100 bpm plus a low bed, so the bar spends time in
                // all three meter zones and the peak-hold tick has something to hold.
                const float beat = std::fmod (phase * (100.0f / 60.0f), 1.0f);
                const float hit  = std::exp (-beat * 9.0f);
                const float bed  = 0.06f + 0.03f * std::sin (phase * 2.1f);
                const float lin  = std::min (1.4f, hit * 0.95f + bed);
                const float db   = 20.0f * std::log10 (std::max (1.0e-5f, lin));

                params.meterInputDb.store (db, std::memory_order_relaxed);

                const float over = db - params.compFetThresholdDb.load (std::memory_order_relaxed);
                params.meterGrDb.store (over > 0.0f ? -over * 0.6f : 0.0f, std::memory_order_relaxed);

                std::this_thread::sleep_until (next);
            }
        });
    }

    void stop()
    {
        if (running.exchange (false) && thread.joinable())
            thread.join();
    }

private:
    StripParams& params;
    std::atomic<bool> running { false };
    std::thread thread;
};

} // namespace duskspike
