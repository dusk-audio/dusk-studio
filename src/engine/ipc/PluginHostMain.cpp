// dusk-studio-plugin-host - the child binary that owns one out-of-process VST3
// (or LV2) instance on behalf of Dusk Studio's main process. Three modes:
//
//   --ipc-stub  : echo input -> output, no JUCE plugin. Exists so the
//                 IPC self-test can validate shm + sync + spawn plumbing
//                 without a plugin in the loop.
//   --ipc-host  : full Phase-2 host. Loads a juce::AudioPluginInstance
//                 via the format manager, runs processBlock on a worker
//                 thread, services control RPCs on a separate socket-
//                 reader thread, runs the JUCE message loop on main.
//   --scan      : one-shot crash-isolated plugin discovery. Scans a single
//                 file/identifier for one format, prints its
//                 PluginDescription(s) as XML to stdout, exits. A plugin
//                 that crashes the scan takes down only this process; the
//                 parent blacklists the file and carries on. See runScan.
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
#include "PluginScanProtocol.h"
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
#include <mutex>
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

// Single mutex guards every outbound write on the control socket so the
// sockThread's sync-RPC replies cannot interleave with the async push
// path (pushParamChangedFromChild — called from a JUCE parameter listener
// that may fire on any thread, including the audio worker via the
// plugin's own callbacks).
std::mutex& channelWriteMutex()
{
    static std::mutex m;
    return m;
}

bool sendControlReply (ipcp::NativeHandle& ch, std::uint32_t op,
                          std::uint32_t status, const void* payload,
                          std::uint32_t payloadLen) noexcept
{
    ControlMsgHeader hdr {};
    hdr.totalLen   = (std::uint32_t) sizeof (hdr) + payloadLen;
    hdr.op         = op;
    hdr.status     = status;
    hdr.payloadLen = payloadLen;
    std::lock_guard<std::mutex> lk (channelWriteMutex());
    if (! ipcp::writeExact (ch, &hdr, sizeof (hdr))) return false;
    if (payloadLen > 0 && ! ipcp::writeExact (ch, payload, payloadLen))
        return false;
    return true;
}

