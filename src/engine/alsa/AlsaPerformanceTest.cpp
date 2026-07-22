#include "AlsaPerformanceTest.h"
#include "AlsaAudioIODevice.h"

#include "../device/ChannelSet.h"
#include "../device/IODeviceCallback.h"
#include "../../foundation/Text.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <memory>
#include <thread>
#include <vector>

namespace duskstudio
{
namespace
{
// Default buffer ladder if Options::bufferSizes is empty. Standard pro-audio
// powers-of-two from 32 to 2048 - covers the latency range users actually
// pick from. 4096+ is rare for live work and stable everywhere; not worth
// the test time.
const int kDefaultBufferLadder[] = { 32, 64, 128, 256, 512, 1024, 2048 };

// Count this many callbacks before we start storing timings - lets the
// audio thread's first-period spikes (page-fault cost, RT priority promotion
// scheduler dance) settle before we take measurements. ~50ms warmup at
// 48k/512 is two periods; we extend to 8 to be safe at very small buffers.
constexpr int kWarmupCallbacks = 8;

// Per-callback timings: pre-allocated ring buffer, lock-free writes from
// the audio thread, read after stop. Sized for worst-case 5s at 32 frames
// per callback at 192 kHz = 30000 callbacks; 64K entries gives plenty of
// headroom and is still trivially small at 8 bytes each (~512 KB).
constexpr int kSampleBufferSize = 65536;

// Verdict thresholds. UNSAFE = any xruns OR p99 within 90% of budget.
// MARGINAL = p99 within 70% of budget. Otherwise SAFE.
constexpr double kP99WarningRatio = 0.70;
constexpr double kP99FailureRatio = 0.90;

using Clock = std::chrono::steady_clock;

void sleepMs (int ms)
{
    std::this_thread::sleep_for (std::chrono::milliseconds (ms));
}

class MeasuringCallback final : public device::IODeviceCallback
{
public:
    explicit MeasuringCallback (int fakeLoadMicroseconds)
        : fakeLoadUs (fakeLoadMicroseconds)
    {
        samples.resize (kSampleBufferSize, 0.0);
    }

    void audioDeviceAboutToStart (device::IODevice* dev) override
    {
        const auto sr = dev->getCurrentSampleRate();
        const auto bs = dev->getCurrentBufferSizeSamples();
        budgetMs = (sr > 0.0) ? 1000.0 * (double) bs / sr : 0.0;
        callbackCount.store (0, std::memory_order_relaxed);
    }

    void audioDeviceStopped() override {}

    void audioDeviceIOCallback (const float* const* /*inputChannelData*/,
                                int /*numInputChannels*/,
                                float* const* outputChannelData,
                                int numOutputChannels,
                                int numSamples,
                                const device::CallbackContext&) override
    {
        const auto t0 = Clock::now();

        // Synthetic DSP load: busy-wait so the audio thread actually consumes
        // wall-clock time. Sleep would yield to the scheduler and produce
        // misleading "free" headroom.
        if (fakeLoadUs > 0)
        {
            const auto deadline = t0 + std::chrono::microseconds (fakeLoadUs);
            while (Clock::now() < deadline) { /* spin */ }
        }

        // Output silence. Tier 1 measures stability, not signal correctness;
        // real-signal validation is Tier 3 and needs loopback hardware.
        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (outputChannelData[ch] != nullptr)
                std::memset (outputChannelData[ch], 0,
                              sizeof (float) * (size_t) numSamples);

        const auto t1 = Clock::now();
        const double elapsedMs =
            std::chrono::duration<double, std::milli> (t1 - t0).count();

        const int idx = callbackCount.fetch_add (1, std::memory_order_relaxed);
        if (idx >= kWarmupCallbacks && (idx - kWarmupCallbacks) < kSampleBufferSize)
            samples[(size_t) (idx - kWarmupCallbacks)] = elapsedMs;
    }

    // After stop(): collect stats from the measured-callback samples.
    struct Stats { double mean = 0, p95 = 0, p99 = 0, max = 0; int n = 0; };

