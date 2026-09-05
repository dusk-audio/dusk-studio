// dusk-studio-plugin-host - the child binary that owns one out-of-process VST3
// (or LV2) instance on behalf of Dusk Studio's main process. Supported modes
// include:
//
//   --ipc-stub-sudden-death: echo stub that exits without publishing a crash
//                 state on the first block command, modelling a SIGKILLed child.
//   --ipc-stub  : echo input -> output, no JUCE plugin. Exists so the
//                 IPC self-test can validate shm + sync + spawn plumbing
//                 without a plugin in the loop.
//   --ipc-argv-stub: return the UTF-8 argv vector over the inherited channel.
//                 Used to validate Windows UTF-16 launch and quoting.
//   --ipc-silent-handshake-stub: keep the inherited channel open without
//                 acknowledging readiness, for the bounded-read regression.
//   --ipc-handle-probe-stub: report which Windows mapping handles reached the
//                 child, for the explicit inheritance-allowlist regression.
//   --ipc-posix-launch-probe-stub: report descriptor and signal inheritance
//                 state, for the POSIX child-isolation regression.
//   --ipc-control-reply-stub: deterministic request-correlation and reply-
//                 validation regression child.
//   --ipc-host  : full Phase-2 host. Loads a juce::AudioPluginInstance
//                 via the format manager, runs processBlock on a worker
//                 thread, services control RPCs on a separate socket-
//                 reader thread, runs the JUCE message loop on main.
//   --scan      : one-shot crash-isolated plugin discovery. Scans a single
//                 file/identifier for one format, prints versioned Dusk
//                 descriptors as JSON between sentinels, exits. A plugin
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

#include "ParamPushQueue.h"
#include "PluginIpc.h"
#include "PluginScanProtocol.h"
#include "WorkerPark.h"
#if DUSKSTUDIO_HAS_NATIVE_CLAP || DUSKSTUDIO_HAS_NATIVE_VST3
 #include "../NativeScanRows.h"
#endif
#include "../../foundation/AutoResetEvent.h"
#include "../../foundation/MessageThread.h"
#include "../JuceCompat.h"
#include "platform/IpcChannel.h"
#include "platform/IpcShm.h"
#include "platform/IpcSync.h"
#include "platform/WindowsSoftwareOpenGL.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <signal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined (_WIN32)
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

#if defined (__linux__)
 #include <sys/prctl.h>
 #include <unistd.h>
#endif

#if ! defined (_WIN32)
 #include <fcntl.h>
#endif

#if defined (DUSKSTUDIO_USE_WINDOWS_SOFTWARE_OPENGL)
namespace
{
// Normally inherited from DuskStudio.exe, but make direct scan/host launches
// deterministic too: plugins can import opengl32.dll before opening an editor.
const duskstudio::platform::ForcePackagedSoftwareOpenGL forcePackagedSoftwareOpenGL;
}
#endif

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Utf8CommandLine.h"

