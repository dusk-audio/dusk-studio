// dusk-studio-plugin-host - the child binary that owns one out-of-process VST3
// (or LV2) instance on behalf of Dusk Studio's main process. Two modes:
//
//   --ipc-stub  : echo input -> output, no JUCE plugin. Exists so the
//                 IPC self-test can validate shm + futex + fork/exec
//                 plumbing without a plugin in the loop.
//   --ipc-host  : full Phase-2 host. Loads a juce::AudioPluginInstance
//                 via the format manager, runs processBlock on a worker
//                 thread, services control RPCs on a separate socket-
//                 reader thread, runs the JUCE message loop on main.
//
// Process layout (--ipc-host):
//
//   main thread          - JUCE message loop (MessageManager dispatch).
//                          Plugins that post async messages to themselves
//                          (parameter listeners, restartComponent
//                          notifications, editor lifecycle in Phase 3)
//                          need this running.
//   socket reader thread - reads length-prefixed control messages from
//                          fd 3, dispatches them: LoadPlugin,
//                          PrepareToPlay, Release, GetState, SetState.
//                          Uses MessageManagerLock when calling APIs
//                          that JUCE marks message-thread-only.
//   audio worker thread  - futex-waits on cmdSeq, calls
//                          plugin->processBlock when a command arrives.
//                          Lock-free read of the atomic instance pointer
//                          so the parent's audio thread isn't gated on
//                          control-plane traffic.

#include "PluginIpc.h"
#include "../JuceCompat.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace
{
using namespace duskstudio::ipc;

constexpr int kSocketFd = 3;

// Receive the SHM fd that the parent passed via SCM_RIGHTS.
int recvFd (int socketFd) noexcept
{
    char dummy = 0;
    struct iovec iov { &dummy, 1 };

    char ctlBuf[CMSG_SPACE (sizeof (int))] {};
    struct msghdr msg {};
    msg.msg_iov     = &iov;
    msg.msg_iovlen  = 1;
    msg.msg_control = ctlBuf;
    msg.msg_controllen = sizeof (ctlBuf);

    if (recvmsg (socketFd, &msg, 0) < 0) return -1;

    for (struct cmsghdr* cm = CMSG_FIRSTHDR (&msg);
         cm != nullptr;
         cm = CMSG_NXTHDR (&msg, cm))
    {
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS)
        {
            int fd = -1;
            std::memcpy (&fd, CMSG_DATA (cm), sizeof (fd));
            return fd;
        }
    }
    return -1;
}

bool readExact (int fd, void* buf, std::size_t n) noexcept
{
    auto* p = static_cast<char*> (buf);
    while (n > 0)
    {
        const ssize_t r = ::read (fd, p, n);
        if (r < 0) { if (errno == EINTR) continue; return false; }
        if (r == 0) return false;
        p += r;
        n -= (std::size_t) r;
    }
    return true;
}

bool writeExact (int fd, const void* buf, std::size_t n) noexcept
{
    auto* p = static_cast<const char*> (buf);
    while (n > 0)
    {
        const ssize_t w = ::write (fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return false; }
        if (w == 0) return false;  // peer closed; avoid infinite loop
        p += w;
        n -= (std::size_t) w;
    }
    return true;
}

bool sendControlReply (std::uint32_t op, std::uint32_t status,
                        const void* payload, std::uint32_t payloadLen) noexcept
{
    ControlMsgHeader hdr {};
    hdr.totalLen   = (std::uint32_t) sizeof (hdr) + payloadLen;
    hdr.op         = op;
    hdr.status     = status;
    hdr.payloadLen = payloadLen;
    if (! writeExact (kSocketFd, &hdr, sizeof (hdr))) return false;
    if (payloadLen > 0 && ! writeExact (kSocketFd, payload, payloadLen))
        return false;
    return true;
}

