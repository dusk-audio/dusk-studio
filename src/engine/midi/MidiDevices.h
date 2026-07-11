#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include "../../foundation/MidiBuffer.h"
#include <array>
#include <memory>
#include <mutex>
#include <vector>

// The single seam between Dusk Studio's engine and JUCE's MIDI device backend.
// Its BOUNDARY type is dusk::MidiBuffer — the RT drain hands the audio thread
// dusk events, and the RT out-queue takes them — so a future native ALSA-
// sequencer backend can replace this file pair without touching the engine.
// Internal storage stays juce-typed (collectors, outputs, the SPSC slots); only
// the API is dusk. This is the only JUCE-device-touching code, hence the
// deliberate juce-allowlist entry.
namespace duskstudio::midi
{
struct MidiDeviceInfo
{
    juce::String name;
    juce::String identifier;
};

// Owns the per-input MidiMessageCollector bank and is itself the juce callback
// bound to every enabled input; routes by source identifier. The detach-rebuild-
// reattach fence around a hot-plug is orchestrated by the engine (which also
// detaches its own audio callback) — this class only exposes the pieces.
class MidiInputClient final : public juce::MidiInputCallback
{
public:
    // Fixed identifier for the synthetic Virtual-Keyboard slot so saved sessions
    // resolve back to it across hot-plug (its index is otherwise re-derived).
    static constexpr const char* kVirtualKeyboardIdentifier = "Dusk Studio:virtual-keyboard";

    // Message thread. Stash the device manager the enable/disable lifecycle and
    // callback (un)registration run against. Call once before rebuild().
    void setDeviceManager (juce::AudioDeviceManager& dm) noexcept { deviceManager = &dm; }

    // Message thread, input callback DETACHED. Enumerate the hardware inputs,
    // enable each on the device manager, build a per-input MidiMessageCollector,
    // then append the synthetic Virtual-Keyboard slot last (fixed identifier, no
    // OS device). Mutating the bank with the callback active is UB — see the
    // detach/attach fence.
    void rebuild (double sampleRate);

    // Detach half of the fence: unregister the input callback (JUCE's remove
    // joins the MIDI dispatch side before returning) and disable every device so
    // the OS handles are released before a rebuild.
    void detachCallback();
    void disableAllDevices();

    // Reattach: empty identifier = every enabled input fans out to
    // handleIncomingMidiMessage, routed there by source identifier.
    void attachCallback();

    // Re-arm every collector to a new sample rate so the MIDI thread's ms
    // timestamps convert to per-block sample offsets (audioDeviceAboutToStart).
    void resetCollectors (double sampleRate);

    const std::vector<MidiDeviceInfo>& getDevices() const noexcept { return devices; }
    int getNumInputs() const noexcept { return (int) collectors.size(); }

    int getVirtualKeyboardIndex() const noexcept { return virtualKeyboardIndex; }
    juce::MidiMessageCollector* getVirtualKeyboardCollector() noexcept;

    // Audio thread. Pull this input's block out of its collector and copy the
    // events into the dusk boundary buffer. RT-safe provided (a) `out` was
    // reserveBytes()'d off the RT path by the caller and (b) the incoming burst
    // fits the collector's pre-sized scratch — a rare overflow grows it, exactly
    // as the pre-flip perInputMidi drain did. Call once per input index per
    // block: the collector drain is destructive.
    void drainBlock (int inputIndex, dusk::MidiBuffer& out, int numSamples) noexcept;

    // juce::MidiInputCallback. Fires on JUCE's MIDI input thread (NOT the audio
    // thread); routes by source identifier to the matching collector, which the
    // audio thread later drains lock-free (JUCE contract).
    void handleIncomingMidiMessage (juce::MidiInput* source,
                                    const juce::MidiMessage& message) override;

private:
    juce::AudioDeviceManager* deviceManager = nullptr;
    std::vector<MidiDeviceInfo> devices;
    std::vector<std::unique_ptr<juce::MidiMessageCollector>> collectors;
    int virtualKeyboardIndex = -1;

