#include "IpcChannel.h"

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace duskstudio::ipc::platform
{

void closeHandle (NativeHandle& h) noexcept
{
    if (h.fd >= 0)
    {
        ::close (h.fd);
        h.fd = -1;
    }
}

bool createChannelPair (ChannelPair& out, std::string& errorOut) noexcept
{
    int sv[2] {};
    if (::socketpair (AF_UNIX, SOCK_STREAM, 0, sv) < 0)
    {
        errorOut = std::string ("socketpair failed: ") + std::strerror (errno);
        return false;
    }
    out.parentEnd.fd = sv[0];
    out.childEnd.fd  = sv[1];
    return true;
}

bool moveHandleToFd (NativeHandle& h, int targetFd) noexcept
{
    if (h.fd < 0) return false;
    if (h.fd == targetFd) return true;
    if (::dup2 (h.fd, targetFd) < 0) return false;
    ::close (h.fd);
    h.fd = targetFd;
    return true;
}

bool readExact (NativeHandle& h, void* buf, std::size_t n) noexcept
{
    auto* p = static_cast<char*> (buf);
    while (n > 0)
    {
        const ssize_t r = ::read (h.fd, p, n);
        if (r < 0)
        {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false;
        p += r;
        n -= (std::size_t) r;
    }
    return true;
}

bool writeExact (NativeHandle& h, const void* buf, std::size_t n) noexcept
{
    auto* p = static_cast<const char*> (buf);
    while (n > 0)
    {
        const ssize_t w = ::write (h.fd, p, n);
        if (w < 0)
        {
            if (errno == EINTR) continue;
            return false;
        }
        if (w == 0) return false;
        p += w;
        n -= (std::size_t) w;
    }
    return true;
}

bool setReadTimeout (NativeHandle& h, int ms) noexcept
{
    if (h.fd < 0) return false;
    struct timeval to { ms / 1000, (ms % 1000) * 1000 };
    return ::setsockopt (h.fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof (to)) == 0;
}

bool sendHandle (NativeHandle& channel, const NativeHandle& payload) noexcept
{
    char dummy = 'x';
    struct iovec iov { &dummy, 1 };

    char ctlBuf[CMSG_SPACE (sizeof (int))] {};
    struct msghdr msg {};
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = ctlBuf;
    msg.msg_controllen = sizeof (ctlBuf);

    struct cmsghdr* cm = CMSG_FIRSTHDR (&msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type  = SCM_RIGHTS;
    cm->cmsg_len   = CMSG_LEN (sizeof (int));
    std::memcpy (CMSG_DATA (cm), &payload.fd, sizeof (payload.fd));

    return ::sendmsg (channel.fd, &msg, 0) >= 0;
}

bool recvHandle (NativeHandle& channel, NativeHandle& payloadOut) noexcept
{
    payloadOut.fd = -1;

    char dummy = 0;
    struct iovec iov { &dummy, 1 };

    char ctlBuf[CMSG_SPACE (sizeof (int))] {};
    struct msghdr msg {};
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = ctlBuf;
    msg.msg_controllen = sizeof (ctlBuf);

    if (::recvmsg (channel.fd, &msg, 0) < 0) return false;

    for (struct cmsghdr* cm = CMSG_FIRSTHDR (&msg);
         cm != nullptr;
         cm = CMSG_NXTHDR (&msg, cm))
    {
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS)
        {
            int fd = -1;
            std::memcpy (&fd, CMSG_DATA (cm), sizeof (fd));
            payloadOut.fd = fd;
            return fd >= 0;
        }
    }
    return false;
}

} // namespace duskstudio::ipc::platform
