#include "MidiDevices.h"
#include <cstdio>

namespace duskstudio::midi
{
// MidiInputClient

void MidiInputClient::rebuild (double sampleRate)
{
    jassert (deviceManager != nullptr);

    const auto avail = juce::MidiInput::getAvailableDevices();
    devices.clear();
    devices.reserve ((size_t) avail.size() + 1);
    collectors.clear();
    collectors.reserve ((size_t) avail.size() + 1);

    for (const auto& d : avail)
    {
        devices.push_back ({ d.name, d.identifier });

        auto col = std::make_unique<juce::MidiMessageCollector>();
        if (sampleRate > 0.0) col->reset (sampleRate);

        // setMidiInputDeviceEnabled returns void - re-query to verify. Failure
        // usually = OS denied access (another app exclusively owns the port).
        deviceManager->setMidiInputDeviceEnabled (d.identifier, true);
        if (! deviceManager->isMidiInputDeviceEnabled (d.identifier))
            std::fprintf (stderr,
                          "[Dusk Studio/MidiDevices] WARNING: failed to enable MIDI input \"%s\" "
                          "(id %s). Another application may be holding it open.\n",
                          d.name.toRawUTF8(), d.identifier.toRawUTF8());

        collectors.push_back (std::move (col));
    }

    // Appended after real hardware so the VKB's index is stable across hot-plug.
    // Not bound to any OS device - the on-screen keyboard addMessageToQueues into
    // its collector directly; the audio thread drains it like any other input.
    devices.push_back ({ juce::String ("Virtual Keyboard (Dusk Studio)"),
                         juce::String (kVirtualKeyboardIdentifier) });
    {
        auto vkb = std::make_unique<juce::MidiMessageCollector>();
        if (sampleRate > 0.0) vkb->reset (sampleRate);
        collectors.push_back (std::move (vkb));
    }
    virtualKeyboardIndex = (int) collectors.size() - 1;

    // Pre-size the RT drain scratch well past the 4096-byte dusk copy cap so a
    // dense burst drops downstream (bounded) instead of reallocating here on
    // the audio thread.
    drainScratch.ensureSize (16384);
}

void MidiInputClient::detachCallback()
{
    if (deviceManager != nullptr)
        deviceManager->removeMidiInputDeviceCallback ({}, this);
}

void MidiInputClient::disableAllDevices()
{
    if (deviceManager == nullptr) return;
    for (const auto& d : devices)
        deviceManager->setMidiInputDeviceEnabled (d.identifier, false);
}

void MidiInputClient::attachCallback()
{
    if (deviceManager != nullptr)
        deviceManager->addMidiInputDeviceCallback ({}, this);
}

void MidiInputClient::resetCollectors (double sampleRate)
{
    for (auto& c : collectors)
        if (c != nullptr) c->reset (sampleRate);
}

juce::MidiMessageCollector* MidiInputClient::getVirtualKeyboardCollector() noexcept
{
    if (virtualKeyboardIndex < 0 || virtualKeyboardIndex >= (int) collectors.size())
        return nullptr;
    return collectors[(size_t) virtualKeyboardIndex].get();
}

void MidiInputClient::drainBlock (int inputIndex, dusk::MidiBuffer& out, int numSamples) noexcept
{
    out.clear();
    if (inputIndex < 0 || inputIndex >= (int) collectors.size()) return;
    auto* col = collectors[(size_t) inputIndex].get();
    if (col == nullptr) return;

    drainScratch.clear();
    col->removeNextBlockOfMessages (drainScratch, numSamples);
    for (const auto meta : drainScratch)
        out.addEvent (meta.data, meta.numBytes, meta.samplePosition);
}

void MidiInputClient::handleIncomingMidiMessage (juce::MidiInput* source,
                                                 const juce::MidiMessage& message)
{
    if (source == nullptr) return;
    // JUCE guarantees identifier stability per device per session.
    const auto sourceId = source->getIdentifier();
    for (int i = 0; i < (int) devices.size(); ++i)
    {
        if (devices[(size_t) i].identifier == sourceId)
        {
            if (i < (int) collectors.size() && collectors[(size_t) i] != nullptr)
                collectors[(size_t) i]->addMessageToQueue (message);
            return;
        }
    }
}

// MidiOutputBank

MidiOutputBank::MidiOutputBank()
{
    // Pre-size every slot buffer so the audio-thread copy in queueRt never
    // allocates (kSlotBytes matches the drop-on-oversize cap there).
    for (auto& slot : queue)
        slot.events.ensureSize (kSlotBytes);
}

MidiOutputBank::~MidiOutputBank()
{
    stopPump();
}

std::vector<MidiDeviceInfo> MidiOutputBank::enumerate()
{
    std::vector<MidiDeviceInfo> result;
    const auto avail = juce::MidiOutput::getAvailableDevices();
    result.reserve ((size_t) avail.size());
    for (const auto& d : avail)
        result.push_back ({ d.name, d.identifier });
    return result;
}

void MidiOutputBank::rebuild()
{
    // Mutating outputs is safe only with the audio callback detached (the audio
    // thread writes port indices into the FIFO while running) and under
    // bankMutex (the pump dereferences the ports).
    const std::lock_guard<std::mutex> lock (bankMutex);

    // Discard queued-but-unsent blocks first: their port indices were minted
    // against the OLD device order and could land on a different physical port
    // after re-enumeration. The callback is detached, so nothing new is being
    // written; the mutex excludes the pump's drain.
    const int stale = fifo.getNumReady();
    if (stale > 0)
    {
        int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
        fifo.prepareToRead (stale, s1, n1, s2, n2);
        fifo.finishedRead (n1 + n2);
    }

    outputs.clear();
    devices = enumerate();
    outputs.resize (devices.size());
}

bool MidiOutputBank::ensureOpen (int index)
{
    if (index < 0 || index >= (int) outputs.size())
        return false;
    if (outputs[(size_t) index] != nullptr)
        return true;  // already open

    auto out = juce::MidiOutput::openDevice (devices[(size_t) index].identifier);
    if (out == nullptr)
    {
        std::fprintf (stderr,
                      "[Dusk Studio/MidiDevices] WARNING: failed to open MIDI output \"%s\" "
                      "(id %s). Another application may be holding it open.\n",
                      devices[(size_t) index].name.toRawUTF8(),
                      devices[(size_t) index].identifier.toRawUTF8());
        return false;
    }
    // Background thread so the pump's sendBlockOfMessages enqueues without
    // blocking on the OS port.
    out->startBackgroundThread();
    {
        // The pump may be dereferencing this slot's pointer.
        const std::lock_guard<std::mutex> lock (bankMutex);
        outputs[(size_t) index] = std::move (out);
    }
    return true;
}

void MidiOutputBank::closeAll()
{
    const std::lock_guard<std::mutex> lock (bankMutex);
    outputs.clear();
}

bool MidiOutputBank::isOpen (int index) const noexcept
{
    return index >= 0 && index < (int) outputs.size() && outputs[(size_t) index] != nullptr;
}

void MidiOutputBank::toJuceBuffer (const dusk::MidiBuffer& in, juce::MidiBuffer& out)
{
    out.clear();
    for (const auto meta : in)
    {
        const auto& m = meta.getMessage();
        out.addEvent (m.getRawData(), m.getRawDataSize(), meta.samplePosition);
    }
}

bool MidiOutputBank::sendJuce (int index, const juce::MidiBuffer& events, double sampleRate) noexcept
{
    if (index < 0 || index >= (int) outputs.size())
        return false;
    auto* out = outputs[(size_t) index].get();
    if (out == nullptr) return false;
    // Absolute ms-since-epoch base; getMillisecondCounterHiRes = "ASAP".
    out->sendBlockOfMessages (events,
                              juce::Time::getMillisecondCounterHiRes(),
                              sampleRate > 0.0 ? sampleRate : 48000.0);
    return true;
}

bool MidiOutputBank::send (int index, const dusk::MidiBuffer& events) noexcept
{
    // Bridge the dusk boundary buffer to juce here (the seam owns the
    // conversion). sampleRate is for time stamps only - the direct send carries
    // no offsets.
    juce::MidiBuffer juceEvents;
    toJuceBuffer (events, juceEvents);
    return sendJuce (index, juceEvents, 48000.0);
}

void MidiOutputBank::queueRt (int port, const dusk::MidiBuffer& events, double sampleRate) noexcept
{
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo.prepareToWrite (1, start1, size1, start2, size2);
    if (size1 + size2 < 1) return;   // pump stalled - drop the block

    // The lone writable slot lands in the SECOND segment when the write wraps
    // (size1 == 0, size2 == 1); use start2 then, not the stale start1.
    auto& slot = queue[(size_t) (size1 > 0 ? start1 : start2)];
    slot.events.clear();   // keeps the pre-sized capacity

    // Convert dusk -> juce directly into the slot, stopping before the pre-sized
    // cap so the copy never reallocates on the audio thread. A block past the cap
    // is dropped whole (same policy as queue-full) rather than realloc'd; the
    // reserved slot is simply left uncommitted (no finishedWrite).
    for (const auto meta : events)
    {
        const auto& m = meta.getMessage();
        const int nb = m.getRawDataSize();
        if ((int) slot.events.data.size() + kJuceEventHeaderBytes + nb > kSlotBytes)
        {
            slot.events.clear();
            return;
        }
        slot.events.addEvent (m.getRawData(), nb, meta.samplePosition);
    }

    slot.port       = port;
    slot.timeMs     = juce::Time::getMillisecondCounterHiRes();
    slot.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    fifo.finishedWrite (1);
}

void MidiOutputBank::drainQueue()
{
    // The whole read cycle holds bankMutex - not just the port dereference - so
    // rebuild can discard stale slots under the same mutex without racing a
    // half-finished drain (the FIFO's reader side is single-consumer). The audio
    // thread only touches the writer side and never takes this mutex.
    const std::lock_guard<std::mutex> lock (bankMutex);

    const int ready = fifo.getNumReady();
    if (ready <= 0) return;

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo.prepareToRead (ready, start1, size1, start2, size2);
    auto sendSlots = [this] (int start, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            auto& slot = queue[(size_t) (start + i)];
            if (slot.port >= 0 && slot.port < (int) outputs.size())
                if (auto* out = outputs[(size_t) slot.port].get())
                    out->sendBlockOfMessages (slot.events, slot.timeMs, slot.sampleRate);
        }
    };
    sendSlots (start1, size1);
    sendSlots (start2, size2);
    fifo.finishedRead (size1 + size2);
}

void MidiOutputBank::Pump::run()
{
    while (! threadShouldExit())
    {
        bank.drainQueue();
        wait (1);
    }
}

void MidiOutputBank::startPump()
{
    pump.startThread (juce::Thread::Priority::high);
}

void MidiOutputBank::stopPump()
{
    pump.stopThread (2000);
}
} // namespace duskstudio::midi