    // Single reused juce scratch the RT drain removes into before copying to the
    // dusk boundary buffer. drainBlock runs sequentially per input on the audio
    // thread, so one instance suffices.
    juce::MidiBuffer drainScratch;
};

// Owns the output-port bank plus the whole RT out-queue apparatus: the SPSC
// FIFO the audio thread pushes per-port blocks into and the 1 ms pump thread
// that drains it into sendBlockOfMessages where blocking is harmless.
class MidiOutputBank
{
public:
    MidiOutputBank();
    ~MidiOutputBank();

    // Message thread, audio callback DETACHED. Discard queued blocks (their port
    // indices were minted against the old device order), close open outputs, and
    // re-enumerate. Does NOT eager-open: opening every port at startup blocks the
    // message thread on each snd_seq_connect_to and spawns a thread per port,
    // stalling MainWindow::setVisible for seconds on USB-MIDI systems. Open on
    // demand via ensureOpen. Session sync/MCU eager-opens stay with the engine.
    void rebuild();

    // Lazy open + start the port's background delivery thread so the pump's
    // sendBlockOfMessages enqueues without blocking on the OS port. Message
    // thread.
    bool ensureOpen (int index);

    void closeAll();

    const std::vector<MidiDeviceInfo>& getDevices() const noexcept { return devices; }
    int  getNumOutputs() const noexcept { return (int) outputs.size(); }
    bool isOpen (int index) const noexcept;

    // Message thread. Direct send with an ms-since-epoch base
    // (getMillisecondCounterHiRes = "ASAP", lower latency than buffering for the
    // next block). juce-typed because McuController still builds juce buffers
    // (events-tower coupling).
    bool send (int index, const juce::MidiBuffer& events) noexcept;

    // Audio thread. sendBlockOfMessages is NOT audio-thread safe (it takes the
    // delivery thread's mutex and heap-allocates under it). The audio thread
    // instead pushes whole per-port blocks into the lock-free FIFO; the pump
    // drains them. Slot buffers are pre-sized so the copy never allocates; a
    // block past the slot cap OR a full queue drops the block (dropping clock
    // bytes beats an xrun). timeMs/sampleRate carry the sample-offset→ms math.
    void queueRt (int port, const dusk::MidiBuffer& events, double sampleRate) noexcept;

    // Pump lifecycle (message thread). High priority so a loaded message thread
    // can't starve clock / note delivery; still below the audio thread.
    void startPump();
    void stopPump();

    // Convert a dusk event buffer into a juce one. Factored out so the send +
    // queue conversions are unit-testable without a real output device.
    static void toJuceBuffer (const dusk::MidiBuffer& in, juce::MidiBuffer& out);

private:
    void drainQueue();     // pump thread
    bool sendJuce (int index, const juce::MidiBuffer& events, double sampleRate) noexcept;
    static std::vector<MidiDeviceInfo> enumerate();

    std::vector<MidiDeviceInfo> devices;
    std::vector<std::unique_ptr<juce::MidiOutput>> outputs;

    // Serialises the pump thread's port access against message-thread bank
    // mutation (rebuild / ensureOpen). The audio thread never takes it — it only
    // touches the FIFO writer side.
    std::mutex bankMutex;

    static constexpr int kQueueSlots = 64;
    static constexpr int kSlotBytes  = 4096;
    // juce::MidiBuffer per-event overhead: int32 samplePosition + uint16 length.
    static constexpr int kJuceEventHeaderBytes = 6;
    struct QueuedMidiOut
    {
        int    port       = -1;
        double timeMs     = 0.0;
        double sampleRate = 48000.0;
        juce::MidiBuffer events;
    };
    juce::AbstractFifo fifo { kQueueSlots };
    std::array<QueuedMidiOut, kQueueSlots> queue;

    struct Pump final : juce::Thread
    {
        explicit Pump (MidiOutputBank& b) : juce::Thread ("Dusk Studio MIDI out"), bank (b) {}
        void run() override;
        MidiOutputBank& bank;
    };
    Pump pump { *this };
};
} // namespace duskstudio::midi