// --- Phase 1 echo mode (kept for the IPC self-test) ----------------------
int runIpcStub() noexcept
{
    int shmFd = recvFd (kSocketFd);
    if (shmFd < 0)
    {
        std::fprintf (stderr, "[dusk-studio-plugin-host] recvFd failed\n");
        return 1;
    }

    void* shm = mmap (nullptr, kTotalSize, PROT_READ | PROT_WRITE,
                       MAP_SHARED, shmFd, 0);
    if (shm == MAP_FAILED)
    {
        std::fprintf (stderr, "[dusk-studio-plugin-host] mmap failed: %s\n",
                      std::strerror (errno));
        ::close (shmFd);
        return 1;
    }
    // The mapping holds an independent reference to the underlying memfd,
    // so the fd itself is no longer needed.
    ::close (shmFd);

    auto* hdr = headerOf (shm);
    if (hdr->magic != kMagic || hdr->version != kVersion)
    {
        std::fprintf (stderr, "[dusk-studio-plugin-host] SHM magic/version mismatch\n");
        munmap (shm, kTotalSize);
        return 1;
    }

    {
        char k = 'k';
        if (write (kSocketFd, &k, 1) != 1)
        {
            munmap (shm, kTotalSize);
            return 1;
        }
    }

    std::uint32_t lastSeq = 0;
    while (true)
    {
        if (hdr->state.load (std::memory_order_acquire) == kStateTeardown)
            break;

        const auto cmd = hdr->cmdSeq.load (std::memory_order_acquire);
        if (cmd == lastSeq)
        {
            (void) syscall (SYS_futex, &hdr->cmdSeq,
                            FUTEX_WAIT_BITSET,
                            cmd, nullptr, nullptr,
                            FUTEX_BITSET_MATCH_ANY);
            continue;
        }

        // Clamp header fields - the SHM is shared with another process so
        // out-of-range values from a malformed/old peer must not be used
        // unchecked as memcpy lengths or channel indices.
        int n  = (int) hdr->numSamples;
        int ci = (int) hdr->numInChans;
        int co = (int) hdr->numOutChans;
        if (n  < 0) n  = 0;  if (n  > kMaxBlock) n  = kMaxBlock;
        if (ci < 0) ci = 0;  if (ci > kMaxChans) ci = kMaxChans;
        if (co < 0) co = 0;  if (co > kMaxChans) co = kMaxChans;

        for (int c = 0; c < co; ++c)
        {
            float* outCh = audioOutChannel (shm, c);
            if (c < ci)
                std::memcpy (outCh, audioInChannel (shm, c),
                             (std::size_t) n * sizeof (float));
            else
                std::memset (outCh, 0, (std::size_t) n * sizeof (float));
        }

        const auto midiInBytes = hdr->midiInBytes <= kMidiBytes ? hdr->midiInBytes : 0u;
        hdr->midiOutBytes = midiInBytes;
        if (midiInBytes > 0)
            std::memcpy (midiOut (shm), midiIn (shm), midiInBytes);

        lastSeq = cmd;
        hdr->replySeq.store (cmd, std::memory_order_release);
        (void) syscall (SYS_futex, &hdr->replySeq,
                        FUTEX_WAKE, 1, nullptr, nullptr, 0);
    }

    munmap (shm, kTotalSize);
    return 0;
}

// --- Phase 2 host mode ---------------------------------------------------

struct HostState
{
    void* shm = nullptr;
    BlockHeader* hdr = nullptr;

    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownList;

    // Owned only by the control thread. Worker reads via the atomic
    // pointer below.
    std::unique_ptr<juce::AudioPluginInstance> ownedInstance;
    std::atomic<juce::AudioPluginInstance*> currentInstance { nullptr };

    // Pre-allocated buffers reused per block on the audio worker. Sized
    // up to (kMaxChans, kMaxBlock) so one Audio Worker doesn't allocate
    // even when the parent shrinks/grows the block.
    juce::AudioBuffer<float> workBuffer { kMaxChans, kMaxBlock };

    double currentSampleRate = 0.0;
    int    currentBlockSize  = 0;

    std::atomic<bool> shouldQuit { false };

    // Editor embedding (Phase 4). The plugin's editor lives in this
    // process; the parent embeds our native X11 window via XEmbed.
    // editorWindow is a borderless toplevel that wraps the plugin's
    // AudioProcessorEditor so it has its own native peer; editor is
    // a non-owning pointer (the wrapper window owns the Component).
    // Both are message-thread-only; the socket-reader thread acquires
    // MessageManagerLock before touching them.
    std::unique_ptr<juce::DocumentWindow>     editorWindow;
    juce::AudioProcessorEditor*               editor { nullptr };
};

