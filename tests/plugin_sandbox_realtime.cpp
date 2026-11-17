#include <catch2/catch_test_macros.hpp>

#include "TestTempDirectory.h"
#include "engine/PluginManager.h"
#include "engine/RealtimeScheduling.h"
#include "engine/RtPriority.h"
#include "engine/ScanProcess.h"
#include "engine/ipc/PluginScanProtocol.h"
#include "engine/ipc/RemotePluginConnection.h"
#include "fixtures/RenderProbeVst3.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined (__linux__)
 #include "TestProcessEnvironment.h"
 #include "engine/RealtimeKit.h"

 #include <csignal>
 #include <cstdint>
 #include <sched.h>
 #include <sys/resource.h>
#elif defined (__APPLE__)
 #include "TestProcessEnvironment.h"

 #include <pthread.h>
#elif defined (_WIN32)
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

#if DUSKSTUDIO_HAS_OOP_PLUGINS && defined (DUSKSTUDIO_RENDER_PROBE_VST3_FIXTURE_PATH)
namespace
{
namespace Probe = duskstudio::test::renderprobevst3;
// The probe's report, which holds only small whole numbers.
using Report = std::array<long, Probe::kReportLength>;

constexpr int kBlockSize = 64;
constexpr long long kBlockTimeoutNs = 500'000'000LL;

std::string readUntilClosed (duskstudio::ScanProcess& child)
{
    std::string output;
    std::vector<char> chunk (4096);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto read = child.readOutput (chunk.data(), chunk.size(), 50);
        if (read.status == duskstudio::ScanProcess::ReadStatus::Data) output.append (chunk.data(), read.bytes);
        else if (read.status != duskstudio::ScanProcess::ReadStatus::Waiting) break;
    }
    return output;
}

std::optional<std::string> probeDescriptionXml()
{
    duskstudio::ScanProcess child;
    if (! child.start ({ DUSKSTUDIO_PLUGIN_HOST_PATH, "--scan", "VST3", DUSKSTUDIO_RENDER_PROBE_VST3_FIXTURE_PATH }))
        return std::nullopt;
    const auto rows = duskstudio::scanproto::parsePayload (
        duskstudio::scanproto::extractPayload (readUntilClosed (child)));
    if (! rows.has_value() || rows->size() != 1) return std::nullopt;
    return duskstudio::PluginManager::descriptorToJuceForTest (rows->front()).createXml()->toString().toStdString();
}

void loadProbe (duskstudio::ipc::RemotePluginConnection& connection, const std::string& xml)
{
    std::string error;
    int numIn = 0, numOut = 0, latency = 0;
    bool instrument = false;
    const bool loaded = connection.loadPlugin (xml, 48000.0, kBlockSize, numIn, numOut, latency, instrument, error);
    INFO (error);
    REQUIRE (loaded);
    REQUIRE (numIn == 2);
    REQUIRE (numOut == 2);
}

Report render (duskstudio::ipc::RemotePluginConnection& connection)
{
    std::array<float, kBlockSize> left {}, right {};
    const float* inputs[] { left.data(), right.data() };
    dusk::MidiBuffer midi;
    REQUIRE (connection.processBlockSync (inputs, 2, 2, kBlockSize, midi, kBlockTimeoutNs));
    Report report {};
    const float* written = connection.readOutChannel (0);
    std::transform (written, written + report.size(), report.begin(),
                    [] (float value) { return std::lround (value); });
    return report;
}

// The marker the child's one scheduling line starts with.
constexpr const char* kSchedulingLine = "[dusk-studio-plugin-host] audio thread ";

std::size_t occurrences (const std::string& text, const std::string& needle)
{
    std::size_t found = 0;
    for (auto at = text.find (needle); at != std::string::npos; at = text.find (needle, at + 1))
        ++found;
    return found;
}

