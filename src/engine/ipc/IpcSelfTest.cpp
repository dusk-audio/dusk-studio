#include "IpcSelfTest.h"
#include "RemotePluginConnection.h"
#include "../JuceCompat.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

namespace duskstudio::ipc
{
int runIpcSelfTest (const std::string& hostExecutablePath,
                     int iterations, int numSamples)
{
    using namespace std::chrono;

    std::fprintf (stdout, "=== Dusk Studio IPC Self-Test ===\n");
    std::fprintf (stdout, "host:    %s\n", hostExecutablePath.c_str());
    std::fprintf (stdout, "iters:   %d\n", iterations);
    std::fprintf (stdout, "samples: %d (stereo)\n", numSamples);

    RemotePluginConnection conn;
    std::string err;
    if (! conn.connect (hostExecutablePath, "--ipc-stub", err))
    {
        std::fprintf (stderr, "FAIL: connect: %s\n", err.c_str());
        return 1;
    }
    std::fprintf (stdout, "child connected. running round-trips...\n");

    // Synthesize a deterministic input pattern. A small ramp + sine so
    // we can spot copy errors visually if they happen.
    std::vector<float> bufL ((std::size_t) numSamples);
    std::vector<float> bufR ((std::size_t) numSamples);
    for (int i = 0; i < numSamples; ++i)
    {
        bufL[(std::size_t) i] = std::sin ((float) i * 0.1f) * 0.5f;
        bufR[(std::size_t) i] = std::cos ((float) i * 0.1f) * 0.5f;
    }
    const float* in[2] { bufL.data(), bufR.data() };

    std::vector<long long> latenciesNs;
    latenciesNs.reserve ((std::size_t) iterations);

    int verifyFails = 0;
    constexpr long long kTimeoutNs = 100'000'000LL;  // 100 ms - generous
    juce::MidiBuffer emptyMidi;

    for (int it = 0; it < iterations; ++it)
    {
        // Vary content so a buggy stub that returns a stale buffer
        // would fail the verify on subsequent iterations.
        for (int i = 0; i < numSamples; ++i)
        {
            bufL[(std::size_t) i] = 0.5f * std::sin (((float) (i + it)) * 0.1f);
            bufR[(std::size_t) i] = 0.5f * std::cos (((float) (i + it)) * 0.1f);
        }

        const auto t0 = steady_clock::now();
        if (! conn.processBlockSync (in, 2, numSamples, emptyMidi, kTimeoutNs))
        {
            std::fprintf (stderr, "FAIL: processBlockSync at iter %d\n", it);
            return 2;
        }
        const auto t1 = steady_clock::now();
        latenciesNs.push_back (duration_cast<nanoseconds> (t1 - t0).count());

        // Verify echo: output channel c should equal input channel c.
        for (int c = 0; c < 2; ++c)
        {
            const float* o = conn.readOutChannel (c);
            const float* expected = (c == 0) ? bufL.data() : bufR.data();
            for (int i = 0; i < numSamples; ++i)
            {
                if (o[i] != expected[i])
                {
                    if (++verifyFails < 5)
                        std::fprintf (stderr,
                                      "FAIL verify iter=%d ch=%d sample=%d: "
                                      "got %.6f want %.6f\n",
                                      it, c, i, o[i], expected[i]);
                    break;
                }
            }
        }
    }

    if (verifyFails > 0)
    {
        std::fprintf (stderr, "FAIL: %d verification mismatches\n", verifyFails);
        return 3;
    }

    if (latenciesNs.empty())
    {
        std::fprintf (stderr, "FAIL: no iterations were executed\n");
        return 4;
    }

    // Stats. Sort once, pull p50 / p99 / max. Nearest-rank percentile so
    // small N doesn't collapse p99 onto p50.
    std::sort (latenciesNs.begin(), latenciesNs.end());
    const auto pct = [&] (double p)
    {
        const long long n = (long long) latenciesNs.size();
        long long idx = (long long) std::ceil ((double) n * p) - 1;
        if (idx < 0) idx = 0;
        if (idx > n - 1) idx = n - 1;
        return latenciesNs[(std::size_t) idx];
    };

    long long sum = 0;
    for (auto v : latenciesNs) sum += v;
    const auto mean = sum / (long long) latenciesNs.size();

    std::fprintf (stdout, "round-trips: %llu\n",
                  (unsigned long long) conn.getRoundTripCount());
    std::fprintf (stdout, "latency ns: mean=%lld  p50=%lld  p99=%lld  max=%lld\n",
                  mean, pct (0.50), pct (0.99), latenciesNs.back());
    std::fprintf (stdout, "PASS\n");
    return 0;
}

int runIpcHostTest (const std::string& hostExecutablePath,
                     const std::string& pluginPath,
                     int iterations, int numSamples)
{
    using namespace std::chrono;

    std::fprintf (stdout, "=== Dusk Studio IPC Host Test (Phase 2) ===\n");
    std::fprintf (stdout, "host:   %s\n", hostExecutablePath.c_str());
    std::fprintf (stdout, "plugin: %s\n", pluginPath.c_str());
    std::fprintf (stdout, "iters:  %d  samples: %d (stereo)\n", iterations, numSamples);

    // 1) Scan the plugin so we can build its PluginDescription. The
    // format manager is parent-side here only to produce the XML; the
    // actual instance lives in the child.
    juce::AudioPluginFormatManager fm;
    duskstudio::juce_compat::addDefaultFormats (fm);
    juce::OwnedArray<juce::PluginDescription> found;
    bool scanned = false;
    for (auto* fmt : fm.getFormats())
    {
        if (fmt == nullptr) continue;
        if (fmt->fileMightContainThisPluginType (pluginPath))
        {
            fmt->findAllTypesForFile (found, pluginPath);
            if (! found.isEmpty()) { scanned = true; break; }
        }
    }
    if (! scanned || found.isEmpty())
    {
        std::fprintf (stderr, "FAIL: no descriptions for %s\n", pluginPath.c_str());
        return 10;
    }
    auto* descPtr = found[0];
    auto descXml  = descPtr->createXml();
    if (descXml == nullptr)
    {
        std::fprintf (stderr, "FAIL: PluginDescription::createXml returned null\n");
        return 11;
    }
    const auto descXmlStr = descXml->toString (juce::XmlElement::TextFormat().singleLine());
    std::fprintf (stdout, "scanned: %s by %s\n",
                  descPtr->name.toRawUTF8(),
                  descPtr->manufacturerName.toRawUTF8());

    // 2) Connect to the child in host mode.
    RemotePluginConnection conn;
    std::string err;
    if (! conn.connect (hostExecutablePath, "--ipc-host", err))
    {
        std::fprintf (stderr, "FAIL: connect: %s\n", err.c_str());
        return 12;
    }

    // 3) Load the plugin in the child.
    constexpr double kSampleRate = 48000.0;
    int numIn = 0, numOut = 0, latency = 0;
    if (! conn.loadPlugin (descXmlStr.toStdString(),
                            kSampleRate, numSamples,
                            numIn, numOut, latency, err))
    {
        std::fprintf (stderr, "FAIL: loadPlugin: %s\n", err.c_str());
        return 13;
    }
    std::fprintf (stdout, "loaded: numIn=%d numOut=%d latency=%d\n",
                  numIn, numOut, latency);

    // Some plugins are output-only (instruments, numIn=0). For those we
    // can still run processBlock - the child fills the input buffer
    // with our data which the plugin ignores - but the "output != input"
    // assertion is meaningless. Skip the change-detection check for
    // instrument plugins.
    const bool isInstrument = (numIn == 0);

    // 4) Run process-block round-trips. Generate deterministic non-zero
    // input so an effect plugin must alter it (an EQ at non-flat settings
    // / a comp / a reverb all will).
    std::vector<float> bufL ((std::size_t) numSamples), bufR ((std::size_t) numSamples);
    const float* in[2] { bufL.data(), bufR.data() };
    std::vector<long long> latenciesNs;
    latenciesNs.reserve ((std::size_t) iterations);

    bool anyDifference = false;
    constexpr long long kTimeoutNs = 1'000'000'000LL;  // 1 s - plugins take time to warm up
    juce::MidiBuffer emptyMidi;

    for (int it = 0; it < iterations; ++it)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            // White-ish content (low freq sine + small noise) so an EQ /
            // comp / saturator has something to chew on.
            const float t = (float) (i + it * numSamples) / (float) kSampleRate;
            bufL[(std::size_t) i] = 0.4f * std::sin (2.0f * 3.14159f * 220.0f * t);
            bufR[(std::size_t) i] = 0.4f * std::sin (2.0f * 3.14159f * 277.0f * t);
        }

