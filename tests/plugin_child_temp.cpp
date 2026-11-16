#include <catch2/catch_test_macros.hpp>

#include "TestTempDirectory.h"
#include "engine/ScanProcess.h"
#include "engine/ipc/PluginScanProtocol.h"
#include "engine/ipc/RemotePluginConnection.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" char** environ;

namespace duskstudio
{
// Test-only linker seam beside the production scan supervisor.
std::array<bool, 3> driveSandboxedScanForTest (
    ScanProcess&, const std::atomic<bool>* abort, std::uint32_t timeoutMs,
    std::size_t outputLimit = 8 * 1024 * 1024);
}

namespace
{
constexpr const char* kScanTempFixture = "plugin scan child temp folder fixture";

class ScopedTempDirVariable
{
public:
    explicit ScopedTempDirVariable (const std::filesystem::path& value)
    {
        if (const char* existing = std::getenv ("TMPDIR")) previous = existing;
        ::setenv ("TMPDIR", value.c_str(), 1);
    }
    ~ScopedTempDirVariable()
    {
        if (previous) ::setenv ("TMPDIR", previous->c_str(), 1);
        else          ::unsetenv ("TMPDIR");
    }

private:
    std::optional<std::string> previous;
};

std::vector<std::string> entriesUnder (const std::filesystem::path& root)
{
    std::vector<std::string> found;
    std::error_code error;
    for (auto it = std::filesystem::recursive_directory_iterator (root, error);
         ! error && it != std::filesystem::recursive_directory_iterator(); it.increment (error))
        found.push_back (it->path().lexically_relative (root).string());
    return found;
}

bool startsWithTemp (const std::string& name)
{
    return name.rfind ("temp_", 0) == 0;
}

struct Descriptor
{
    int value = -1;
    ~Descriptor() { if (value >= 0) ::close (value); }
};

struct ScanRun
{
    std::string output;
    bool sawPayloadEnd = false;
    int exitCode = -1;
    std::vector<std::string> created;
};

// Runs `--scan <format> <file>` with TMPDIR naming `temp` and records every
// name created directly in it. The child is spawned here rather than through
// ScanProcess, which would give it a temp folder of its own that the watch
// cannot see into. With killAtPayloadEnd the child is killed the moment its
// payload end marker arrives, as the scan supervisor does.
ScanRun runScanChild (const std::string& format, const std::string& file,
                      const std::filesystem::path& temp, bool killAtPayloadEnd)
{
    Descriptor watch { ::inotify_init1 (IN_NONBLOCK | IN_CLOEXEC) };
    REQUIRE (watch.value >= 0);
    REQUIRE (::inotify_add_watch (watch.value, temp.c_str(), IN_CREATE) >= 0);

    std::vector<std::string> variables;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry)
        if (std::strncmp (*entry, "TMPDIR=", 7) != 0) variables.emplace_back (*entry);
    variables.push_back ("TMPDIR=" + temp.string());
    std::vector<char*> environment;
    for (auto& variable : variables) environment.push_back (variable.data());
    environment.push_back (nullptr);

    int ends[2] { -1, -1 };
    REQUIRE (::pipe2 (ends, O_CLOEXEC) == 0);
    Descriptor reader { ends[0] };
    Descriptor writer { ends[1] };
    posix_spawn_file_actions_t actions;
    REQUIRE (::posix_spawn_file_actions_init (&actions) == 0);
    REQUIRE (::posix_spawn_file_actions_adddup2 (&actions, writer.value, STDOUT_FILENO) == 0);
    std::string host = DUSKSTUDIO_PLUGIN_HOST_PATH, scan = "--scan", name = format, path = file;
    char* arguments[] { host.data(), scan.data(), name.data(), path.data(), nullptr };
    pid_t pid = -1;
    const int spawned = ::posix_spawn (&pid, host.c_str(), &actions, nullptr, arguments, environment.data());
    ::posix_spawn_file_actions_destroy (&actions);
    REQUIRE (spawned == 0);
    ::close (writer.value);
    writer.value = -1;

    ScanRun run;
    bool closed = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
    while (! closed && std::chrono::steady_clock::now() < deadline)
    {
        pollfd ready { reader.value, POLLIN, 0 };
        if (::poll (&ready, 1, 50) <= 0) continue;
        char chunk[4096];
        const auto bytes = ::read (reader.value, chunk, sizeof (chunk));
        if (bytes <= 0) { closed = true; break; }
        run.output.append (chunk, (std::size_t) bytes);
        run.sawPayloadEnd = run.output.find (duskstudio::scanproto::kPayloadEnd) != std::string::npos;
        if (run.sawPayloadEnd && killAtPayloadEnd) break;
    }
    if (killAtPayloadEnd || ! closed) ::kill (pid, SIGKILL);
    int status = 0;
    REQUIRE (::waitpid (pid, &status, 0) == pid);
    if (WIFEXITED (status)) run.exitCode = WEXITSTATUS (status);

    alignas (inotify_event) char events[16384];
    for (auto bytes = ::read (watch.value, events, sizeof (events)); bytes > 0;
         bytes = ::read (watch.value, events, sizeof (events)))
        for (const char* at = events; at < events + bytes;)
        {
            const auto* event = reinterpret_cast<const inotify_event*> (at);
            if (event->len > 0) run.created.emplace_back (event->name);
            at += sizeof (inotify_event) + event->len;
        }
    return run;
}
} // namespace

