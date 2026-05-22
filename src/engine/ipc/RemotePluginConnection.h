#pragma once

#include "PluginIpc.h"
#include "platform/IpcChannel.h"
#include "platform/IpcProcess.h"
#include "platform/IpcShm.h"
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>  // juce::MidiBuffer

namespace duskstudio::ipc
{
// Parent-side connection to a dusk-studio-plugin-host child process. Owns:
//   • the SHM file descriptor (memfd_create) and its mmap.
//   • the child PID + fork/exec + waitpid lifecycle.
//   • the futex round-trip that drives processBlockSync.
//
// Phase 1 surface is intentionally minimal: just construct, run a stub
// echo round-trip, tear down. Plugin loading, state save/restore, editor
// embedding, and crash recovery come in subsequent phases — they're
// stubbed here so callers in Phase 2+ have stable signatures.
//
// Threading rules:
//   • construct / connect / disconnect       — message thread only.
//   • processBlockSync                       — audio thread, RT-safe
//     (no allocations, no syscalls beyond the futex pair).
//   • isCrashed                              — any thread, atomic load.
class RemotePluginConnection
{
public:
    RemotePluginConnection() = default;
    ~RemotePluginConnection();

    RemotePluginConnection (const RemotePluginConnection&) = delete;
    RemotePluginConnection& operator= (const RemotePluginConnection&) = delete;

    // Fork + exec the child binary at `hostExecutablePath`, pass it the
    // memfd via SCM_RIGHTS over a socketpair, wait for the child's
    // "ready" handshake. Returns false on any failure with `errorOut`
    // populated. Idempotent: a second connect() on a live connection is
    // a no-op.
    //
    // `extraArg` selects the host's mode: "--ipc-stub" runs the
    // dependency-light Phase-1 echo loop (used by the IPC self-test),
    // "--ipc-host" runs the full JUCE-backed plugin host (Phase 2+).
    bool connect (const std::string& hostExecutablePath,
                   const std::string& extraArg,
                   std::string& errorOut);

    // --- Control plane (Phase 2 - message-thread only) -------------------
    // Each of these does a synchronous request/reply over the control
    // socket. Not RT-safe; not callable from the audio thread.

    // Heartbeat round-trip. Returns true if the child responds within
    // `timeoutMs`. Used by the supervisor heartbeat in Phase 4.
    bool ping (int timeoutMs, std::string& errorOut);

    // Load a plugin from its `juce::PluginDescription::createXml()`
    // string at the given sample rate / block size. Fills `numInOut`
    // with the plugin's reported channel layout and `latencyOut` with
    // its reported latency in samples.
    bool loadPlugin (const std::string& pluginDescriptionXml,
                      double sampleRate, int blockSize,
                      int& numInOut, int& numOutOut, int& latencyOut,
                      std::string& errorOut);

    // Re-prepare the loaded plugin (sample-rate or block-size change).
    bool prepareToPlay (double sampleRate, int blockSize,
                         std::string& errorOut);

    // Unload the current plugin. Idempotent - a release on an empty
    // host succeeds silently.
    bool release (std::string& errorOut);

    // Plugin-state save / restore. The blob travels through the SHM
    // staging area, not the socket, to avoid exhausting the socket
    // buffer for heavy plugins.
    bool getState (std::vector<std::uint8_t>& blobOut, std::string& errorOut);
    bool setState (const std::uint8_t* data, std::size_t size,
                    std::string& errorOut);

    // Editor RPCs (Phase 4). The child wraps the plugin's editor in its
    // own borderless toplevel and reports the native window ID; the
    // parent embeds that window via XEmbed.
    //
    // showEditor: tell the child to create / show the plugin's editor
    // and report its native window ID + initial size. windowIdOut is
    // a host-OS native handle (X11 Window on Linux, cast through
    // std::uint64_t for portability).
    bool showEditor (std::uint64_t& windowIdOut, int& widthOut, int& heightOut,
                      std::string& errorOut);

    // hideEditor: tell the child to drop the editor toplevel (and its
    // peer). Idempotent; calling on a hidden editor is a no-op success.
    bool hideEditor (std::string& errorOut);

