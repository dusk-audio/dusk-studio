// dusk-studio-plugin-host - the child binary that owns one out-of-process VST3
// (or LV2) instance on behalf of Dusk Studio's main process. Two modes:
//
//   --ipc-stub  : echo input -> output, no JUCE plugin. Exists so the
//                 IPC self-test can validate shm + sync + spawn plumbing
//                 without a plugin in the loop.
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
//                          kChildInheritFd, dispatches them: LoadPlugin,
//                          PrepareToPlay, Release, GetState, SetState.
//                          Uses MessageManagerLock when calling APIs
//                          that JUCE marks message-thread-only.
//   audio worker thread  - waits on cmdSeq, calls plugin->processBlock
//                          when a command arrives. Lock-free read of the
//                          atomic instance pointer so the parent's audio
//                          thread isn't gated on control-plane traffic.

#include "PluginIpc.h"
#include "../JuceCompat.h"
#include "platform/IpcChannel.h"
#include "platform/IpcShm.h"
#include "platform/IpcSync.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>

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
namespace ipcp = duskstudio::ipc::platform;

bool sendControlReply (ipcp::NativeHandle& ch, std::uint32_t op,
                          std::uint32_t status, const void* payload,
                          std::uint32_t payloadLen) noexcept
{
    ControlMsgHeader hdr {};
    hdr.totalLen   = (std::uint32_t) sizeof (hdr) + payloadLen;
    hdr.op         = op;
    hdr.status     = status;
    hdr.payloadLen = payloadLen;
    if (! ipcp::writeExact (ch, &hdr, sizeof (hdr))) return false;
    if (payloadLen > 0 && ! ipcp::writeExact (ch, payload, payloadLen))
        return false;
    return true;
}

// --- Phase 1 echo mode (kept for the IPC self-test) ----------------------
int runIpcStub (int argc, const char* const* argv) noexcept
{
    ipcp::NativeHandle channel = ipcp::locateInheritedChannel (argc, argv);
    if (! ipcp::isValid (channel))
    {
        std::fprintf (stderr, "[dusk-studio-plugin-host] no inherited channel\n");
        return 1;
    }

    ipcp::NativeHandle shmHandle;
    if (! ipcp::recvHandle (channel, shmHandle))
    {
        std::fprintf (stderr, "[dusk-studio-plugin-host] recvHandle failed\n");
        return 1;
    }

    ipcp::SharedMemory shm;
    std::string err;
    if (! shm.mapInheritedHandle (shmHandle, kTotalSize, err))
    {
        std::fprintf (stderr, "[dusk-studio-plugin-host] %s\n", err.c_str());
        return 1;
    }

    auto* hdr = headerOf (shm.data());
    if (hdr->magic != kMagic || hdr->version != kVersion)
    {
        std::fprintf (stderr, "[dusk-studio-plugin-host] SHM magic/version mismatch\n");
        return 1;
    }

    {
        char k = 'k';
        if (! ipcp::writeExact (channel, &k, 1)) return 1;
    }

    std::uint32_t lastSeq = 0;
    while (true)
    {
        if (hdr->state.load (std::memory_order_acquire) == kStateTeardown)
            break;

        const auto cmd = hdr->cmdSeq.load (std::memory_order_acquire);
        if (cmd == lastSeq)
        {
            (void) ipcp::waitOnAddress (&hdr->cmdSeq, cmd, nullptr);
            continue;
        }

        int n  = (int) hdr->numSamples;
        int ci = (int) hdr->numInChans;
        int co = (int) hdr->numOutChans;
        if (n  < 0) n  = 0;  if (n  > kMaxBlock) n  = kMaxBlock;
        if (ci < 0) ci = 0;  if (ci > kMaxChans) ci = kMaxChans;
        if (co < 0) co = 0;  if (co > kMaxChans) co = kMaxChans;

        for (int c = 0; c < co; ++c)
        {
            float* outCh = audioOutChannel (shm.data(), c);
            if (c < ci)
                std::memcpy (outCh, audioInChannel (shm.data(), c),
                             (std::size_t) n * sizeof (float));
            else
                std::memset (outCh, 0, (std::size_t) n * sizeof (float));
        }

        const auto midiInBytes = hdr->midiInBytes <= kMidiBytes ? hdr->midiInBytes : 0u;
        hdr->midiOutBytes = midiInBytes;
        if (midiInBytes > 0)
            std::memcpy (midiOut (shm.data()), midiIn (shm.data()), midiInBytes);

        lastSeq = cmd;
        hdr->replySeq.store (cmd, std::memory_order_release);
        ipcp::wakeOneAddress (&hdr->replySeq);
    }

    return 0;
}

// --- Phase 2 host mode ---------------------------------------------------