        const auto t0 = steady_clock::now();
        if (! conn.processBlockSync (in, juce::jmax (numIn, 1), numSamples, emptyMidi, kTimeoutNs))
        {
            std::fprintf (stderr, "FAIL: processBlockSync at iter %d\n", it);
            return 14;
        }
        const auto t1 = steady_clock::now();
        latenciesNs.push_back (duration_cast<nanoseconds> (t1 - t0).count());

        // Skip the first ~10 blocks for warm-up (plugins with
        // oversamplers / look-ahead need a few callbacks to flush the
        // initial silence).
        if (! isInstrument && it >= 10 && ! anyDifference)
        {
            for (int c = 0; c < juce::jmin (2, numOut); ++c)
            {
                const float* o = conn.readOutChannel (c);
                const float* expected = (c == 0) ? bufL.data() : bufR.data();
                for (int i = 0; i < numSamples; ++i)
                {
                    if (std::abs (o[i] - expected[i]) > 1.0e-5f)
                    {
                        anyDifference = true; break;
                    }
                }
                if (anyDifference) break;
            }
        }
    }

    if (latenciesNs.empty())
    {
        std::fprintf (stderr, "FAIL: no iterations were executed\n");
        return 16;
    }

    std::sort (latenciesNs.begin(), latenciesNs.end());
    const auto pct = [&] (double p)
    {
        const long long n = (long long) latenciesNs.size();
        long long idx = (long long) std::ceil ((double) n * p) - 1;
        if (idx < 0) idx = 0;
        if (idx > n - 1) idx = n - 1;
        return latenciesNs[(std::size_t) idx];
    };
    long long sum = 0;
    for (auto v : latenciesNs) sum += v;
    const auto mean = sum / (long long) latenciesNs.size();
    std::fprintf (stdout, "latency ns: mean=%lld  p50=%lld  p99=%lld  max=%lld\n",
                  mean, pct (0.50), pct (0.99), latenciesNs.back());

    if (! isInstrument && ! anyDifference)
    {
        std::fprintf (stderr,
                      "FAIL: plugin did not modify the signal across %d blocks. "
                      "(Either the plugin is bypassed or processBlock isn't running.)\n",
                      iterations);
        return 15;
    }

    if (! conn.release (err))
    {
        std::fprintf (stderr, "WARN: release failed: %s\n", err.c_str());
    }

    std::fprintf (stdout, "PASS\n");
    return 0;
}
} // namespace duskstudio::ipc