namespace
{
using namespace duskstudio::ipc;
namespace ipcp = duskstudio::ipc::platform;

#if defined (__linux__)
bool armParentDeathSignal (int argc, char* const* argv) noexcept
{
    constexpr char prefix[] = "--ipc-parent-pid=";
    for (int i = 1; i < argc; ++i)
    {
        if (std::strncmp (argv[i], prefix, sizeof (prefix) - 1) != 0)
            continue;

        char* end = nullptr;
        errno = 0;
        const long long parsed = std::strtoll (argv[i] + sizeof (prefix) - 1,
                                               &end, 10);
        if (errno != 0 || end == nullptr || *end != '\0' || parsed <= 1
            || parsed > (long long) std::numeric_limits<pid_t>::max())
            return false;

        const auto expectedParent = (pid_t) parsed;
        if (::prctl (PR_SET_PDEATHSIG, SIGTERM) != 0)
            return false;

        // The parent can die between posix_spawn() and this prctl(). In that
        // race the kernel cannot deliver the configured signal retroactively,
        // so reject a helper that has already been reparented.
        return ::getppid() == expectedParent;
    }
    return true;
}
#endif

// Single mutex guards every outbound write on the control socket so the
// sockThread's sync-RPC replies cannot interleave with the message thread's
// mirror pushes (writeParamChangedLocked). Both callers are off the audio
// path: the plugin threads that originate a mirror push hand their value to
// ParamPushQueue and never reach this mutex.
std::mutex& channelWriteMutex()
{
    static std::mutex m;
    return m;
}

bool sendControlReply (ipcp::NativeHandle& ch, const ControlMsgHeader& request,
                       std::uint32_t status, const void* payload,
                       std::uint32_t payloadLen) noexcept
{
    ControlMsgHeader hdr {};
    hdr.totalLen   = (std::uint32_t) sizeof (hdr) + payloadLen;
    hdr.op         = request.op;
    hdr.requestId  = request.requestId;
    hdr.status     = status;
    hdr.payloadLen = payloadLen;
    std::lock_guard<std::mutex> lk (channelWriteMutex());
    if (! ipcp::writeExact (ch, &hdr, sizeof (hdr))) return false;
    if (payloadLen > 0 && ! ipcp::writeExact (ch, payload, payloadLen))
        return false;
    return true;
}

// One-shot outbound push, emitted when the plugin changes a value of its own
// (host automation, MIDI-mapped controller, preset reload). Wire format matches
// duskstudio::ipc::ParamChangedPayload. Returns false on socket write failure
// (peer closed), which the caller treats as recoverable-by-relink.
//
// The caller holds channelWriteMutex, so it can decide whether the value is
// still worth sending in the same critical section as the send. That ordering
// is what keeps a value queued by a retired plugin from landing after the
// LoadPlugin reply that replaced it.
[[maybe_unused]] bool writeParamChangedLocked (ipcp::NativeHandle& ch,
                                                  const ParamChangedPayload& in) noexcept
{
    ParamChangedPayload p = in;
    p.value = std::min (1.0f, std::max (0.0f, in.value));

    ControlMsgHeader hdr {};
    hdr.totalLen   = (std::uint32_t) sizeof (hdr) + (std::uint32_t) sizeof (p);
    hdr.op         = (std::uint32_t) OpCode::ParamChangedFromChild;
    hdr.requestId  = 0;
    hdr.status     = 0;
    hdr.payloadLen = (std::uint32_t) sizeof (p);

    if (! ipcp::writeExact (ch, &hdr, sizeof (hdr))) return false;
    if (! ipcp::writeExact (ch, &p, sizeof (p)))    return false;
    return true;
}

int runIpcArgvStub (int argc, const char* const* argv) noexcept
{
    ipcp::NativeHandle channel = ipcp::locateInheritedChannel (argc, argv);
    if (! ipcp::isValid (channel)) return 1;

    const auto count = (std::uint32_t) argc;
    if (! ipcp::writeExact (channel, &count, sizeof (count))) return 1;
    for (int i = 0; i < argc; ++i)
    {
        const auto length = (std::uint32_t) std::strlen (argv[i]);
        if (! ipcp::writeExact (channel, &length, sizeof (length))
            || (length > 0 && ! ipcp::writeExact (channel, argv[i], length)))
            return 1;
    }
    return 0;
}

int runIpcSilentHandshakeStub (int argc, const char* const* argv) noexcept
{
    ipcp::NativeHandle channel = ipcp::locateInheritedChannel (argc, argv);
    if (! ipcp::isValid (channel)) return 1;

    // Stay alive and keep the channel open without writing the ready byte.
    // Closing the parent end after its deadline releases this read so the
    // child can tear down normally rather than needing a forced kill.
    char ignored = 0;
    (void) ipcp::readExact (channel, &ignored, sizeof (ignored));
    return 0;
}

#if defined (_WIN32)
bool findHandleArgument (int argc, const char* const* argv,
                         const char* prefix, HANDLE& handleOut) noexcept
{
    const auto prefixLength = std::strlen (prefix);
    for (int i = 1; i < argc; ++i)
    {
        if (argv[i] == nullptr || std::strncmp (argv[i], prefix, prefixLength) != 0)
            continue;

        const char* const valueText = argv[i] + prefixLength;
        char* end = nullptr;
        errno = 0;
        const auto value = std::strtoull (valueText, &end, 0);
        if (errno != 0 || end == valueText || *end != '\0'
            || value == 0
            || value > (unsigned long long) std::numeric_limits<std::uintptr_t>::max())
            return false;
        handleOut = reinterpret_cast<HANDLE> ((std::uintptr_t) value);
        return true;
    }
    return false;
}

std::uint8_t readMappingMarker (HANDLE handle) noexcept
{
    const void* const view = ::MapViewOfFile (handle, FILE_MAP_READ, 0, 0, 1);
    if (view == nullptr) return 0;
    const auto marker = *static_cast<const std::uint8_t*> (view);
    (void) ::UnmapViewOfFile (view);
    return marker;
}

int runIpcHandleProbeStub (int argc, const char* const* argv) noexcept
{
    ipcp::NativeHandle channel = ipcp::locateInheritedChannel (argc, argv);
    if (! ipcp::isValid (channel)) return 1;

    HANDLE mappingA = nullptr;
    HANDLE mappingB = nullptr;
    if (! findHandleArgument (argc, argv, "--ipc-probe-a=", mappingA)
        || ! findHandleArgument (argc, argv, "--ipc-probe-b=", mappingB))
        return 1;

    const std::uint8_t markers[2] {
        readMappingMarker (mappingA), readMappingMarker (mappingB)
    };
    if (! ipcp::writeExact (channel, markers, sizeof (markers))) return 1;

    // Keep the inherited mapping alive until the owning connection closes.
    char ignored = 0;
    (void) ipcp::readExact (channel, &ignored, sizeof (ignored));
    return 0;
}
#endif

#if ! defined (_WIN32)
bool findFdArgument (int argc, const char* const* argv,
                     const char* prefix, int& fdOut) noexcept
{
    const auto prefixLength = std::strlen (prefix);
    for (int i = 1; i < argc; ++i)
    {
        if (argv[i] == nullptr || std::strncmp (argv[i], prefix, prefixLength) != 0)
            continue;

        const char* const valueText = argv[i] + prefixLength;
        char* end = nullptr;
        errno = 0;
        const long value = std::strtol (valueText, &end, 10);
        if (errno != 0 || end == valueText || *end != '\0'
            || value < 0 || value > std::numeric_limits<int>::max())
            return false;
        fdOut = (int) value;
        return true;
    }
    return false;
}

int runIpcPosixLaunchProbeStub (int argc, const char* const* argv) noexcept
{
    ipcp::NativeHandle channel = ipcp::locateInheritedChannel (argc, argv);
    if (! ipcp::isValid (channel)) return 1;

    int siblingChannelFd = -1;
    int siblingShmFd = -1;
    if (! findFdArgument (argc, argv, "--ipc-probe-channel-fd=", siblingChannelFd)
        || ! findFdArgument (argc, argv, "--ipc-probe-shm-fd=", siblingShmFd))
        return 1;

    sigset_t mask;
    struct sigaction userSignalAction {};
    if (::sigprocmask (SIG_SETMASK, nullptr, &mask) != 0
        || ::sigaction (SIGUSR1, nullptr, &userSignalAction) != 0)
        return 1;

    const std::uint8_t state[4] {
        (std::uint8_t) (::fcntl (siblingChannelFd, F_GETFD) >= 0),
        (std::uint8_t) (::fcntl (siblingShmFd, F_GETFD) >= 0),
        (std::uint8_t) (sigismember (&mask, SIGTERM) == 1),
        (std::uint8_t) (userSignalAction.sa_handler == SIG_DFL)
    };
    return ipcp::writeExact (channel, state, sizeof (state)) ? 0 : 1;
}
#endif

// --- Phase 1 echo mode (kept for the IPC self-test) ----------------------
// How the echo stub answers a block command. SuddenDeath models a SIGKILLed
// child: the process disappears without publishing kStateCrashed, so the
// parent has only its closed control channel to notice by.
enum class StubReplyMode { Normal, Suppress, SuddenDeath };

int runIpcStub (int argc, const char* const* argv, StubReplyMode replyMode) noexcept
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

    ipcp::InterprocessSignal commandSignal;
    ipcp::InterprocessSignal replySignal;
    if (! commandSignal.receiveFromParent (channel)
        || ! replySignal.receiveFromParent (channel))
    {
        std::fprintf (stderr, "[dusk-studio-plugin-host] recv sync handle failed\n");
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
            (void) commandSignal.wait (&hdr->cmdSeq, cmd, nullptr);
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
        if (replyMode == StubReplyMode::SuddenDeath)
            std::_Exit (EXIT_FAILURE);
        if (replyMode == StubReplyMode::Suppress)
            continue;
        hdr->replySeq.store (cmd, std::memory_order_release);
        replySignal.wake (&hdr->replySeq);
    }