struct HostState
{
    ipcp::SharedMemory shm;
    BlockHeader* hdr = nullptr;
    ipcp::NativeHandle channel {};

    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownList;

    std::unique_ptr<juce::AudioPluginInstance> ownedInstance;
    std::atomic<juce::AudioPluginInstance*> currentInstance { nullptr };

    juce::AudioBuffer<float> workBuffer { kMaxChans, kMaxBlock };

    double currentSampleRate = 0.0;
    int    currentBlockSize  = 0;

    std::atomic<bool> shouldQuit { false };

    // Editor embedding (Phase 4). The plugin's editor lives in this
    // process; the parent either embeds our native window (Linux XEmbed)
    // or lets it float as a native-titlebar toplevel (Win/Mac — see
    // handleShowEditor for the per-platform chrome choice). editorWindow
    // wraps the plugin's AudioProcessorEditor so it has its own native
    // peer; editor is a non-owning pointer (the wrapper window owns the
    // Component). Both are message-thread-only; the socket-reader thread
    // acquires MessageManagerLock before touching them.
    std::unique_ptr<juce::DocumentWindow>     editorWindow;
    juce::AudioProcessorEditor*               editor { nullptr };
};

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
    host.currentInstance.store (host.ownedInstance.get(),
                                  std::memory_order_release);
}

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

    host.currentInstance.store (nullptr, std::memory_order_release);

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
    if (host.ownedInstance == nullptr) return 0;
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
        const juce::MessageManagerLock mml;
        host.ownedInstance->getStateInformation (mb);
    });
    if (mb.getSize() > kStateBytes) return 2;
    std::memcpy (static_cast<char*> (host.shm.data()) + kStateOffset,
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
            static_cast<const char*> (host.shm.data()) + kStateOffset, (int) sz);
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

        if (host.editor == nullptr)
        {
            host.editor = host.ownedInstance->createEditorIfNeeded();
            if (host.editor == nullptr) return 2;
        }

        if (host.editorWindow == nullptr)
        {
            // The plugin's name (best-effort) labels the floating window
            // on Win/Mac so the user can tell it apart from other windows
            // when the OOP editor isn't embedded.
            juce::String title = "dusk-studio-plugin-host";
            if (host.ownedInstance != nullptr)
                title = host.ownedInstance->getName();

            auto win = std::make_unique<juce::DocumentWindow> (
                title,
                juce::Colours::black,
                juce::DocumentWindow::closeButton);
           #if JUCE_LINUX
            // Linux: borderless toplevel because the parent XEmbeds the
            // X11 window and provides its own chrome via PluginEditorWindow.
            win->setUsingNativeTitleBar (false);
            win->setTitleBarHeight (0);
           #else
            // Windows / macOS: real titlebar + close button so the
            // floating editor window is movable + closable without the
            // parent having to track its lifecycle. Cross-process HWND /
            // NSView embedding can come later; for now the OOP editor
            // lives as a top-level window the OS already knows how to
            // manage.
            win->setUsingNativeTitleBar (true);
           #endif
            win->setOpaque (true);
            win->setContentNonOwned (host.editor, true);
            const int w = host.editor->getWidth()  > 0 ? host.editor->getWidth()  : 480;
            const int h = host.editor->getHeight() > 0 ? host.editor->getHeight() : 360;
            win->centreWithSize (w, h);
            win->setVisible (true);
            host.editorWindow = std::move (win);
        }
        else
        {
            host.editorWindow->setVisible (true);
            host.editorWindow->toFront (true);
        }

        if (auto* peer = host.editorWindow->getPeer())
        {
            auto* nativeHandle = peer->getNativeHandle();
            reply.windowId = (std::uint64_t) (std::uintptr_t) nativeHandle;
        }
        reply.width  = host.editorWindow->getWidth();
        reply.height = host.editorWindow->getHeight();
        reply.reserved = 0;
    }

    if (reply.windowId == 0) return 3;

    replyOut.resize (sizeof (reply));
    std::memcpy (replyOut.data(), &reply, sizeof (reply));
    return 0;
}