    Stats computeStats() const
    {
        const int total = callbackCount.load (std::memory_order_acquire);
        const int measured = std::max (0, total - kWarmupCallbacks);
        const int n = std::min (measured, kSampleBufferSize);
        Stats s;
        s.n = n;
        if (n == 0)
            return s;

        std::vector<double> sorted (samples.begin(), samples.begin() + n);
        std::sort (sorted.begin(), sorted.end());

        double sum = 0.0;
        for (double v : sorted) sum += v;
        s.mean = sum / (double) n;
        s.max  = sorted.back();
        // Linear-interpolation percentiles. For our sample sizes (hundreds
        // to tens of thousands), exact-rank is fine.
        auto pct = [&] (double p)
        {
            const double idxF = p * (double) (n - 1);
            const int lo = (int) std::floor (idxF);
            const int hi = (int) std::ceil  (idxF);
            const double frac = idxF - (double) lo;
            return sorted[(size_t) lo] * (1.0 - frac) + sorted[(size_t) hi] * frac;
        };
        s.p95 = pct (0.95);
        s.p99 = pct (0.99);
        return s;
    }

    int totalCallbacks() const { return callbackCount.load (std::memory_order_acquire); }
    double getBudgetMs() const { return budgetMs; }

private:
    std::vector<double> samples;
    std::atomic<int>    callbackCount { 0 };
    int                 fakeLoadUs    = 0;
    double              budgetMs      = 0.0;
};

// Open + start an AlsaAudioIODevice in duplex mode. Both directions opened
// against the same hw: device id, two channels active each way. Exercises
// snd_pcm_link, readi, and the deinterleave path - the input data isn't
// analyzed by the buffer-sweep callback, but going through the code paths
// is what catches duplex-only bugs (link drift, capture xrun, deinterleave
// stride). Caller owns the device, must stop+close and dispose through
// AlsaAudioIODevice::destroyOrPark so a wedged I/O thread leaks the device
// instead of racing its destructor.
std::unique_ptr<AlsaAudioIODevice> openDuplex (const std::string& deviceId,
                                                  unsigned int sampleRate,
                                                  int          bufferSize,
                                                  device::IODeviceCallback* callback,
                                                  std::string& errorOut)
{
    auto dev = std::make_unique<AlsaAudioIODevice> (deviceId, /*inId*/ deviceId, /*outId*/ deviceId);
    device::ChannelSet outMask, inMask;
    outMask.setRange (0, 2, true);  // bits 0 + 1 of output
    inMask .setRange (0, 2, true);  // bits 0 + 1 of input

    const auto err = dev->open (inMask, outMask, (double) sampleRate, bufferSize);
    if (! err.empty())
    {
        errorOut = err;
        return nullptr;
    }
    dev->start (callback);
    return dev;
}

std::string formatVerdict (int xruns, double p99, double budget)
{
    if (xruns > 0)                                  return "UNSAFE (xruns)";
    if (budget <= 0.0 || p99 <= 0.0)                return "?";
    if (p99 >= budget * kP99FailureRatio)           return "UNSAFE (p99 near budget)";
    if (p99 >= budget * kP99WarningRatio)           return "MARGINAL";
    return "SAFE";
}

std::string formatTableRow (const AlsaPerformanceTest::Result& r)
{
    auto fmt = [] (double v)
    {
        if (v <= 0.0) return std::string ("    -  ");
        return dusk::text::format ("%6.3f ms", v);
    };
    auto bufFromCfg = [] (const std::string& cfg)
    {
        return dusk::text::upToFirstOccurrenceOf (
                   dusk::text::fromFirstOccurrenceOf (cfg, "buf=", false), " ", false);
    };

    return dusk::text::format (
        " %5s |  %s | %s | %s | %s | %s |    %4d   | %s",
        bufFromCfg (r.configuration).c_str(),
        fmt (r.budgetMs).c_str(),
        fmt (r.meanCallbackMs).c_str(),
        fmt (r.p95CallbackMs).c_str(),
        fmt (r.p99CallbackMs).c_str(),
        fmt (r.maxCallbackMs).c_str(),
        r.xruns,
        r.verdict.c_str());
}
} // namespace

std::vector<AlsaPerformanceTest::Result>
AlsaPerformanceTest::runBufferSweep (const Options& opts)
{
    std::vector<int> sizes = opts.bufferSizes;
    if (sizes.empty())
        for (int b : kDefaultBufferLadder) sizes.push_back (b);

    // Sort + dedupe so the "first SAFE/MARGINAL row in the results = minimum
    // stable buffer" invariant holds for runAll's recommended-min picker.
    // The default ladder above is already tidy; an opts.bufferSizes supplied
    // by a programmatic caller (or a future env-var path) might not be.
    std::sort (sizes.begin(), sizes.end());
    sizes.erase (std::unique (sizes.begin(), sizes.end()), sizes.end());

    std::vector<Result> results;
    for (int bs : sizes)
    {
        Result r;
        r.testName      = "buffer sweep";
        r.configuration = dusk::text::format ("buf=%d rate=%u", bs, opts.sampleRate);

        MeasuringCallback cb (opts.fakeDspLoadUs);
        std::string openErr;
        auto dev = openDuplex (opts.deviceId, opts.sampleRate, bs, &cb, openErr);
        if (dev == nullptr)
        {
            r.passed   = false;
            r.verdict  = "FAIL (open)";
            r.details  = openErr;
            results.push_back (r);
            continue;
        }

        // Capture negotiated format info from the live device before stop,
        // so the matrix report can highlight when the kernel snapped to a
        // different bit depth (e.g. capture S32_LE / playback S24_3LE on
        // class-compliant USB).
        r.negotiatedBitDepth  = dev->getCurrentBitDepth();
        r.activeOutChannels   = dev->getActiveOutputChannels().count();
        r.activeInChannels    = dev->getActiveInputChannels() .count();

        sleepMs (opts.durationMs);

        dev->stop();
        const int xruns = dev->getXRunCount();
        const auto stats = cb.computeStats();
        dev->close();
        AlsaAudioIODevice::destroyOrPark (std::move (dev));

        r.xruns           = xruns;
        r.budgetMs        = cb.getBudgetMs();
        r.meanCallbackMs  = stats.mean;
        r.p95CallbackMs   = stats.p95;
        r.p99CallbackMs   = stats.p99;
        r.maxCallbackMs   = stats.max;
        r.callbackCount   = stats.n;
        r.verdict         = formatVerdict (xruns, stats.p99, r.budgetMs);
        r.passed          = r.verdict == "SAFE" || r.verdict == "MARGINAL";
        results.push_back (r);
    }

    return results;
}

AlsaPerformanceTest::Result
AlsaPerformanceTest::runOpenCloseStress (const Options& opts)
{
    Result r;
    r.testName = "open/close cycle stress";
    r.configuration = dusk::text::format ("cycles=%d", opts.openCloseCycles);

    int failures = 0;
    std::string firstFailure;

    for (int i = 0; i < opts.openCloseCycles; ++i)
    {
        AlsaAudioIODevice dev (opts.deviceId, /*inId*/ opts.deviceId, /*outId*/ opts.deviceId);
        device::ChannelSet outMask, inMask;
        outMask.setRange (0, 2, true);
        inMask .setRange (0, 2, true);

        const auto err = dev.open (inMask, outMask, (double) opts.sampleRate, 1024);
        if (! err.empty())
        {
            ++failures;
            if (firstFailure.empty())
                firstFailure = dusk::text::format ("cycle %d: ", i) + err;
            continue;
        }
        // No start/stop - just immediate close. Tests the unstart-then-close
        // path which is hit by "user changed their mind" UI flows.
        dev.close();
    }

    r.passed  = failures == 0;
    r.verdict = failures == 0 ? std::string ("SAFE")
                              : dusk::text::format ("UNSAFE (%d/%d failed)",
                                                    failures, opts.openCloseCycles);
    r.details = firstFailure;
    return r;
}

AlsaPerformanceTest::Result
AlsaPerformanceTest::runStartStopRace (const Options& opts)
{
    Result r;
    r.testName = "start/stop race";
    r.configuration = dusk::text::format ("cycles=%d", opts.startStopCycles);

    auto dev = std::make_unique<AlsaAudioIODevice> (opts.deviceId, /*inId*/ opts.deviceId, /*outId*/ opts.deviceId);
    device::ChannelSet outMask, inMask;
    outMask.setRange (0, 2, true);
    inMask .setRange (0, 2, true);

    const auto err = dev->open (inMask, outMask, (double) opts.sampleRate, 1024);
    if (! err.empty())
    {
        r.passed  = false;
        r.verdict = "FAIL (open)";
        r.details = err;
        return r;
    }

    MeasuringCallback cb (0);
    int failures = 0;
    int firstFailureCycle = -1;

    for (int i = 0; i < opts.startStopCycles; ++i)
    {
        dev->start (&cb);
        // Brief moment so start() actually fires the device before stop kicks
        // in - 50ms = a few periods at 1024/48k. Without this, the test
        // measures only the queueing layer and misses real start_threshold
        // behavior.
        sleepMs (50);

        // dev->isOpen() reflects open()/close() only, not start()/stop(), so
        // it stays true after dev->stop() and would rubber-stamp every cycle.
        // Use the callback count instead - audioDeviceAboutToStart resets it
        // to 0 at each start, and after a 50ms sleep at 1024/48k a healthy
        // device will have produced at least two callbacks. Zero callbacks
        // means start() didn't actually get the audio thread running
        // (pre-fill failed, recovery cascade, or a deeper open-state
        // inconsistency).
        const int callbacksDuringRun = cb.totalCallbacks();

        dev->stop();

        if (callbacksDuringRun == 0)
        {
            ++failures;
            if (firstFailureCycle < 0)
                firstFailureCycle = i;
        }
    }

    dev->close();

    r.xruns   = dev->getXRunCount();
    r.passed  = failures == 0;
    r.verdict = failures == 0
                  ? std::string ("SAFE")
                  : dusk::text::format ("UNSAFE (%d/%d cycles produced no callbacks; first at #%d)",
                                        failures, opts.startStopCycles, firstFailureCycle);
    r.details = dusk::text::format ("xruns over all cycles: %d", r.xruns);
    AlsaAudioIODevice::destroyOrPark (std::move (dev));
    return r;
}

// Loopback callback: writes silence by default, sends a short 1 kHz burst on
// output channel 0 after a fixed warmup, scans input channel 0 for the first
// sample crossing a threshold AFTER the burst was sent. Round-trip latency
// is the difference between the burst-start sample frame and the first
// detected input sample frame.
//
// Detection threshold of 0.1 (~ -20 dBFS) is well above any reasonable USB
// interface noise floor (~ -90 dBFS for unconnected inputs). Burst amplitude
// of 0.7 (~ -3 dBFS) is loud enough to survive a low-gain input stage and
// short enough (one period) that it doesn't dominate the test runtime.
namespace
{
class LoopbackCallback final : public device::IODeviceCallback
{
public:
    void audioDeviceAboutToStart (device::IODevice* dev) override
    {
        sampleRate = dev->getCurrentSampleRate();
        callbackCount.store (0, std::memory_order_relaxed);
        burstStartSample.store (-1, std::memory_order_relaxed);
        firstSignalSample.store (-1, std::memory_order_relaxed);
    }

