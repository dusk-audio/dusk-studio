// Linux-only IPC round-trip regression test. Verifies that:
//   • RemotePluginConnection can fork+exec the dusk-studio-plugin-host child in
//     --ipc-stub mode (the dependency-light Phase 1 echo loop).
//   • processBlockSync round-trips audio buffers through SHM + futex pair
//     without timing out.
//   • The stub's echo is byte-exact (output equals input) — catches any
//     SHM offset / channel-stride drift between parent and child.
//
// Compiled only on Linux. The IPC primitives use memfd_create + futex
// which don't exist on macOS / Windows. CMakeLists conditionally adds
// this source + RemotePluginConnection.cpp + a dependency on the
// dusk-studio-plugin-host binary so $<TARGET_FILE:dusk-studio-plugin-host> resolves
// at build time.

#if defined(__linux__)

#include <catch2/catch_test_macros.hpp>
#include "engine/ipc/RemotePluginConnection.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>
#include <string>
#include <vector>

namespace
{
constexpr int  kBlockSize  = 256;
constexpr int  kNumChans   = 2;
constexpr int  kIterations = 32;
constexpr long long kTimeoutNs = 100'000'000LL;  // 100 ms
} // namespace

TEST_CASE ("ipc-stub: connect, round-trip 32 blocks, byte-exact echo",
            "[ipc][linux]")
{
    duskstudio::ipc::RemotePluginConnection conn;

    std::string err;
    REQUIRE (conn.connect (DUSKSTUDIO_PLUGIN_HOST_PATH, "--ipc-stub", err));
    REQUIRE (err.empty());

    std::vector<float> bufL ((std::size_t) kBlockSize);
    std::vector<float> bufR ((std::size_t) kBlockSize);
    const float* in[kNumChans] { bufL.data(), bufR.data() };
    juce::MidiBuffer midi;  // empty in/out for stub mode

    for (int it = 0; it < kIterations; ++it)
    {
        // Vary content per iteration so a buggy stub returning a stale
        // SHM region would fail the byte-exact check below on iter 1+.
        for (int i = 0; i < kBlockSize; ++i)
        {
            bufL[(std::size_t) i] = 0.5f * std::sin (((float) (i + it)) * 0.1f);
            bufR[(std::size_t) i] = 0.5f * std::cos (((float) (i + it)) * 0.1f);
        }

        REQUIRE (conn.processBlockSync (in, kNumChans, kBlockSize, midi, kTimeoutNs));

        for (int c = 0; c < kNumChans; ++c)
        {
            const float* out = conn.readOutChannel (c);
            const float* expected = (c == 0) ? bufL.data() : bufR.data();
            for (int i = 0; i < kBlockSize; ++i)
            {
                // Stub mode is a memcpy echo — bit-exact equality is the
                // right check. Floating-point tolerance is wrong here:
                // any drift would mask a real SHM corruption bug.
                REQUIRE (out[i] == expected[i]);
            }
        }
    }

    REQUIRE (conn.getRoundTripCount() == (std::uint64_t) kIterations);
    REQUIRE_FALSE (conn.isCrashed());
}

TEST_CASE ("ipc-stub: rejects oversize block", "[ipc][linux]")
{
    duskstudio::ipc::RemotePluginConnection conn;

    std::string err;
    REQUIRE (conn.connect (DUSKSTUDIO_PLUGIN_HOST_PATH, "--ipc-stub", err));

    // PluginIpc.h hardcodes kMaxBlock = 1024. processBlockSync must
    // return false rather than overrun the SHM audio region.
    std::vector<float> oversize (4096, 0.0f);
    const float* in[1] { oversize.data() };
    juce::MidiBuffer midi;
    REQUIRE_FALSE (conn.processBlockSync (in, 1, 4096, midi, 1'000'000LL));
    REQUIRE_FALSE (conn.isCrashed());  // bad-input rejection isn't a crash
}

#endif // __linux__