    return 0;
}

// Test-only child for control-reply correlation and validation. It withholds
// reply A until request B arrives, then writes A before B. This guarantees the
// late-reply ordering without scheduler sleeps: B cannot be sent until A has
// timed out in the parent, and A cannot be released until B is on the wire.
int runIpcControlReplyStub (int argc, const char* const* argv) noexcept
{
    ipcp::NativeHandle channel = ipcp::locateInheritedChannel (argc, argv);
    if (! ipcp::isValid (channel)) return 1;

    ipcp::NativeHandle shmHandle;
    if (! ipcp::recvHandle (channel, shmHandle)) return 1;

    ipcp::InterprocessSignal commandSignal;
    ipcp::InterprocessSignal replySignal;
    if (! commandSignal.receiveFromParent (channel)
        || ! replySignal.receiveFromParent (channel))
        return 1;

    ipcp::SharedMemory shm;
    std::string error;
    if (! shm.mapInheritedHandle (shmHandle, kTotalSize, error)) return 1;

    auto* block = headerOf (shm.data());
    if (block->magic != kMagic || block->version != kVersion) return 1;

    char ready = 'k';
    if (! ipcp::writeExact (channel, &ready, 1)) return 1;

    auto readRequest = [&channel] (ControlMsgHeader& request,
                                    std::vector<std::uint8_t>& payload)
    {
        if (! ipcp::readExact (channel, &request, sizeof (request))) return false;
        if (request.requestId == 0
            || request.payloadLen > kMaxControlPayload
            || request.totalLen != (std::uint32_t) sizeof (request) + request.payloadLen)
            return false;
        payload.resize (request.payloadLen);
        return request.payloadLen == 0
            || ipcp::readExact (channel, payload.data(), request.payloadLen);
    };

    ControlMsgHeader first {};
    ControlMsgHeader second {};
    std::vector<std::uint8_t> payload;
    if (! readRequest (first, payload) || first.op != (std::uint32_t) OpCode::Ping)
        return 1;
    if (! readRequest (second, payload) || second.op != (std::uint32_t) OpCode::Ping)
        return 1;

    if (! sendControlReply (channel, first, 17, nullptr, 0)
        || ! sendControlReply (channel, second, 0, nullptr, 0))
        return 1;

    ControlMsgHeader oversized {};
    if (! readRequest (oversized, payload)
        || oversized.op != (std::uint32_t) OpCode::LoadPlugin)
        return 1;
    std::vector<std::uint8_t> oversizedReply (sizeof (LoadPluginReply) + 1);
    if (! sendControlReply (channel, oversized, 0,
                            oversizedReply.data(),
                            (std::uint32_t) oversizedReply.size()))
        return 1;

    ControlMsgHeader wrongOpcode {};
    if (! readRequest (wrongOpcode, payload)
        || wrongOpcode.op != (std::uint32_t) OpCode::Ping)
        return 1;
    auto wrongOpcodeReply = wrongOpcode;
    wrongOpcodeReply.op = (std::uint32_t) OpCode::Release;
    if (! sendControlReply (channel, wrongOpcodeReply, 0, nullptr, 0))
        return 1;

    ControlMsgHeader wrongPayload {};
    if (! readRequest (wrongPayload, payload)
        || wrongPayload.op != (std::uint32_t) OpCode::Ping)
        return 1;
    const std::uint8_t unexpectedPayload = 0;
    return sendControlReply (channel, wrongPayload, 0,
                             &unexpectedPayload, sizeof (unexpectedPayload)) ? 0 : 1;
}

// Test-only child shape used by the cross-platform regression suite. It runs
// the real shared-memory worker/control topology with a processor that remains
// inside processBlock longer than the production park deadline. The mutation
// callback aborts if it is ever entered while processing, so receiving the
// explicit park-timeout reply proves the callback was not touched first.
struct ParkTimeoutTestProcessor
{
    std::atomic<bool> processing { false };

    void processBlock()
    {
        processing.store (true, std::memory_order_release);
        std::this_thread::sleep_for (std::chrono::milliseconds (250));
        processing.store (false, std::memory_order_release);
    }

    void mutate()
    {
        if (processing.load (std::memory_order_acquire))
            std::abort();
    }
};

int runIpcParkTimeoutStub (int argc, const char* const* argv) noexcept
{
    ipcp::NativeHandle channel = ipcp::locateInheritedChannel (argc, argv);
    if (! ipcp::isValid (channel)) return 1;

    ipcp::NativeHandle shmHandle;
    if (! ipcp::recvHandle (channel, shmHandle)) return 1;

    ipcp::InterprocessSignal commandSignal;
    ipcp::InterprocessSignal replySignal;
    if (! commandSignal.receiveFromParent (channel)
        || ! replySignal.receiveFromParent (channel))
        return 1;

    ipcp::SharedMemory shm;
    std::string err;
    if (! shm.mapInheritedHandle (shmHandle, kTotalSize, err)) return 1;

    auto* block = headerOf (shm.data());
    if (block->magic != kMagic || block->version != kVersion) return 1;

    char ready = 'k';
    if (! ipcp::writeExact (channel, &ready, 1)) return 1;

    ParkTimeoutTestProcessor processor;
    std::atomic<ParkTimeoutTestProcessor*> currentProcessor { &processor };
    std::atomic<bool> shouldQuit { false };

    std::thread worker ([&]
    {
        std::uint32_t lastSequence = 0;
        while (! shouldQuit.load (std::memory_order_acquire))
        {
            const auto command = block->cmdSeq.load (std::memory_order_acquire);
            if (command == lastSequence)
            {
                (void) commandSignal.wait (&block->cmdSeq, command, nullptr);
                continue;
            }

            if (auto* current = currentProcessor.load (std::memory_order_acquire))
                current->processBlock();

            lastSequence = command;
            block->replySeq.store (command, std::memory_order_release);
            replySignal.wake (&block->replySeq);
        }
    });

    while (true)
    {
        ControlMsgHeader request {};
        if (! ipcp::readExact (channel, &request, sizeof (request))) break;
        if (request.payloadLen > kMaxControlPayload
            || request.totalLen != (std::uint32_t) sizeof (request) + request.payloadLen)
            break;

        std::vector<std::uint8_t> payload (request.payloadLen);
        if (request.payloadLen > 0
            && ! ipcp::readExact (channel, payload.data(), request.payloadLen))
            break;

        std::uint32_t status = 99;
        const auto op = (OpCode) request.op;
        if (op == OpCode::Ping)
        {
            status = processor.processing.load (std::memory_order_acquire) ? 0u : 1u;
        }
        else if (op == OpCode::PrepareToPlay || op == OpCode::Release
                 || op == OpCode::GetState || op == OpCode::SetState)
        {
            const bool parked = duskstudio::ipc::withParkedWorker (
                currentProcessor, block->cmdSeq, block->replySeq,
                [&] { processor.mutate(); });
            if (parked)
                currentProcessor.store (&processor, std::memory_order_release);
            status = parked ? 0u : kControlStatusWorkerParkTimeout;
        }

        if (status == kControlStatusWorkerParkTimeout)
            block->state.store (kStateCrashed, std::memory_order_release);

        const bool replySent = sendControlReply (channel, request, status, nullptr, 0);

        if (status == kControlStatusWorkerParkTimeout)
        {
            replySignal.wake (&block->replySeq);
            std::_Exit (EXIT_FAILURE);
        }
        if (! replySent) break;
    }

    shouldQuit.store (true, std::memory_order_release);
    block->state.store (kStateTeardown, std::memory_order_release);
    commandSignal.wake (&block->cmdSeq);
    worker.join();
    return 0;
}

