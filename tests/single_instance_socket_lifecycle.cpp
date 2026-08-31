#include <catch2/catch_test_macros.hpp>

#include "util/SingleInstance.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#else
 #include <sys/socket.h>
 #include <sys/stat.h>
 #include <sys/un.h>
 #include <unistd.h>
#endif

namespace
{
namespace fs = std::filesystem;

class ScopedSlot
{
public:
    ScopedSlot()
    {
        duskstudio::single_instance::release();
        duskstudio::single_instance::testing::setDispatcher (
            [] (std::function<void()> fn)
            {
                fn();
                return true;
            });

#if defined (_WIN32)
        static std::atomic<unsigned long> counter { 0 };
        slotName = std::to_string ((unsigned long) ::GetCurrentProcessId()) + "-"
                 + std::to_string (counter.fetch_add (1));
        _putenv_s ("DUSKSTUDIO_TEST_SINGLE_INSTANCE_SLOT", slotName.c_str());
#else
 #if defined (__APPLE__)
        variable = "TMPDIR";
 #else
        variable = "XDG_RUNTIME_DIR";
 #endif
        if (const char* prev = std::getenv (variable.c_str()))
        {
            previous = prev;
            hadPrevious = true;
        }

        // Terse name leaves sockaddr_un headroom even under a long system
        // temporary root. The production path rejects rather than truncates.
        std::string tmpl = (fs::temp_directory_path() / "dusk-si-XXXXXX").string();
        REQUIRE (tmpl.size() + std::strlen ("/dusk-studio/instance-0123456789abcdef.sock")
                   < sizeof (sockaddr_un::sun_path));
        REQUIRE (::mkdtemp (&tmpl[0]) != nullptr);
        dir = tmpl;
        REQUIRE (::setenv (variable.c_str(), dir.c_str(), 1) == 0);
#endif
    }

    ~ScopedSlot()
    {
        duskstudio::single_instance::release();
        duskstudio::single_instance::testing::setDispatcher ({});
#if defined (_WIN32)
        _putenv_s ("DUSKSTUDIO_TEST_SINGLE_INSTANCE_SLOT", "");
#else
        if (hadPrevious) ::setenv (variable.c_str(), previous.c_str(), 1);
        else             ::unsetenv (variable.c_str());
        std::error_code ec;
        fs::remove_all (dir, ec);
#endif
    }

#if ! defined (_WIN32)
    const fs::path& path() const noexcept { return dir; }
#endif

private:
#if defined (_WIN32)
    std::string slotName;
#else
    fs::path dir;
    std::string variable;
    std::string previous;
    bool hadPrevious = false;
#endif
};

struct Deliveries
{
    void accept (std::string payload)
    {
        std::lock_guard<std::mutex> lock (mutex);
        payloads.push_back (std::move (payload));
        changed.notify_all();
    }

    bool waitFor (std::size_t count)
    {
        std::unique_lock<std::mutex> lock (mutex);
        return changed.wait_for (lock, std::chrono::seconds (5),
                                 [&] { return payloads.size() >= count; });
    }

    std::vector<std::string> copy()
    {
        std::lock_guard<std::mutex> lock (mutex);
        return payloads;
    }

    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> payloads;
};

void noPayload (std::string) {}

#if ! defined (_WIN32)
fs::path soleSocketIn (const fs::path& runtimeDir)
{
    fs::path found;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator (runtimeDir / "dusk-studio", ec))
        if (entry.path().extension() == ".sock")
        {
            REQUIRE (found.empty());
            found = entry.path();
        }
    return found;
}

ino_t inodeOf (const fs::path& p)
{
    struct stat st {};
    return ::lstat (p.c_str(), &st) == 0 ? st.st_ino : 0;
}

int bindSocketAt (const fs::path& p)
{
    const int fd = ::socket (AF_UNIX, SOCK_STREAM, 0);
    REQUIRE (fd >= 0);

    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    const auto name = p.string();
    REQUIRE (name.size() < sizeof addr.sun_path);
    std::memcpy (addr.sun_path, name.c_str(), name.size());
    REQUIRE (::bind (fd, (const sockaddr*) &addr, sizeof addr) == 0);
    return fd;
}
#endif
} // namespace