// Build a juce::PluginDescription from a string holding the XML emitted
// by juce::PluginDescription::createXml(). Returns true on success.
bool parsePluginDescriptionXml (const juce::String& xml,
                                  juce::PluginDescription& out)
{
    if (auto root = juce::parseXML (xml))
        return out.loadFromXml (*root);
    return false;
}

// Park the audio worker so the socket-reader-thread handler can mutate
// the plugin (prepareToPlay / releaseResources / get/setStateInformation)
// without overlapping processBlock. JUCE's contract is that those calls
// MUST NOT overlap on the same instance; a sizeable share of real-world
// plugins crash hard if the host violates it (the parent uses the same
// pattern around its own state I/O via AtomicPark.h).
//
// Mechanism: clear currentInstance so the worker no-ops on any new
// cmdSeq bump, then wait until cmdSeq == replySeq (worker has drained
// every command it had in flight). 50 ms ceiling caps the stall if the
// worker somehow falls behind; we'd rather risk one stale audio block
// than wedge the control channel.
template <typename Fn>
void withParkedWorker (HostState& host, Fn&& fn)
{
    host.currentInstance.store (nullptr, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now()
                         + std::chrono::milliseconds (50);
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto cs = host.hdr->cmdSeq .load (std::memory_order_acquire);
        const auto rs = host.hdr->replySeq.load (std::memory_order_acquire);
        if (cs == rs) break;
        std::this_thread::sleep_for (std::chrono::microseconds (200));
    }
    fn();
    // Restore from ownedInstance; fn may have replaced or cleared it
    // (handleRelease nulls it out, handleLoadPlugin would swap it).
    host.currentInstance.store (host.ownedInstance.get(),
                                  std::memory_order_release);
}

// Control-plane handlers. Each returns the status field for the reply
// (0 = ok). They run on the socket-reader thread; ones that touch JUCE
// APIs marked message-thread-only acquire MessageManagerLock first.

std::uint32_t handleLoadPlugin (HostState& host,
                                  const std::vector<std::uint8_t>& payload,
                                  std::vector<std::uint8_t>& replyOut)
{
    if (payload.size() < sizeof (PrepareToPlayPayload)) return 1;
    PrepareToPlayPayload hdr {};
    std::memcpy (&hdr, payload.data(), sizeof (hdr));
    const auto xmlSize = payload.size() - sizeof (hdr);
    juce::String xml (reinterpret_cast<const char*> (payload.data() + sizeof (hdr)),
                       xmlSize);

    juce::PluginDescription desc;
    if (! parsePluginDescriptionXml (xml, desc))
    {
        const char* err = "failed to parse PluginDescription XML";
        replyOut.assign (err, err + std::strlen (err));
        return 2;
    }

    // Park the worker by clearing the live pointer first.
    host.currentInstance.store (nullptr, std::memory_order_release);

    // createPluginInstance is OK off the message thread (JUCE handles
    // the necessary locking inside). prepareToPlay is too.
    juce::String errorMsg;
    auto fresh = host.formatManager.createPluginInstance (
        desc, hdr.sampleRate, hdr.blockSize, errorMsg);

    if (fresh == nullptr)
    {
        const auto bytes = errorMsg.toRawUTF8();
        replyOut.assign (bytes, bytes + std::strlen (bytes));
        return 3;
    }

    fresh->setPlayConfigDetails (fresh->getTotalNumInputChannels(),
                                  fresh->getTotalNumOutputChannels(),
                                  hdr.sampleRate, hdr.blockSize);
    fresh->prepareToPlay (hdr.sampleRate, hdr.blockSize);

    LoadPluginReply reply {};
    reply.numInChans     = fresh->getTotalNumInputChannels();
    reply.numOutChans    = fresh->getTotalNumOutputChannels();
    reply.latencySamples = fresh->getLatencySamples();
    reply.reserved       = 0;
    replyOut.resize (sizeof (reply));
    std::memcpy (replyOut.data(), &reply, sizeof (reply));

    host.ownedInstance = std::move (fresh);
    host.currentSampleRate = hdr.sampleRate;
    host.currentBlockSize  = hdr.blockSize;
    host.currentInstance.store (host.ownedInstance.get(),
                                  std::memory_order_release);
    return 0;
}