// --- Phase 2 host mode ---------------------------------------------------

#if JUCE_MAC
class ChildParamListener;  // forward - defined below HostState
#endif

using PluginInstancePtr = std::unique_ptr<juce::AudioPluginInstance>;

struct HostState
{
    ipcp::SharedMemory shm;
    BlockHeader* hdr = nullptr;
    ipcp::NativeHandle channel {};
    ipcp::InterprocessSignal commandSignal;
    ipcp::InterprocessSignal replySignal;

    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownList;

    PluginInstancePtr ownedInstance;
    std::atomic<juce::AudioPluginInstance*> currentInstance { nullptr };

    juce::AudioBuffer<float> workBuffer { kMaxChans, kMaxBlock };

    double currentSampleRate = 0.0;
    int    currentBlockSize  = 0;

    std::atomic<bool> shouldQuit { false };

   #if JUCE_MAC
    // Mirror state (3c-3b, Mac-only - Linux/Windows children don't
    // have a parent-side shell editor to mirror to, so installing
    // listeners + pushing back over the socket would be pure waste).
    //
    // applyingFromMirror: set across handleSetParamAsync's
    // setValueNotifyingHost so the child-side listener doesn't echo
    // the parent's own update back as a ParamChangedFromChild push.
    //
    // outboundParamSeq: monotonic counter stamped into every push.
    // Parent doesn't yet use it (loop-breaker is the flag); allocated
    // for future inflight-tracking. Stamped by the listener, which can
    // fire on several threads at once, hence the atomic increment.
    //
    // paramPushQueue: hand-off from those listener threads to the
    // message thread, which owns the socket write. See ParamPushQueue.h.
    //
    // mirrorGeneration: bumped whenever the loaded instance is detached,
    // so the drain can discard values a retired plugin queued rather than
    // apply them to the same parameter index on its replacement.
    std::atomic<bool>         applyingFromMirror { false };
    std::atomic<std::uint32_t> outboundParamSeq  { 0 };
    std::atomic<std::uint32_t> mirrorGeneration  { 0 };
    ParamPushQueue            paramPushQueue;
    std::unique_ptr<ChildParamListener> paramListener;
   #endif

    // Editor embedding (Phase 4). The plugin's editor lives in this
    // process; the parent either embeds our native window (Linux XEmbed)
    // or lets it float as a native-titlebar toplevel (Win/Mac - see
    // handleShowEditor for the per-platform chrome choice). editorWindow
    // wraps the plugin's AudioProcessorEditor so it has its own native
    // peer, holding it via setContentNonOwned - so this pointer is the
    // owner, and the editor must outlive the window but never the
    // processor (~AudioProcessor asserts on an editor that is still
    // attached). Both are message-thread-only; control handlers dispatch
    // their window work there before replying.
    std::unique_ptr<juce::DocumentWindow>       editorWindow;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
};

// Ceiling on one socket-thread to message-thread round trip. Every RPC that
// makes one has a parent-side deadline of at least 5 s, and Release makes two
// back to back, so 2 s each keeps the handler inside that budget with a second
// left for the reply to travel. Unbounded, a message thread that stopped
// dispatching - an editor spinning a nested modal loop, or a post that landed
// after stopDispatchLoop - parks the child's only control reader for good, and
// every later RPC then burns its own timeout instead.
constexpr int kHostMessageThreadWaitMs = 2000;

// ShowEditor is the one hop whose work is legitimately slow: a large plugin
// builds its whole GUI inside createEditorIfNeeded. Its parent deadline is
// 10 s, so it gets a longer ceiling of its own.
constexpr int kHostEditorCreateWaitMs = 8000;

enum class HostDispatchResult
{
    Done,      // the work ran to completion on the message thread
    Failed,    // the post was refused, or the work threw
    TimedOut   // nothing came back before the ceiling - the child is wedged
};

template <typename Fn>
HostDispatchResult runOnHostMessageThreadAndWait (
    Fn&& fn, int timeoutMs = kHostMessageThreadWaitMs)
{
    // Shared with the posted work rather than held on this frame: a timed-out
    // call returns while the message thread may still run that work later, and
    // it must not signal an event the socket thread has already destroyed.
    struct Shared
    {
        dusk::AutoResetEvent completion;
        bool                 succeeded = false;
    };
    auto shared = std::make_shared<Shared>();

    const bool posted = dusk::callAsync (
        [shared, work = std::forward<Fn> (fn)]() mutable
        {
            try
            {
                work();
                shared->succeeded = true;
            }
            catch (...)
            {
                // A plugin exception must still release the waiting socket thread.
            }
            shared->completion.signal();
        });
    if (! posted) return HostDispatchResult::Failed;
    if (! shared->completion.wait (timeoutMs)) return HostDispatchResult::TimedOut;
    return shared->succeeded ? HostDispatchResult::Done : HostDispatchResult::Failed;
}