    void audioDeviceStopped() override {}

    void audioDeviceIOCallback (const float* const* inputChannelData,
                                int numInputChannels,
                                float* const* outputChannelData,
                                int numOutputChannels,
                                int numSamples,
                                const device::CallbackContext&) override
    {
        const int cb = callbackCount.fetch_add (1, std::memory_order_relaxed);
        const int currentSampleStart = cb * numSamples;

        // Output: silence everywhere except the burst callback.
        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (outputChannelData[ch] != nullptr)
                std::memset (outputChannelData[ch], 0, sizeof (float) * (size_t) numSamples);

        if (cb == kBurstCallback && numOutputChannels > 0 && outputChannelData[0] != nullptr
            && sampleRate > 0.0)
        {
            constexpr double kTwoPi = 6.283185307179586;
            for (int i = 0; i < numSamples; ++i)
            {
                const double t = (double) (currentSampleStart + i) / sampleRate;
                const float  v = (float) (kBurstAmplitude * std::sin (kTwoPi * 1000.0 * t));
                outputChannelData[0][i] = v;
            }
            burstStartSample.store (currentSampleStart, std::memory_order_release);
        }

        // Input: only scan once a burst has been queued. acquire pairs with
        // the release in the burst-send branch so we never see a partially-
        // written burstStartSample.
        if (firstSignalSample.load (std::memory_order_relaxed) < 0
             && burstStartSample.load (std::memory_order_acquire) >= 0
             && numInputChannels > 0
             && inputChannelData[0] != nullptr)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                if (std::abs (inputChannelData[0][i]) > kSignalThreshold)
                {
                    firstSignalSample.store (currentSampleStart + i, std::memory_order_relaxed);
                    break;
                }
            }
        }
    }