std::uint32_t handlePrepareToPlay (HostState& host,
                                     const std::vector<std::uint8_t>& payload)
{
    if (payload.size() < sizeof (PrepareToPlayPayload)) return 1;
    PrepareToPlayPayload p {};
    std::memcpy (&p, payload.data(), sizeof (p));
    if (host.ownedInstance == nullptr) return 0;  // nothing to prepare
    withParkedWorker (host, [&]
    {
        host.ownedInstance->prepareToPlay (p.sampleRate, p.blockSize);
        host.currentSampleRate = p.sampleRate;
        host.currentBlockSize  = p.blockSize;
    });
    return 0;
}

std::uint32_t handleRelease (HostState& host)
{
    withParkedWorker (host, [&]
    {
        if (host.ownedInstance != nullptr)
        {
            host.ownedInstance->releaseResources();
            host.ownedInstance.reset();
        }
    });
    return 0;
}

std::uint32_t handleGetState (HostState& host,
                                std::vector<std::uint8_t>& replyOut)
{
    if (host.ownedInstance == nullptr) return 1;
    juce::MemoryBlock mb;
    withParkedWorker (host, [&]
    {
        const juce::MessageManagerLock mml;  // some plugins need it
        host.ownedInstance->getStateInformation (mb);
    });
    if (mb.getSize() > kStateBytes) return 2;
    std::memcpy (static_cast<char*> (host.shm) + kStateOffset,
                  mb.getData(), mb.getSize());
    const std::uint32_t sz = (std::uint32_t) mb.getSize();
    replyOut.resize (sizeof (sz));
    std::memcpy (replyOut.data(), &sz, sizeof (sz));
    return 0;
}

std::uint32_t handleSetState (HostState& host,
                                const std::vector<std::uint8_t>& payload)
{
    if (host.ownedInstance == nullptr) return 1;
    if (payload.size() != sizeof (std::uint32_t)) return 2;
    std::uint32_t sz = 0;
    std::memcpy (&sz, payload.data(), sizeof (sz));
    if (sz > kStateBytes) return 3;
    withParkedWorker (host, [&]
    {
        const juce::MessageManagerLock mml;
        host.ownedInstance->setStateInformation (
            static_cast<const char*> (host.shm) + kStateOffset, (int) sz);
    });
    return 0;
}

std::uint32_t handleShowEditor (HostState& host,
                                  std::vector<std::uint8_t>& replyOut)
{
    if (host.ownedInstance == nullptr) return 1;

    ShowEditorReply reply {};
    {
        const juce::MessageManagerLock mml;

        // Lazily create the editor + its borderless wrapper window. The
        // wrapper is sized to the editor's preferred size; the parent's
        // XEmbedComponent will resize-on-mount to whatever space it has.
        if (host.editor == nullptr)
        {
            host.editor = host.ownedInstance->createEditorIfNeeded();
            if (host.editor == nullptr) return 2;  // plugin has no editor
        }

        if (host.editorWindow == nullptr)
        {
            // Borderless DocumentWindow so the plugin's GUI is unencumbered
            // by host chrome - the parent's PluginEditorWindow already
            // provides a frame + close button. Native peer required so we
            // have an X11 Window ID to embed.
            auto win = std::make_unique<juce::DocumentWindow> (
                "dusk-studio-plugin-host editor",
                juce::Colours::black,
                juce::DocumentWindow::closeButton);
            win->setUsingNativeTitleBar (false);
            win->setTitleBarHeight (0);
            win->setOpaque (true);
            win->setContentNonOwned (host.editor, true);
            // Match the editor's preferred size so a JUCE-default
            // resized() inside the plugin populates against real bounds.
            const int w = host.editor->getWidth()  > 0 ? host.editor->getWidth()  : 480;
            const int h = host.editor->getHeight() > 0 ? host.editor->getHeight() : 360;
            win->centreWithSize (w, h);
            // setVisible(true) realizes the native peer so getNativeHandle
            // is non-null. Toplevel needs to be addToDesktop'd; centre+
            // setVisible does that internally.
            win->setVisible (true);
            host.editorWindow = std::move (win);
        }
        else
        {
            host.editorWindow->setVisible (true);
        }

        if (auto* peer = host.editorWindow->getPeer())
        {
            auto* nativeHandle = peer->getNativeHandle();
            // On Linux, getNativeHandle returns an X11 Window cast to
            // void*. Window is `unsigned long`, which is 64-bit on
            // x86_64 Linux; pack into uint64_t for wire transport.
            reply.windowId = (std::uint64_t) (std::uintptr_t) nativeHandle;
        }
        reply.width  = host.editorWindow->getWidth();
        reply.height = host.editorWindow->getHeight();
        reply.reserved = 0;
    }

    if (reply.windowId == 0) return 3;  // peer wasn't ready

    replyOut.resize (sizeof (reply));
    std::memcpy (replyOut.data(), &reply, sizeof (reply));
    return 0;
}

