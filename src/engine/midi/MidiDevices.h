#pragma once

#include "MidiBackend.h"
#include "../../foundation/AutoResetEvent.h"
#include "../../foundation/MidiBuffer.h"
#include "../../foundation/MidiCollector.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// The single seam between Dusk Studio's engine and the platform MIDI backend.
// Everything above it - the engine, the UI, the session - speaks dusk types;
// below it an IMidiInputBackend / IMidiOutputBackend pair does the OS work
// (native ALSA sequencer on Linux, a JUCE-backed fallback elsewhere). The seam
// owns what the backends deliberately do not: the index<->identifier mapping the
// session persists, the per-input retiming collectors, and the RT out-queue.
namespace duskstudio::midi
{
struct MidiDeviceInfo
{
    std::string name;
    std::string identifier;
};

// Owns the per-input collector bank and receives every enabled input's bytes on
// the backend's MIDI thread, routing them by source identifier. The
// detach-rebuild-reattach fence around a hot-plug is orchestrated by the engine
// (which also detaches its own audio callback) - this class only exposes the
// pieces.
class MidiInputClient final
{
public:
    // Fixed identifier for the synthetic Virtual-Keyboard slot so saved sessions
    // resolve back to it across hot-plug (its index is otherwise re-derived).
    static constexpr const char* kVirtualKeyboardIdentifier = "Dusk Studio:virtual-keyboard";

    MidiInputClient();
    ~MidiInputClient();

    // Message thread, input callback DETACHED. Enumerate the hardware inputs,
    // enable each on the backend, build a per-input collector, then append the
    // synthetic Virtual-Keyboard slot last (fixed identifier, no OS device).
    // Mutating the bank with the callback active is UB - see the detach/attach
    // fence.
    void rebuild (double sampleRate);

    // Detach half of the fence: stop the backend's dispatch (its stop() joins
    // that side before returning) and release the OS handles before a rebuild.
    void detachCallback();
    void disableAllDevices();

    // Reattach: start the backend's dispatch again. Every enabled input fans in
    // to one receiver, routed to a collector by source identifier.
    void attachCallback();

    // Re-arm every collector to a new sample rate so the MIDI thread's ms
    // timestamps convert to per-block sample offsets (audioDeviceAboutToStart).
    void resetCollectors (double sampleRate);

    const std::vector<MidiDeviceInfo>& getDevices() const noexcept { return devices; }
    int getNumInputs() const noexcept { return (int) collectors.size(); }

    int getVirtualKeyboardIndex() const noexcept { return virtualKeyboardIndex; }

    // Message thread. Saved-identifier -> current index, with the backend's
    // migration fallback for identifiers minted by an earlier backend. -1 when
    // the device is gone (or the identifier is empty).
    int resolveIndex (const std::string& savedIdentifier);

    // Message thread. Push a complete MIDI message into the synthetic
    // Virtual-Keyboard slot, stamped with the same clock the backends use so it
    // retimes like hardware input. No-op before the bank is built.
    void postVirtualKeyboardMidi (const std::uint8_t* bytes, int numBytes) noexcept;

    // Audio thread. Retime this input's pending events into the dusk boundary
    // buffer. RT-safe: the collector's ring is pre-sized and never grows (an
    // over-capacity burst drops whole records), and `out` was reserveBytes()'d
    // off the RT path by the caller. Call once per input index per block - the
    // drain is destructive.
    void drainBlock (int inputIndex, dusk::MidiBuffer& out, int numSamples) noexcept;

private:
    // Backend MIDI thread. Routes by source identifier to the matching
    // collector, which the audio thread later drains lock-free.
    void handleIncoming (const std::string& identifier,
                         const std::uint8_t* bytes, int numBytes, double timeMs) noexcept;

    std::unique_ptr<IMidiInputBackend> backend;
    std::vector<MidiDeviceInfo> devices;
    std::vector<std::unique_ptr<dusk::MidiCollector>> collectors;

    // Identifier -> collector index for the receiver's demux. Rebuilt only with
    // the backend stopped, so the MIDI thread reads it without synchronisation.
    std::unordered_map<std::string, int> indexByIdentifier;