// Spawned by the scan case below. Like a plug-in scan child it leaves a folder
// in its temp folder, reports a complete payload and is killed before it can
// exit. The deadline keeps an aborted parent from leaving it running.
TEST_CASE (kScanTempFixture, "[.][scan-temp-fixture]")
{
    const char* temp = std::getenv ("TMPDIR");
    REQUIRE (temp != nullptr);
    const auto folder = std::filesystem::path (temp) / "temp_fixture";
    REQUIRE (std::filesystem::create_directory (folder));
    REQUIRE (dusk::fs::writeStringToFile (folder / "left-behind", "x"));
    static constexpr char payload[] = "==DUSK_SCAN_BEGIN==\n{\"version\":1,\"descriptors\":[]}\n==DUSK_SCAN_END==\n";
    REQUIRE (::write (STDOUT_FILENO, payload, sizeof (payload) - 1) == (ssize_t) (sizeof (payload) - 1));
    const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds (10);
    while (std::chrono::steady_clock::now() < giveUp)
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
}

// The supervisor kills a scan child as soon as it has read a whole payload.
TEST_CASE ("A scan child killed after its payload leaves nothing in the temp folder", "[plugin-scan]")
{
    duskstudio::test::TempDirectory root ("dusk-tmpdir-test-");
    const ScopedTempDirVariable redirect (root.path());
    duskstudio::ScanProcess child;
    REQUIRE (child.start ({ dusk::fs::currentExecutablePath().string(), kScanTempFixture }));

    const auto [completed, cancelled, timedOut] = duskstudio::driveSandboxedScanForTest (child, nullptr, 10000);
    REQUIRE (completed);
    CHECK_FALSE (cancelled);
    CHECK_FALSE (timedOut);
    const auto left = entriesUnder (root.path());
    CAPTURE (left);
    CHECK (left.empty());
}

// JUCE's LV2 host writes its LV2 specifications to a folder in the temp folder
// and removes it only from its destructor, which neither the SIGKILL a crash
// stands for nor the SIGTERM an unload sends lets run.
TEST_CASE ("A plug-in host child that is killed leaves nothing in the temp folder", "[ipc][plugin]")
{
    duskstudio::test::TempDirectory root ("dusk-tmpdir-test-");
    const ScopedTempDirVariable redirect (root.path());
    duskstudio::ipc::RemotePluginConnection connection;
    std::string error;
    REQUIRE (connection.connect (DUSKSTUDIO_PLUGIN_HOST_PATH, "--ipc-host", error));

    juce::PluginDescription description;
    description.name = "Absent";
    description.pluginFormatName = "LV2";
    description.fileOrIdentifier = "urn:dusk-studio:test:absent";
    int numIn = 0, numOut = 0, latency = 0;
    bool instrument = false;
    CHECK_FALSE (connection.loadPlugin (description.createXml()->toString().toStdString(),
                                        48000.0, 64, numIn, numOut, latency, instrument, error));
    const auto loaded = entriesUnder (root.path());
    CAPTURE (loaded);
    REQUIRE (std::any_of (loaded.begin(), loaded.end(), [] (const std::string& entry)
                          { return startsWithTemp (std::filesystem::path (entry).filename().string()); }));

    SECTION ("reaped by the crash check")
    {
        const int pid = connection.getChildPid();
        REQUIRE (pid > 0);
        REQUIRE (::kill ((pid_t) pid, SIGKILL) == 0);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (10);
        bool reaped = false;
        while (! (reaped = connection.pollReaper()) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        REQUIRE (reaped);
    }
    SECTION ("reaped by an unload")
    {
        connection.disconnect();
    }
    const auto left = entriesUnder (root.path());
    CAPTURE (left);
    CHECK (left.empty());
}

#if defined (DUSKSTUDIO_SPLIT_STATE_VST3_FIXTURE_PATH)
// The child exits by itself here, and the LV2 format would remove its folder on
// the way out, so only the folder's creation can show.
TEST_CASE ("A scan child builds only the format it is asked to scan", "[ipc][plugin-scan]")
{
    duskstudio::test::TempDirectory root ("dusk-tmpdir-test-");
    const auto run = runScanChild ("VST3", DUSKSTUDIO_SPLIT_STATE_VST3_FIXTURE_PATH, root.path(), false);
    CHECK (run.exitCode == 0);
    const auto rows = duskstudio::scanproto::parsePayload (duskstudio::scanproto::extractPayload (run.output));
    REQUIRE (rows.has_value());
    CHECK (rows->size() == 1);
    CAPTURE (run.created);
    CHECK (std::none_of (run.created.begin(), run.created.end(), startsWithTemp));
}
#endif

#if defined (DUSKSTUDIO_STATE_PROBE_LV2_FIXTURE_PATH)
// Where the parent gives a scan child no temp folder of its own (macOS and
// Windows), only the child can remove the LV2 folder, and the supervisor kills
// it as soon as the payload is complete.
TEST_CASE ("An LV2 scan child has removed its temp folder by the time its payload ends", "[ipc][plugin-scan]")
{
    duskstudio::test::TempDirectory root ("dusk-tmpdir-test-");
    const auto run = runScanChild ("LV2", DUSKSTUDIO_STATE_PROBE_LV2_FIXTURE_PATH, root.path(), true);
    REQUIRE (run.sawPayloadEnd);
    const auto rows = duskstudio::scanproto::parsePayload (duskstudio::scanproto::extractPayload (run.output));
    REQUIRE (rows.has_value());
    CHECK (rows->size() == 1);
    CAPTURE (run.created);
    CHECK (std::any_of (run.created.begin(), run.created.end(), startsWithTemp));
    const auto left = entriesUnder (root.path());
    CAPTURE (left);
    CHECK (left.empty());
}
#endif