std::uint32_t handleHideEditor (HostState& host)
{
    const juce::MessageManagerLock mml;
    if (host.editorWindow != nullptr)
    {
        host.editorWindow->clearContentComponent();
        host.editorWindow.reset();
    }
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
            (void) ipcp::waitOnAddress (&host.hdr->cmdSeq, cmd, nullptr);
            continue;
        }

        int n  = (int) host.hdr->numSamples;
        int ci = (int) host.hdr->numInChans;
        int co = (int) host.hdr->numOutChans;
        if (n  < 0) n  = 0;  if (n  > kMaxBlock) n  = kMaxBlock;
        if (ci < 0) ci = 0;  if (ci > kMaxChans) ci = kMaxChans;
        if (co < 0) co = 0;  if (co > kMaxChans) co = kMaxChans;

        auto* p = host.currentInstance.load (std::memory_order_acquire);

        if (p == nullptr || n <= 0 || co <= 0)
        {
            for (int c = 0; c < co; ++c)
                std::memset (audioOutChannel (host.shm.data(), c), 0,
                             (std::size_t) n * sizeof (float));
        }
        else
        {
            const int bufCh = juce::jmax (ci, co);
            for (int c = 0; c < bufCh; ++c)
            {
                if (c < ci)
                    std::memcpy (host.workBuffer.getWritePointer (c),
                                  audioInChannel (host.shm.data(), c),
                                  (std::size_t) n * sizeof (float));
                else
                    std::memset (host.workBuffer.getWritePointer (c), 0,
                                  (std::size_t) n * sizeof (float));
            }

            midiScratch.clear();
            const auto midiInBytes = host.hdr->midiInBytes;
            if (midiInBytes > 0 && midiInBytes <= kMidiBytes)
            {
                const std::uint8_t* base = midiIn (host.shm.data());
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
                host.hdr->state.store (kStateCrashed, std::memory_order_release);
                break;
            }

            for (int c = 0; c < co; ++c)
                std::memcpy (audioOutChannel (host.shm.data(), c),
                              host.workBuffer.getReadPointer (c),
                              (std::size_t) n * sizeof (float));

            std::uint8_t* out = midiOut (host.shm.data());
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
        ipcp::wakeOneAddress (&host.hdr->replySeq);
    }
}

int runIpcHost (int argc, const char* const* argv) noexcept
{
    HostState host;
    host.channel = ipcp::locateInheritedChannel (argc, argv);
    if (! ipcp::isValid (host.channel))
    {
        std::fprintf (stderr, "no inherited channel\n");
        return 1;
    }

    ipcp::NativeHandle shmHandle;
    if (! ipcp::recvHandle (host.channel, shmHandle))
    {
        std::fprintf (stderr, "recvHandle failed\n");
        return 1;
    }

    std::string err;
    if (! host.shm.mapInheritedHandle (shmHandle, kTotalSize, err))
    {
        std::fprintf (stderr, "%s\n", err.c_str());
        return 1;
    }
    host.hdr = headerOf (host.shm.data());
    if (host.hdr->magic != kMagic || host.hdr->version != kVersion)
    {
        std::fprintf (stderr, "SHM magic/version mismatch\n");
        return 1;
    }

    juce::ScopedJuceInitialiser_GUI juceInit;

    duskstudio::juce_compat::addDefaultFormats (host.formatManager);

    {
        char k = 'k';
        if (! ipcp::writeExact (host.channel, &k, 1)) return 1;
    }

    std::thread worker (audioWorkerLoop, std::ref (host));

    std::thread sockThread ([&host]
    {
        while (! host.shouldQuit.load (std::memory_order_acquire))
        {
            ControlMsgHeader hdr {};
            if (! ipcp::readExact (host.channel, &hdr, sizeof (hdr))) break;
            std::vector<std::uint8_t> payload (hdr.payloadLen);
            if (hdr.payloadLen > 0
                && ! ipcp::readExact (host.channel, payload.data(), hdr.payloadLen))
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

            if (! sendControlReply (host.channel, hdr.op, status,
                                     reply.empty() ? nullptr : reply.data(),
                                     (std::uint32_t) reply.size()))
                break;
        }
        host.shouldQuit.store (true, std::memory_order_release);
        host.hdr->state.store (kStateTeardown, std::memory_order_release);
        ipcp::wakeOneAddress (&host.hdr->cmdSeq);
        juce::MessageManager::getInstance()->stopDispatchLoop();
    });

    juce::MessageManager::getInstance()->runDispatchLoop();

    sockThread.join();
    worker.join();

    if (host.ownedInstance != nullptr)
    {
        host.ownedInstance->releaseResources();
        host.ownedInstance.reset();
    }

    return 0;
}
} // namespace

int main (int argc, char** argv)
{
   #if ! defined (_WIN32)
    signal (SIGPIPE, SIG_IGN);
   #endif

    bool ipcStub = false;
    bool ipcHost = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp (argv[i], "--ipc-stub") == 0) ipcStub = true;
        if (std::strcmp (argv[i], "--ipc-host") == 0) ipcHost = true;
    }

    if (ipcStub) return runIpcStub (argc, argv);
    if (ipcHost) return runIpcHost (argc, argv);

    std::fprintf (stderr,
                  "dusk-studio-plugin-host: pass --ipc-stub or --ipc-host.\n");
    return 64;
}
