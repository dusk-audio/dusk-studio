#include <catch2/catch_test_macros.hpp>

#include "util/SingleInstance.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace
{
namespace fs = std::filesystem;

// A private XDG_RUNTIME_DIR per case: the slot lives under it, so a real Dusk
// Studio on this desktop is neither seen nor disturbed. Restored on the way out
// (and the listener released) so a thrown REQUIRE still leaves the process as
// it found it.
class ScopedRuntimeDir
{
public:
    ScopedRuntimeDir()
    {
        if (const char* prev = std::getenv ("XDG_RUNTIME_DIR"))
        {
            previous = prev;
            hadPrevious = true;
        }

        // Terse name on purpose: the socket path built under here has to fit
        // sockaddr_un::sun_path, and acquire() quietly gives up when it doesn't,
        // which would surface as a socket-lifecycle failure rather than as a
        // too-long TMPDIR. Check the headroom before anything exists to clean up.
        std::string tmpl = (fs::temp_directory_path() / "dusk-si-XXXXXX").string();
        REQUIRE (tmpl.size() + std::strlen ("/dusk-studio/instance-0123456789abcdef.sock")
                   < sizeof (sockaddr_un::sun_path));
        REQUIRE (::mkdtemp (&tmpl[0]) != nullptr);
        dir = tmpl;
        ::setenv ("XDG_RUNTIME_DIR", dir.c_str(), 1);
    }

    ~ScopedRuntimeDir()
    {
        duskstudio::single_instance::release();
        if (hadPrevious) ::setenv ("XDG_RUNTIME_DIR", previous.c_str(), 1);
        else             ::unsetenv ("XDG_RUNTIME_DIR");
        std::error_code ec;
        fs::remove_all (dir, ec);
    }

    const fs::path& path() const noexcept { return dir; }

private:
    fs::path dir;
    std::string previous;
    bool hadPrevious = false;
};

// acquire() names the socket after a hash of the display, so the tests find the
// file the primary made rather than recomputing that hash.
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

// 0 when nothing is there, which is what the "was it removed?" assertions want.
ino_t inodeOf (const fs::path& p)
{
    struct stat st {};
    return ::lstat (p.c_str(), &st) == 0 ? st.st_ino : 0;
}

int bindSocketAt (const fs::path& p)
{
    const int fd = ::socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    REQUIRE (fd >= 0);

    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    const auto name = p.string();
    REQUIRE (name.size() < sizeof addr.sun_path);
    std::memcpy (addr.sun_path, name.c_str(), name.size());
    REQUIRE (::bind (fd, (const sockaddr*) &addr, sizeof addr) == 0);
    return fd;
}

void noPayload (std::string) {}
} // namespace

TEST_CASE ("a handoff that fails without a refusal leaves the primary's socket alone",
           "[single-instance][socket]")
{
    ScopedRuntimeDir runtime;

    REQUIRE (duskstudio::single_instance::acquire ("", noPayload));
    const auto sock = soleSocketIn (runtime.path());
    REQUIRE_FALSE (sock.empty());
    const auto primaryInode = inodeOf (sock);
    REQUIRE (primaryInode != 0);

    // The primary is listening and healthy; this launch cannot reach it.
    // Connecting to a mode-000 socket fails with EACCES, which says nothing
    // about whether the process behind it is still alive.
    REQUIRE (::chmod (sock.c_str(), 0) == 0);
    const bool secondActsAsPrimary =
        duskstudio::single_instance::acquire ("/tmp/elsewhere/session.json", noPayload);
    REQUIRE (::chmod (sock.c_str(), 0755) == 0);

    // A broken handshake must not block the launch...
    REQUIRE (secondActsAsPrimary);
    // ...but the live primary keeps the slot: clearing it here would put a
    // second engine on the same audio device.
    REQUIRE (inodeOf (sock) == primaryInode);
}

TEST_CASE ("a refused handoff still reclaims the socket a dead primary left behind",
           "[single-instance][socket]")
{
    ScopedRuntimeDir runtime;

    REQUIRE (duskstudio::single_instance::acquire ("", noPayload));
    const auto sock = soleSocketIn (runtime.path());
    REQUIRE_FALSE (sock.empty());

    duskstudio::single_instance::release();
    REQUIRE (inodeOf (sock) == 0);

    // What a primary killed with SIGKILL leaves at that path: a socket file
    // with nothing accepting on it, so connecting to it is refused. The fd
    // stays open across the acquire below - a bound socket that never listens
    // still refuses, and holding it pins the inode so the filesystem cannot
    // hand the same number back to the socket that replaces it.
    const int staleFd = bindSocketAt (sock);
    const auto staleInode = inodeOf (sock);
    REQUIRE (staleInode != 0);

    REQUIRE (duskstudio::single_instance::acquire ("", noPayload));
    const auto reclaimedInode = inodeOf (sock);
    ::close (staleFd);
    REQUIRE (reclaimedInode != 0);
    REQUIRE (reclaimedInode != staleInode);
}

TEST_CASE ("release leaves a socket a newer primary bound at the same path",
           "[single-instance][socket]")
{
    ScopedRuntimeDir runtime;

    REQUIRE (duskstudio::single_instance::acquire ("", noPayload));
    const auto sock = soleSocketIn (runtime.path());
    REQUIRE_FALSE (sock.empty());

    // Stand in for a successor that took the slot after this process lost it:
    // same name, different file.
    ::unlink (sock.c_str());
    const int successorFd = bindSocketAt (sock);
    const auto successorInode = inodeOf (sock);
    REQUIRE (successorInode != 0);

    duskstudio::single_instance::release();

    REQUIRE (inodeOf (sock) == successorInode);

    ::close (successorFd);
    ::unlink (sock.c_str());
}