    // resizeEditor: parent's embedding window changed size; resize the
    // child's editor wrapper to match so the plugin sees a resized()
    // callback. Width/height are the inner content size in points.
    bool resizeEditor (int width, int height, std::string& errorOut);

    // Send the audio + MIDI buffers in shared memory, signal the child,
    // wait up to `timeoutNs` nanoseconds for the reply. On timeout the
    // connection's `crashed` flag is set and the function returns false
    // (caller should engage bypass). On success the audio output buffer
    // in SHM is filled and ready to read, and `midi` has been overwritten
    // with whatever the plugin emitted (matching JUCE's processBlock
    // contract — input MIDI is consumed, output MIDI replaces it).
    //
    // Audio data is `numIn` channel pointers each `numSamples` long;
    // they're memcpy'd into the SHM input region. After successful
    // return, the caller can read from `audioOutChannel(...)` for
    // `numOut` channels.
    //
    // MIDI is serialised in the same wire format the dusk-studio-plugin-host
    // child uses: each event is `[int sample][uint16 len][bytes]`, total
    // capped at PluginIpc::kMidiBytes (16 KB). Events that don't fit are
    // dropped — same behaviour as the child's serialiser.
    //
    // RT-safe: only memcpy + futex syscalls, no allocation. The output
    // MIDI buffer is rebuilt via clear() + addEvent() into the caller's
    // `midi`; juce::MidiBuffer::clear is RT-safe and addEvent only
    // allocates when capacity is exceeded — the engine pre-sizes per-
    // track scratch buffers so this stays allocation-free in practice.
    // numOut is the plugin's output channel count (queried at scan/load
    // and stored in PluginSlot::remoteNumOut). The child copies its
    // plugin's processBlock output into the first `numOut` SHM channels;
    // the parent then reads via readOutChannel(0..numOut-1). Passing the
    // wrong numOut breaks instrument plugins (numIn=0, numOut=2): with
    // numIn used as numOut, the child writes 0 channels and the parent
    // reads stale SHM.
    bool processBlockSync (const float* const* inChannels, int numIn, int numOut,
                            int numSamples,
                            juce::MidiBuffer& midi,
                            long long timeoutNs) noexcept;

    // Read the i'th output channel from SHM. Valid pointer for the life
    // of the connection. Audio thread calls this after processBlockSync
    // returns true.
    const float* readOutChannel (int chan) const noexcept
    {
        return audioOutChannel (shm.data(), chan);
    }

    // True once the child has been declared dead (timeout, crash, or
    // explicit disconnect). Sticky for the life of the connection.
    bool isCrashed() const noexcept { return crashed.load (std::memory_order_acquire); }

    // Non-blocking child-exit check. Calls waitpid(childPid, &status,
    // WNOHANG); if the child has exited, sets `crashed` and returns
    // true. Returns false if the child is still alive or the connection
    // isn't connected. Idempotent — once crashed, returns false on
    // subsequent calls because childPid has been reaped (set to -1).
    //
    // Designed to be called from a low-frequency message-thread timer
    // (~1 Hz). Catches the case where the child dies while the audio
    // thread isn't actively running processBlockSync — without this
    // the slot would still appear loaded until the user resumed
    // playback and the audio thread's futex deadline tripped.
    //
    // Message thread only.
    bool pollReaper() noexcept;

    // Tear down. Idempotent. Sends SIGTERM to the child, waits briefly,
    // then SIGKILL. Unmaps SHM, closes fds.
    void disconnect();

    // Total round-trip blocks completed since connect() returned true.
    // Used by the self-test to assert progress; not RT-relevant.
    std::uint64_t getRoundTripCount() const noexcept
    {
        return roundTrips.load (std::memory_order_relaxed);
    }

private:
    platform::SharedMemory  shm;
    platform::ChildProcess  child;
    platform::NativeHandle  controlChannel {};

    std::uint32_t   localSeq { 0 };
    std::atomic<std::uint64_t> roundTrips { 0 };
    std::atomic<bool> crashed { false };
};
} // namespace duskstudio::ipc