// Message thread only. The wrapper window goes first so no peer is left
// dispatching paint or mouse events into the editor, then the editor itself -
// deleting it is what clears the processor's active editor.
void destroyEditorOnMessageThread (HostState& host)
{
    if (host.editorWindow != nullptr)
    {
        host.editorWindow->clearContentComponent();
        host.editorWindow.reset();
    }
    host.editor.reset();
}

// Message thread only: releaseResources, then the destructor at scope exit.
void destroyInstanceOnMessageThread (PluginInstancePtr victim)
{
    if (victim != nullptr)
        victim->releaseResources();
}

// Socket-thread entry points for the two teardowns above. Every path that drops
// an instance goes through both, in this order: shutdown phase 3b sends Release
// while the parent still embeds the child's editor window, so an editor left
// alive would keep painting into a freed processor. Waiting for the editor
// teardown also flushes any editor work the message thread still has queued -
// including a ShowEditor that timed out - so the instance can only be mutated
// once nothing on the message thread is still using it.
HostDispatchResult destroyEditorFromSocketThread (HostState& host)
{
    return runOnHostMessageThreadAndWait ([&host] { destroyEditorOnMessageThread (host); });
}

HostDispatchResult destroyInstanceFromSocketThread (PluginInstancePtr victim)
{
    if (victim == nullptr) return HostDispatchResult::Done;

    // The posted work owns the instance outright: after a timeout this call
    // returns while the message thread may still run later, so the pointer
    // cannot stay tied to the socket thread's frame. shared_ptr because
    // callAsync's std::function needs a copyable callable.
    auto owned = std::make_shared<PluginInstancePtr> (std::move (victim));
    return runOnHostMessageThreadAndWait (
        [owned] { destroyInstanceOnMessageThread (std::move (*owned)); });
}

#if JUCE_MAC
// Drain cadence for the mirror queue. Matches the listener's per-param
// min-interval below, so moving the socket write onto the message thread
// costs at most one extra tick of mirror latency.
constexpr int kParamMirrorDrainHz = 200;

// Listener installed on every parameter of the loaded DSP instance.
// Fires on whichever thread the plugin chose to call setValueNotifyingHost
// on - host automation lanes (the audio worker, from inside processBlock),
// preset-load callbacks (message thread), MIDI-mapped controllers
// (sockThread via our SetParam -> callAsync path) - and JUCE promises
// nothing about which, or about only one at a time.
//
// So this callback must not take channelWriteMutex or touch the socket.
// Doing either stalled the audio worker of any plugin that emits automation
// every block - on a mutex, then on a write() - while the parent's audio
// thread sat parked on replySeq, which the parent timed out and latched as a
// sticky auto-bypass. The callback stamps a sequence number and hands the
// value to ParamPushQueue; ParamMirrorDrain writes it from the message
// thread.
//
// applyingFromMirror is checked first so a parent -> child SetParam
// doesn't echo back as a push (handleSetParamAsync sets the flag
// across its setValueNotifyingHost call).
//
// 3c-4 rate-limit: high-density modulation matrices (LFO rates, FM
// depth automation, complex MPE controllers) can fire hundreds of
// listener callbacks per audio block. Saturating the control socket
// stutters the parent's message thread because callAsync queues
// build up faster than the message loop drains. Per-param
// deduplication + a min-interval gate caps the push rate at ~200 Hz
// per param - well above any human-perceptible knob-twiddle rate,
// well below the worst-case automation flood.
class ChildParamListener final : public juce::AudioProcessorParameter::Listener
{
public:
    ChildParamListener (HostState& h, int numParams, std::uint32_t mirrorGeneration)
        : host (h),
          lastSentValue ((std::size_t) std::max (0, numParams)),
          lastSentTimeNs ((std::size_t) std::max (0, numParams)),
          generation (mirrorGeneration)
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
        // each slot tracks its own state - no cross-param invariant
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
        host.paramPushQueue.push ({ { (std::uint32_t) paramIndex, newValue, seq },
                                    generation });
    }
    void parameterGestureChanged (int, bool) override {}

private:
    HostState& host;
    std::vector<std::atomic<float>>        lastSentValue;
    std::vector<std::atomic<std::int64_t>> lastSentTimeNs;
    const std::uint32_t                    generation;
};

// Consumer side of the mirror. Lives on the child's message thread for the
// life of the host, so a plugin swap never has to stop or restart it.
class ParamMirrorDrain final : public dusk::Timer
{
public:
    explicit ParamMirrorDrain (HostState& h) noexcept : host (h) {}

private:
    void timerCallback() override
    {
        ParamPushRecord queued {};
        for (std::size_t i = 0; i < ParamPushQueue::kCapacity; ++i)
        {
            if (! host.paramPushQueue.pop (queued)) break;

            // Both the generation bump and the LoadPlugin reply happen on the
            // sockThread, bump first, and the reply takes this same mutex. So
            // a value the retired plugin queued either goes out before that
            // reply - where it still describes the plugin the parent has - or
            // is seen here as stale and dropped.
            std::lock_guard<std::mutex> lk (channelWriteMutex());
            if (queued.generation != host.mirrorGeneration.load (std::memory_order_acquire))
                continue;
            if (! writeParamChangedLocked (host.channel, queued.payload))
                break;
        }
    }