    int  getBurstStartSample()  const { return burstStartSample .load (std::memory_order_acquire); }
    int  getFirstSignalSample() const { return firstSignalSample.load (std::memory_order_acquire); }
    int  getTotalCallbacks()    const { return callbackCount    .load (std::memory_order_acquire); }

private:
    static constexpr int    kBurstCallback   = 100;   // ~2s warmup at 1024/48k
    static constexpr float  kSignalThreshold = 0.1f;  // ~ -20 dBFS
    static constexpr double kBurstAmplitude  = 0.7;   // ~ -3 dBFS

    double sampleRate = 0.0;
    std::atomic<int> callbackCount     { 0 };
    std::atomic<int> burstStartSample  { -1 };
    std::atomic<int> firstSignalSample { -1 };
};
} // namespace

AlsaPerformanceTest::LoopbackResult
AlsaPerformanceTest::runLoopbackProbe (const Options& opts)
{
    LoopbackResult r;

    LoopbackCallback cb;
    std::string openErr;
    // Use a moderate buffer (1024) so the warmup callback budget gives the
    // device time to start streaming cleanly before we send the burst.
    auto dev = openDuplex (opts.deviceId, opts.sampleRate, 1024, &cb, openErr);
    if (dev == nullptr)
    {
        r.details = "open failed: " + openErr;
        return r;
    }

    // Run long enough that we're well past the burst callback (100) plus
    // round-trip latency plus a safety margin. 4 seconds at 1024/48k =
    // ~187 callbacks; sends burst at #100, leaves ~87 callbacks to detect.
    sleepMs (4000);

    dev->stop();
    dev->close();
    AlsaAudioIODevice::destroyOrPark (std::move (dev));

    r.burstStartSample  = cb.getBurstStartSample();
    r.firstSignalSample = cb.getFirstSignalSample();

    if (r.burstStartSample < 0)
    {
        r.details = dusk::text::format (
            "burst was never sent (got only %d callbacks, needed >= 101)",
            cb.getTotalCallbacks());
        return r;
    }

    if (r.firstSignalSample < 0)
    {
        r.details = "burst sent, but no signal detected on input - check that "
                    "a loopback path exists (physical cable from output 1 to "
                    "input 1, or snd-aloop loaded with deviceId pointing at it)";
        return r;
    }

    r.signalDetected = true;
    r.latencySamples = r.firstSignalSample - r.burstStartSample;
    r.latencyMs      = 1000.0 * (double) r.latencySamples / (double) opts.sampleRate;
    r.details        = "1 kHz burst at -3 dBFS, threshold 0.1 (-20 dBFS)";
    return r;
}

