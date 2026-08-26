#include <catch2/catch_test_macros.hpp>

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

#if DUSKSTUDIO_HAS_OOP_PLUGINS || DUSKSTUDIO_HAS_NATIVE_CLAP || DUSKSTUDIO_HAS_NATIVE_VST3
namespace duskstudio
{
// Test-only linker seam implemented beside the production supervisor. The
// fields are completed, cancelled, and timedOut, in that order.
std::array<bool, 3> driveSandboxedScanForTest (
    juce::ChildProcess&, const std::atomic<bool>* abort, std::uint32_t timeoutMs);
}

namespace
{
constexpr const char* kSilentChildTest = "plugin scan watchdog silent child fixture";

void startSilentChild (juce::ChildProcess& child)
{
    const auto executable = juce::File::getSpecialLocation (
        juce::File::currentExecutableFile).getFullPathName();
    const juce::StringArray arguments { executable, kSilentChildTest };
    REQUIRE (child.start (arguments, juce::ChildProcess::wantStdOut));
}

class FailFastWatchdog
{
public:
    explicit FailFastWatchdog (int timeoutMs)
        : timeout (timeoutMs), thread ([this]
          {
              const auto deadline = std::chrono::steady_clock::now()
                                  + std::chrono::milliseconds (timeout);
              while (! done.load (std::memory_order_acquire)
                     && std::chrono::steady_clock::now() < deadline)
                  std::this_thread::sleep_for (std::chrono::milliseconds (10));

              if (! done.load (std::memory_order_acquire))
              {
                  std::fprintf (stderr,
                                "Plugin scan watchdog did not release a blocked read within %d ms"
                                " - issue #347 regressed.\n",
                                timeout);
                  std::fflush (stderr);
                  std::abort();
              }
          })
    {
    }

    ~FailFastWatchdog()
    {
        done.store (true, std::memory_order_release);
        thread.join();
    }

private:
    int timeout;
    std::atomic<bool> done { false };
    std::thread thread;
};
} // namespace

// Spawned by the two parent tests below. It never emits scan-protocol payload
// sentinels or exits before the parent deadline, matching the plugin/license-
// dialog failure behind issue #347. The fallback deadline keeps an aborted
// parent from leaving an immortal process holding CI's output pipe open.
// The hidden tag keeps CTest from scheduling the fixture as a standalone test.
TEST_CASE (kSilentChildTest, "[.][scan-watchdog-fixture]")
{
    const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds (10);
    while (std::chrono::steady_clock::now() < giveUp)
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
}

TEST_CASE ("Plugin scan watchdog times out a silent child",
           "[plugin-scan][regression][issue-347]")
{
    juce::ChildProcess child;
    startSilentChild (child);
    FailFastWatchdog failFast (5000);

    const auto [completed, cancelled, timedOut] = duskstudio::driveSandboxedScanForTest (
        child, nullptr, 100);

    REQUIRE_FALSE (completed);
    REQUIRE_FALSE (cancelled);
    REQUIRE (timedOut);
}

TEST_CASE ("Plugin scan watchdog cancels a silent child",
           "[plugin-scan][regression][issue-347]")
{
    juce::ChildProcess child;
    startSilentChild (child);
    std::atomic<bool> abortRequested { false };
    std::thread cancel ([&abortRequested]
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
        abortRequested.store (true, std::memory_order_relaxed);
    });
    FailFastWatchdog failFast (5000);

    const auto [completed, cancelled, timedOut] = duskstudio::driveSandboxedScanForTest (
        child, &abortRequested, 2000);
    cancel.join();

    REQUIRE_FALSE (completed);
    REQUIRE (cancelled);
    REQUIRE_FALSE (timedOut);
}
#endif