#if defined (_WIN32)
// What the children started during `fn` write to their standard error. A child
// inherits this process's standard error handle only when it is inheritable.
template <typename Fn>
std::string captureChildStderr (const std::filesystem::path& directory, Fn&& fn)
{
    const auto sinkPath = directory / "stderr.txt";
    SECURITY_ATTRIBUTES inheritable { sizeof (inheritable), nullptr, TRUE };
    const HANDLE sink = ::CreateFileW (sinkPath.c_str(), GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       &inheritable, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE (sink != INVALID_HANDLE_VALUE);
    struct Restore
    {
        HANDLE saved;
        HANDLE sink;
        ~Restore()
        {
            ::SetStdHandle (STD_ERROR_HANDLE, saved);
            ::CloseHandle (sink);
        }
    };
    {
        const Restore restore { ::GetStdHandle (STD_ERROR_HANDLE), sink };
        REQUIRE (::SetStdHandle (STD_ERROR_HANDLE, sink));
        fn();
    }
    std::ifstream input (sinkPath, std::ios::binary);
    return { std::istreambuf_iterator<char> (input), std::istreambuf_iterator<char>() };
}
#else
template <typename Fn>
std::string captureChildStderr (const std::filesystem::path& directory, Fn&& fn)
{
    return duskstudio::test::captureStderr (directory, std::forward<Fn> (fn));
}
#endif

#if defined (__linux__)
class ScopedRealtimePriorityCeiling
{
public:
    explicit ScopedRealtimePriorityCeiling (rlim_t soft)
    {
        if (::getrlimit (RLIMIT_RTPRIO, &saved) != 0) return;
        rlimit lowered = saved;
        lowered.rlim_cur = std::min (soft, saved.rlim_max);
        applied = ::setrlimit (RLIMIT_RTPRIO, &lowered) == 0;
    }
    ~ScopedRealtimePriorityCeiling() { if (applied) ::setrlimit (RLIMIT_RTPRIO, &saved); }
    ScopedRealtimePriorityCeiling (const ScopedRealtimePriorityCeiling&) = delete;
    ScopedRealtimePriorityCeiling& operator= (const ScopedRealtimePriorityCeiling&) = delete;

    bool applied = false;

private:
    rlimit saved {};
};

bool catchesSignal (int pid, int signal)
{
    std::ifstream status ("/proc/" + std::to_string (pid) + "/status");
    std::string line;
    while (std::getline (status, line))
        if (line.rfind ("SigCgt:", 0) == 0)
            return ((std::stoull (line.substr (7), nullptr, 16) >> (signal - 1)) & 1u) != 0;
    return false;
}

rlimit realtimeCpuLimitOf (int pid)
{
    rlimit limit {};
    REQUIRE (::prlimit ((pid_t) pid, RLIMIT_RTTIME, nullptr, &limit) == 0);
    return limit;
}
#endif

// The calling thread at the scheduling the engine gives the thread that waits
// on a sandboxed block, for as long as the object lives.
class ScopedEngineAudioScheduling
{
public:
    explicit ScopedEngineAudioScheduling (double periodSeconds)
    {
       #if defined (_WIN32)
        (void) periodSeconds;
        joined = duskstudio::rt::joinProAudioTask();
       #elif defined (__APPLE__)
        (void) duskstudio::rt::takeTimeConstraint (pthread_self(), periodSeconds);
       #else
        (void) periodSeconds;
        const int priority = duskstudio::rt::queryRealtimePriority().jucePriority;
        if (priority >= 0) (void) duskstudio::rt::applyRealtimeSchedRR (priority);
       #endif
    }
   #if defined (_WIN32)
    ~ScopedEngineAudioScheduling() { duskstudio::rt::leaveProAudioTask (joined); }

private:
    duskstudio::rt::ProAudioTask joined;
   #endif
};

// Nearest rank, in microseconds.
double percentileMicros (const std::vector<long long>& sortedNs, double fraction)
{
    const auto count = (long long) sortedNs.size();
    const auto index = std::clamp ((long long) std::ceil ((double) count * fraction) - 1, 0LL, count - 1);
    return (double) sortedNs[(std::size_t) index] / 1000.0;
}
} // namespace

