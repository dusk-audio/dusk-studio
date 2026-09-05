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
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined (__linux__) || defined (__APPLE__)
 #include <cerrno>
 #include <fcntl.h>
 #include <poll.h>
 #include <sys/file.h>
 #include <sys/socket.h>
 #include <sys/stat.h>
 #include <sys/un.h>
 #include <unistd.h>
#elif defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <aclapi.h>
#endif

namespace duskstudio::single_instance
{
namespace
{
#if defined (DUSKSTUDIO_SINGLE_INSTANCE_TESTING)
testing::Dispatcher testDispatcher;
#if defined (_WIN32)
std::atomic<bool> dropNextAcknowledgementForTest { false };
#endif
#endif

bool dispatch (std::function<void()> fn)
{
#if defined (DUSKSTUDIO_SINGLE_INSTANCE_TESTING)
    if (testDispatcher) return testDispatcher (std::move (fn));
#endif
    return dusk::callAsync (std::move (fn));
}
} // namespace

#if defined (__linux__) || defined (__APPLE__)
namespace
{
constexpr std::size_t kMaxPayloadBytes = 64 * 1024;
constexpr int kReadBudgetMs = 2000;
constexpr int kDeliveryBudgetMs = 2000;
constexpr int kRefusalRetryMs = 250;
constexpr int kSlotLockBudgetMs = 250;
constexpr int kProbeBudgetMs = 250;
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

#if defined (__linux__)
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
#endif

// Linux uses one slot per display under XDG_RUNTIME_DIR. macOS uses one slot
// per login user under its private temporary directory. In both cases the 0700
// directory is the permission boundary an abstract socket would lack: without
// it another uid could bind the name first or feed the running instance a
// session of its choosing, whose plugin references would then be loaded.
// Returns nullptr once out holds the slot path, or why the gate cannot be
// keyed, which the caller reports rather than starting a silent second copy.
const char* makeSocketPath (std::string& out)
{
    std::string runtimeDir;
#if defined (__linux__)
    const char* fromEnvironment = std::getenv ("XDG_RUNTIME_DIR");
    if (fromEnvironment == nullptr || *fromEnvironment != '/')
        return "XDG_RUNTIME_DIR is unset or not an absolute path";
    runtimeDir = fromEnvironment;
#else
    // TMPDIR is per-process and mutable, so a Finder launch and a shell launch
    // of the same user can carry different values and claim different slots.
    // The confstr directory is the one private per-user path both agree on.
 #if defined (DUSKSTUDIO_SINGLE_INSTANCE_TESTING)
    if (const char* fromEnvironment = std::getenv ("TMPDIR");
        fromEnvironment != nullptr && *fromEnvironment == '/')
        runtimeDir = fromEnvironment;
 #endif
    if (runtimeDir.empty())
    {
        char buffer[1024] = {};
        const std::size_t size = ::confstr (_CS_DARWIN_USER_TEMP_DIR, buffer, sizeof buffer);
        if (size == 0 || size > sizeof buffer || buffer[0] != '/')
            return "the per-user temporary directory is unavailable";
        runtimeDir = buffer;
    }
    while (runtimeDir.size() > 1 && runtimeDir.back() == '/') runtimeDir.pop_back();
#endif

    const std::string dir = runtimeDir + "/dusk-studio";
    if (::mkdir (dir.c_str(), 0700) != 0 && errno != EEXIST)
        return "the runtime directory could not be created";

    struct stat st {};
    if (::lstat (dir.c_str(), &st) != 0)
        return "the runtime directory could not be inspected";
    if (! S_ISDIR (st.st_mode)
        || st.st_uid != ::getuid()
        || (st.st_mode & (S_IRWXG | S_IRWXO)) != 0)
        return "the runtime directory is not a private directory owned by this user";

    char name[40] = {};
#if defined (__linux__)
    const char* display = std::getenv ("WAYLAND_DISPLAY");
    if (display == nullptr || *display == '\0') display = std::getenv ("DISPLAY");
    if (display == nullptr) display = "";
    std::snprintf (name, sizeof name, "/instance-%016llx.sock",
                   (unsigned long long) hashOf (display));
#else
    std::snprintf (name, sizeof name, "/instance.sock");
#endif
    out = dir + name;
    if (out.size() >= sizeof (sockaddr_un::sun_path))
        return "the runtime directory path is too long for a Unix socket";
    return nullptr;
}

void makeAddress (const std::string& path, sockaddr_un& addr, socklen_t& len)
{
    std::memset (&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    std::memcpy (addr.sun_path, path.c_str(), path.size());
    len = (socklen_t) (offsetof (sockaddr_un, sun_path) + path.size() + 1);
}

#if defined (__APPLE__)
bool configureFd (int fd, bool nonBlocking)
{
    const int descriptorFlags = ::fcntl (fd, F_GETFD);
    if (descriptorFlags < 0 || ::fcntl (fd, F_SETFD, descriptorFlags | FD_CLOEXEC) != 0)
        return false;

    if (nonBlocking)
    {
        const int statusFlags = ::fcntl (fd, F_GETFL);
        if (statusFlags < 0 || ::fcntl (fd, F_SETFL, statusFlags | O_NONBLOCK) != 0)
            return false;
    }
    return true;
}
#endif

int openSocket (bool nonBlocking)
{
#if defined (__linux__)
    int type = SOCK_STREAM | SOCK_CLOEXEC;
    if (nonBlocking) type |= SOCK_NONBLOCK;
    return ::socket (AF_UNIX, type, 0);
#else
    const int fd = ::socket (AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (! configureFd (fd, nonBlocking))
    {
        ::close (fd);
        return -1;
    }
    const int noSigPipe = 1;
    if (::setsockopt (fd, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof noSigPipe) != 0)
    {
        ::close (fd);
        return -1;
    }
    return fd;
#endif
}

bool makeWakePipe (int (&fds)[2])
{
#if defined (__linux__)
    return ::pipe2 (fds, O_CLOEXEC) == 0;
#else
    if (::pipe (fds) != 0) return false;
    if (configureFd (fds[0], false) && configureFd (fds[1], false)) return true;
    ::close (fds[0]);
    ::close (fds[1]);
    fds[0] = fds[1] = -1;
    return false;
#endif
}

int acceptPeer (int fd)
{
#if defined (__linux__)
    return ::accept4 (fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
#else
    const int peer = ::accept (fd, nullptr, nullptr);
    if (peer < 0) return -1;
    if (configureFd (peer, true)) return peer;
    ::close (peer);
    return -1;
#endif
}

// The invariant every launch relies on: the directory entry at the slot path is
// created, made listenable, and removed only under this lock. A stat before the
// unlink cannot stand in for it, because the entry can be replaced between the
// stat and the unlink - two launches racing after a crash would then each drop
// what the other just bound, and both would run as primary over one session
// directory. Holding it across bind and listen is what lets a refusal seen
// under the lock be read as "the owner is gone" rather than "not listening
// yet". Failing to take it is never fatal: the caller leaves the entry alone.
class SlotLock
{
public:
    explicit SlotLock (const std::string& socketPath)
    {
        fd = ::open ((socketPath + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) return;

        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds (kSlotLockBudgetMs);
        for (;;)
        {
            if (::flock (fd, LOCK_EX | LOCK_NB) == 0) return;
            if (errno != EWOULDBLOCK && errno != EINTR) break;
            if (std::chrono::steady_clock::now() >= deadline) break;
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }
        ::close (fd);
        fd = -1;
    }

    ~SlotLock()
    {
        if (fd < 0) return;
        ::flock (fd, LOCK_UN);
        ::close (fd);
    }

    SlotLock (const SlotLock&) = delete;
    SlotLock& operator= (const SlotLock&) = delete;

    bool isHeld() const noexcept { return fd >= 0; }

private:
    int fd = -1;
};

// Closing the listening fd is what makes a later launch fall back to acting as
// primary; leaving it bound but unserviced would strand every launch after it.
// Both exchanges keep this safe to call from the listener thread and from
// release() without double-closing.
void teardown (Listener& l)
{
    const int fd = l.listenFd.exchange (-1);
    if (fd >= 0) ::close (fd);
    if (l.unlinked.exchange (true) || ! l.socketIdentified) return;

    const SlotLock lock (l.socketPath);
    if (! lock.isHeld()) return;

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
#if defined (__linux__)
    ucred cred {};
    socklen_t len = sizeof cred;
    if (::getsockopt (fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return false;
    return len == sizeof cred && cred.uid == ::getuid();
#else
    uid_t effectiveUid = 0;
    gid_t effectiveGid = 0;
    return ::getpeereid (fd, &effectiveUid, &effectiveGid) == 0
        && effectiveUid == ::geteuid();
#endif
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
#if defined (__linux__)
    constexpr int sendFlags = MSG_NOSIGNAL;
#else
    constexpr int sendFlags = 0; // SO_NOSIGPIPE is installed by openSocket().
#endif
    std::size_t sent = 0;
    while (sent < payload.size())
    {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        const ssize_t n = ::send (fd, payload.data() + sent, payload.size() - sent, sendFlags);
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

enum class Receive { Complete, Dropped, Stopping };

// One deadline for the whole exchange, not per read: a peer dribbling a byte
// at a time under a per-read timeout would hold the listener open forever, and
// release() waits on this thread.
Receive readExactly (int fd, int wakeFd, void* bytes, std::size_t size, const Deadline deadline)
{
    auto* cursor = static_cast<unsigned char*> (bytes);
    std::size_t received = 0;

    while (received < size)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return Receive::Dropped;
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
            return Receive::Dropped;
        }
        if (ready == 0) return Receive::Dropped;
        if (fds[1].revents != 0) return Receive::Stopping;
        if (fds[0].revents == 0) continue;

        const ssize_t got = ::read (fd, cursor + received, size - received);
        if (got > 0) { received += (std::size_t) got; continue; }
        if (got < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        return Receive::Dropped;
    }
    return Receive::Complete;
}

// The length prefix mirrors the Windows pipe framing. Without it a sender that
// stalls or dies mid-path delivers a prefix of an absolute path, which can
// still name a directory the primary would then open as a session.
Receive readPayload (int fd, int wakeFd, std::string& payload)
{
    const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds (kReadBudgetMs);

    std::uint32_t payloadSize = 0;
    const Receive header = readExactly (fd, wakeFd, &payloadSize, sizeof payloadSize, deadline);
    if (header != Receive::Complete) return header;
    if (payloadSize > kMaxPayloadBytes) return Receive::Dropped;
    if (payloadSize == 0) return Receive::Complete;

    payload.assign (payloadSize, '\0');
    const Receive body = readExactly (fd, wakeFd, &payload[0], payload.size(), deadline);
    if (body != Receive::Complete) payload.clear();
    return body;
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

        const int peer = acceptPeer (fd);
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
            const Receive outcome = readPayload (peer, l->wakeFds[0], payload);
            ::close (peer);
            if (outcome == Receive::Stopping) return;
            if (outcome == Receive::Complete)
            {
                auto fn = l->onCommandLine;
                dispatch ([fn, payload] { fn (payload); });
            }
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
    if (! makeWakePipe (l->wakeFds))
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

// What the slot path holds, as seen from inside the lock, where the answer
// cannot change under us: Orphaned is the only state that permits an unlink.
enum class Slot { Vacant, Live, Orphaned, Unknown };

Slot probeSlot (const sockaddr_un& addr, socklen_t len)
{
    const int fd = openSocket (true);
    if (fd < 0) return Slot::Unknown;

    int result = 0;
    if (::connect (fd, (const sockaddr*) &addr, len) != 0)
    {
        result = errno;
        if (result == EINPROGRESS || result == EALREADY || result == EINTR
            || result == EAGAIN || result == EWOULDBLOCK)
        {
            const auto deadline = std::chrono::steady_clock::now()
                                + std::chrono::milliseconds (kProbeBudgetMs);
            const int revents = pollUntil (fd, POLLOUT, deadline);
            result = ETIMEDOUT;
            if ((revents & POLLNVAL) == 0 && revents != 0)
            {
                socklen_t errorLen = sizeof result;
                if (::getsockopt (fd, SOL_SOCKET, SO_ERROR, &result, &errorLen) != 0)
                    result = errno;
            }
        }
    }
    ::close (fd);

    if (result == 0) return Slot::Live;
    if (result == ECONNREFUSED) return Slot::Orphaned;
    if (result == ENOENT) return Slot::Vacant;
    return Slot::Unknown;
}

// An unattached instance is otherwise indistinguishable from a normal launch
// until a session opened from the desktop lands in a window that is not the one
// in front of the user, so every path that gives up the slot says why.
constexpr char kUnattachedTail[] =
    " - starting without the single-instance slot; this window will not receive "
    "sessions opened from the desktop\n";

void reportUnattached (const char* what, int failure)
{
    std::fprintf (stderr, "[Dusk Studio/SingleInstance] %s (%s)%s",
                  what, std::strerror (failure), kUnattachedTail);
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
    if (payload.size() > kMaxPayloadBytes)
    {
        failureErrno = EMSGSIZE;
        return Handoff::Failed;
    }

    const std::uint32_t payloadSize = (std::uint32_t) payload.size();
    std::string framed (sizeof payloadSize + payload.size(), '\0');
    std::memcpy (&framed[0], &payloadSize, sizeof payloadSize);
    std::memcpy (&framed[sizeof payloadSize], payload.data(), payload.size());

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds (kDeliveryBudgetMs);
    const auto refusalDeadline = std::chrono::steady_clock::now()
                               + std::chrono::milliseconds (kRefusalRetryMs);
    for (;;)
    {
        const int fd = openSocket (true);
        if (fd < 0) { failureErrno = errno; return Handoff::Failed; }

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
            const bool sent = writeAll (fd, framed, deadline);
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

        // A simultaneous primary may have bound but not reached listen() yet.
        // Retry refusals only long enough to cover the bind-to-listen race.
        // A dead primary should not add the full delivery timeout to startup.
        if (std::chrono::steady_clock::now() >= refusalDeadline) break;
        std::this_thread::sleep_for (std::chrono::milliseconds (25));
    }
    return Handoff::Refused;
}
} // namespace

bool acquire (const std::string& payload, std::function<void (std::string)> onCommandLine)
{
    std::string path;
    if (const char* reason = makeSocketPath (path))
    {
        std::fprintf (stderr, "[Dusk Studio/SingleInstance] %s%s", reason, kUnattachedTail);
        return true;
    }

    sockaddr_un addr {};
    socklen_t len = 0;
    makeAddress (path, addr, len);

    int handoffErrno = 0;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        {
            const SlotLock lock (path);
            if (lock.isHeld())
            {
                // A refusal seen here is final: a live primary completed
                // listen() before it released the lock, and nothing can create
                // or remove the entry while we hold it.
                const Slot state = probeSlot (addr, len);
                if (state == Slot::Orphaned && ::unlink (path.c_str()) != 0 && errno != ENOENT)
                {
                    reportUnattached ("the socket a crashed instance left behind could not "
                                      "be removed", errno);
                    return true;
                }

                if (state != Slot::Live)
                {
                    const int fd = openSocket (false);
                    if (fd < 0)
                    {
                        reportUnattached ("a Unix socket could not be created", errno);
                        return true;
                    }

                    if (::bind (fd, (const sockaddr*) &addr, len) == 0)
                    {
                        if (! startListener (fd, path, std::move (onCommandLine)))
                        {
                            const int failure = errno;
                            ::close (fd);
                            ::unlink (path.c_str());
                            reportUnattached ("the single-instance listener could not start",
                                              failure);
                        }
                        return true;
                    }
                    ::close (fd);
                }
            }
        }

        const Handoff outcome = handOver (addr, len, payload, handoffErrno);
        if (outcome == Handoff::Delivered) return false;
        // Anything short of a refusal may be a primary we could not talk to.
        // Run unattached rather than clear its slot: unlinking here promotes
        // this launch to a second primary, and two of those contend for one
        // audio device and one session directory.
        if (outcome != Handoff::Refused) break;
        handoffErrno = ECONNREFUSED;
    }

    reportUnattached ("could not reach the running instance", handoffErrno);
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

#elif defined (_WIN32)

namespace
{
constexpr std::size_t kMaxPayloadBytes = 64 * 1024;
constexpr DWORD kDeliveryBudgetMs = 2000;
using Deadline = std::chrono::steady_clock::time_point;

struct Security
{
    std::vector<unsigned char> tokenUser;
    PSID userSid = nullptr;
    PACL acl = nullptr;
    SECURITY_DESCRIPTOR descriptor {};
    SECURITY_ATTRIBUTES attributes {};

    ~Security()
    {
        if (acl != nullptr) ::LocalFree (acl);
    }

    bool initialise()
    {
        HANDLE token = nullptr;
        if (! ::OpenProcessToken (::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

        DWORD bytes = 0;
        ::GetTokenInformation (token, TokenUser, nullptr, 0, &bytes);
        if (bytes == 0)
        {
            ::CloseHandle (token);
            return false;
        }

        tokenUser.resize (bytes);
        const bool gotUser = ::GetTokenInformation (
            token, TokenUser, tokenUser.data(), bytes, &bytes) != FALSE;
        ::CloseHandle (token);
        if (! gotUser) return false;

        userSid = reinterpret_cast<TOKEN_USER*> (tokenUser.data())->User.Sid;
        if (! ::IsValidSid (userSid)) return false;

        EXPLICIT_ACCESSW access {};
        access.grfAccessPermissions = GENERIC_ALL;
        access.grfAccessMode = SET_ACCESS;
        access.grfInheritance = NO_INHERITANCE;
        ::BuildTrusteeWithSidW (&access.Trustee, userSid);
        if (::SetEntriesInAclW (1, &access, nullptr, &acl) != ERROR_SUCCESS) return false;

        if (! ::InitializeSecurityDescriptor (&descriptor, SECURITY_DESCRIPTOR_REVISION)
            || ! ::SetSecurityDescriptorDacl (&descriptor, TRUE, acl, FALSE))
            return false;

        attributes.nLength = sizeof attributes;
        attributes.lpSecurityDescriptor = &descriptor;
        attributes.bInheritHandle = FALSE;
        return true;
    }
};

std::uint64_t hashBytes (const void* bytes, std::size_t size,
                         std::uint64_t h = 1469598103934665603ull) noexcept
{
    const auto* p = static_cast<const unsigned char*> (bytes);
    for (std::size_t i = 0; i < size; ++i)
    {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// The mutex lives in the per-session Local namespace and a named pipe has only
// the machine-global one, so the shared key carries the Terminal Services
// session that scopes both. Keyed differently, a second RDP or fast-user-switch
// session finds the mutex free, fails to create a pipe the first session
// already owns, and runs as a primary no launch can reach.
constexpr wchar_t kSlotNamespace[] = L"Local\\";

bool makeNames (PSID sid, std::wstring& mutexName, std::wstring& pipeName)
{
    DWORD sessionId = 0;
    if (! ::ProcessIdToSessionId (::GetCurrentProcessId(), &sessionId)) return false;

    std::uint64_t hash = hashBytes (sid, ::GetLengthSid (sid));
    hash = hashBytes (&sessionId, sizeof sessionId, hash);
#if defined (DUSKSTUDIO_SINGLE_INSTANCE_TESTING)
    if (const char* suffix = std::getenv ("DUSKSTUDIO_TEST_SINGLE_INSTANCE_SLOT"))
        hash = hashBytes (suffix, std::strlen (suffix), hash);
#endif
    const std::wstring key = std::to_wstring ((unsigned long long) hash);
    mutexName = kSlotNamespace + std::wstring (L"DuskStudio.SingleInstance.") + key;
    pipeName = L"\\\\.\\pipe\\DuskStudio.SingleInstance." + key;
    return true;
}

struct Listener
{
    Security security;
    std::thread thread;
    HANDLE slotMutex = nullptr;
    HANDLE stopEvent = nullptr;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::wstring pipeName;
    std::function<void (std::string)> onCommandLine;

    ~Listener()
    {
        if (pipe != INVALID_HANDLE_VALUE) ::CloseHandle (pipe);
        if (stopEvent != nullptr) ::CloseHandle (stopEvent);
        if (slotMutex != nullptr) ::CloseHandle (slotMutex);
    }
};

Listener* listener = nullptr;

DWORD remainingMillis (Deadline deadline)
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return 0;
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds> (
        deadline - now).count();
    return (DWORD) std::max<std::int64_t> (1, milliseconds);
}

void cancelAndDrain (HANDLE handle, OVERLAPPED& operation)
{
    ::CancelIoEx (handle, &operation);
    DWORD ignored = 0;
    ::GetOverlappedResult (handle, &operation, &ignored, TRUE);
}

bool transferExact (HANDLE pipe, void* bytes, std::size_t size, bool writing,
                    HANDLE stopEvent, Deadline deadline)
{
    auto* cursor = static_cast<unsigned char*> (bytes);
    std::size_t transferred = 0;
    while (transferred < size)
    {
        const DWORD remaining = remainingMillis (deadline);
        if (remaining == 0) return false;

        HANDLE event = ::CreateEventW (nullptr, TRUE, FALSE, nullptr);
        if (event == nullptr) return false;
        OVERLAPPED operation {};
        operation.hEvent = event;

        DWORD completed = 0;
        const DWORD chunk = (DWORD) std::min<std::size_t> (size - transferred, MAXDWORD);
        const BOOL started = writing
            ? ::WriteFile (pipe, cursor + transferred, chunk, &completed, &operation)
            : ::ReadFile (pipe, cursor + transferred, chunk, &completed, &operation);

        bool ok = started != FALSE;
        if (! ok && ::GetLastError() == ERROR_IO_PENDING)
        {
            HANDLE events[2] = { event, stopEvent };
            const DWORD count = stopEvent != nullptr ? 2u : 1u;
            const DWORD wait = ::WaitForMultipleObjects (count, events, FALSE, remaining);
            if (wait == WAIT_OBJECT_0)
                ok = ::GetOverlappedResult (pipe, &operation, &completed, FALSE) != FALSE;
            else
                cancelAndDrain (pipe, operation);
        }

        ::CloseHandle (event);
        if (! ok || completed == 0) return false;
        transferred += completed;
    }
    return true;
}

bool processIsThisUser (ULONG processId, PSID expectedSid)
{
    HANDLE process = ::OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) return false;
    HANDLE token = nullptr;
    const bool openedToken = ::OpenProcessToken (process, TOKEN_QUERY, &token) != FALSE;
    ::CloseHandle (process);
    if (! openedToken) return false;

    DWORD bytes = 0;
    ::GetTokenInformation (token, TokenUser, nullptr, 0, &bytes);
    std::vector<unsigned char> tokenUser (bytes);
    const bool gotUser = bytes != 0
        && ::GetTokenInformation (token, TokenUser, tokenUser.data(), bytes, &bytes) != FALSE;
    ::CloseHandle (token);
    if (! gotUser) return false;

    const auto sid = reinterpret_cast<TOKEN_USER*> (tokenUser.data())->User.Sid;
    return ::IsValidSid (sid) && ::EqualSid (expectedSid, sid) != FALSE;
}

bool clientIsThisUser (HANDLE pipe, PSID expectedSid)
{
    ULONG processId = 0;
    return ::GetNamedPipeClientProcessId (pipe, &processId) != FALSE
        && processIsThisUser (processId, expectedSid);
}

// The pipe name is a SID hash in the machine-global namespace, so another local
// account can create it first and collect the absolute session path of every
// launch this user makes. Nothing goes out until the process on the other end
// is this user; SECURITY_IDENTIFICATION only stops it impersonating us back.
bool serverIsThisUser (HANDLE pipe, PSID expectedSid, ULONG& serverProcessId)
{
    serverProcessId = 0;
    return ::GetNamedPipeServerProcessId (pipe, &serverProcessId) != FALSE
        && processIsThisUser (serverProcessId, expectedSid);
}

HANDLE createPipe (Listener& l)
{
    return ::CreateNamedPipeW (
        l.pipeName.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1,
        (DWORD) kMaxPayloadBytes + sizeof (std::uint32_t),
        (DWORD) kMaxPayloadBytes + sizeof (std::uint32_t),
        0,
        &l.security.attributes);
}

bool awaitConnection (Listener& l)
{
    HANDLE event = ::CreateEventW (nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) return false;
    OVERLAPPED operation {};
    operation.hEvent = event;

    bool connected = ::ConnectNamedPipe (l.pipe, &operation) != FALSE;
    if (! connected)
    {
        const DWORD error = ::GetLastError();
        if (error == ERROR_PIPE_CONNECTED)
        {
            connected = true;
        }
        else if (error == ERROR_IO_PENDING)
        {
            HANDLE events[2] = { event, l.stopEvent };
            const DWORD wait = ::WaitForMultipleObjects (2, events, FALSE, INFINITE);
            if (wait == WAIT_OBJECT_0)
            {
                DWORD ignored = 0;
                connected = ::GetOverlappedResult (
                    l.pipe, &operation, &ignored, FALSE) != FALSE;
            }
            else
            {
                cancelAndDrain (l.pipe, operation);
            }
        }
    }

    ::CloseHandle (event);
    return connected;
}

void serveConnection (Listener& l)
{
    if (! clientIsThisUser (l.pipe, l.security.userSid)) return;

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds (kDeliveryBudgetMs);
    std::uint32_t payloadSize = 0;
    if (! transferExact (l.pipe, &payloadSize, sizeof payloadSize, false,
                         l.stopEvent, deadline)
        || payloadSize > kMaxPayloadBytes)
        return;

    std::string payload (payloadSize, '\0');
    if (payloadSize != 0
        && ! transferExact (l.pipe, &payload[0], payload.size(), false,
                            l.stopEvent, deadline))
        return;

    auto fn = l.onCommandLine;
    unsigned char accepted = dispatch ([fn, payload] { fn (payload); }) ? 1u : 0u;
#if defined (DUSKSTUDIO_SINGLE_INSTANCE_TESTING)
    if (accepted != 0 && dropNextAcknowledgementForTest.exchange (false)) return;
#endif
    if (! transferExact (l.pipe, &accepted, sizeof accepted, true,
                         l.stopEvent, deadline))
        return;

    // Wait for the client to consume the acknowledgement before disconnecting;
    // DisconnectNamedPipe discards unread buffered data. This read shares the
    // handoff deadline and is cancelled by release(), so it cannot block join.
    unsigned char confirmed = 0;
    transferExact (l.pipe, &confirmed, sizeof confirmed, false,
                   l.stopEvent, deadline);
}

void listenLoop (Listener* l)
{
    while (::WaitForSingleObject (l->stopEvent, 0) != WAIT_OBJECT_0)
    {
        if (l->pipe == INVALID_HANDLE_VALUE) l->pipe = createPipe (*l);
        if (l->pipe == INVALID_HANDLE_VALUE) return;

        if (awaitConnection (*l)) serveConnection (*l);
        ::DisconnectNamedPipe (l->pipe);
        ::CloseHandle (l->pipe);
        l->pipe = INVALID_HANDLE_VALUE;
    }
}

bool startListener (std::unique_ptr<Listener> candidate,
                    std::function<void (std::string)> onCommandLine,
                    DWORD& failureError)
{
    candidate->stopEvent = ::CreateEventW (nullptr, TRUE, FALSE, nullptr);
    if (candidate->stopEvent != nullptr)
    {
        candidate->pipe = createPipe (*candidate);
        if (candidate->pipe != INVALID_HANDLE_VALUE)
        {
            candidate->onCommandLine = std::move (onCommandLine);
            listener = candidate.release();
            listener->thread = std::thread (listenLoop, listener);
            return true;
        }
        failureError = ::GetLastError();
        ::CloseHandle (candidate->stopEvent);
        candidate->stopEvent = nullptr;
    }
    else
    {
        failureError = ::GetLastError();
    }

    // Keep the mutex even though nothing is listening, so release() is what
    // finally drops it. Handing it back here would let the next launch in this
    // session repeat the same failure and call itself primary too, and windows
    // no handoff can reach would pile up without a word.
    listener = candidate.release();
    return false;
}

enum class Handoff { Delivered, Submitted, Foreign, Failed };

Handoff handOver (const std::wstring& pipeName, PSID expectedSid,
                  const std::string& payload, DWORD& failureError)
{
    failureError = ERROR_SUCCESS;
    if (payload.size() > kMaxPayloadBytes)
    {
        failureError = ERROR_BUFFER_OVERFLOW;
        return Handoff::Failed;
    }

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds (kDeliveryBudgetMs);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    while (remainingMillis (deadline) != 0)
    {
        pipe = ::CreateFileW (
            pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
            nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;

        failureError = ::GetLastError();
        if (failureError != ERROR_FILE_NOT_FOUND && failureError != ERROR_PIPE_BUSY)
            return Handoff::Failed;
        ::WaitNamedPipeW (pipeName.c_str(), std::min<DWORD> (50, remainingMillis (deadline)));
    }
    if (pipe == INVALID_HANDLE_VALUE) return Handoff::Failed;

    ULONG primaryProcessId = 0;
    if (! serverIsThisUser (pipe, expectedSid, primaryProcessId))
    {
        ::CloseHandle (pipe);
        return Handoff::Foreign;
    }
    if (primaryProcessId != 0) ::AllowSetForegroundWindow (primaryProcessId);

    std::uint32_t payloadSize = (std::uint32_t) payload.size();
    bool delivered = transferExact (pipe, &payloadSize,
                                    sizeof payloadSize, true, nullptr, deadline);
    if (delivered && payloadSize != 0)
        delivered = transferExact (pipe, const_cast<char*> (payload.data()), payload.size(),
                                   true, nullptr, deadline);

    const bool submitted = delivered;
    unsigned char accepted = 0;
    if (submitted)
        delivered = transferExact (pipe, &accepted, sizeof accepted, false, nullptr, deadline);

    if (delivered)
    {
        unsigned char confirmed = 1;
        transferExact (pipe, &confirmed, sizeof confirmed, true, nullptr, deadline);
    }

    if (! delivered) failureError = ::GetLastError();
    ::CloseHandle (pipe);
    if (delivered) return accepted == 1u ? Handoff::Delivered : Handoff::Failed;
    return submitted ? Handoff::Submitted : Handoff::Failed;
}
} // namespace

bool acquire (const std::string& payload, std::function<void (std::string)> onCommandLine)
{
    bool submitted = false;
    DWORD handoffError = ERROR_SUCCESS;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        auto candidate = std::make_unique<Listener>();
        if (! candidate->security.initialise()) return submitted ? false : true;

        std::wstring mutexName;
        if (! makeNames (candidate->security.userSid, mutexName, candidate->pipeName))
            return submitted ? false : true;

        ::SetLastError (ERROR_SUCCESS);
        candidate->slotMutex = ::CreateMutexW (
            &candidate->security.attributes, FALSE, mutexName.c_str());
        if (candidate->slotMutex == nullptr) return submitted ? false : true;

        if (::GetLastError() != ERROR_ALREADY_EXISTS)
        {
            DWORD listenerError = ERROR_SUCCESS;
            if (! startListener (std::move (candidate), std::move (onCommandLine), listenerError))
                std::fprintf (stderr,
                              "[Dusk Studio/SingleInstance] the single-instance listener could "
                              "not start (Windows error %lu); this window will not receive "
                              "sessions opened from the desktop\n",
                              (unsigned long) listenerError);
            return true;
        }

        if (attempt != 0)
        {
            ::CloseHandle (candidate->slotMutex);
            candidate->slotMutex = nullptr;
            if (submitted) return false;

            std::fprintf (stderr,
                          "[Dusk Studio/SingleInstance] could not reach the running instance "
                          "(Windows error %lu) - starting without the single-instance slot; "
                          "this window will not receive sessions opened from the desktop\n",
                          (unsigned long) handoffError);
            return true;
        }

        const Handoff outcome = handOver (candidate->pipeName, candidate->security.userSid,
                                          payload, handoffError);
        ::CloseHandle (candidate->slotMutex);
        candidate->slotMutex = nullptr;
        if (outcome == Handoff::Delivered) return false;

        // The name is held by a process that is not this user. Sending it the
        // session path is what the check exists to prevent, and quitting would
        // hand a stranger the power to close every launch, so this one runs
        // unattached instead.
        if (outcome == Handoff::Foreign)
        {
            std::fprintf (stderr,
                          "[Dusk Studio/SingleInstance] the single-instance pipe belongs to "
                          "another user's process - starting without the single-instance slot; "
                          "this window will not receive sessions opened from the desktop\n");
            return true;
        }
        submitted = outcome == Handoff::Submitted;

        // Closing our handle lets the kernel remove a slot whose primary died
        // during handoff. A second CreateMutex distinguishes that stale owner
        // from a live but unreachable primary without replaying a request
        // whose acknowledgement may have been lost after dispatch.
    }
    return true;
}

void release()
{
    Listener* l = listener;
    if (l == nullptr) return;
    listener = nullptr;

    if (l->stopEvent != nullptr) ::SetEvent (l->stopEvent);
    if (l->thread.joinable()) l->thread.join();

    delete l;
}

#else

bool acquire (const std::string&, std::function<void (std::string)>) { return true; }
void release() {}

#endif

#if defined (DUSKSTUDIO_SINGLE_INSTANCE_TESTING)
void testing::setDispatcher (Dispatcher dispatcher)
{
    testDispatcher = std::move (dispatcher);
}

#if defined (_WIN32)
void testing::dropNextAcknowledgement()
{
    dropNextAcknowledgementForTest.store (true);
}
#endif
#endif
} // namespace duskstudio::single_instance