std::uint32_t handleHideEditor (HostState& host)
{
    const juce::MessageManagerLock mml;
    if (host.editorWindow != nullptr)
    {
        // Detach the editor from the window BEFORE destroying the window.
        // setContentNonOwned was called earlier; clearing it explicitly
        // ensures the editor isn't dragged through ~DocumentWindow's
        // child-component teardown (some plugins react badly to that).
        host.editorWindow->clearContentComponent();
        host.editorWindow.reset();
    }
    // Keep host.editor alive — the plugin owns its editor's lifecycle
    // through the AudioPluginInstance; we just stop hosting it. A
    // subsequent ShowEditor re-attaches the same editor to a fresh
    // wrapper window (cheaper than re-creating the editor itself,
    // which can be slow for GUI-heavy plugins).
    return 0;
}

std::uint32_t handleResizeEditor (HostState& host,
                                    const std::vector<std::uint8_t>& payload)
{
    if (payload.size() != sizeof (ResizeEditorPayload)) return 1;
    ResizeEditorPayload p {};
    std::memcpy (&p, payload.data(), sizeof (p));
    const juce::MessageManagerLock mml;
    if (host.editorWindow == nullptr) return 2;
    host.editorWindow->setSize (juce::jmax (1, (int) p.width),
                                  juce::jmax (1, (int) p.height));
    return 0;
}

