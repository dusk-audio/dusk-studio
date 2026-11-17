#include <catch2/catch_test_macros.hpp>

#include "TestTempDirectory.h"
#include "engine/PluginManager.h"
#include "engine/ScanProcess.h"
#include "engine/ipc/PluginScanProtocol.h"
#include "engine/ipc/RemotePluginConnection.h"
#include "fixtures/RenderProbeVst3.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#if defined (__linux__)
 #include "TestProcessEnvironment.h"
 #include "engine/RealtimeKit.h"
 #include "engine/RtPriority.h"

 #include <csignal>
 #include <cstdint>
 #include <filesystem>
 #include <fstream>
 #include <sched.h>
 #include <sys/resource.h>
 #include <thread>
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

#if defined (__linux__)
// The marker the child's one scheduling line starts with.
constexpr const char* kSchedulingLine = "[dusk-studio-plugin-host] audio thread ";

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

std::size_t occurrences (const std::string& text, const std::string& needle)
{
    std::size_t found = 0;
    for (auto at = text.find (needle); at != std::string::npos; at = text.find (needle, at + 1))
        ++found;
    return found;
}
#endif
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
#endif