// One-shot outbound push. Stubbed entry point — 3c-3b installs the
// parameter listener on the child's DSP instance and calls this when
// the plugin changes a value (host automation, MIDI-mapped controller,
// preset reload). Wire format matches duskstudio::ipc::ParamChangedPayload.
// Returns false on socket write failure (peer closed); 3c-3b will
// surface this as a recoverable-by-relink condition.
[[maybe_unused]] bool pushParamChangedFromChild (ipcp::NativeHandle& ch,
                                                    int paramIndex, float value01,
                                                    std::uint32_t sequenceNumber) noexcept
{
    if (paramIndex < 0) return false;
    ParamChangedPayload p {};
    p.paramIndex     = (std::uint32_t) paramIndex;
    p.value          = std::min (1.0f, std::max (0.0f, value01));
    p.sequenceNumber = sequenceNumber;

    ControlMsgHeader hdr {};
    hdr.totalLen   = (std::uint32_t) sizeof (hdr) + (std::uint32_t) sizeof (p);
    hdr.op         = (std::uint32_t) OpCode::ParamChangedFromChild;
    hdr.status     = 0;
    hdr.payloadLen = (std::uint32_t) sizeof (p);

    std::lock_guard<std::mutex> lk (channelWriteMutex());
    if (! ipcp::writeExact (ch, &hdr, sizeof (hdr))) return false;
    if (! ipcp::writeExact (ch, &p, sizeof (p)))    return false;
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

#if JUCE_MAC
class ChildParamListener;  // forward — defined below HostState
#endif

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

   #if JUCE_MAC
    // Mirror state (3c-3b, Mac-only — Linux/Windows children don't
    // have a parent-side shell editor to mirror to, so installing
    // listeners + pushing back over the socket would be pure waste).
    //
    // applyingFromMirror: set across handleSetParamAsync's
    // setValueNotifyingHost so the child-side listener doesn't echo
    // the parent's own update back as a ParamChangedFromChild push.
    //
    // outboundParamSeq: monotonic counter stamped into every push.
    // Parent doesn't yet use it (loop-breaker is the flag); allocated
    // for future inflight-tracking. Increment + read under no lock —
    // listener fires sequentially per plugin contract.
    std::atomic<bool>         applyingFromMirror { false };
    std::atomic<std::uint32_t> outboundParamSeq  { 0 };
    std::unique_ptr<ChildParamListener> paramListener;
   #endif

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

#if JUCE_MAC
// Listener installed on every parameter of the loaded DSP instance.
// Fires on whichever thread the plugin chose to call setValueNotifying
// Host on — host automation lanes (audio worker), preset-load
// callbacks (message thread), MIDI-mapped controllers (sockThread via
// our SetParam → callAsync path). pushParamChangedFromChild takes
// channelWriteMutex internally so concurrent writes can't byte-
// interleave with the sockThread's reply traffic.
//
// applyingFromMirror is checked first so a parent → child SetParam
// doesn't echo back as a push (handleSetParamAsync sets the flag
// across its setValueNotifyingHost call).
//
// 3c-4 rate-limit: high-density modulation matrices (LFO rates, FM
// depth automation, complex MPE controllers) can fire hundreds of
// listener callbacks per audio block. Saturating the control socket
// stutters the parent's message thread because callAsync queues
// build up faster than the message loop drains. Per-param
// deduplication + a min-interval gate caps the push rate at ~200 Hz
// per param — well above any human-perceptible knob-twiddle rate,
// well below the worst-case automation flood.
class ChildParamListener final : public juce::AudioProcessorParameter::Listener
{
public:
    explicit ChildParamListener (HostState& h, int numParams)
        : host (h),
          lastSentValue ((std::size_t) juce::jmax (0, numParams)),
          lastSentTimeNs ((std::size_t) juce::jmax (0, numParams))
    {
        // Seed both vectors to "never sent" sentinels. atomic<float>
        // has no value-init for non-trivial init, hence the explicit
        // store loop.
        for (auto& v : lastSentValue)  v.store (std::numeric_limits<float>::lowest(),
                                                  std::memory_order_relaxed);
        for (auto& t : lastSentTimeNs) t.store (0, std::memory_order_relaxed);
    }

    void parameterValueChanged (int paramIndex, float newValue) override
    {
        if (host.applyingFromMirror.load (std::memory_order_acquire)) return;
        if (paramIndex < 0 || (std::size_t) paramIndex >= lastSentValue.size()) return;

        // Per-parameter dedup state is std::atomic<...> so that a
        // plugin firing parameterValueChanged from multiple threads
        // simultaneously (rare but real for sloppy hosts and
        // modulation matrices that touch the same param from worker
        // + message thread) does not race-tear lastSentValue /
        // lastSentTimeNs. ThreadSanitizer would flag the previous
        // non-atomic shape; relaxed ordering is sufficient because
        // each slot tracks its own state — no cross-param invariant
        // depends on these reads/writes happening in any particular
        // order.
        constexpr float kMinDelta = 1.0e-4f;
        constexpr std::int64_t kMinIntervalNs =
            std::chrono::duration_cast<std::chrono::nanoseconds> (
                std::chrono::milliseconds (5)).count();

        const auto nowNs = std::chrono::steady_clock::now()
                              .time_since_epoch().count();
        const auto lastNs = lastSentTimeNs[(std::size_t) paramIndex]
                                  .load (std::memory_order_relaxed);
        const float lastV = lastSentValue[(std::size_t) paramIndex]
                                  .load (std::memory_order_relaxed);
        const float delta = std::fabs (newValue - lastV);
        const bool tooSoon = (nowNs - lastNs) < kMinIntervalNs;

        if (delta < kMinDelta && tooSoon) return;

        lastSentValue[(std::size_t) paramIndex]
            .store (newValue, std::memory_order_relaxed);
        lastSentTimeNs[(std::size_t) paramIndex]
            .store (nowNs, std::memory_order_relaxed);

        const auto seq = host.outboundParamSeq.fetch_add (1, std::memory_order_acq_rel) + 1;
        (void) pushParamChangedFromChild (host.channel, paramIndex, newValue, seq);
    }
    void parameterGestureChanged (int, bool) override {}

private:
    HostState& host;
    std::vector<std::atomic<float>>        lastSentValue;
    std::vector<std::atomic<std::int64_t>> lastSentTimeNs;
};
#endif

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

   #if JUCE_MAC
    // Mac-only: install the mirror listener on every parameter so any
    // plugin-initiated change (host automation, preset reload, MIDI
    // CC routed to this instance, modulator output) is pushed back
    // to the parent as ParamChangedFromChild. The parent's shell
    // editor reflects it via PluginSlot::applyShellParamFromChild.
    // applyingFromMirror gates the listener so SetParam echoes from
    // the parent don't loop back.
    //
    // Listener gets a fresh state vector sized to this plugin's
    // parameter count so the 3c-4 rate-limit dedup tracks each
    // param independently. A subsequent LoadPlugin (slot replace)
    // reaches handleRelease first → detach + reset listener → fresh
    // sizing on the next load. Plugins with thousands of params
    // (Diva, Massive X) pay ~12 bytes per param of tracking state —
    // bounded + bounded-lifetime.
    const int paramCount = host.ownedInstance->getParameters().size();
    host.paramListener = std::make_unique<ChildParamListener> (host, paramCount);
    for (auto* p : host.ownedInstance->getParameters())
        if (p != nullptr) p->addListener (host.paramListener.get());
   #endif

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
           #if JUCE_MAC
            // Detach the mirror listener BEFORE releaseResources +
            // instance reset. JUCE stores listeners on the parameter
            // objects (which live inside the AudioProcessor); not
            // removing here would dangle the listener pointer if
            // anything else still referenced the parameter list.
            if (host.paramListener != nullptr)
                for (auto* p : host.ownedInstance->getParameters())
                    if (p != nullptr) p->removeListener (host.paramListener.get());
           #endif
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

// Inbound SetParam from parent (3c-3a). Marshals onto the JUCE message
// thread via callAsync so setValueNotifyingHost runs where JUCE expects
// (the same thread that owns the editor + listener machinery). We
// intentionally do NOT take a MessageManagerLock on the sockThread —
// blocking the sockThread until the message thread is free has caused
// a teardown-time deadlock in prior phases (sockThread holds lock,
// message thread tries to join sockThread during shutdown).
//
// One-shot: NO reply is written, matching the parent's setRemoteParam
// fire-and-forget contract. SetParam frames are demuxed by opcode in
// the sockThread switch below.
void handleSetParamAsync (HostState& host,
                            const std::vector<std::uint8_t>& payload)
{
    if (payload.size() != sizeof (SetParamPayload)) return;
    SetParamPayload p {};
    std::memcpy (&p, payload.data(), sizeof (p));

    juce::MessageManager::callAsync ([&host, p]
    {
        auto* instance = host.currentInstance.load (std::memory_order_acquire);
        if (instance == nullptr) return;
        const auto& params = instance->getParameters();
        if ((int) p.paramIndex >= params.size()) return;
        if (auto* param = params[(int) p.paramIndex])
        {
           #if JUCE_MAC
            // Loop breaker: prevent the Mac child-side listener from
            // echoing this change back to the parent as a
            // ParamChangedFromChild push. Set / clear bracket the
            // synchronous listener-fire chain inside JUCE.
            host.applyingFromMirror.store (true, std::memory_order_release);
           #endif
            param->setValueNotifyingHost (std::min (1.0f, std::max (0.0f, p.value)));
           #if JUCE_MAC
            host.applyingFromMirror.store (false, std::memory_order_release);
           #endif
        }
    });
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

            // SetParam is one-shot per the protocol contract — no reply
            // is written. Skip the switch + sendControlReply path
            // entirely and continue to the next inbound frame.
            if ((OpCode) hdr.op == OpCode::SetParam)
            {
                handleSetParamAsync (host, payload);
                continue;
            }

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
                case OpCode::SetParam:       continue;  // handled above
                case OpCode::ParamChangedFromChild:
                    // Push frames are child → parent only; receiving one
                    // here means the parent shipped junk. Drop + continue.
                    continue;
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

// --- Plugin-scan mode -----------------------------------------------------
// Crash isolation for plugin discovery. The parent (PluginManager's
// OutOfProcessPluginScanner) spawns one of these per candidate file:
//
//   dusk-studio-plugin-host --scan <formatName> <fileOrIdentifier>
//
// We instantiate the plugin just far enough to read its PluginDescription(s)
// and print them as XML between sentinel markers on stdout. If the plugin
// segfaults or hangs in findAllTypesForFile, only THIS process dies — the
// parent times it out / sees the crash, blacklists the file, and keeps
// running. The plugin's own stdout chatter (if any) is emitted by its init
// code, which runs BEFORE we print kScanPayloadBegin, so the parent's
// extract-between-sentinels parse skips it.
//
int runScan (int argc, const char* const* argv) noexcept
{
    int scanIdx = -1;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp (argv[i], "--scan") == 0) { scanIdx = i; break; }

    if (scanIdx < 0 || scanIdx + 2 >= argc)
    {
        std::fprintf (stderr, "[dusk-studio-plugin-host] --scan needs <format> <file>\n");
        return 64;
    }

    // The parent always passes the format name and the file/identifier as two
    // separate StringArray elements, so each arrives as exactly one argv slot
    // (no re-splitting on spaces needed).
    // Brace-init, not paren-init: MSVC parses
    // `const String x (CharPointer_UTF8 (argv[i]))` as a function declaration
    // (most-vexing-parse), then fails because argv[i] isn't a constant array
    // bound. Braces can't be a parameter list, so they disambiguate.
    const juce::String formatName { juce::CharPointer_UTF8 (argv[scanIdx + 1]) };
    const juce::String fileOrId   { juce::CharPointer_UTF8 (argv[scanIdx + 2]) };

    // Some formats post messages to themselves while probing; give them a
    // MessageManager to dispatch to.
    juce::ScopedJuceInitialiser_GUI juceInit;

    juce::AudioPluginFormatManager fm;
    duskstudio::juce_compat::addDefaultFormats (fm);

    juce::AudioPluginFormat* chosen = nullptr;
    for (auto* f : fm.getFormats())
        if (f != nullptr && f->getName() == formatName) { chosen = f; break; }

    if (chosen == nullptr)
    {
        std::fprintf (stderr, "[dusk-studio-plugin-host] unknown format \"%s\"\n",
                      formatName.toRawUTF8());
        return 65;
    }

    juce::OwnedArray<juce::PluginDescription> found;
    chosen->findAllTypesForFile (found, fileOrId);   // may crash/hang — that is the point

    std::fputs (duskstudio::scanproto::makePayload (found).toRawUTF8(), stdout);
    std::fflush (stdout);
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
    bool scan    = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp (argv[i], "--ipc-stub") == 0) ipcStub = true;
        if (std::strcmp (argv[i], "--ipc-host") == 0) ipcHost = true;
        if (std::strcmp (argv[i], "--scan")     == 0) scan    = true;
    }

    if (scan)    return runScan (argc, argv);
    if (ipcStub) return runIpcStub (argc, argv);
    if (ipcHost) return runIpcHost (argc, argv);

    std::fprintf (stderr,
                  "dusk-studio-plugin-host: pass --ipc-stub, --ipc-host or --scan.\n");
    return 64;
}