    int virtualKeyboardIndex = -1;
};

// Owns the output-port bank plus the whole RT out-queue apparatus: the SPSC
// queue the audio thread pushes per-port blocks into and the 1 ms pump thread
// that drains it into the backend, where blocking is harmless.
class MidiOutputBank
{
public:
    MidiOutputBank();
    ~MidiOutputBank();

    // Message thread, audio callback DETACHED. Discard queued blocks (their port
    // indices were minted against the old device order), close open outputs, and
    // re-enumerate. Does NOT eager-open: opening every port at startup blocks the
    // message thread on each subscription, stalling MainWindow::setVisible for
    // seconds on USB-MIDI systems. Open on demand via ensureOpen. Session
    // sync/MCU eager-opens stay with the engine.
    void rebuild();

    // Lazy open. Message thread.
    bool ensureOpen (int index);

    void closeAll();

    const std::vector<MidiDeviceInfo>& getDevices() const noexcept { return devices; }
    int  getNumOutputs() const noexcept { return (int) devices.size(); }

    // As MidiInputClient::resolveIndex.
    int resolveIndex (const std::string& savedIdentifier);

    // Audio thread reads this to skip queueing at a closed port, so it is an
    // atomic flag rather than a backend query (which walks a string-keyed map
    // the message thread mutates).
    bool isOpen (int index) const noexcept;

    // Message thread. Direct send, serialised against the pump's drain - the
    // backends own one encoder and one connection per direction.
    bool send (int index, const dusk::MidiBuffer& events) noexcept;

    // Blocks the audio thread can have in flight at once. A push past this is
    // dropped, so it also bounds how much the pump can deliver per wake.
    static constexpr int kQueueDepth = 64;

    // Audio thread. Backend sends are not audio-thread safe (they block on the
    // OS port), so the audio thread pushes whole per-port blocks into the
    // lock-free queue and the pump drains them. Slot buffers are pre-sized so
    // the copy never allocates; a closed port, a block past the slot cap, or a
    // full queue drops the block (dropping clock bytes beats an xrun).
    // sampleRate carries the sample-offset->ms math.
    void queueRt (int port, const dusk::MidiBuffer& events, double sampleRate) noexcept;

    // Pump lifecycle (message thread).
    void startPump();
    void stopPump();

private:
    void drainQueue();     // pump thread
    void pumpLoop();

    std::unique_ptr<IMidiOutputBackend> backend;
    std::vector<MidiDeviceInfo> devices;

    // Ports the audio thread may see beyond this are not addressable; no
    // sequencer exposes anywhere near it.
    static constexpr int kMaxPorts = 256;

    // Parallel to `devices`, but fixed storage with a published count: the audio
    // thread reads these in isOpen while the message thread re-enumerates, so
    // neither the array nor its bound may be swapped under it. rebuild()
    // publishes 0 before it touches anything and the new count after, so a
    // racing reader sees "closed", never a stale bound.
    std::array<std::atomic<bool>, kMaxPorts> openFlags {};
    std::atomic<int> numOpenFlags { 0 };

    // Serialises the pump thread's backend access against message-thread bank
    // mutation (rebuild / ensureOpen / send). The audio thread never takes it -
    // it only touches the queue's writer side.
    std::mutex bankMutex;

    static constexpr int kSlotBytes  = 4096;
    struct QueuedMidiOut
    {
        int              port       = -1;
        double           timeMs     = 0.0;
        double           sampleRate = 48000.0;
        dusk::MidiBuffer events;
    };
    std::array<QueuedMidiOut, kQueueDepth> queue;

    // SPSC cursors over `queue`: monotonic counters, slot index = counter %
    // kQueueDepth. The audio thread owns writeCount, the pump owns readCount;
    // each publishes with release and observes the other with acquire.
    std::atomic<std::uint32_t> writeCount { 0 };
    std::atomic<std::uint32_t> readCount  { 0 };

    std::thread       pump;
    std::atomic<bool> pumpRunning { false };
    dusk::AutoResetEvent pumpWake;
};
} // namespace duskstudio::midi