TEST_CASE ("single-instance handoff preserves quoted Unicode and empty payloads",
           "[single-instance][issue-368]")
{
    ScopedSlot slot;
    Deliveries delivered;
    const auto receive = [&] (std::string payload) { delivered.accept (std::move (payload)); };

    REQUIRE (duskstudio::single_instance::acquire ("", receive));
    const std::string session = "\"C:\\Sessions\\Björk – take 2\\mix.dusksession\"";
    REQUIRE_FALSE (duskstudio::single_instance::acquire (session, noPayload));
    REQUIRE_FALSE (duskstudio::single_instance::acquire ("", noPayload));
    REQUIRE (delivered.waitFor (2));
    REQUIRE (delivered.copy() == std::vector<std::string> { session, "" });
}

TEST_CASE ("simultaneous single-instance launches elect exactly one owner",
           "[single-instance][issue-368]")
{
    ScopedSlot slot;
    Deliveries delivered;
    const auto receive = [&] (std::string payload) { delivered.accept (std::move (payload)); };
    constexpr int launchCount = 8;
    std::atomic<int> ready { 0 };
    std::atomic<bool> go { false };
    std::vector<int> primary (launchCount, 0);
    std::vector<std::string> payloads;
    std::vector<std::thread> launches;

    for (std::size_t i = 0; i < (std::size_t) launchCount; ++i)
        payloads.push_back ("launch-" + std::to_string (i));
    for (std::size_t i = 0; i < (std::size_t) launchCount; ++i)
        launches.emplace_back ([&, i]
        {
            ready.fetch_add (1);
            while (! go.load()) std::this_thread::yield();
            primary[i] = duskstudio::single_instance::acquire (payloads[i], receive) ? 1 : 0;
        });

    while (ready.load() != launchCount) std::this_thread::yield();
    go.store (true);
    for (auto& launch : launches) launch.join();

    REQUIRE (std::count (primary.begin(), primary.end(), 1) == 1);
    REQUIRE (delivered.waitFor (launchCount - 1));
    auto received = delivered.copy();
    REQUIRE (received.size() == launchCount - 1);

    const auto owner = (std::size_t) std::distance (
        primary.begin(), std::find (primary.begin(), primary.end(), 1));
    payloads.erase (payloads.begin() + (std::ptrdiff_t) owner);
    std::sort (payloads.begin(), payloads.end());
    std::sort (received.begin(), received.end());
    REQUIRE (received == payloads);
}

TEST_CASE ("single-instance release permits clean reacquisition",
           "[single-instance][issue-368]")
{
    ScopedSlot slot;
    REQUIRE (duskstudio::single_instance::acquire ("", noPayload));
    duskstudio::single_instance::release();
    REQUIRE (duskstudio::single_instance::acquire ("", noPayload));
}

#if ! defined (_WIN32)
TEST_CASE ("a handoff failure leaves the live primary socket alone",
           "[single-instance][socket][issue-368]")
{
    ScopedSlot slot;
    REQUIRE (duskstudio::single_instance::acquire ("", noPayload));
    const auto sock = soleSocketIn (slot.path());
    REQUIRE_FALSE (sock.empty());
    const auto primaryInode = inodeOf (sock);
    REQUIRE (primaryInode != 0);

    REQUIRE (::chmod (sock.c_str(), 0) == 0);
    const bool secondActsAsPrimary =
        duskstudio::single_instance::acquire ("/tmp/elsewhere/session.json", noPayload);
    REQUIRE (::chmod (sock.c_str(), 0755) == 0);

    REQUIRE (secondActsAsPrimary);
    REQUIRE (inodeOf (sock) == primaryInode);
}

