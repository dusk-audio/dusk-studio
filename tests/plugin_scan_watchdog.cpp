#include <catch2/catch_test_macros.hpp>
#include "TestTempDirectory.h"
#include "engine/ScanProcess.h"
#include "engine/sfz/SfzDirectoryLease.h"

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#if defined(_WIN32)
 #include <windows.h>
#else
 #include <cerrno>
 #include <csignal>
 #include <spawn.h>
 #include <unistd.h>
 extern "C" char** environ;
#endif

#if DUSKSTUDIO_HAS_OOP_PLUGINS || DUSKSTUDIO_HAS_NATIVE_CLAP || DUSKSTUDIO_HAS_NATIVE_VST3
namespace duskstudio
{
// Test-only linker seam implemented beside the production supervisor. The
// fields are completed, cancelled, and timedOut, in that order.
std::array<bool, 3> driveSandboxedScanForTest (
    ScanProcess&, const std::atomic<bool>* abort, std::uint32_t timeoutMs,
    std::size_t outputLimit = 8 * 1024 * 1024);
}

namespace
{
constexpr const char* kSilentChildTest = "plugin scan watchdog silent child fixture";
constexpr const char* kInheritedWriterTest = "plugin scan watchdog inherited writer fixture";
constexpr const char* kTreeDirectory = "DUSK_TEST_SCAN_TREE_DIRECTORY";
constexpr const char* kTreeLeaf = "DUSK_TEST_SCAN_TREE_LEAF";
constexpr const char* kTreeParentExits = "DUSK_TEST_SCAN_TREE_PARENT_EXITS";
constexpr const char* kTreeReady = "==DUSK_SCAN_TREE_READY==";

std::uint64_t currentPid()
{
   #if defined(_WIN32)
    return ::GetCurrentProcessId();
   #else
    return static_cast<std::uint64_t> (::getpid());
   #endif
}

struct Environment
{
    Environment (const char* variable, std::string value) : name (variable)
    {
        if (const auto* current = std::getenv (name.c_str())) before = current;
        set (value);
    }
    ~Environment() { set (before); }
    void set (const std::string& value) const
    {
       #if defined(_WIN32)
        ::_putenv_s (name.c_str(), value.c_str());
       #else
        if (value.empty()) ::unsetenv (name.c_str());
        else               ::setenv (name.c_str(), value.c_str(), 1);
       #endif
    }
    std::string name, before;
};

bool processLive (std::uint64_t pid)
{
   #if defined(_WIN32)
    const auto process = ::OpenProcess (SYNCHRONIZE, FALSE, static_cast<DWORD> (pid));
    if (process == nullptr) return false;
    const bool live = ::WaitForSingleObject (process, 0) == WAIT_TIMEOUT;
    ::CloseHandle (process);
    return live;
   #else
    if (::kill (static_cast<pid_t> (pid), 0) != 0 && errno == ESRCH) return false;
    #if defined(__linux__)
    const auto status = dusk::fs::loadFileAsString ("/proc/" + std::to_string (pid) + "/stat");
    const auto end = status.rfind (')');
    if (end != std::string::npos && end + 2 < status.size() && status[end + 2] == 'Z') return false;
    #endif
    return true;
   #endif
}

void checkInheritedWriter (bool cancel, bool parentExits, std::size_t outputLimit = 8 * 1024 * 1024)
{
    duskstudio::test::TempDirectory directory ("dusk-scan-tree-");
    const Environment root (kTreeDirectory, directory.path().u8string());
    const Environment leaf (kTreeLeaf, "");
    const Environment exits (kTreeParentExits, parentExits ? "1" : "");
    const auto executable = juce::File::getSpecialLocation (
        juce::File::currentExecutableFile).getFullPathName();
    duskstudio::ScanProcess child;
    REQUIRE (child.start ({ executable.toStdString(), kInheritedWriterTest }));
    std::string ready;
    while (ready.find (kTreeReady) == std::string::npos && ready.size() < 8192)
    {
        char next = 0;
        const auto read = child.readOutput (&next, 1);
        if (read.status == duskstudio::ScanProcess::ReadStatus::Data) ready.push_back (next);
        else if (read.status != duskstudio::ScanProcess::ReadStatus::Waiting) break;
    }
    REQUIRE (ready.find (kTreeReady) != std::string::npos);
    const auto readPid = [&directory] (const char* name)
    {
        return std::strtoull (dusk::fs::loadFileAsString (directory.path() / name).c_str(), nullptr, 10);
    };
    const auto parentPid = readPid ("parent.pid"), leafPid = readPid ("leaf.pid");
    REQUIRE (parentPid > 0);
    REQUIRE (leafPid > 0);
    std::atomic<bool> abortRequested { cancel };
    const auto started = std::chrono::steady_clock::now();
    const auto [completed, cancelled, timedOut] = duskstudio::driveSandboxedScanForTest (
        child, cancel ? &abortRequested : nullptr, 100, outputLimit);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK (elapsed < std::chrono::milliseconds (500));
    CHECK (completed == parentExits);
    CHECK (cancelled == cancel);
    CHECK (timedOut == (! parentExits && ! cancel));
    const auto cleanupDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds (250);
    while ((processLive (parentPid) || processLive (leafPid))
           && std::chrono::steady_clock::now() < cleanupDeadline)
        std::this_thread::yield();
    CHECK_FALSE (processLive (parentPid));
    CHECK_FALSE (processLive (leafPid));
    duskstudio::sfz::DirectoryLease released;
    std::string error;
    CHECK (released.acquire (directory.path(), duskstudio::sfz::DirectoryLease::Access::Exclusive, error));
}

void startSilentChild (duskstudio::ScanProcess& child)
{
    const auto executable = juce::File::getSpecialLocation (
        juce::File::currentExecutableFile).getFullPathName();
    REQUIRE (child.start ({ executable.toStdString(), kSilentChildTest }));
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
    duskstudio::ScanProcess child;
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
    duskstudio::ScanProcess child;
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

TEST_CASE (kInheritedWriterTest, "[.][scan-grandchild-fixture][ipc]")
{
    const auto* root = std::getenv (kTreeDirectory);
    REQUIRE (root != nullptr);
    const auto directory = std::filesystem::u8path (root);
    if (const auto* isLeaf = std::getenv (kTreeLeaf); isLeaf != nullptr && std::string (isLeaf) == "1")
    {
        duskstudio::sfz::DirectoryLease ownership;
        std::string error;
        REQUIRE (ownership.acquire (directory, duskstudio::sfz::DirectoryLease::Access::Exclusive, error));
        REQUIRE (dusk::fs::writeStringToFile (directory / "leaf.pid", std::to_string (currentPid())));
        std::fputs (kTreeReady, stdout);
        std::fflush (stdout);
        REQUIRE (dusk::fs::writeStringToFile (directory / "ready", "ready"));
        std::this_thread::sleep_for (std::chrono::milliseconds (1500));
        return;
    }
    REQUIRE (dusk::fs::writeStringToFile (directory / "parent.pid", std::to_string (currentPid())));
    std::fflush (stdout);
    const Environment leaf (kTreeLeaf, "1");
    const auto executable = juce::File::getSpecialLocation (
        juce::File::currentExecutableFile).getFullPathName();
   #if defined(_WIN32)
    std::wstring command = std::wstring (L"\"") + executable.toWideCharPointer() + L"\" \""
                         + juce::String (kInheritedWriterTest).toWideCharPointer() + L"\"";
    STARTUPINFOW startup {};
    startup.cb = sizeof (startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = ::GetStdHandle (STD_INPUT_HANDLE);
    startup.hStdOutput = ::GetStdHandle (STD_OUTPUT_HANDLE);
    startup.hStdError = ::GetStdHandle (STD_ERROR_HANDLE);
    PROCESS_INFORMATION process {};
    REQUIRE (::CreateProcessW (executable.toWideCharPointer(), command.data(), nullptr, nullptr, TRUE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
    ::CloseHandle (process.hThread);
    ::CloseHandle (process.hProcess);
   #else
    const auto program = executable.toStdString();
    char* arguments[] { const_cast<char*> (program.c_str()), const_cast<char*> (kInheritedWriterTest), nullptr };
    pid_t descendant = -1;
    REQUIRE (::posix_spawn (&descendant, program.c_str(), nullptr, nullptr, arguments, environ) == 0);
   #endif
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (2);
    while (! std::filesystem::exists (directory / "ready") && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    REQUIRE (std::filesystem::exists (directory / "ready"));
    if (const auto* exits = std::getenv (kTreeParentExits); exits != nullptr && std::string (exits) == "1")
    {
        constexpr char payload[] = "==DUSK_SCAN_BEGIN==\n{\"version\":1,\"descriptors\":[]}\n==DUSK_SCAN_END==\n";
       #if defined(_WIN32)
        DWORD written = 0;
        REQUIRE (::WriteFile (::GetStdHandle (STD_OUTPUT_HANDLE), payload,
                              static_cast<DWORD> (sizeof (payload) - 1), &written, nullptr));
        REQUIRE (written == sizeof (payload) - 1);
       #else
        REQUIRE (::write (STDOUT_FILENO, payload, sizeof (payload) - 1)
                    == static_cast<ssize_t> (sizeof (payload) - 1));
       #endif
        return;
    }
    std::this_thread::sleep_for (std::chrono::seconds (2));
}

TEST_CASE ("Plugin scan timeout terminates inherited stdout descendants", "[plugin-scan][ipc]")
{
    checkInheritedWriter (false, false);
}

TEST_CASE ("Plugin scan cancellation terminates inherited stdout descendants", "[plugin-scan][ipc]")
{
    checkInheritedWriter (true, false);
}

TEST_CASE ("Plugin scan completion terminates inherited stdout descendants", "[plugin-scan][ipc]")
{
    SECTION ("a complete frame releases the child tree immediately")
    {
        checkInheritedWriter (false, true);
    }
    SECTION ("a complete frame in the terminal output chunk wins the cap")
    {
        checkInheritedWriter (false, true, 16);
    }
}
#endif