// Audio worker. Wakes on cmdSeq, runs processBlock, signals replySeq.
void audioWorkerLoop (HostState& host) noexcept
{
    juce::MidiBuffer midiScratch;
    std::uint32_t lastSeq = 0;

    while (! host.shouldQuit.load (std::memory_order_acquire))
    {
        if (host.hdr->state.load (std::memory_order_acquire) == kStateTeardown)
            break;

        const auto cmd = host.hdr->cmdSeq.load (std::memory_order_acquire);
        if (cmd == lastSeq)
        {
            (void) syscall (SYS_futex, &host.hdr->cmdSeq,
                            FUTEX_WAIT_BITSET,
                            cmd, nullptr, nullptr,
                            FUTEX_BITSET_MATCH_ANY);
            continue;
        }

        // Clamp header fields - the SHM is shared so a malformed peer must
        // not be able to drive memcpy/memset past the channel buffers.
        int n  = (int) host.hdr->numSamples;
        int ci = (int) host.hdr->numInChans;
        int co = (int) host.hdr->numOutChans;
        if (n  < 0) n  = 0;  if (n  > kMaxBlock) n  = kMaxBlock;
        if (ci < 0) ci = 0;  if (ci > kMaxChans) ci = kMaxChans;
        if (co < 0) co = 0;  if (co > kMaxChans) co = kMaxChans;

        auto* p = host.currentInstance.load (std::memory_order_acquire);

        if (p == nullptr || n <= 0 || co <= 0)
        {
            // No plugin (or pre-load): zero the output. Parent observes
            // this as silence, not as an error.
            for (int c = 0; c < co; ++c)
                std::memset (audioOutChannel (host.shm, c), 0,
                             (std::size_t) n * sizeof (float));
        }
        else
        {
            // Copy SHM input into the pre-allocated work buffer, run the
            // plugin in place, copy the result back to SHM output.
            const int bufCh = juce::jmax (ci, co);
            // Resize is a no-op if already large enough; in release the
            // buffer is fixed at (kMaxChans, kMaxBlock) from ctor so this
            // never allocates.
            for (int c = 0; c < bufCh; ++c)
            {
                if (c < ci)
                    std::memcpy (host.workBuffer.getWritePointer (c),
                                  audioInChannel (host.shm, c),
                                  (std::size_t) n * sizeof (float));
                else
                    std::memset (host.workBuffer.getWritePointer (c), 0,
                                  (std::size_t) n * sizeof (float));
            }

            // MIDI in: deserialise once into midiScratch. The writer side
            // packs each event as native-endian [int sample][uint16 len]
            // [bytes], so we read with raw memcpy here to match. Stack-
            // allocated event buffer keeps this RT-safe (no heap on the
            // audio worker).
            midiScratch.clear();
            const auto midiInBytes = host.hdr->midiInBytes;
            if (midiInBytes > 0 && midiInBytes <= kMidiBytes)
            {
                const std::uint8_t* base = midiIn (host.shm);
                std::uint32_t off = 0;
                std::uint8_t evBuf[256];
                while (off + 6 <= midiInBytes)
                {
                    int sample = 0;
                    std::memcpy (&sample, base + off, 4); off += 4;
                    std::uint16_t l16 = 0;
                    std::memcpy (&l16, base + off, 2); off += 2;
                    const int eventLen = (int) l16;
                    if (eventLen <= 0 || eventLen > (int) sizeof (evBuf)) break;
                    if (off + (std::uint32_t) eventLen > midiInBytes) break;
                    std::memcpy (evBuf, base + off, (std::size_t) eventLen);
                    off += (std::uint32_t) eventLen;
                    midiScratch.addEvent (juce::MidiMessage (evBuf, eventLen), sample);
                }
            }

            juce::AudioBuffer<float> view (host.workBuffer.getArrayOfWritePointers(),
                                              bufCh, n);
            try
            {
                p->processBlock (view, midiScratch);
            }
            catch (...)
            {
                // Plugin threw - mark the connection crashed and exit
                // the audio loop so the parent's futex wait times out.
                host.hdr->state.store (kStateCrashed, std::memory_order_release);
                break;
            }

            for (int c = 0; c < co; ++c)
                std::memcpy (audioOutChannel (host.shm, c),
                              host.workBuffer.getReadPointer (c),
                              (std::size_t) n * sizeof (float));

            // Serialise MIDI out (events the plugin emitted - synth
            // notes, automation, etc.).
            std::uint8_t* out = midiOut (host.shm);
            std::uint32_t written = 0;
            for (const auto meta : midiScratch)
            {
                const auto m = meta.getMessage();
                const int len = m.getRawDataSize();
                if (written + 4 + 2 + (std::uint32_t) len > kMidiBytes) break;
                const int sample = meta.samplePosition;
                std::memcpy (out + written, &sample, 4); written += 4;
                const std::uint16_t l16 = (std::uint16_t) len;
                std::memcpy (out + written, &l16, 2); written += 2;
                std::memcpy (out + written, m.getRawData(), (std::size_t) len);
                written += (std::uint32_t) len;
            }
            host.hdr->midiOutBytes = written;
        }

        lastSeq = cmd;
        host.hdr->replySeq.store (cmd, std::memory_order_release);
        (void) syscall (SYS_futex, &host.hdr->replySeq,
                        FUTEX_WAKE, 1, nullptr, nullptr, 0);
    }
}