TEST_CASE ("a refused handoff reclaims a dead primary socket",
           "[single-instance][socket][issue-368]")
{
    ScopedSlot slot;
    REQUIRE (duskstudio::single_instance::acquire ("", noPayload));
    const auto sock = soleSocketIn (slot.path());
    REQUIRE_FALSE (sock.empty());

    duskstudio::single_instance::release();
    REQUIRE (inodeOf (sock) == 0);

    const int staleFd = bindSocketAt (sock);
    const auto staleInode = inodeOf (sock);
    REQUIRE (staleInode != 0);

    REQUIRE (duskstudio::single_instance::acquire ("", noPayload));
    const auto reclaimedInode = inodeOf (sock);
    ::close (staleFd);
    REQUIRE (reclaimedInode != 0);
    REQUIRE (reclaimedInode != staleInode);
}

TEST_CASE ("release leaves a newer primary socket at the same path",
           "[single-instance][socket][issue-368]")
{
    ScopedSlot slot;
    REQUIRE (duskstudio::single_instance::acquire ("", noPayload));
    const auto sock = soleSocketIn (slot.path());
    REQUIRE_FALSE (sock.empty());

    ::unlink (sock.c_str());
    const int successorFd = bindSocketAt (sock);
    const auto successorInode = inodeOf (sock);
    REQUIRE (successorInode != 0);

    duskstudio::single_instance::release();
    REQUIRE (inodeOf (sock) == successorInode);

    ::close (successorFd);
    ::unlink (sock.c_str());
}
#endif

#if defined (_WIN32)
TEST_CASE ("single-instance crash owner helper", "[.single-instance-crash-helper]")
{
    const char* readyPath = std::getenv ("DUSKSTUDIO_SINGLE_INSTANCE_CRASH_READY");
    REQUIRE (readyPath != nullptr);
    duskstudio::single_instance::testing::setDispatcher (
        [] (std::function<void()> fn) { fn(); return true; });
    REQUIRE (duskstudio::single_instance::acquire ("", noPayload));

    HANDLE ready = ::CreateFileA (readyPath, GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE (ready != INVALID_HANDLE_VALUE);
    ::CloseHandle (ready);
    ::Sleep (INFINITE);
}

TEST_CASE ("Windows recovers ownership after the primary process crashes",
           "[single-instance][issue-368]")
{
    ScopedSlot slot;
    char tempPath[MAX_PATH] = {};
    REQUIRE (::GetTempPathA (MAX_PATH, tempPath) != 0);
    const std::string readyPath = std::string (tempPath) + "dusk-si-ready-"
                                + std::to_string ((unsigned long) ::GetCurrentProcessId());
    ::DeleteFileA (readyPath.c_str());
    _putenv_s ("DUSKSTUDIO_SINGLE_INSTANCE_CRASH_READY", readyPath.c_str());

    wchar_t executable[MAX_PATH] = {};
    REQUIRE (::GetModuleFileNameW (nullptr, executable, MAX_PATH) != 0);
    std::wstring command = L"\"" + std::wstring (executable)
                         + L"\" \"single-instance crash owner helper\" --reporter compact";
    std::vector<wchar_t> commandLine (command.begin(), command.end());
    commandLine.push_back (L'\0');

    STARTUPINFOW startup {};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION child {};
    REQUIRE (::CreateProcessW (nullptr, commandLine.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child));
    ::CloseHandle (child.hThread);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (10);
    while (::GetFileAttributesA (readyPath.c_str()) == INVALID_FILE_ATTRIBUTES
           && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (20));

    const bool ready = ::GetFileAttributesA (readyPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (! ready)
    {
        ::TerminateProcess (child.hProcess, 1);
        ::WaitForSingleObject (child.hProcess, 5000);
        ::CloseHandle (child.hProcess);
    }
    REQUIRE (ready);

    REQUIRE (::TerminateProcess (child.hProcess, 3));
    REQUIRE (::WaitForSingleObject (child.hProcess, 5000) == WAIT_OBJECT_0);
    ::CloseHandle (child.hProcess);
    REQUIRE (duskstudio::single_instance::acquire ("", noPayload));

    _putenv_s ("DUSKSTUDIO_SINGLE_INSTANCE_CRASH_READY", "");
    ::DeleteFileA (readyPath.c_str());
}
#endif