std::string AlsaPerformanceTest::runAll (const Options& opts)
{
    std::vector<std::string> out;

    const bool willRunMatrix = opts.sampleRates.size() > 1;
    out.push_back (willRunMatrix
                ? std::string ("=== ALSA Backend Performance Test (Tier 2: rate matrix) ===")
                : std::string ("=== ALSA Backend Performance Test (Tier 1) ==="));
    {
        const std::time_t now = std::time (nullptr);
        char timeBuf[64] = { 0 };
        std::strftime (timeBuf, sizeof (timeBuf), "%d %b %Y %H:%M:%S", std::localtime (&now));
        out.push_back (dusk::text::format ("Time:     %s", timeBuf));
    }
    out.push_back (dusk::text::format ("Device:   %s", opts.deviceId.c_str()));
    if (willRunMatrix)
    {
        std::vector<std::string> rateLabels;
        for (auto r : opts.sampleRates) rateLabels.push_back (std::to_string (r));
        out.push_back (dusk::text::format ("Rates:    %s Hz",
                                           dusk::text::joinIntoString (rateLabels, ", ").c_str()));
    }
    else
    {
        out.push_back (dusk::text::format ("Rate:     %u Hz", opts.sampleRate));
    }
    out.push_back (dusk::text::format ("Duration: %d ms per buffer", opts.durationMs));
    out.push_back (dusk::text::format ("DSP load: %d us / callback (synthetic)", opts.fakeDspLoadUs));
    out.push_back ("");

    // Build the rate list. Empty Options::sampleRates -> single rate from
    // opts.sampleRate (Tier 1 behaviour). Non-empty -> Tier 2 matrix mode,
    // a buffer sweep at each rate with its own table.
    std::vector<unsigned int> rates = opts.sampleRates;
    if (rates.empty())
        rates.push_back (opts.sampleRate);

    const bool matrixMode = rates.size() > 1;
    out.push_back (matrixMode ? "--- Rate x Buffer matrix (duplex, silent) ---"
                              : "--- Buffer-size sweep (duplex, silent) ---");
    out.push_back ("");

    int globalRecommendedMin = -1;

    for (unsigned int rate : rates)
    {
        Options perRate = opts;
        perRate.sampleRate = rate;
        const auto sweep = runBufferSweep (perRate);

        // Per-rate header: pick negotiated format info from the first
        // successful cell. If every cell failed to open at this rate,
        // surface that explicitly.
        std::string formatLabel;
        for (const auto& r : sweep)
        {
            if (r.negotiatedBitDepth > 0)
            {
                formatLabel = dusk::text::format ("%d-bit int / %d out, %d in",
                                                  r.negotiatedBitDepth,
                                                  r.activeOutChannels,
                                                  r.activeInChannels);
                break;
            }
        }
        if (formatLabel.empty())
            formatLabel = "(no successful open at this rate)";

        out.push_back (dusk::text::format ("Rate: %u Hz   Format: %s",
                                           rate, formatLabel.c_str()));
        out.push_back (" Buffer | Budget    | Mean      | P95       | P99       | Max       | XRuns/run | Verdict");
        out.push_back ("--------|-----------|-----------|-----------|-----------|-----------|-----------|---------");

        int rateRecommendedMin = -1;
        for (const auto& r : sweep)
        {
            out.push_back (formatTableRow (r));
            if (rateRecommendedMin < 0 && (r.verdict == "SAFE" || r.verdict == "MARGINAL"))
            {
                const auto bufStr = dusk::text::upToFirstOccurrenceOf (
                    dusk::text::fromFirstOccurrenceOf (r.configuration, "buf=", false), " ", false);
                rateRecommendedMin = dusk::text::getIntValue (bufStr);
            }
            if (! r.details.empty())
                out.push_back ("        " + r.details);
        }

        if (rateRecommendedMin > 0)
            out.push_back (dusk::text::format (
                "  Recommended min buffer at %u Hz: %d frames (50%% headroom: %d)",
                rate, rateRecommendedMin, (rateRecommendedMin * 3) / 2));
        else
            out.push_back ("  No buffer size stable at this rate.");

        // The "global" recommended min is the highest minimum across all
        // rates - the safest default that works everywhere.
        if (rateRecommendedMin > globalRecommendedMin)
            globalRecommendedMin = rateRecommendedMin;

        out.push_back ("");
    }

    if (matrixMode)
    {
        if (globalRecommendedMin > 0)
            out.push_back (dusk::text::format (
                "Global recommended min buffer (max across rates): %d frames", globalRecommendedMin));
        else
            out.push_back ("No buffer size was stable at any rate. Check open errors above.");
        out.push_back ("");
    }

    out.push_back ("--- Open/close cycle stress ---");
    {
        const auto r = runOpenCloseStress (opts);
        out.push_back (dusk::text::format ("  %d cycles  %s", opts.openCloseCycles, r.verdict.c_str()));
        if (! r.details.empty()) out.push_back ("        " + r.details);
    }
    out.push_back ("");

    out.push_back ("--- Start/stop race ---");
    {
        const auto r = runStartStopRace (opts);
        out.push_back (dusk::text::format ("  %d cycles  %s", opts.startStopCycles, r.verdict.c_str()));
        if (! r.details.empty()) out.push_back ("        " + r.details);
    }
    out.push_back ("");

    if (opts.runLoopback)
    {
        out.push_back ("--- Loopback round-trip probe ---");
        const auto r = runLoopbackProbe (opts);
        if (r.signalDetected)
        {
            out.push_back (dusk::text::format (
                "  Signal detected: latency %d samples (%.2f ms) at %u Hz",
                r.latencySamples, r.latencyMs, opts.sampleRate));
            out.push_back (dusk::text::format (
                "  Burst sent at sample %d, detected at sample %d",
                r.burstStartSample, r.firstSignalSample));
        }
        else
        {
            out.push_back ("  No round-trip signal detected");
        }
        if (! r.details.empty())
            out.push_back ("  " + r.details);
        out.push_back ("");
    }

    out.push_back ("=== End of Performance Test ===");
    return dusk::text::joinIntoString (out, "\n");
}

} // namespace duskstudio