    HostState& host;
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
// Mechanism: clear currentInstance so the worker no-ops on any new cmdSeq
// bump, then wait until cmdSeq == replySeq (worker has drained every command
// it had in flight). A worker that misses the bounded deadline is unsafe to
// mutate: leave the pointer parked, report a terminal RPC error, and let the
// control loop exit the child after sending that reply.
template <typename Fn>
std::uint32_t withParkedHostWorker (HostState& host, Fn&& fn)
{
    if (! duskstudio::ipc::withParkedWorker (
            host.currentInstance, host.hdr->cmdSeq, host.hdr->replySeq, fn))
    {
        host.hdr->state.store (kStateCrashed, std::memory_order_release);
        return kControlStatusWorkerParkTimeout;
    }
    host.currentInstance.store (host.ownedInstance.get(), std::memory_order_release);
    return 0;
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

    // Build the replacement before disturbing the live instance: a creation
    // failure then leaves the slot exactly as it was rather than parked on a
    // null pointer with an orphaned ownedInstance, which silences the slot
    // until some later RPC happens to republish it.
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
    juce::PluginDescription loadedDescription;
    fresh->fillInPluginDescription (loadedDescription);
    reply.isInstrument = loadedDescription.isInstrument ? 1u : 0u;

    // The editor belongs to the instance about to be deposed, so it goes
    // before the swap - and before anything touches ownedInstance.
    if (host.ownedInstance != nullptr
        && destroyEditorFromSocketThread (host) == HostDispatchResult::TimedOut)
    {
        const char* err = "child message thread wedged during LoadPlugin";
        replyOut.assign (err, err + std::strlen (err));
        (void) destroyInstanceFromSocketThread (std::move (fresh));
        return kControlStatusMessageThreadTimeout;
    }

    // Swap under the park. Nulling currentInstance alone does not stop a
    // worker that has already loaded the pointer, so destroying the deposed
    // instance outside the park frees an AudioPluginInstance under
    // processBlock. The deposed instance is destroyed after the park ends -
    // the worker can only reach the republished pointer by then.
    PluginInstancePtr deposed;
    const auto parkStatus = withParkedHostWorker (host, [&]
    {
       #if JUCE_MAC
        if (host.ownedInstance != nullptr && host.paramListener != nullptr)
            for (auto* p : host.ownedInstance->getParameters())
                if (p != nullptr) p->removeListener (host.paramListener.get());
        host.paramListener.reset();
        host.mirrorGeneration.fetch_add (1, std::memory_order_release);
       #endif
        deposed = std::move (host.ownedInstance);
        host.ownedInstance = std::move (fresh);
        host.currentSampleRate = hdr.sampleRate;
        host.currentBlockSize  = hdr.blockSize;
    });
    if (parkStatus != 0)
    {
        const char* err = "worker park timed out during LoadPlugin";
        replyOut.assign (err, err + std::strlen (err));
        (void) destroyInstanceFromSocketThread (std::move (fresh));
        return parkStatus;
    }

    replyOut.resize (sizeof (reply));
    std::memcpy (replyOut.data(), &reply, sizeof (reply));

    // A wedged message thread here does not undo the load: the swap happened
    // and the reply describes the live instance, so the deposed one rides the
    // message queue and the RPC still reports success.
    (void) destroyInstanceFromSocketThread (std::move (deposed));

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
    // param independently. The swap above detaches and drops the
    // previous listener under the park, so this is always a fresh
    // sizing. Plugins with thousands of params (Diva, Massive X) pay
    // ~12 bytes per param of tracking state - bounded +
    // bounded-lifetime.
    const int paramCount = host.ownedInstance->getParameters().size();
    host.paramListener = std::make_unique<ChildParamListener> (
        host, paramCount, host.mirrorGeneration.load (std::memory_order_acquire));
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
    return withParkedHostWorker (host, [&]
    {
        host.ownedInstance->prepareToPlay (p.sampleRate, p.blockSize);
        host.currentSampleRate = p.sampleRate;
        host.currentBlockSize  = p.blockSize;
    });
}

std::uint32_t handleRelease (HostState& host)
{
    if (destroyEditorFromSocketThread (host) == HostDispatchResult::TimedOut)
        return kControlStatusMessageThreadTimeout;

    PluginInstancePtr deposed;
    const auto parkStatus = withParkedHostWorker (host, [&]
    {
        if (host.ownedInstance != nullptr)
        {
           #if JUCE_MAC
            // Detach the mirror listener BEFORE the instance leaves
            // ownedInstance. JUCE stores listeners on the parameter
            // objects (which live inside the AudioProcessor); not
            // removing here would dangle the listener pointer if
            // anything else still referenced the parameter list.
            if (host.paramListener != nullptr)
                for (auto* p : host.ownedInstance->getParameters())
                    if (p != nullptr) p->removeListener (host.paramListener.get());
            host.mirrorGeneration.fetch_add (1, std::memory_order_release);
           #endif
            deposed = std::move (host.ownedInstance);
        }
    });
    if (parkStatus != 0) return parkStatus;

    if (destroyInstanceFromSocketThread (std::move (deposed)) == HostDispatchResult::TimedOut)
        return kControlStatusMessageThreadTimeout;
    return 0;
}

std::uint32_t handleGetState (HostState& host,
                                std::vector<std::uint8_t>& replyOut)
{
    if (host.ownedInstance == nullptr) return 1;
    juce::MemoryBlock mb;
    const auto parkStatus = withParkedHostWorker (host, [&]
    {
        const juce::MessageManagerLock mml;
        host.ownedInstance->getStateInformation (mb);
    });
    if (parkStatus != 0) return parkStatus;
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
    const auto parkStatus = withParkedHostWorker (host, [&]
    {
        const juce::MessageManagerLock mml;
        host.ownedInstance->setStateInformation (
            static_cast<const char*> (host.shm.data()) + kStateOffset, (int) sz);
    });
    return parkStatus;
}

// Reply state for an editor RPC. Shared with the posted work instead of living
// on the socket thread's frame: a timed-out call returns, and the message
// thread may still run that work afterwards.
struct EditorCallResult
{
    std::uint32_t             status = 0;
    std::vector<std::uint8_t> reply;
};

std::uint32_t handleShowEditor (HostState& host,
                                  std::vector<std::uint8_t>& replyOut)
{
    if (host.ownedInstance == nullptr) return 1;

    auto result = std::make_shared<EditorCallResult>();
    const auto dispatched = runOnHostMessageThreadAndWait ([&host, result]
    {
        // A Release can land between the post and the run, so the instance is
        // re-checked here rather than trusted from the socket thread.
        if (host.ownedInstance == nullptr)
        {
            result->status = 1;
            return;
        }

        ShowEditorReply reply {};

        if (host.editor == nullptr)
        {
            host.editor.reset (host.ownedInstance->createEditorIfNeeded());
            if (host.editor == nullptr)
            {
                result->status = 2;
                return;
            }
        }

        if (host.editorWindow == nullptr)
        {
            // The plugin's name labels the floating window on Win/Mac so the
            // user can tell it apart from other windows when the OOP editor
            // isn't embedded.
            auto win = std::make_unique<juce::DocumentWindow> (
                host.ownedInstance->getName(),
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
            win->setContentNonOwned (host.editor.get(), true);
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
        if (reply.windowId == 0)
        {
            result->status = 3;
            return;
        }

        result->reply.resize (sizeof (reply));
        std::memcpy (result->reply.data(), &reply, sizeof (reply));
        result->status = 0;
    }, kHostEditorCreateWaitMs);

    if (dispatched == HostDispatchResult::TimedOut)
        return kControlStatusMessageThreadTimeout;
    if (dispatched == HostDispatchResult::Failed) return 4;
    replyOut = std::move (result->reply);
    return result->status;
}

std::uint32_t handleHideEditor (HostState& host)
{
    const auto dispatched = destroyEditorFromSocketThread (host);
    if (dispatched == HostDispatchResult::TimedOut)
        return kControlStatusMessageThreadTimeout;
    return dispatched == HostDispatchResult::Done ? 0u : 1u;
}

std::uint32_t handleResizeEditor (HostState& host,
                                    const std::vector<std::uint8_t>& payload)
{
    if (payload.size() != sizeof (ResizeEditorPayload)) return 1;
    ResizeEditorPayload p {};
    std::memcpy (&p, payload.data(), sizeof (p));

    auto result = std::make_shared<EditorCallResult>();
    result->status = 2;
    const auto dispatched = runOnHostMessageThreadAndWait ([&host, p, result]
    {
        if (host.editorWindow == nullptr) return;
        host.editorWindow->setSize (std::max (1, (int) p.width),
                                    std::max (1, (int) p.height));
        result->status = 0;
    });

    if (dispatched == HostDispatchResult::TimedOut)
        return kControlStatusMessageThreadTimeout;
    if (dispatched == HostDispatchResult::Failed) return 3;
    return result->status;
}

// Inbound SetParam from parent (3c-3a). Marshals onto the JUCE message
// thread via callAsync so setValueNotifyingHost runs where JUCE expects
// (the same thread that owns the editor + listener machinery). We
// intentionally do NOT take a MessageManagerLock on the sockThread -
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

    dusk::callAsync ([&host, p]
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
            (void) host.commandSignal.wait (&host.hdr->cmdSeq, cmd, nullptr);
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
            const int bufCh = std::max (ci, co);
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
        host.replySignal.wake (&host.hdr->replySeq);
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
    if (! host.commandSignal.receiveFromParent (host.channel)
        || ! host.replySignal.receiveFromParent (host.channel))
    {
        std::fprintf (stderr, "recv sync handle failed\n");
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
            if (hdr.payloadLen > kMaxControlPayload) break;  // refuse oversized alloc
            // Header self-consistency: both writers set totalLen = sizeof(hdr) +
            // payloadLen, so a frame where they disagree is a framing bug or
            // corruption - drop the link rather than trust a malformed header.
            if (hdr.totalLen != (std::uint32_t) sizeof (hdr) + hdr.payloadLen) break;
            std::vector<std::uint8_t> payload (hdr.payloadLen);
            if (hdr.payloadLen > 0
                && ! ipcp::readExact (host.channel, payload.data(), hdr.payloadLen))
                break;

            std::vector<std::uint8_t> reply;
            std::uint32_t status = 0;

            // SetParam is one-shot per the protocol contract - no reply
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
                    // Push frames are child -> parent only; receiving one
                    // here means the parent shipped junk. Drop + continue.
                    continue;
                default:                     status = 99; break;
            }

            const bool replySent = sendControlReply (
                host.channel, hdr, status,
                reply.empty() ? nullptr : reply.data(),
                (std::uint32_t) reply.size());

            if (status == kControlStatusWorkerParkTimeout)
            {
                host.replySignal.wake (&host.hdr->replySeq);
                std::_Exit (EXIT_FAILURE);
            }
            if (! replySent) break;
        }
        host.shouldQuit.store (true, std::memory_order_release);
        host.hdr->state.store (kStateTeardown, std::memory_order_release);
        host.commandSignal.wake (&host.hdr->cmdSeq);
        juce::MessageManager::getInstance()->stopDispatchLoop();
    });