TEST_CASE ("A sandboxed plug-in renders with denormals flushed", "[ipc][plugin]")
{
    const auto xml = probeDescriptionXml();
    REQUIRE (xml.has_value());
    duskstudio::ipc::RemotePluginConnection connection;
    std::string error;
    REQUIRE (connection.connect (DUSKSTUDIO_PLUGIN_HOST_PATH, "--ipc-host", error));
    loadProbe (connection, *xml);

    const auto report = render (connection);
    CHECK (report[Probe::kFlushesResults] == 1);
    CHECK (report[Probe::kFlushesInputs] == 1);
}

#if defined (__linux__)
// Where the test cannot take SCHED_RR itself, neither can the child it starts:
// it inherits the same RLIMIT_RTPRIO. The case then has nothing to show.
TEST_CASE ("A sandboxed plug-in renders on a realtime thread at the priority of the DSP threads",
           "[ipc][plugin][realtime-fixture]")
{
    const int jucePriority = duskstudio::rt::queryRealtimePriority().jucePriority;
    bool attainable = false;
    if (jucePriority >= 0)
        std::thread ([&] { attainable = duskstudio::rt::applyRealtimeSchedRR (jucePriority); }).join();
    if (! attainable)
        SKIP ("RLIMIT_RTPRIO grants this host no SCHED_RR, so the child's own request is refused here "
              "and the realtime priority it renders at cannot be shown");

    const auto xml = probeDescriptionXml();
    REQUIRE (xml.has_value());
    duskstudio::ipc::RemotePluginConnection connection;
    std::string error;
    REQUIRE (connection.connect (DUSKSTUDIO_PLUGIN_HOST_PATH, "--ipc-host", error));
    loadProbe (connection, *xml);

    const int rrMin = sched_get_priority_min (SCHED_RR);
    const int rrMax = sched_get_priority_max (SCHED_RR);
    const auto report = render (connection);
    CHECK (report[Probe::kPolicy] == SCHED_RR);
    CHECK (report[Probe::kPriority] == rrMin + jucePriority * (rrMax - rrMin) / 10);
    CHECK (report[Probe::kFlushesResults] == 1);

    const auto limit = realtimeCpuLimitOf (connection.getChildPid());
    CHECK (limit.rlim_cur != RLIM_INFINITY);
    CHECK ((limit.rlim_max == RLIM_INFINITY || limit.rlim_cur < limit.rlim_max));
    CHECK (catchesSignal (connection.getChildPid(), SIGXCPU));
}

// A child whose RLIMIT_RTPRIO refuses realtime still loads and renders. It
// says what it got once, from the thread it renders on for its whole life,
// however many plug-ins it loads.
TEST_CASE ("A sandboxed plug-in refused realtime renders at normal priority and says so once", "[ipc][plugin]")
{
    const auto xml = probeDescriptionXml();
    REQUIRE (xml.has_value());
    duskstudio::test::TempDirectory temp ("dusk-sandbox-realtime-");
    duskstudio::ipc::RemotePluginConnection connection;
    Report first {}, reloaded {};
    const auto log = duskstudio::test::captureStderr (temp.path(), [&]
    {
        {
            const ScopedRealtimePriorityCeiling ceiling (0);
            REQUIRE (ceiling.applied);
            std::string error;
            REQUIRE (connection.connect (DUSKSTUDIO_PLUGIN_HOST_PATH, "--ipc-host", error));
        }
        loadProbe (connection, *xml);
        for (int block = 0; block < 32; ++block)
            first = render (connection);
        loadProbe (connection, *xml);
        for (int block = 0; block < 32; ++block)
            reloaded = render (connection);
    });

    CHECK (first[Probe::kPolicy] == SCHED_OTHER);
    CHECK (first[Probe::kFlushesResults] == 1);
    CHECK (first[Probe::kFlushesInputs] == 1);
    CHECK (reloaded[Probe::kThreadId] == first[Probe::kThreadId]);
    CHECK (reloaded[Probe::kPolicy] == SCHED_OTHER);
    CAPTURE (log);
    CHECK (occurrences (log, kSchedulingLine) == 1);

    const auto limit = realtimeCpuLimitOf (connection.getChildPid());
    CHECK (limit.rlim_cur != RLIM_INFINITY);
    CHECK (catchesSignal (connection.getChildPid(), SIGXCPU));
}

