#include "RemotePluginConnection.h"

#include "platform/IpcSync.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
 #define WIN32_LEAN_AND_MEAN
 #define NOMINMAX
 #include <windows.h>
#else
 #include <sys/socket.h>
#endif

#include <juce_events/juce_events.h>

namespace duskstudio::ipc
{
namespace
{
// 3c-4 Windows-wake helper. Cancels any in-flight blocking I/O on the
// control channel so the reader thread's readExact returns immediately
// rather than waiting for kernel-level cancellation on disconnect.
//
// On Windows the previous shape relied on closing the HANDLE while the
// reader was mid-ReadFile, which behaves "implementation-defined"
// (ReadFile can return INVALID_HANDLE_VALUE / ERROR_INVALID_HANDLE but
// some pipe configurations sit blocked until the kernel-side IRP is
// torn down). CancelIoEx is the documented way: it cancels pending
// operations on the calling thread's behalf without closing the handle,
// so the close that follows is benign.
//
// On Linux + macOS, shutdown(fd, SHUT_RD) wakes the blocked read with
// EOF semantics — the standard portable way to unblock a thread sitting
// on read(socketpair_fd, ...). Safe to call before the actual close.
//
// Idempotent: invalid handle → no-op success.
void wakeReaderForShutdown (platform::NativeHandle& ch) noexcept
{
   #if defined(_WIN32)
    if (ch.h == nullptr || ch.h == reinterpret_cast<void*> (-1)) return;
    // CancelIoEx returns FALSE on Win 7+ if no pending I/O — that's not
    // an error condition for our use case, so the return value is
    // ignored. The handle pointer doubles as both SOCKET and HANDLE on
    // Windows; either type is valid for CancelIoEx.
    (void) ::CancelIoEx (ch.h, nullptr);
   #else
    if (ch.fd < 0) return;
    // SHUT_RD: future reads return 0 (EOF). The reader's readExact
    // returns false on the next iteration. EBADF / ENOTSOCK from a
    // non-socket fd is harmless — disconnect immediately proceeds to
    // closeHandle. Return value intentionally unused.
    (void) ::shutdown (ch.fd, SHUT_RD);
   #endif
}


// Send a control request: [header][payload]. Returns true on success.
// Caller (sendAndAwaitReply / setRemoteParam) holds controlMutex so
// concurrent senders can't interleave bytes on the socket.
bool sendControl (platform::NativeHandle& ch, OpCode op,
                    const void* payload, std::uint32_t payloadLen) noexcept
{
    ControlMsgHeader hdr {};
    hdr.totalLen   = (std::uint32_t) sizeof (hdr) + payloadLen;
    hdr.op         = (std::uint32_t) op;
    hdr.status     = 0;
    hdr.payloadLen = payloadLen;
    if (! platform::writeExact (ch, &hdr, sizeof (hdr))) return false;
    if (payloadLen > 0 && ! platform::writeExact (ch, payload, payloadLen))
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
    if (child.isAlive())
        return true;

    if (! shm.createAnonymous ("dusk-studio-plugin-shm", kTotalSize, errorOut))
        return false;

    auto* hdr = headerOf (shm.data());
    new (hdr) BlockHeader();
    hdr->magic   = kMagic;
    hdr->version = kVersion;
    hdr->cmdSeq.store (0, std::memory_order_relaxed);
    hdr->replySeq.store (0, std::memory_order_relaxed);
    hdr->state.store (kStateReady, std::memory_order_relaxed);

    platform::ChannelPair pair;
    if (! platform::createChannelPair (pair, errorOut))
    {
        disconnect();
        return false;
    }

    std::vector<std::string> args { extraArg };
    if (! child.spawn (hostExecutablePath, args, pair.childEnd, errorOut))
    {
        platform::closeHandle (pair.parentEnd);
        platform::closeHandle (pair.childEnd);
        disconnect();
        return false;
    }

    controlChannel = pair.parentEnd;

    if (! platform::sendHandle (controlChannel, shm.handle()))
    {
        errorOut = std::string ("sendHandle failed: ") + std::strerror (errno);
        disconnect();
        return false;
    }

    platform::setReadTimeout (controlChannel, 5000);
    char ack = 0;
    if (! platform::readExact (controlChannel, &ack, 1) || ack != 'k')
    {
        errorOut = "child did not send ready handshake";
        disconnect();
        return false;
    }
    // Disable the kernel-level timeout now that the handshake is past.
    // Sync RPCs use replyCv.wait_until for per-call timeouts; reader
    // thread must be able to block indefinitely on an idle socket.
    platform::setReadTimeout (controlChannel, 0);

    // --- ipc-host only: start the demultiplexing reader thread -------
    // The --ipc-stub child never writes a control frame after 'k', so
    // a reader thread there would block forever on readExact. The
    // self-test connects with extraArg=="--ipc-stub" and would
    // otherwise leak a permanently-blocked thread per connection.
    if (extraArg == "--ipc-host")
        startReaderThread();

    return true;
}

bool RemotePluginConnection::processBlockSync (const float* const* inChannels,
                                                 int numIn, int numOut, int numSamples,
                                                 juce::MidiBuffer& midi,
                                                 long long timeoutNs) noexcept
{
    if (! shm.isMapped() || crashed.load (std::memory_order_acquire))
        return false;

    if (numSamples <= 0 || numSamples > kMaxBlock) return false;
    if (numIn     <  0 || numIn      > kMaxChans) return false;
    if (numOut    <  0 || numOut     > kMaxChans) return false;

    auto* hdr = headerOf (shm.data());

    for (int c = 0; c < numIn; ++c)
    {
        if (inChannels[c] != nullptr)
            std::memcpy (audioInChannel (shm.data(), c), inChannels[c],
                          (std::size_t) numSamples * sizeof (float));
    }

    {
        std::uint8_t* out = midiIn (shm.data());
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
    hdr->numOutChans = (std::uint32_t) numOut;
    hdr->midiOutBytes = 0;

    const std::uint32_t mySeq = ++localSeq;
    hdr->cmdSeq.store (mySeq, std::memory_order_release);
    platform::wakeOneAddress (&hdr->cmdSeq);

    auto deserialiseMidiOut = [&]
    {
        midi.clear();
        const std::uint32_t midiOutBytes = hdr->midiOutBytes;
        if (midiOutBytes == 0 || midiOutBytes > kMidiBytes) return;
        const std::uint8_t* base = midiOut (shm.data());
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

    constexpr int kSpinIters = 2000;
    for (int i = 0; i < kSpinIters; ++i)
    {
        if (hdr->replySeq.load (std::memory_order_acquire) == mySeq)
        {
            deserialiseMidiOut();
            roundTrips.fetch_add (1, std::memory_order_relaxed);
            return true;
        }
        platform::cpuRelax();
    }

    const auto deadline = platform::deadlineFromNow (timeoutNs);
    for (;;)
    {
        const auto seen = hdr->replySeq.load (std::memory_order_acquire);
        if (seen == mySeq) break;
        // Fast crash detection: the child publishes kStateCrashed
        // when its handler trips before getting a chance to bump
        // replySeq. Without this check the parent would wait the
        // full futex deadline before noticing.
        if (hdr->state.load (std::memory_order_acquire) == kStateCrashed)
        {
            crashed.store (true, std::memory_order_release);
            return false;
        }
        // Pass the CURRENT replySeq as the "expected" value for the
        // futex. After a prior call's timeout, replySeq can be stuck
        // at an older value (e.g. mySeq-2); a hardcoded `mySeq-1`
        // would mismatch and cause waitOnAddress to spin-return
        // ValueChanged until the deadline. Reading the live value
        // makes the wait correctly block until the child actually
        // bumps replySeq.
        const auto r = platform::waitOnAddress (&hdr->replySeq, seen, &deadline);
        if (r == platform::WaitResult::Timeout)
        {
            crashed.store (true, std::memory_order_release);
            return false;
        }
        if (r == platform::WaitResult::Error)
        {
            crashed.store (true, std::memory_order_release);
            return false;
        }
        // Awoken / ValueChanged / Interrupted: re-loop and re-check.
    }

    deserialiseMidiOut();
    roundTrips.fetch_add (1, std::memory_order_relaxed);
    return true;
}

bool RemotePluginConnection::sendAndAwaitReply (OpCode op,
                                                  const void* payload,
                                                  std::uint32_t payloadLen,
                                                  ControlMsgHeader& hdrOut,
                                                  std::vector<std::uint8_t>& replyOut,
                                                  std::string& errorOut,
                                                  int timeoutMs)
{
    if (! platform::isValid (controlChannel)) { errorOut = "not connected"; return false; }

    std::unique_lock<std::mutex> lk (controlMutex);

    if (readerExited.load (std::memory_order_acquire))
    {
        errorOut = "reader thread has exited";
        return false;
    }

    // Clear stale slot in case a prior call timed out without consuming
    // its reply. If a late reply for that previous call lands AFTER we
    // send, we'd misattribute — but: the message thread is single, so
    // a timeout means the caller already returned and no other call
    // can be in flight. Discarding the prior slot is correct.
    replyReady   = false;
    replyHeader  = {};
    replyPayload.clear();

    if (! sendControl (controlChannel, op, payload, payloadLen))
    {
        errorOut = std::string ("control send failed: ") + std::strerror (errno);
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds (timeoutMs);
    while (! replyReady && ! readerExited.load (std::memory_order_acquire))
    {
        if (replyCv.wait_until (lk, deadline) == std::cv_status::timeout)
        {
            errorOut = "reply timeout";
            return false;
        }
    }

    if (! replyReady)
    {
        errorOut = "reader thread exited before reply arrived";
        return false;
    }

    hdrOut   = replyHeader;
    replyOut = std::move (replyPayload);
    replyReady = false;
    return true;
}

bool RemotePluginConnection::ping (int timeoutMs, std::string& errorOut)
{
    ControlMsgHeader hdr {};
    std::vector<std::uint8_t> reply;
    if (! sendAndAwaitReply (OpCode::Ping, nullptr, 0, hdr, reply, errorOut, timeoutMs))
        return false;
    if (hdr.status != 0) { errorOut = "Ping reply status != 0"; return false; }
    return true;
}

bool RemotePluginConnection::loadPlugin (const std::string& pluginDescriptionXml,
                                          double sampleRate, int blockSize,
                                          int& numInOut, int& numOutOut,
                                          int& latencyOut, std::string& errorOut)
{
    PrepareToPlayPayload header {};
    header.sampleRate = sampleRate;
    header.blockSize  = (std::int32_t) blockSize;
    header.reserved   = 0;

    std::vector<std::uint8_t> payload;
    payload.resize (sizeof (header) + pluginDescriptionXml.size());
    std::memcpy (payload.data(), &header, sizeof (header));
    std::memcpy (payload.data() + sizeof (header),
                  pluginDescriptionXml.data(), pluginDescriptionXml.size());

    ControlMsgHeader hdr {};
    std::vector<std::uint8_t> reply;
    if (! sendAndAwaitReply (OpCode::LoadPlugin,
                              payload.data(), (std::uint32_t) payload.size(),
                              hdr, reply, errorOut, 30000))
        return false;

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
    PrepareToPlayPayload p {};
    p.sampleRate = sampleRate;
    p.blockSize  = (std::int32_t) blockSize;
    ControlMsgHeader hdr {};
    std::vector<std::uint8_t> reply;
    if (! sendAndAwaitReply (OpCode::PrepareToPlay, &p, sizeof (p),
                               hdr, reply, errorOut, 10000))
        return false;
    if (hdr.status != 0) { errorOut = "PrepareToPlay status != 0"; return false; }
    return true;
}

bool RemotePluginConnection::release (std::string& errorOut)
{
    ControlMsgHeader hdr {};
    std::vector<std::uint8_t> reply;
    if (! sendAndAwaitReply (OpCode::Release, nullptr, 0,
                               hdr, reply, errorOut, 5000))
        return false;
    if (hdr.status != 0) { errorOut = "Release status != 0"; return false; }
    return true;
}

bool RemotePluginConnection::getState (std::vector<std::uint8_t>& blobOut,
                                         std::string& errorOut)
{
    ControlMsgHeader hdr {};
    std::vector<std::uint8_t> reply;
    if (! sendAndAwaitReply (OpCode::GetState, nullptr, 0,
                               hdr, reply, errorOut, 30000))
        return false;
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
        reinterpret_cast<const std::uint8_t*> (shm.data()) + kStateOffset,
        reinterpret_cast<const std::uint8_t*> (shm.data()) + kStateOffset + blobSize);
    return true;
}

bool RemotePluginConnection::setState (const std::uint8_t* data, std::size_t size,
                                         std::string& errorOut)
{
    if (size > kStateBytes)
    {
        errorOut = "state blob exceeds staging area";
        return false;
    }
    std::memcpy (static_cast<char*> (shm.data()) + kStateOffset, data, size);
    const std::uint32_t sz = (std::uint32_t) size;
    ControlMsgHeader hdr {};
    std::vector<std::uint8_t> reply;
    if (! sendAndAwaitReply (OpCode::SetState, &sz, sizeof (sz),
                               hdr, reply, errorOut, 30000))
        return false;
    if (hdr.status != 0) { errorOut = "SetState status != 0"; return false; }
    return true;
}

bool RemotePluginConnection::showEditor (std::uint64_t& windowIdOut,
                                            int& widthOut, int& heightOut,
                                            std::string& errorOut)
{
    windowIdOut = 0; widthOut = 0; heightOut = 0;
    ControlMsgHeader hdr {};
    std::vector<std::uint8_t> reply;
    if (! sendAndAwaitReply (OpCode::ShowEditor, nullptr, 0,
                               hdr, reply, errorOut, 10000))
        return false;
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
    ControlMsgHeader hdr {};
    std::vector<std::uint8_t> reply;
    if (! sendAndAwaitReply (OpCode::HideEditor, nullptr, 0,
                               hdr, reply, errorOut, 5000))
        return false;
    if (hdr.status != 0) { errorOut = "HideEditor status != 0"; return false; }
    return true;
}

bool RemotePluginConnection::resizeEditor (int width, int height, std::string& errorOut)
{
    ResizeEditorPayload p {};
    p.width  = (std::int32_t) width;
    p.height = (std::int32_t) height;
    ControlMsgHeader hdr {};
    std::vector<std::uint8_t> reply;
    if (! sendAndAwaitReply (OpCode::ResizeEditor, &p, sizeof (p),
                               hdr, reply, errorOut, 5000))
        return false;
    if (hdr.status != 0) { errorOut = "ResizeEditor status != 0"; return false; }
    return true;
}

// --- Parameter mirror (3c-3a) -------------------------------------------

bool RemotePluginConnection::setRemoteParam (int paramIndex, float value01) noexcept
{
    if (! platform::isValid (controlChannel)) return false;
    if (paramIndex < 0) return false;

    SetParamPayload p {};
    p.paramIndex = (std::uint32_t) paramIndex;
    p.value      = std::min (1.0f, std::max (0.0f, value01));

    std::lock_guard<std::mutex> lk (controlMutex);
    if (readerExited.load (std::memory_order_acquire)) return false;
    // Fire-and-forget: child suppresses the reply for SetParam (per
    // PluginIpc.h opcode contract), so we do not wait on replyCv.
    return sendControl (controlChannel, OpCode::SetParam, &p, sizeof (p));
}

void RemotePluginConnection::setParamChangedSink (ParamChangedSink sink)
{
    // Mutate the shared SinkState under its own mutex (independent of
    // controlMutex — that one guards the reply-correlation queue only).
    std::lock_guard<std::mutex> lk (sinkState_->mutex);
    sinkState_->sink = std::move (sink);
}

// --- Reader-thread infrastructure ---------------------------------------

void RemotePluginConnection::startReaderThread()
{
    if (readerStarted) return;
    readerExited.store (false, std::memory_order_release);
    readerThread = std::thread ([this] { readerLoop(); });
    readerStarted = true;
}

void RemotePluginConnection::stopReaderThread() noexcept
{
    if (! readerStarted) return;
    // Wake any sender currently blocked on replyCv first.
    {
        std::lock_guard<std::mutex> lk (controlMutex);
        readerExited.store (true, std::memory_order_release);
        replyCv.notify_all();
    }
    // 3c-4: explicitly wake the reader thread. Before this call the
    // reader could be sitting in a blocking readExact even after the
    // child terminated — on Windows the close-during-blocking-ReadFile
    // race left the reader stuck until the kernel finished tearing
    // down the IRP, sometimes seconds. Now CancelIoEx (Win) /
    // shutdown(SHUT_RD) (POSIX) returns the reader from its syscall
    // in O(microseconds) so the join below completes promptly.
    wakeReaderForShutdown (controlChannel);
    if (readerThread.joinable())
        readerThread.join();
    readerStarted = false;
}

void RemotePluginConnection::readerLoop()
{
    while (! readerExited.load (std::memory_order_acquire))
    {
        ControlMsgHeader hdr {};
        if (! platform::readExact (controlChannel, &hdr, sizeof (hdr)))
            break;  // EOF / peer closed / EBADF on disconnect

        std::vector<std::uint8_t> payload (hdr.payloadLen);
        if (hdr.payloadLen > 0
            && ! platform::readExact (controlChannel, payload.data(), hdr.payloadLen))
            break;

        if (hdr.op == (std::uint32_t) OpCode::ParamChangedFromChild)
        {
            // Async push from child. Marshal to the JUCE message thread
            // via callAsync — never invoke the sink directly here (we're
            // on the reader thread, sink callers may touch UI / JUCE
            // listener state).
            //
            // Lifetime: capture the SinkState shared_ptr BY VALUE. The
            // lambda no longer references `this`, so if the parent's
            // RemotePluginConnection is destroyed before this lambda
            // dispatches (e.g. PluginSlot::unload tears down the
            // connection between read and message-loop tick), the
            // shared_ptr keeps the SinkState alive until the lambda
            // runs + the last reference drops. No UAF on controlMutex
            // or paramChangedSink_.
            if (payload.size() == sizeof (ParamChangedPayload))
            {
                ParamChangedPayload p {};
                std::memcpy (&p, payload.data(), sizeof (p));
                auto state = sinkState_;
                juce::MessageManager::callAsync ([state, p]
                {
                    ParamChangedSink localSink;
                    {
                        std::lock_guard<std::mutex> lk (state->mutex);
                        localSink = state->sink;
                    }
                    if (localSink)
                        localSink ((int) p.paramIndex, p.value, p.sequenceNumber);
                });
            }
            continue;
        }

        // Anything else is a sync-RPC reply. Park it for the waiting
        // sender. Single in-flight RPC is the invariant (message thread
        // is JUCE-single-threaded), so overwriting an unconsumed reply
        // would be a bug — we still publish to avoid deadlocking, and
        // drop the prior unread slot on the floor with a stderr line.
        std::lock_guard<std::mutex> lk (controlMutex);
        if (replyReady)
            std::fprintf (stderr,
                          "[Dusk Studio/RemotePluginConnection] reader: "
                          "dropping unread reply slot (op=%u) - caller "
                          "did not consume the previous reply.\n",
                          (unsigned) replyHeader.op);
        replyHeader = hdr;
        replyPayload = std::move (payload);
        replyReady = true;
        replyCv.notify_all();
    }

    // Exiting — either peer-closed (child terminated) or disconnect
    // explicitly requested. Wake any sender waiting on replyCv so it
    // returns with an error rather than blocking forever.
    std::lock_guard<std::mutex> lk (controlMutex);
    readerExited.store (true, std::memory_order_release);
    replyCv.notify_all();
}

bool RemotePluginConnection::pollReaper() noexcept
{
    if (! child.isAlive()) return false;
    if (! child.pollExit()) return false;
    crashed.store (true, std::memory_order_release);
    return true;
}

void RemotePluginConnection::disconnect()
{
    // Order matters:
    //   1. Signal SHM teardown + ask the child to exit. The child's
    //      sockThread closes its socket end on the way out, which makes
    //      our reader's readExact return false.
    //   2. stopReaderThread joins after readerExited/replyCv have been
    //      published — any sender currently in CV wait wakes up and
    //      returns with an error.
    //   3. Close our half of the control channel.
    //   4. shm.close drops the mmap.
    //
    // Closing the channel BEFORE joining the reader is the standard
    // race: on Linux closing the fd while another thread is mid-read
    // returns EBADF and the read fails (well-defined since 2.6.31).
    // On macOS the close-during-blocking-read behaviour is the same;
    // on Windows the equivalent is CancelIoEx — out of scope for
    // 3c-3a but documented as a 3c-4 hardening item.
    if (child.isAlive())
    {
        if (shm.isMapped())
        {
            auto* hdr = headerOf (shm.data());
            hdr->state.store (kStateTeardown, std::memory_order_release);
            platform::wakeOneAddress (&hdr->cmdSeq);
        }
        child.terminate (500);
    }
    stopReaderThread();
    platform::closeHandle (controlChannel);
    shm.close();
}

} // namespace duskstudio::ipc