   #if JUCE_MAC
    ParamMirrorDrain paramDrain (host);
    paramDrain.startTimerHz (kParamMirrorDrainHz);
   #endif

    juce::MessageManager::getInstance()->runDispatchLoop();

    sockThread.join();
    worker.join();

    // The dispatch loop has returned, so this thread is the message thread and
    // a post would queue work nothing will ever run: tear down directly, editor
    // first, exactly as the socket-thread paths do.
    destroyEditorOnMessageThread (host);
    destroyInstanceOnMessageThread (std::move (host.ownedInstance));

    return 0;
}

// --- Plugin-scan mode -----------------------------------------------------
// Crash isolation for plugin discovery. The parent (PluginManager's
// OutOfProcessPluginScanner) spawns one of these per candidate file:
//
//   dusk-studio-plugin-host --scan <formatName> <fileOrIdentifier>
//
// We instantiate the plugin just far enough to read its PluginDescription(s)
// and print Dusk descriptors between sentinel markers on stdout. If the plugin
// segfaults or hangs in findAllTypesForFile, only THIS process dies - the
// parent times it out / sees the crash, blacklists the file, and keeps
// running. The plugin's own stdout chatter (if any) is emitted by its init
// code, which runs BEFORE we print kScanPayloadBegin, so the parent's
// extract-between-sentinels parse skips it.
//
std::vector<duskstudio::PluginDescriptor> descriptorsFromJuce (
    const juce::OwnedArray<juce::PluginDescription>& found)
{
    std::vector<duskstudio::PluginDescriptor> rows;
    rows.reserve ((size_t) found.size());
    for (const auto* source : found)
    {
        if (source == nullptr) continue;
        duskstudio::PluginDescriptor descriptor;
        descriptor.name = source->name.toStdString();
        descriptor.descriptiveName = source->descriptiveName.toStdString();
        descriptor.manufacturer = source->manufacturerName.toStdString();
        descriptor.category = source->category.toStdString();
        descriptor.version = source->version.toStdString();
        descriptor.formatName = source->pluginFormatName.toStdString();
        descriptor.backend = duskstudio::PluginBackend::JuceLegacy;
        descriptor.location = source->fileOrIdentifier.toStdString();
        descriptor.uniqueId = source->uniqueId;
        descriptor.deprecatedUid = source->deprecatedUid;
        descriptor.numInputChannels = source->numInputChannels;
        descriptor.numOutputChannels = source->numOutputChannels;
        descriptor.lastFileModificationMs = source->lastFileModTime.toMilliseconds();
        descriptor.lastInfoUpdateMs = source->lastInfoUpdateTime.toMilliseconds();
        descriptor.isInstrument = source->isInstrument;
        descriptor.hasSharedContainer = source->hasSharedContainer;
        descriptor.hasAraExtension = source->hasARAExtension;
        rows.push_back (std::move (descriptor));
    }
    return rows;
}