// The probe holds the scan child inside its initialize() until released, so
// every thread the child has while it loads plug-in code can be looked at.
TEST_CASE ("A plug-in scan child takes no realtime scheduling and no CPU-time guard", "[ipc][plugin-scan]")
{
    duskstudio::test::TempDirectory hold ("dusk-render-probe-hold-");
    const duskstudio::test::ScopedEnvironmentVariable holdFolder (Probe::kHoldVariable, hold.path().string());
    duskstudio::ScanProcess child;
    REQUIRE (child.start ({ DUSKSTUDIO_PLUGIN_HOST_PATH, "--scan", "VST3", DUSKSTUDIO_RENDER_PROBE_VST3_FIXTURE_PATH }));

    const auto held = hold.path() / "held";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
    while (! std::filesystem::exists (held) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    long long pid = 0;
    std::ifstream (held) >> pid;
    const auto release = [&] { std::ofstream (hold.path() / "release") << "go"; };
    if (pid <= 0) release();
    REQUIRE (pid > 0);

    std::vector<std::int64_t> threads;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator ("/proc/" + std::to_string (pid) + "/task", error))
        threads.push_back (std::stoll (entry.path().filename().string()));
    std::vector<std::int64_t> realtime;
    for (const auto thread : threads)
        if (duskstudio::rt::threadScheduling (thread).isRealtime()) realtime.push_back (thread);
    rlimit own {};
    ::getrlimit (RLIMIT_RTTIME, &own);
    const auto theirs = realtimeCpuLimitOf ((int) pid);
    const bool guarded = catchesSignal ((int) pid, SIGXCPU);
    release();

    CHECK_FALSE (threads.empty());
    CAPTURE (realtime);
    CHECK (realtime.empty());
    CHECK (theirs.rlim_cur == own.rlim_cur);
    CHECK_FALSE (guarded);

    const auto rows = duskstudio::scanproto::parsePayload (
        duskstudio::scanproto::extractPayload (readUntilClosed (child)));
    REQUIRE (rows.has_value());
    CHECK (rows->size() == 1);
}
#endif

#if defined (_WIN32)
// The child's own line says what its audio thread got, and so what the probe
// must find on it. MMCSS lowers a registered thread to 1-7 for the rest of a
// period once its category has used its share of the processor, so an MMCSS
// thread has to show the realtime range in some blocks, not in every one.
TEST_CASE ("A sandboxed plug-in renders in the MMCSS task the DSP threads join", "[ipc][plugin]")
{
    const auto xml = probeDescriptionXml();
    REQUIRE (xml.has_value());
    duskstudio::test::TempDirectory temp ("dusk-sandbox-mmcss-");
    duskstudio::ipc::RemotePluginConnection connection;
    std::vector<Report> reports;
    const auto log = captureChildStderr (temp.path(), [&]
    {
        std::string error;
        REQUIRE (connection.connect (DUSKSTUDIO_PLUGIN_HOST_PATH, "--ipc-host", error));
        loadProbe (connection, *xml);
        for (int block = 0; block < 32; ++block)
            reports.push_back (render (connection));
    });

    std::string samples;
    for (const auto& report : reports)
        samples += std::to_string (report[Probe::kPolicy]) + "/" + std::to_string (report[Probe::kPriority]) + " ";
    CAPTURE (log, samples);
    REQUIRE (occurrences (log, kSchedulingLine) == 1);
    const bool proAudio = log.find (std::string (kSchedulingLine) + "realtime in the MMCSS Pro Audio task")
                          != std::string::npos;
    const bool timeCritical = log.find ("MMCSS refused the Pro Audio task: at time-critical priority")
                              != std::string::npos;
    CHECK ((proAudio || timeCritical));
    if (proAudio)
        CHECK (std::any_of (reports.begin(), reports.end(),
                            [] (const Report& report) { return report[Probe::kPriority] >= 16; }));
    if (timeCritical)
        CHECK (std::all_of (reports.begin(), reports.end(), [] (const Report& report)
                            {
                                return report[Probe::kPriority] == 15
                                    && report[Probe::kPolicy] == THREAD_PRIORITY_TIME_CRITICAL;
                            }));
    CHECK (reports.back()[Probe::kFlushesResults] == 1);
}
#endif