int runIpcHost() noexcept
{
    HostState host;

    // 1) SHM handoff.
    int shmFd = recvFd (kSocketFd);
    if (shmFd < 0) { std::fprintf (stderr, "recvFd failed\n"); return 1; }

    host.shm = mmap (nullptr, kTotalSize, PROT_READ | PROT_WRITE,
                      MAP_SHARED, shmFd, 0);
    if (host.shm == MAP_FAILED)
    {
        std::fprintf (stderr, "mmap failed\n");
        ::close (shmFd);
        return 1;
    }
    // Mapping retains its own ref on the memfd; the fd itself can go.
    ::close (shmFd);
    host.hdr = headerOf (host.shm);
    if (host.hdr->magic != kMagic || host.hdr->version != kVersion)
    {
        std::fprintf (stderr, "SHM magic/version mismatch\n");
        return 1;
    }

    // 2) JUCE init. ScopedJuceInitialiser_GUI is fine for headless use -
    // it sets up MessageManager + fonts + a no-op event loop. We need
    // it for any plugin that posts async messages.
    juce::ScopedJuceInitialiser_GUI juceInit;

    // Register the formats we host. Compat shim covers the upstream-vs-
    // wayland-fork API split.
    duskstudio::juce_compat::addDefaultFormats (host.formatManager);

    // 3) Ready handshake.
    {
        char k = 'k';
        if (write (kSocketFd, &k, 1) != 1) return 1;
    }

    // 4) Audio worker thread.
    std::thread worker (audioWorkerLoop, std::ref (host));

    // 5) Socket-reader thread reads control messages and dispatches.
    // Runs on its own thread so the JUCE message loop on main can
    // process messages plugins post to themselves.
    std::thread sockThread ([&host]
    {
        while (! host.shouldQuit.load (std::memory_order_acquire))
        {
            ControlMsgHeader hdr {};
            if (! readExact (kSocketFd, &hdr, sizeof (hdr))) break;
            std::vector<std::uint8_t> payload (hdr.payloadLen);
            if (hdr.payloadLen > 0
                && ! readExact (kSocketFd, payload.data(), hdr.payloadLen))
                break;

            std::vector<std::uint8_t> reply;
            std::uint32_t status = 0;

            switch ((OpCode) hdr.op)
            {
                case OpCode::Ping:           break;
                case OpCode::LoadPlugin:     status = handleLoadPlugin (host, payload, reply); break;
                case OpCode::PrepareToPlay:  status = handlePrepareToPlay (host, payload); break;
                case OpCode::Release:        status = handleRelease (host); break;
                case OpCode::GetState:       status = handleGetState (host, reply); break;
                case OpCode::SetState:       status = handleSetState (host, payload); break;
                case OpCode::ShowEditor:     status = handleShowEditor (host, reply); break;
                case OpCode::HideEditor:     status = handleHideEditor (host); break;
                case OpCode::ResizeEditor:   status = handleResizeEditor (host, payload); break;
                default:                     status = 99; break;
            }

            if (! sendControlReply (hdr.op, status,
                                     reply.empty() ? nullptr : reply.data(),
                                     (std::uint32_t) reply.size()))
                break;
        }
        host.shouldQuit.store (true, std::memory_order_release);
        // Wake the audio worker so it observes shouldQuit.
        host.hdr->state.store (kStateTeardown, std::memory_order_release);
        (void) syscall (SYS_futex, &host.hdr->cmdSeq,
                        FUTEX_WAKE, 1, nullptr, nullptr, 0);
        // Tell the JUCE message loop to exit.
        juce::MessageManager::getInstance()->stopDispatchLoop();
    });

    // 6) JUCE message loop on main. Returns when stopDispatchLoop is
    // called from the socket thread.
    juce::MessageManager::getInstance()->runDispatchLoop();

    sockThread.join();
    worker.join();

    if (host.ownedInstance != nullptr)
    {
        host.ownedInstance->releaseResources();
        host.ownedInstance.reset();
    }

    munmap (host.shm, kTotalSize);
    return 0;
}
} // namespace

int main (int argc, char** argv)
{
    signal (SIGPIPE, SIG_IGN);

    bool ipcStub = false;
    bool ipcHost = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp (argv[i], "--ipc-stub") == 0) ipcStub = true;
        if (std::strcmp (argv[i], "--ipc-host") == 0) ipcHost = true;
    }

    if (ipcStub) return runIpcStub();
    if (ipcHost) return runIpcHost();

    std::fprintf (stderr,
                  "dusk-studio-plugin-host: pass --ipc-stub or --ipc-host.\n");
    return 64;
}
