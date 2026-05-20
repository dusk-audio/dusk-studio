#include "RemotePluginConnection.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace duskstudio::ipc
{
namespace
{
// memfd_create wrapper. glibc only added a symbol around 2.27; rather
// than depend on the glibc version, just hit the syscall directly.
inline int memfdCreate (const char* name, unsigned int flags) noexcept
{
    return (int) syscall (SYS_memfd_create, name, flags);
}

// Polite spin pause - reduces SMT contention with the child while we
// wait for replySeq. Per-arch fallbacks so non-x86 builds compile.
inline void cpuRelax() noexcept
{
   #if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
   #elif defined(__aarch64__) || defined(__arm__)
    asm volatile ("yield" ::: "memory");
   #else
    std::this_thread::yield();
   #endif
}

// Send the SHM fd to the child over the connected socketpair using
// SCM_RIGHTS ancillary data. Returns 0 on success, -1 on error.
int sendFd (int socket, int fd) noexcept
{
    char dummy = 'x';
    struct iovec iov { &dummy, 1 };

    char ctlBuf[CMSG_SPACE (sizeof (int))] {};
    struct msghdr msg {};
    msg.msg_iov     = &iov;
    msg.msg_iovlen  = 1;
    msg.msg_control = ctlBuf;
    msg.msg_controllen = sizeof (ctlBuf);

    struct cmsghdr* cm = CMSG_FIRSTHDR (&msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type  = SCM_RIGHTS;
    cm->cmsg_len   = CMSG_LEN (sizeof (int));
    std::memcpy (CMSG_DATA (cm), &fd, sizeof (fd));

    return sendmsg (socket, &msg, 0) >= 0 ? 0 : -1;
}
} // namespace

namespace
{
// Read exactly `n` bytes or fail. Returns true on full read, false on
// EOF / error / short read. socketFd should be in blocking mode (with a
// SO_RCVTIMEO if the caller wants a deadline).
bool readExact (int socketFd, void* buf, std::size_t n) noexcept
{
    auto* p = static_cast<char*> (buf);
    while (n > 0)
    {
        const ssize_t r = ::read (socketFd, p, n);
        if (r < 0)
        {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false;  // EOF
        p += r;
        n -= (std::size_t) r;
    }
    return true;
}

bool writeExact (int socketFd, const void* buf, std::size_t n) noexcept
{
    auto* p = static_cast<const char*> (buf);
    while (n > 0)
    {
        const ssize_t w = ::write (socketFd, p, n);
        if (w < 0)
        {
            if (errno == EINTR) continue;
            return false;
        }
        if (w == 0) return false;  // peer closed; avoid infinite loop
        p += w;
        n -= (std::size_t) w;
    }
    return true;
}

// Send a control request: [header][payload]. Returns true on success.
bool sendControl (int socketFd, duskstudio::ipc::OpCode op,
                   const void* payload, std::uint32_t payloadLen) noexcept
{
    using duskstudio::ipc::ControlMsgHeader;
    ControlMsgHeader hdr {};
    hdr.totalLen   = (std::uint32_t) sizeof (hdr) + payloadLen;
    hdr.op         = (std::uint32_t) op;
    hdr.status     = 0;
    hdr.payloadLen = payloadLen;
    if (! writeExact (socketFd, &hdr, sizeof (hdr))) return false;
    if (payloadLen > 0 && ! writeExact (socketFd, payload, payloadLen))
        return false;
    return true;
}

// Read a control reply. Allocates a vector of size payloadLen; for
// fixed-size replies the caller can ignore the vector and rely on the
// status field. Returns the parsed header.
bool recvControl (int socketFd,
                   duskstudio::ipc::ControlMsgHeader& hdrOut,
                   std::vector<std::uint8_t>& payloadOut) noexcept
{
    if (! readExact (socketFd, &hdrOut, sizeof (hdrOut))) return false;
    payloadOut.resize (hdrOut.payloadLen);
    if (hdrOut.payloadLen > 0
        && ! readExact (socketFd, payloadOut.data(), hdrOut.payloadLen))
        return false;
    return true;
}
} // namespace

RemotePluginConnection::~RemotePluginConnection()
{
    disconnect();
}

bool RemotePluginConnection::connect (const std::string& hostExecutablePath,
                                        const std::string& extraArg,
                                        std::string& errorOut)
{
    if (childPid > 0)
        return true;  // already connected

    // 1) Create the SHM region. memfd_create gives us an anonymous
    // file-backed mapping that survives across fork+exec when we pass
    // the fd via SCM_RIGHTS (or simpler: F_CLOEXEC is OFF by default
    // for memfd_create, so the fd is inherited - but the child needs to
    // know which fd number, hence SCM_RIGHTS with a known protocol).
    shmFd = memfdCreate ("dusk-studio-plugin-shm", 0 /* default: not cloexec */);
    if (shmFd < 0)
    {
        errorOut = std::string ("memfd_create failed: ") + std::strerror (errno);
        return false;
    }

    if (ftruncate (shmFd, kTotalSize) < 0)
    {
        errorOut = std::string ("ftruncate failed: ") + std::strerror (errno);
        disconnect();
        return false;
    }

    mappedShm = mmap (nullptr, kTotalSize, PROT_READ | PROT_WRITE,
                       MAP_SHARED, shmFd, 0);
    if (mappedShm == MAP_FAILED)
    {
        mappedShm = nullptr;
        errorOut  = std::string ("mmap failed: ") + std::strerror (errno);
        disconnect();
        return false;
    }

    // 2) Initialise the header. The child checks the magic before
    // touching anything.
    auto* hdr = headerOf (mappedShm);
    new (hdr) BlockHeader();   // placement-new to initialise atomics
    hdr->magic   = kMagic;
    hdr->version = kVersion;
    hdr->cmdSeq.store (0, std::memory_order_relaxed);
    hdr->replySeq.store (0, std::memory_order_relaxed);
    hdr->state.store (kStateReady, std::memory_order_relaxed);

    // 3) Create the socketpair used to hand the SHM fd to the child.
    int sv[2] {};
    if (socketpair (AF_UNIX, SOCK_STREAM, 0, sv) < 0)
    {
        errorOut = std::string ("socketpair failed: ") + std::strerror (errno);
        disconnect();
        return false;
    }

    // 4) Fork + exec. Child execs dusk-studio-plugin-host with the socketpair
    // end as fd 3 plus an --ipc-stub flag to enable Phase-1 echo mode.
    childPid = fork();
    if (childPid < 0)
    {
        errorOut = std::string ("fork failed: ") + std::strerror (errno);
        ::close (sv[0]);
        ::close (sv[1]);
        disconnect();
        return false;
    }

    if (childPid == 0)
    {
        // --- child --------------------------------------------------
        ::close (sv[0]);                // child uses sv[1]
        // Move sv[1] to fd 3 so the child finds it at a known fd.
        if (sv[1] != 3)
        {
            dup2 (sv[1], 3);
            ::close (sv[1]);
        }

        // Best-effort: kill the child if the parent dies.
        prctl (PR_SET_PDEATHSIG, SIGTERM);

        const char* argv[] = { hostExecutablePath.c_str(), extraArg.c_str(), nullptr };
        execv (hostExecutablePath.c_str(), const_cast<char* const*> (argv));
        // exec failed.
        std::fprintf (stderr, "[dusk-studio-plugin-host] execv failed: %s\n",
                      std::strerror (errno));
        _exit (127);
    }

    // --- parent ---------------------------------------------------------
    ::close (sv[1]);                    // parent uses sv[0]
    socketFd = sv[0];

    // Send the SHM fd to the child.
    if (sendFd (socketFd, shmFd) < 0)
    {
        errorOut = std::string ("sendFd failed: ") + std::strerror (errno);
        disconnect();
        return false;
    }

    // Wait for the child to confirm "ready" by writing a single byte
    // back. Bounded so a broken host can't hang us forever.
    char ack = 0;
    struct timeval to { 5, 0 };
    setsockopt (socketFd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof (to));
    if (::read (socketFd, &ack, 1) != 1 || ack != 'k')
    {
        errorOut = "child did not send ready handshake";
        disconnect();
        return false;
    }

    return true;
}

bool RemotePluginConnection::processBlockSync (const float* const* inChannels,
                                                 int numIn, int numSamples,
                                                 juce::MidiBuffer& midi,
                                                 long long timeoutNs) noexcept
{
    if (mappedShm == nullptr || crashed.load (std::memory_order_acquire))
        return false;

    if (numSamples <= 0 || numSamples > kMaxBlock) return false;
    if (numIn     <  0 || numIn      > kMaxChans) return false;

    auto* hdr = headerOf (mappedShm);

    // Copy input audio to SHM. memcpy only - no allocation.
    for (int c = 0; c < numIn; ++c)
    {
        if (inChannels[c] != nullptr)
            std::memcpy (audioInChannel (mappedShm, c), inChannels[c],
                         (std::size_t) numSamples * sizeof (float));
    }

    // Serialise input MIDI to SHM. Wire format mirrors PluginHostMain's
    // child-side reader: each event is native-endian
    //   [int sample(4)][uint16 len(2)][bytes(len)]
    // Events that don't fit get dropped — same behaviour as the child's
    // serialiser when a plugin emits a flood of events.
    {
        std::uint8_t* out = midiIn (mappedShm);
        std::uint32_t written = 0;
        for (const auto meta : midi)
        {
            const auto m = meta.getMessage();
            const int len = m.getRawDataSize();
            if (len <= 0) continue;
            if (written + 4 + 2 + (std::uint32_t) len > kMidiBytes) break;
            const int sample = meta.samplePosition;
            std::memcpy (out + written, &sample, 4);             written += 4;
            const std::uint16_t l16 = (std::uint16_t) len;
            std::memcpy (out + written, &l16, 2);                written += 2;
            std::memcpy (out + written, m.getRawData(),
                         (std::size_t) len);                      written += (std::uint32_t) len;
        }
        hdr->midiInBytes = written;
    }

    hdr->numSamples  = (std::uint32_t) numSamples;
    hdr->numInChans  = (std::uint32_t) numIn;
    hdr->numOutChans = (std::uint32_t) numIn;   // stub: same as input
    hdr->midiOutBytes = 0;

    // Bump cmdSeq + wake the child.
    const std::uint32_t mySeq = ++localSeq;
    hdr->cmdSeq.store (mySeq, std::memory_order_release);
    futexWakeOne (&hdr->cmdSeq);

    // Replace caller's MIDI buffer with whatever the plugin emitted.
    // Mirror of the child's serialiser; parses the same wire format.
    // RT-safe assuming `midi` was pre-sized at engine prepare-time so
    // addEvent reuses existing capacity.
    auto deserialiseMidiOut = [&]
    {
        midi.clear();
        const std::uint32_t midiOutBytes = hdr->midiOutBytes;
        if (midiOutBytes == 0 || midiOutBytes > kMidiBytes) return;
        const std::uint8_t* base = midiOut (mappedShm);
        std::uint32_t off = 0;
        std::uint8_t evBuf[256];
        while (off + 6 <= midiOutBytes)
        {
            int sample = 0;
            std::memcpy (&sample, base + off, 4); off += 4;
            std::uint16_t l16 = 0;
            std::memcpy (&l16, base + off, 2); off += 2;
            const int eventLen = (int) l16;
            if (eventLen <= 0 || eventLen > (int) sizeof (evBuf)) break;
            if (off + (std::uint32_t) eventLen > midiOutBytes)   break;
            std::memcpy (evBuf, base + off, (std::size_t) eventLen);
            off += (std::uint32_t) eventLen;
            midi.addEvent (juce::MidiMessage (evBuf, eventLen), sample);
        }
    };

    // Bounded spin first - typical block finishes in a few microseconds,
    // and a syscall pair (~600 ns) per block is wasteful at 64-sample
    // buffers. Then fall through to a futex-wait with absolute timeout.
    constexpr int kSpinIters = 2000;
    for (int i = 0; i < kSpinIters; ++i)
    {
        if (hdr->replySeq.load (std::memory_order_acquire) == mySeq)
        {
            deserialiseMidiOut();
            roundTrips.fetch_add (1, std::memory_order_relaxed);
            return true;
        }
        cpuRelax();
    }

    auto deadline = absTimeoutFromNow (timeoutNs);
    while (hdr->replySeq.load (std::memory_order_acquire) != mySeq)
    {
        // Wait while replySeq is still its old value (mySeq - 1).
        long r = futexWaitOnce (&hdr->replySeq, mySeq - 1, &deadline);
        if (r == -1 && errno == ETIMEDOUT)
        {
            crashed.store (true, std::memory_order_release);
            return false;
        }
        // EINTR / EAGAIN: re-check the seq and either return success or
        // retry the wait. EAGAIN means replySeq already advanced past
        // (mySeq - 1) before we entered the kernel - common; just retry.
        if (r == -1 && errno != EINTR && errno != EAGAIN)
        {
            crashed.store (true, std::memory_order_release);
            return false;
        }
    }

    deserialiseMidiOut();
    roundTrips.fetch_add (1, std::memory_order_relaxed);
    return true;
}

// --- Control-plane RPCs --------------------------------------------------

bool RemotePluginConnection::ping (int timeoutMs, std::string& errorOut)
{
    if (socketFd < 0) { errorOut = "not connected"; return false; }

    struct timeval to { timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
    setsockopt (socketFd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof (to));

    if (! sendControl (socketFd, OpCode::Ping, nullptr, 0))
    {
        errorOut = std::string ("Ping write failed: ") + std::strerror (errno);
        return false;
    }
    ControlMsgHeader hdr {};
    std::vector<std::uint8_t> payload;
    if (! recvControl (socketFd, hdr, payload))
    {
        errorOut = std::string ("Ping read failed: ") + std::strerror (errno);
        return false;
    }
    if (hdr.status != 0)
    {
        errorOut = "Ping reply status != 0";
        return false;
    }
    return true;
}

bool RemotePluginConnection::loadPlugin (const std::string& pluginDescriptionXml,
                                          double sampleRate, int blockSize,
                                          int& numInOut, int& numOutOut,
                                          int& latencyOut, std::string& errorOut)
{
    if (socketFd < 0) { errorOut = "not connected"; return false; }

    // Payload: [PrepareToPlayPayload][xml bytes].
    PrepareToPlayPayload header {};
    header.sampleRate = sampleRate;
    header.blockSize  = (std::int32_t) blockSize;
    header.reserved   = 0;

    std::vector<std::uint8_t> payload;
    payload.resize (sizeof (header) + pluginDescriptionXml.size());
    std::memcpy (payload.data(), &header, sizeof (header));
    std::memcpy (payload.data() + sizeof (header),
                  pluginDescriptionXml.data(), pluginDescriptionXml.size());

    // 30 s timeout - plugin instantiation can be slow on first scan.
    struct timeval to { 30, 0 };
    setsockopt (socketFd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof (to));

    if (! sendControl (socketFd, OpCode::LoadPlugin,
                        payload.data(), (std::uint32_t) payload.size()))
    {
        errorOut = std::string ("LoadPlugin write failed: ") + std::strerror (errno);
        return false;
    }

    ControlMsgHeader hdr {};
    std::vector<std::uint8_t> reply;
    if (! recvControl (socketFd, hdr, reply))
    {
        errorOut = std::string ("LoadPlugin read failed: ") + std::strerror (errno);
        return false;
    }
    if (hdr.status != 0)
    {
        errorOut = reply.empty()
                     ? "LoadPlugin failed (no message)"
                     : std::string (reinterpret_cast<const char*> (reply.data()),
                                      reply.size());
        return false;
    }
    if (reply.size() < sizeof (LoadPluginReply))
    {
        errorOut = "LoadPlugin reply too small";
        return false;
    }
    LoadPluginReply r {};
    std::memcpy (&r, reply.data(), sizeof (r));
    numInOut    = r.numInChans;
    numOutOut   = r.numOutChans;
    latencyOut  = r.latencySamples;
    return true;
}

bool RemotePluginConnection::prepareToPlay (double sampleRate, int blockSize,
                                              std::string& errorOut)
{
    if (socketFd < 0) { errorOut = "not connected"; return false; }
    PrepareToPlayPayload p {};
    p.sampleRate = sampleRate;
    p.blockSize  = (std::int32_t) blockSize;
    if (! sendControl (socketFd, OpCode::PrepareToPlay, &p, sizeof (p)))
    {
        errorOut = std::string ("PrepareToPlay write failed: ") + std::strerror (errno);
        return false;
    }
    ControlMsgHeader hdr {};
    std::vector<std::uint8_t> reply;
    if (! recvControl (socketFd, hdr, reply))
    {
        errorOut = std::string ("PrepareToPlay read failed: ") + std::strerror (errno);
        return false;
    }
    if (hdr.status != 0) { errorOut = "PrepareToPlay status != 0"; return false; }
    return true;
}

bool RemotePluginConnection::release (std::string& errorOut)
{
    if (socketFd < 0) { errorOut = "not connected"; return false; }
    if (! sendControl (socketFd, OpCode::Release, nullptr, 0))
    {
        errorOut = std::string ("Release write failed: ") + std::strerror (errno);
        return false;
    }
    ControlMsgHeader hdr {};
    std::vector<std::uint8_t> reply;
    if (! recvControl (socketFd, hdr, reply))
    {
        errorOut = std::string ("Release read failed: ") + std::strerror (errno);
        return false;
    }
    if (hdr.status != 0) { errorOut = "Release status != 0"; return false; }
    return true;
}

bool RemotePluginConnection::getState (std::vector<std::uint8_t>& blobOut,
                                         std::string& errorOut)
{
    if (socketFd < 0) { errorOut = "not connected"; return false; }
    if (! sendControl (socketFd, OpCode::GetState, nullptr, 0))
    {
        errorOut = std::string ("GetState write failed: ") + std::strerror (errno);
        return false;
    }
    ControlMsgHeader hdr {};
    std::vector<std::uint8_t> reply;
    if (! recvControl (socketFd, hdr, reply))
    {
        errorOut = std::string ("GetState read failed: ") + std::strerror (errno);
        return false;
    }
    if (hdr.status != 0) { errorOut = "GetState status != 0"; return false; }
    if (reply.size() != sizeof (std::uint32_t))
    {
        errorOut = "GetState reply expected uint32 size";
        return false;
    }
    std::uint32_t blobSize = 0;
    std::memcpy (&blobSize, reply.data(), sizeof (blobSize));
    if (blobSize > kStateBytes) { errorOut = "state blob exceeds staging area"; return false; }
    blobOut.assign (
        reinterpret_cast<const std::uint8_t*> (mappedShm) + kStateOffset,
        reinterpret_cast<const std::uint8_t*> (mappedShm) + kStateOffset + blobSize);
    return true;
}

bool RemotePluginConnection::setState (const std::uint8_t* data, std::size_t size,
                                         std::string& errorOut)
{
    if (socketFd < 0) { errorOut = "not connected"; return false; }
    if (size > kStateBytes)
    {
        errorOut = "state blob exceeds staging area";
        return false;
    }
    std::memcpy (static_cast<char*> (mappedShm) + kStateOffset, data, size);
    const std::uint32_t sz = (std::uint32_t) size;
    if (! sendControl (socketFd, OpCode::SetState, &sz, sizeof (sz)))
    {
        errorOut = std::string ("SetState write failed: ") + std::strerror (errno);
        return false;
    }
    ControlMsgHeader hdr {};
    std::vector<std::uint8_t> reply;
    if (! recvControl (socketFd, hdr, reply))
    {
        errorOut = std::string ("SetState read failed: ") + std::strerror (errno);
        return false;
    }
    if (hdr.status != 0) { errorOut = "SetState status != 0"; return false; }
    return true;
}

bool RemotePluginConnection::showEditor (std::uint64_t& windowIdOut,
                                            int& widthOut, int& heightOut,
                                            std::string& errorOut)
{
    windowIdOut = 0; widthOut = 0; heightOut = 0;
    if (socketFd < 0) { errorOut = "not connected"; return false; }
    if (! sendControl (socketFd, OpCode::ShowEditor, nullptr, 0))
    {
        errorOut = std::string ("ShowEditor write failed: ") + std::strerror (errno);
        return false;
    }
    ControlMsgHeader hdr {};
    std::vector<std::uint8_t> reply;
    if (! recvControl (socketFd, hdr, reply))
    {
        errorOut = std::string ("ShowEditor read failed: ") + std::strerror (errno);
        return false;
    }
    if (hdr.status != 0) { errorOut = "ShowEditor status != 0"; return false; }
    if (reply.size() != sizeof (ShowEditorReply))
    {
        errorOut = "ShowEditor reply size mismatch";
        return false;
    }
    ShowEditorReply r {};
    std::memcpy (&r, reply.data(), sizeof (r));
    windowIdOut = r.windowId;
    widthOut    = (int) r.width;
    heightOut   = (int) r.height;
    return true;
}

bool RemotePluginConnection::hideEditor (std::string& errorOut)
{
    if (socketFd < 0) { errorOut = "not connected"; return false; }
    if (! sendControl (socketFd, OpCode::HideEditor, nullptr, 0))
    {
        errorOut = std::string ("HideEditor write failed: ") + std::strerror (errno);
        return false;
    }
    ControlMsgHeader hdr {};
    std::vector<std::uint8_t> reply;
    if (! recvControl (socketFd, hdr, reply))
    {
        errorOut = std::string ("HideEditor read failed: ") + std::strerror (errno);
        return false;
    }
    if (hdr.status != 0) { errorOut = "HideEditor status != 0"; return false; }
    return true;
}

bool RemotePluginConnection::resizeEditor (int width, int height, std::string& errorOut)
{
    if (socketFd < 0) { errorOut = "not connected"; return false; }
    ResizeEditorPayload p {};
    p.width  = (std::int32_t) width;
    p.height = (std::int32_t) height;
    if (! sendControl (socketFd, OpCode::ResizeEditor, &p, sizeof (p)))
    {
        errorOut = std::string ("ResizeEditor write failed: ") + std::strerror (errno);
        return false;
    }
    ControlMsgHeader hdr {};
    std::vector<std::uint8_t> reply;
    if (! recvControl (socketFd, hdr, reply))
    {
        errorOut = std::string ("ResizeEditor read failed: ") + std::strerror (errno);
        return false;
    }
    if (hdr.status != 0) { errorOut = "ResizeEditor status != 0"; return false; }
    return true;
}

bool RemotePluginConnection::pollReaper() noexcept
{
    if (childPid <= 0) return false;
    int status = 0;
    const pid_t r = waitpid (childPid, &status, WNOHANG);
    if (r == 0)         return false;            // still alive
    if (r < 0)          return false;            // ECHILD / error - already reaped elsewhere
    // r == childPid: child has exited.
    childPid = -1;
    crashed.store (true, std::memory_order_release);
    return true;
}

void RemotePluginConnection::disconnect()
{
    if (childPid > 0)
    {
        if (auto* hdr = mappedShm != nullptr ? headerOf (mappedShm) : nullptr)
        {
            hdr->state.store (kStateTeardown, std::memory_order_release);
            // Wake the child so it observes the teardown state and
            // exits its wait loop cleanly.
            futexWakeOne (&hdr->cmdSeq);
        }
        ::kill (childPid, SIGTERM);
        // Brief grace period for clean shutdown, then SIGKILL.
        for (int i = 0; i < 50; ++i)
        {
            int status = 0;
            pid_t r = waitpid (childPid, &status, WNOHANG);
            if (r == childPid) { childPid = -1; break; }
            usleep (10000);  // 10ms
        }
        if (childPid > 0)
        {
            ::kill (childPid, SIGKILL);
            int status = 0;
            waitpid (childPid, &status, 0);
            childPid = -1;
        }
    }
    if (socketFd >= 0) { ::close (socketFd); socketFd = -1; }
    if (mappedShm != nullptr)
    {
        munmap (mappedShm, kTotalSize);
        mappedShm = nullptr;
    }
    if (shmFd >= 0) { ::close (shmFd); shmFd = -1; }
}

} // namespace duskstudio::ipc