#if defined (__APPLE__)
// The period follows the parent's block: set by the load, moved by a prepare.
TEST_CASE ("A sandboxed plug-in renders under the time-constraint policy for the parent's block period",
           "[ipc][plugin]")
{
    const auto xml = probeDescriptionXml();
    REQUIRE (xml.has_value());
    duskstudio::ipc::RemotePluginConnection connection;
    std::string error;
    REQUIRE (connection.connect (DUSKSTUDIO_PLUGIN_HOST_PATH, "--ipc-host", error));
    loadProbe (connection, *xml);
    const auto periodMicros = [] (int blockSize) { return std::lround (1.0e6 * blockSize / 48000.0); };

    const auto loaded = render (connection);
    CHECK (loaded[Probe::kPolicy] == 1);
    CHECK (std::abs (loaded[Probe::kPriority] - periodMicros (kBlockSize)) <= 1);
    CHECK (loaded[Probe::kFlushesResults] == 1);

    REQUIRE (connection.prepareToPlay (48000.0, 4 * kBlockSize, error));
    const auto prepared = render (connection);
    CHECK (prepared[Probe::kPolicy] == 1);
    CHECK (std::abs (prepared[Probe::kPriority] - periodMicros (4 * kBlockSize)) <= 1);
    CHECK (prepared[Probe::kThreadId] == loaded[Probe::kThreadId]);
}
#endif

#if defined (_WIN32) || defined (__APPLE__)
// However many plug-ins a child loads, it renders them on one thread and says
// once how that thread is scheduled.
TEST_CASE ("A sandboxed plug-in's child says once how its audio thread is scheduled", "[ipc][plugin]")
{
    const auto xml = probeDescriptionXml();
    REQUIRE (xml.has_value());
    duskstudio::test::TempDirectory temp ("dusk-sandbox-realtime-");
    duskstudio::ipc::RemotePluginConnection connection;
    Report first {}, reloaded {};
    const auto log = captureChildStderr (temp.path(), [&]
    {
        std::string error;
        REQUIRE (connection.connect (DUSKSTUDIO_PLUGIN_HOST_PATH, "--ipc-host", error));
        loadProbe (connection, *xml);
        for (int block = 0; block < 32; ++block)
            first = render (connection);
        loadProbe (connection, *xml);
        for (int block = 0; block < 32; ++block)
            reloaded = render (connection);
    });

    CHECK (reloaded[Probe::kThreadId] == first[Probe::kThreadId]);
    CAPTURE (log);
    CHECK (occurrences (log, kSchedulingLine) == 1);
}
#endif