#if DUSKSTUDIO_HAS_NATIVE_CLAP || DUSKSTUDIO_HAS_NATIVE_VST3
// Sandboxed native-format discovery: load ONE bundle through the native host's
// own loader and print the resulting picker rows between the scan sentinels.
// Same crash-isolation contract as --scan: a broken .so kills this process,
// the parent times out / sees no payload and skips the bundle.
int runScanNative (int argc, const char* const* argv) noexcept
{
    int idx = -1;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp (argv[i], "--scan-native") == 0) { idx = i; break; }
    if (idx < 0 || idx + 2 >= argc)
    {
        std::fprintf (stderr, "[dusk-studio-plugin-host] --scan-native needs <clap|vst3> <bundle>\n");
        return 64;
    }
    const juce::String format { juce::CharPointer_UTF8 (argv[idx + 1]) };
    const juce::File   bundle { juce::String (juce::CharPointer_UTF8 (argv[idx + 2])) };

    juce::ScopedJuceInitialiser_GUI juceInit;

    std::vector<duskstudio::PluginDescriptor> rows;
    const auto bundlePath = std::filesystem::u8path (
        bundle.getFullPathName().toStdString());
#if DUSKSTUDIO_HAS_NATIVE_CLAP
    if (format == "clap") duskstudio::nativescan::appendClapRows (bundlePath, rows);
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
    if (format == "vst3") duskstudio::nativescan::appendVst3Rows (bundlePath, rows);
#endif

    const auto payload = duskstudio::scanproto::makePayload (rows);
    std::fwrite (payload.data(), 1, payload.size(), stdout);
    std::fflush (stdout);
    return 0;
}
#endif // native formats

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
    chosen->findAllTypesForFile (found, fileOrId);   // may crash/hang - that is the point

    const auto payload = duskstudio::scanproto::makePayload (
        descriptorsFromJuce (found));
    std::fwrite (payload.data(), 1, payload.size(), stdout);
    std::fflush (stdout);
    return 0;
}
} // namespace

int main (int argc, char** argv)
{
   #if defined (__linux__)
    if (! armParentDeathSignal (argc, argv)) return 70;
   #endif

   #if ! defined (_WIN32)
    signal (SIGPIPE, SIG_IGN);
   #endif

    const char* const* args = argv;
   #if defined (_WIN32)
    const duskstudio::ipc::Utf8CommandLine utf8CommandLine;
    if (utf8CommandLine.argc() > 0)
    {
        argc = utf8CommandLine.argc();
        args = utf8CommandLine.argv();
    }
   #endif

    bool ipcStub = false;
    bool ipcStubTimeout = false;
    bool ipcStubSuddenDeath = false;
    bool ipcArgvStub = false;
    bool ipcSilentHandshakeStub = false;
   #if defined (_WIN32)
    bool ipcHandleProbeStub = false;
   #else
    bool ipcPosixLaunchProbeStub = false;
   #endif
    bool ipcControlReplyStub = false;
    bool ipcParkTimeoutStub = false;
    bool ipcHost = false;
    bool scan    = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp (args[i], "--ipc-stub") == 0) ipcStub = true;
        if (std::strcmp (args[i], "--ipc-stub-timeout") == 0) ipcStubTimeout = true;
        if (std::strcmp (args[i], "--ipc-stub-sudden-death") == 0)
            ipcStubSuddenDeath = true;
        if (std::strcmp (args[i], "--ipc-argv-stub") == 0) ipcArgvStub = true;
        if (std::strcmp (args[i], "--ipc-silent-handshake-stub") == 0)
            ipcSilentHandshakeStub = true;
       #if defined (_WIN32)
        if (std::strcmp (args[i], "--ipc-handle-probe-stub") == 0)
            ipcHandleProbeStub = true;
       #else
        if (std::strcmp (args[i], "--ipc-posix-launch-probe-stub") == 0)
            ipcPosixLaunchProbeStub = true;
       #endif
        if (std::strcmp (args[i], "--ipc-control-reply-stub") == 0) ipcControlReplyStub = true;
        if (std::strcmp (args[i], "--ipc-park-timeout-stub") == 0) ipcParkTimeoutStub = true;
        if (std::strcmp (args[i], "--ipc-host") == 0) ipcHost = true;
        if (std::strcmp (args[i], "--scan")     == 0) scan    = true;
#if DUSKSTUDIO_HAS_NATIVE_CLAP || DUSKSTUDIO_HAS_NATIVE_VST3
        if (std::strcmp (args[i], "--scan-native") == 0) return runScanNative (argc, args);
#endif
    }

    if (scan)    return runScan (argc, args);
    if (ipcArgvStub) return runIpcArgvStub (argc, args);
    if (ipcSilentHandshakeStub) return runIpcSilentHandshakeStub (argc, args);
   #if defined (_WIN32)
    if (ipcHandleProbeStub) return runIpcHandleProbeStub (argc, args);
   #else
    if (ipcPosixLaunchProbeStub) return runIpcPosixLaunchProbeStub (argc, args);
   #endif
    if (ipcStub || ipcStubTimeout || ipcStubSuddenDeath)
        return runIpcStub (argc, args,
                           ipcStubSuddenDeath ? StubReplyMode::SuddenDeath
                           : ipcStubTimeout   ? StubReplyMode::Suppress
                                              : StubReplyMode::Normal);
    if (ipcControlReplyStub) return runIpcControlReplyStub (argc, args);
    if (ipcParkTimeoutStub) return runIpcParkTimeoutStub (argc, args);
    if (ipcHost) return runIpcHost (argc, args);

    std::fprintf (stderr,
                  "dusk-studio-plugin-host: pass --ipc-stub, --ipc-host or --scan.\n");
    return 64;
}
