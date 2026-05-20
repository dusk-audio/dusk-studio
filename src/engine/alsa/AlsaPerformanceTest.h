#pragma once

#if ! (defined(__linux__) || defined(__gnu_linux__))
 #error "AlsaPerformanceTest.h is ALSA/Linux-only"
#endif

#include <juce_core/juce_core.h>

namespace duskstudio
{
// Performance + stability test harness for the Dusk Studio-owned ALSA backend.
// Tier 1: drives an AlsaAudioIODevice directly with a measuring callback,
// no JUCE AudioDeviceManager involvement, no audible content, no special
// hardware setup beyond a real ALSA hw: device.
//
// Reports per-buffer-size:
//   - xrun count over a fixed-duration silent run
//   - per-callback wall-clock time: mean, p95, p99, max
//   - "verdict" against the period's audio-time budget
//
// Plus open/close cycle stress (catches resource leaks) and start/stop
// race (catches deadlock or inconsistent state).
//
// Hooked via DUSKSTUDIO_RUN_ALSA_PERF=1; see runHeadlessAlsaPerfTest in
// DuskStudioApp.cpp for the env var surface.
class AlsaPerformanceTest
{
public:
    struct Result
    {
        juce::String testName;
        juce::String configuration;     // "buf=512 rate=48000"
        bool         passed = false;

        int    xruns           = 0;
        double budgetMs        = 0.0;   // (period * 1000.0) / sampleRate
        double meanCallbackMs  = 0.0;
        double p95CallbackMs   = 0.0;
        double p99CallbackMs   = 0.0;
        double maxCallbackMs   = 0.0;
        int    callbackCount   = 0;

        // Negotiated format information from the most recent successful
        // open under this configuration. Populated per-cell so the matrix
        // report can flag rates where the device fell back to a different
        // bit depth than expected.
        int          negotiatedBitDepth = 0;     // 16 / 24 / 32, 0 if open failed
        int          activeOutChannels   = 0;
        int          activeInChannels    = 0;

        juce::String verdict;           // "SAFE" / "MARGINAL" / "UNSAFE" / "FAIL"
        juce::String details;
    };

    struct Options
    {
        juce::String              deviceId        = "hw:0,0";
        unsigned int              sampleRate      = 48000;
        int                       durationMs      = 5000;     // per buffer-size step
        int                       fakeDspLoadUs   = 0;        // synthetic CPU work in callback
        int                       openCloseCycles = 50;
        int                       startStopCycles = 20;
        bool                      runLoopback     = false;    // require user-supplied loopback path
        juce::Array<int>          bufferSizes;                // empty -> use default ladder
        juce::Array<unsigned int> sampleRates;                // empty -> just opts.sampleRate
    };

    // Loopback round-trip measurement. Only meaningful when there is a
    // physical loopback cable plugged from the device's first output to its
    // first input, OR snd-aloop is loaded and the deviceId points at the
    // loopback card. Without loopback the burst goes nowhere and signalDetected
    // stays false.
    struct LoopbackResult
    {
        bool         signalDetected = false;
        int          latencySamples = -1;
        double       latencyMs      = -1.0;   // -1 == not measured (matches latencySamples sentinel)
        int          burstStartSample = -1;
        int          firstSignalSample = -1;
        juce::String details;
    };

    // Run the full Tier 1 suite (buffer sweep + open/close + start/stop)
    // and return a markdown-style report. If opts.runLoopback is true,
    // also runs the loopback probe and includes results.
    static juce::String runAll (const Options& opts);

    // Individual entry points - useful for targeted reruns.
    static juce::Array<Result> runBufferSweep    (const Options& opts);
    static Result              runOpenCloseStress (const Options& opts);
    static Result              runStartStopRace   (const Options& opts);
    static LoopbackResult      runLoopbackProbe   (const Options& opts);
};
} // namespace duskstudio