// Not run by ctest: a measurement. Each block is paced at its period from a
// thread at the engine's audio scheduling, first with the machine idle, then
// with a normal-priority thread spinning on every core:
//   dusk-studio-tests "[sandbox-load]"
// In a virtual machine with more processors than its host has cores, confine
// the run to fewer of them (start /affinity, taskset), which the child
// inherits: with every one busy the host's scheduler, not the guest's, decides
// when the child runs.
TEST_CASE ("Sandboxed block round trip, idle and under CPU load", "[.][sandbox-load]")
{
    static constexpr int kLoadBlockSize = 128;
    static constexpr double kSampleRate = 48000.0;
    static constexpr int kBlocks = 3000;
    static constexpr long long kBypassNs = 100'000'000LL;   // PluginSlot's kOopProcessTimeoutNs
    const auto period = std::chrono::nanoseconds ((long long) (1.0e9 * kLoadBlockSize / kSampleRate));

    const auto xml = probeDescriptionXml();
    REQUIRE (xml.has_value());
    duskstudio::ipc::RemotePluginConnection connection;
    std::string error;
    REQUIRE (connection.connect (DUSKSTUDIO_PLUGIN_HOST_PATH, "--ipc-host", error));
    int numIn = 0, numOut = 0, latency = 0;
    bool instrument = false;
    REQUIRE (connection.loadPlugin (*xml, kSampleRate, kLoadBlockSize, numIn, numOut, latency, instrument, error));

    const auto measure = [&] (const char* label, unsigned busyThreads)
    {
        std::atomic<bool> stop { false };
        std::vector<std::thread> load;
        for (unsigned i = 0; i < busyThreads; ++i)
            load.emplace_back ([&stop]
            {
                volatile double sink = 1.0;
                while (! stop.load (std::memory_order_relaxed))
                    for (int n = 0; n < 4096; ++n)
                        sink = sink * 1.0000001 + 1.0e-9;
            });

        std::vector<long long> roundTripsNs;
        roundTripsNs.reserve (kBlocks);
        bool allReplied = true;
        std::chrono::nanoseconds elapsed {};
        std::thread ([&]
        {
            const ScopedEngineAudioScheduling scheduling (1.0e-9 * (double) period.count());
            std::array<float, kLoadBlockSize> left {}, right {};
            const float* inputs[] { left.data(), right.data() };
            dusk::MidiBuffer midi;
            const auto start = std::chrono::steady_clock::now();
            auto next = start;
            for (int block = 0; block < kBlocks && allReplied; ++block)
            {
                next += period;
                std::this_thread::sleep_until (next);
                midi.clear();
                const auto sent = std::chrono::steady_clock::now();
                allReplied = connection.processBlockSync (inputs, 2, 2, kLoadBlockSize, midi, 1'000'000'000LL);
                roundTripsNs.push_back (std::chrono::duration_cast<std::chrono::nanoseconds> (
                    std::chrono::steady_clock::now() - sent).count());
            }
            elapsed = std::chrono::steady_clock::now() - start;
        }).join();

        stop.store (true, std::memory_order_relaxed);
        for (auto& thread : load) thread.join();
        REQUIRE (allReplied);

        const auto overPeriod = std::count_if (roundTripsNs.begin(), roundTripsNs.end(),
                                               [&] (long long ns) { return ns > period.count(); });
        const auto overBypass = std::count_if (roundTripsNs.begin(), roundTripsNs.end(),
                                               [] (long long ns) { return ns > kBypassNs; });
        std::sort (roundTripsNs.begin(), roundTripsNs.end());
        std::printf ("[sandbox-load] %s, %u busy threads: %zu blocks of %d at %.0f Hz, one every %.2f ms; "
                     "round trip us p50 %.1f p99 %.1f p99.9 %.1f max %.1f; over the %.2f ms period %ld, "
                     "over 100 ms %ld\n",
                     label, busyThreads, roundTripsNs.size(), kLoadBlockSize, kSampleRate,
                     1.0e-6 * (double) elapsed.count() / (double) roundTripsNs.size(),
                     percentileMicros (roundTripsNs, 0.50), percentileMicros (roundTripsNs, 0.99),
                     percentileMicros (roundTripsNs, 0.999), percentileMicros (roundTripsNs, 1.0),
                     1.0e-6 * (double) period.count(), (long) overPeriod, (long) overBypass);
        std::fflush (stdout);
    };

    measure ("idle", 0);
    measure ("loaded", std::max (1u, std::thread::hardware_concurrency()));
}
#endif
