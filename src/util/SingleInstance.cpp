#include "SingleInstance.h"

#include "../foundation/MessageThread.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>

#if defined (__linux__)
 #include <cerrno>
 #include <fcntl.h>
 #include <poll.h>
 #include <sys/socket.h>
 #include <sys/stat.h>
 #include <sys/un.h>
 #include <unistd.h>
#endif

namespace duskstudio::single_instance
{
#if defined (__linux__)
namespace
{
constexpr std::size_t kMaxPayloadBytes = 64 * 1024;
constexpr int kReadBudgetMs = 2000;
constexpr int kDeliveryBudgetMs = 2000;
using Deadline = std::chrono::steady_clock::time_point;

struct Listener
{
    std::thread thread;
    std::atomic<int> listenFd { -1 };
    std::atomic<bool> unlinked { false };
    int wakeFds[2] = { -1, -1 };
    std::string socketPath;
    dev_t socketDev = 0;
    ino_t socketIno = 0;
    bool socketIdentified = false;
    std::function<void (std::string)> onCommandLine;
};

// Raw, never owned by a static destructor: a plugin that calls exit() on a
// license failure skips shutdown(), and destroying a still-joinable thread at
// static teardown is std::terminate. release() is the only path that frees it;
// an abnormal exit leaks it, which the process exit cleans up anyway.
Listener* listener = nullptr;

std::uint64_t hashOf (const char* s) noexcept
{
    std::uint64_t h = 1469598103934665603ull;
    for (; *s != '\0'; ++s)
    {
        h ^= (std::uint64_t) (unsigned char) *s;
        h *= 1099511628211ull;
    }
    return h;
}

// $XDG_RUNTIME_DIR/dusk-studio/instance-<display hash>.sock. The runtime dir is
// per-user and 0700, which is the permission bit an abstract-namespace socket
// does not have: without it any local process of any uid could bind the name
// first (every real launch then hands off to nobody and quits) or connect and
// feed the running instance a session of its choosing, whose plugin references
// get loaded. The display is hashed rather than embedded because it can be an
// absolute path, and a name long enough to truncate would collapse two
// displays onto one slot.
bool makeSocketPath (std::string& out)
{
    const char* runtimeDir = std::getenv ("XDG_RUNTIME_DIR");
    if (runtimeDir == nullptr || *runtimeDir != '/') return false;

    const std::string dir = std::string (runtimeDir) + "/dusk-studio";
    if (::mkdir (dir.c_str(), 0700) != 0 && errno != EEXIST) return false;

    struct stat st {};
    if (::lstat (dir.c_str(), &st) != 0) return false;
    if (! S_ISDIR (st.st_mode)
        || st.st_uid != ::getuid()
        || (st.st_mode & (S_IRWXG | S_IRWXO)) != 0)
        return false;

    const char* display = std::getenv ("WAYLAND_DISPLAY");
    if (display == nullptr || *display == '\0') display = std::getenv ("DISPLAY");
    if (display == nullptr) display = "";

    char name[40] = {};
    std::snprintf (name, sizeof name, "/instance-%016llx.sock",
                   (unsigned long long) hashOf (display));
    out = dir + name;
    return out.size() < sizeof (sockaddr_un::sun_path);
}

void makeAddress (const std::string& path, sockaddr_un& addr, socklen_t& len)
{
    std::memset (&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    std::memcpy (addr.sun_path, path.c_str(), path.size());
    len = (socklen_t) (offsetof (sockaddr_un, sun_path) + path.size() + 1);
}

// Closing the listening fd is what makes a later launch fall back to acting as
// primary; leaving it bound but unserviced would strand every launch after it.
// Both exchanges keep this safe to call from the listener thread and from
// release() without double-closing.
void teardown (Listener& l)
{
    const int fd = l.listenFd.exchange (-1);
    if (fd >= 0) ::close (fd);
    if (l.unlinked.exchange (true) || ! l.socketIdentified) return;

    // Remove only the file this process bound. A newer primary may already have
    // created its own socket at the same name, and unlinking that one would
    // leave it listening on a path no launch can reach. fstat on the listening
    // fd cannot settle the identity: it reports the sockfs inode, not the
    // directory entry bind() made, hence the dev+inode recorded from the path.
    struct stat named {};
    if (::lstat (l.socketPath.c_str(), &named) != 0) return;
    if (named.st_dev == l.socketDev && named.st_ino == l.socketIno)
        ::unlink (l.socketPath.c_str());
}

bool peerIsThisUser (int fd)
{
    ucred cred {};
    socklen_t len = sizeof cred;
    if (::getsockopt (fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return false;
    return len == sizeof cred && cred.uid == ::getuid();
}

int pollUntil (int fd, short events, const Deadline deadline)
{
    for (;;)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return 0;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds> (
            deadline - now);

        pollfd p {};
        p.fd = fd;
        p.events = events;
        const int ready = ::poll (&p, 1, std::max (1, (int) remaining.count()));
        if (ready > 0) return p.revents;
        if (ready == 0) return 0;
        if (errno != EINTR) return 0;
    }
}

bool writeAll (int fd, const std::string& payload, const Deadline deadline)
{
    std::size_t sent = 0;
    while (sent < payload.size())
    {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        const ssize_t n = ::send (fd, payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
        if (n > 0) { sent += (std::size_t) n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            const int revents = pollUntil (fd, POLLOUT, deadline);
            if ((revents & POLLOUT) != 0) continue;
        }
        return false;
    }
    return true;
}

// One deadline for the whole exchange, not per read: a peer dribbling a byte
// at a time under a per-read timeout would hold the listener open forever, and
// release() waits on this thread.
bool readPayload (int fd, int wakeFd, std::string& payload)
{
    const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds (kReadBudgetMs);
    char buf[1024];

    while (payload.size() < kMaxPayloadBytes)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds> (deadline - now);

        pollfd fds[2] {};
        fds[0].fd = fd;
        fds[0].events = POLLIN;
        fds[1].fd = wakeFd;
        fds[1].events = POLLIN;
        const int ready = ::poll (fds, 2, std::max (1, (int) remaining.count()));
        if (ready < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) break;
        if (fds[1].revents != 0) return false;
        if (fds[0].revents == 0) continue;

        const ssize_t got = ::read (fd, buf, sizeof buf);
        if (got > 0) { payload.append (buf, (std::size_t) got); continue; }
        if (got < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        break;
    }
    return true;
}

void listenLoop (Listener* l)
{
    for (;;)
    {
        const int fd = l->listenFd.load();
        if (fd < 0) return;

        pollfd fds[2] {};
        fds[0].fd = fd;
        fds[0].events = POLLIN;
        fds[1].fd = l->wakeFds[0];
        fds[1].events = POLLIN;

        if (::poll (fds, 2, -1) < 0)
        {
            if (errno == EINTR) continue;
            teardown (*l);
            return;
        }
        if (fds[1].revents != 0) return;                        // release() woke us
        if ((fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            teardown (*l);
            return;
        }
        if ((fds[0].revents & POLLIN) == 0) continue;

        const int peer = ::accept4 (fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (peer < 0)
        {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED)
                continue;
            // Out of descriptors and the like: the fd stays readable, so
            // continuing here would spin the thread at 100% CPU.
            teardown (*l);
            return;
        }

        if (peerIsThisUser (peer))
        {
            std::string payload;
            const bool completed = readPayload (peer, l->wakeFds[0], payload);
            ::close (peer);
            if (! completed) return;
            auto fn = l->onCommandLine;
            dusk::callAsync ([fn, payload] { fn (payload); });
        }
        else
        {
            ::close (peer);
        }
    }
}

bool startListener (int fd, const std::string& path,
                    std::function<void (std::string)> onCommandLine)
{
    if (::listen (fd, 8) != 0) return false;

    auto* l = new Listener();
    if (::pipe2 (l->wakeFds, O_CLOEXEC) != 0)
    {
        delete l;
        return false;
    }
    l->listenFd.store (fd);
    l->socketPath = path;
    struct stat bound {};
    if (::lstat (path.c_str(), &bound) == 0)
    {
        l->socketDev = bound.st_dev;
        l->socketIno = bound.st_ino;
        l->socketIdentified = true;
    }
    l->onCommandLine = std::move (onCommandLine);
    listener = l;
    l->thread = std::thread (listenLoop, l);
    return true;
}

// Refused is the only outcome that says the socket file outlived the process
// that made it. Failed covers a peer we could not reach for any other reason -
// no permission, a wedged primary, a backlog with no room - all of which leave
// a socket whose owner may still be running.
enum class Handoff { Delivered, Refused, Failed };

// A fresh socket per attempt: a connect that failed leaves the fd unusable for
// a second try. failureErrno carries what went wrong on Failed, for the log
// line that is the only trace a second unattached instance leaves.
Handoff handOver (const sockaddr_un& addr, socklen_t len, const std::string& payload,
                  int& failureErrno)
{
    failureErrno = 0;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const int fd = ::socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0) { failureErrno = errno; return Handoff::Failed; }

        const auto deadline = std::chrono::steady_clock::now()
                                + std::chrono::milliseconds (kDeliveryBudgetMs);
        bool connected = false;
        int connectErr = 0;
        if (::connect (fd, (const sockaddr*) &addr, len) == 0)
        {
            connected = true;
        }
        else
        {
            connectErr = errno;
            if (connectErr == EINPROGRESS || connectErr == EALREADY
                || connectErr == EINTR || connectErr == EAGAIN
                || connectErr == EWOULDBLOCK)
            {
                const int revents = pollUntil (fd, POLLOUT, deadline);
                if ((revents & POLLNVAL) == 0 && revents != 0)
                {
                    socklen_t errorLen = sizeof connectErr;
                    if (::getsockopt (fd, SOL_SOCKET, SO_ERROR,
                                      &connectErr, &errorLen) == 0)
                        connected = connectErr == 0;
                    else
                        connectErr = errno;
                }
            }
        }

        if (connected)
        {
            const bool sent = writeAll (fd, payload, deadline);
            if (sent) ::shutdown (fd, SHUT_WR);
            else      failureErrno = errno;   // before close(), which clobbers it
            ::close (fd);
            return sent ? Handoff::Delivered : Handoff::Failed;
        }
        ::close (fd);
        if (connectErr != ECONNREFUSED)
        {
            failureErrno = connectErr;
            return Handoff::Failed;
        }

        // A primary that has bound but not yet called listen() refuses
        // connections for a moment. Retry once before reading the refusal as
        // "the file outlived the process that made it".
        if (attempt == 0)
            std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }
    return Handoff::Refused;
}
} // namespace

bool acquire (const std::string& payload, std::function<void (std::string)> onCommandLine)
{
    std::string path;
    if (! makeSocketPath (path)) return true;

    sockaddr_un addr {};
    socklen_t len = 0;
    makeAddress (path, addr, len);

    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const int fd = ::socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return true;

        if (::bind (fd, (const sockaddr*) &addr, len) == 0)
        {
            if (! startListener (fd, path, std::move (onCommandLine)))
            {
                ::close (fd);
                ::unlink (path.c_str());
            }
            return true;
        }
        const int bindErr = errno;
        ::close (fd);
        if (bindErr != EADDRINUSE) return true;

        int handoffErrno = 0;
        const Handoff outcome = handOver (addr, len, payload, handoffErrno);
        if (outcome == Handoff::Delivered) return false;
        // Anything short of a refusal may be a primary we could not talk to.
        // Run unattached rather than clear its slot: unlinking here promotes
        // this launch to a second primary, and two of those contend for one
        // audio device and one session directory. Say so - an unattached
        // instance is otherwise indistinguishable from a normal launch.
        if (outcome != Handoff::Refused)
        {
            std::fprintf (stderr,
                          "[Dusk Studio/SingleInstance] could not reach the running instance "
                          "(%s) - starting without the single-instance slot; this window will "
                          "not receive sessions opened from the desktop\n",
                          std::strerror (handoffErrno));
            return true;
        }

        // Nothing is listening: a primary died without cleaning up. Drop the
        // file and take the slot on the next pass.
        if (::unlink (path.c_str()) != 0 && errno != ENOENT) return true;
    }
    return true;
}

void release()
{
    Listener* l = listener;
    if (l == nullptr) return;
    listener = nullptr;

    if (l->wakeFds[1] >= 0)
    {
        const char wake = 0;
        while (::write (l->wakeFds[1], &wake, 1) < 0 && errno == EINTR) {}
    }
    if (l->thread.joinable()) l->thread.join();

    teardown (*l);
    if (l->wakeFds[0] >= 0) ::close (l->wakeFds[0]);
    if (l->wakeFds[1] >= 0) ::close (l->wakeFds[1]);
    delete l;
}

#else

bool acquire (const std::string&, std::function<void (std::string)>) { return true; }
void release() {}

#endif
} // namespace duskstudio::single_instance
