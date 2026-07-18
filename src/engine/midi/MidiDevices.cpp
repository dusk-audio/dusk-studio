#include "MidiDevices.h"

#if defined(__linux__)
 #include "AlsaSeqMidi.h"
#else
 #include "JuceMidiBackend.h"
#endif

#include <cstdio>
#include <utility>

namespace duskstudio::midi
{
namespace
{
// Per-input ring capacity. Sized well past a dense controller burst plus a
// 1 kB-class sysex so the drop-whole-record policy only fires when the audio
// thread has genuinely stopped draining.
constexpr std::size_t kInputRingBytes = 16384;

// Stand-in rate for a collector built before the audio device reports one.
constexpr double kFallbackSampleRate = 48000.0;

int indexOfIdentifier (const std::vector<MidiDeviceInfo>& devices, const std::string& identifier)
{
    for (int i = 0; i < (int) devices.size(); ++i)
        if (devices[(size_t) i].identifier == identifier)
            return i;
    return -1;
}

std::unique_ptr<IMidiInputBackend> makeInputBackend()
{
   #if defined(__linux__)
    return std::make_unique<AlsaSeqMidiInput>();
   #else
    return makeJuceMidiInputBackend();
   #endif
}

std::unique_ptr<IMidiOutputBackend> makeOutputBackend()
{
   #if defined(__linux__)
    return std::make_unique<AlsaSeqMidiOutput>();
   #else
    return makeJuceMidiOutputBackend();
   #endif
}
} // namespace

// MidiInputClient

MidiInputClient::MidiInputClient()
    : backend (makeInputBackend())
{
    backend->setReceiver ([this] (const std::string& identifier,
                                  const std::uint8_t* bytes, int numBytes, double timeMs)
    {
        handleIncoming (identifier, bytes, numBytes, timeMs);
    });
}

MidiInputClient::~MidiInputClient()
{
    backend->stop();
}

void MidiInputClient::rebuild (double sampleRate)
{
    devices.clear();
    collectors.clear();
    indexByIdentifier.clear();

    const auto avail = backend->enumerate();
    devices.reserve (avail.size() + 1);
    collectors.reserve (avail.size() + 1);

    // A collector must never be left unseeded: its retime is relative to
    // lastCallbackTime, so a drain against an unset one would measure the whole
    // steady-clock epoch and overflow the sample number. The engine rebuilds
    // with sampleRate 0 before a device is open (and after one stops), so fall
    // back to a placeholder rate here; audioDeviceAboutToStart resets to the
    // real one before the first drain.
    const double now = backendClockMs();
    const double rate = sampleRate > 0.0 ? sampleRate : kFallbackSampleRate;
    for (const auto& d : avail)
    {
        auto col = std::make_unique<dusk::MidiCollector> (kInputRingBytes);
        col->reset (rate, now);

        // Failure usually = OS denied access (another app exclusively owns the
        // port). Keep the slot: it stays addressable, it just never fires.
        if (! backend->enable (d.identifier))
            std::fprintf (stderr,
                          "[Dusk Studio/MidiDevices] WARNING: failed to enable MIDI input \"%s\" "
                          "(id %s). Another application may be holding it open.\n",
                          d.name.c_str(), d.identifier.c_str());

        indexByIdentifier[d.identifier] = (int) collectors.size();
        devices.push_back ({ d.name, d.identifier });
        collectors.push_back (std::move (col));
    }

    // Appended after real hardware so the VKB's index is stable across hot-plug.
    // Not bound to any OS device - the on-screen keyboard posts into its
    // collector directly; the audio thread drains it like any other input.
    devices.push_back ({ "Virtual Keyboard (Dusk Studio)", kVirtualKeyboardIdentifier });
    {
        auto vkb = std::make_unique<dusk::MidiCollector> (kInputRingBytes);
        vkb->reset (rate, now);
        collectors.push_back (std::move (vkb));
    }
    virtualKeyboardIndex = (int) collectors.size() - 1;
}

void MidiInputClient::detachCallback()  { backend->stop(); }
void MidiInputClient::disableAllDevices() { backend->disableAll(); }
void MidiInputClient::attachCallback()  { backend->start(); }

void MidiInputClient::resetCollectors (double sampleRate)
{
    const double now  = backendClockMs();
    const double rate = sampleRate > 0.0 ? sampleRate : kFallbackSampleRate;
    for (auto& c : collectors)
        if (c != nullptr) c->reset (rate, now);
}

int MidiInputClient::resolveIndex (const std::string& savedIdentifier)
{
    if (savedIdentifier.empty()) return -1;
    const int exact = indexOfIdentifier (devices, savedIdentifier);
    if (exact >= 0) return exact;
    const auto migrated = backend->migrateIdentifier (savedIdentifier);
    return migrated.empty() ? -1 : indexOfIdentifier (devices, migrated);
}

void MidiInputClient::postVirtualKeyboardMidi (const std::uint8_t* bytes, int numBytes) noexcept
{
    if (virtualKeyboardIndex < 0 || virtualKeyboardIndex >= (int) collectors.size()) return;
    if (auto* col = collectors[(size_t) virtualKeyboardIndex].get())
        col->addMessage (bytes, numBytes, backendClockMs());
}

void MidiInputClient::drainBlock (int inputIndex, dusk::MidiBuffer& out, int numSamples) noexcept
{
    out.clear();
    if (inputIndex < 0 || inputIndex >= (int) collectors.size()) return;
    if (auto* col = collectors[(size_t) inputIndex].get())
        col->removeNextBlock (out, numSamples, backendClockMs());
}

void MidiInputClient::handleIncoming (const std::string& identifier,
                                      const std::uint8_t* bytes, int numBytes, double timeMs) noexcept
{
    const auto it = indexByIdentifier.find (identifier);
    if (it == indexByIdentifier.end()) return;
    if (it->second < (int) collectors.size())
        if (auto* col = collectors[(size_t) it->second].get())
            col->addMessage (bytes, numBytes, timeMs);
}

// MidiOutputBank

MidiOutputBank::MidiOutputBank()
    : backend (makeOutputBackend())
{
    // Pre-size every slot buffer so the audio-thread copy in queueRt never
    // allocates - reserveBytes also caps it, so an over-cap block is dropped by
    // addEvent rather than reallocated.
    for (auto& slot : queue)
        slot.events.reserveBytes (kSlotBytes);
}

MidiOutputBank::~MidiOutputBank()
{
    stopPump();
}

void MidiOutputBank::rebuild()
{
    // Mutating the bank is safe only with the audio callback detached (the audio
    // thread writes port indices into the queue while running) and under
    // bankMutex (the pump is sending through the backend).
    const std::lock_guard<std::mutex> lock (bankMutex);

    // Discard queued-but-unsent blocks first: their port indices were minted
    // against the OLD device order and could land on a different physical port
    // after re-enumeration. The callback is detached, so nothing new is being
    // written; the mutex excludes the pump's drain.
    readCount.store (writeCount.load (std::memory_order_acquire), std::memory_order_release);

    // Retract the open flags before the ports move: anything still reading them
    // sees "closed" rather than a bound that no longer matches the bank.
    numOpenFlags.store (0, std::memory_order_release);

    backend->closeAll();
    devices.clear();
    for (const auto& d : backend->enumerate())
    {
        if ((int) devices.size() >= kMaxPorts)
        {
            std::fprintf (stderr,
                          "[Dusk Studio/MidiDevices] WARNING: more than %d MIDI outputs; "
                          "the rest are not addressable.\n", kMaxPorts);
            break;
        }
        devices.push_back ({ d.name, d.identifier });
    }

    for (int i = 0; i < (int) devices.size(); ++i)
        openFlags[(size_t) i].store (false, std::memory_order_relaxed);
    numOpenFlags.store ((int) devices.size(), std::memory_order_release);
}

int MidiOutputBank::resolveIndex (const std::string& savedIdentifier)
{
    if (savedIdentifier.empty()) return -1;
    const int exact = indexOfIdentifier (devices, savedIdentifier);
    if (exact >= 0) return exact;
    const std::lock_guard<std::mutex> lock (bankMutex);
    const auto migrated = backend->migrateIdentifier (savedIdentifier);
    return migrated.empty() ? -1 : indexOfIdentifier (devices, migrated);
}

bool MidiOutputBank::ensureOpen (int index)
{
    if (index < 0 || index >= (int) devices.size())
        return false;
    if (isOpen (index))
        return true;

    bool opened = false;
    {
        // The pump may be sending through the backend.
        const std::lock_guard<std::mutex> lock (bankMutex);
        opened = backend->open (devices[(size_t) index].identifier);
    }
    if (! opened)
    {
        std::fprintf (stderr,
                      "[Dusk Studio/MidiDevices] WARNING: failed to open MIDI output \"%s\" "
                      "(id %s). Another application may be holding it open.\n",
                      devices[(size_t) index].name.c_str(),
                      devices[(size_t) index].identifier.c_str());
        return false;
    }
    openFlags[(size_t) index].store (true, std::memory_order_release);
    return true;
}

void MidiOutputBank::closeAll()
{
    const std::lock_guard<std::mutex> lock (bankMutex);
    backend->closeAll();
    for (int i = 0; i < numOpenFlags.load (std::memory_order_acquire); ++i)
        openFlags[(size_t) i].store (false, std::memory_order_release);
}

bool MidiOutputBank::isOpen (int index) const noexcept
{
    return index >= 0 && index < numOpenFlags.load (std::memory_order_acquire)
             && openFlags[(size_t) index].load (std::memory_order_acquire);
}

bool MidiOutputBank::send (int index, const dusk::MidiBuffer& events) noexcept
{
    if (index < 0 || index >= (int) devices.size()) return false;
    const std::lock_guard<std::mutex> lock (bankMutex);
    // sampleRate is for the offset->ms conversion only; a direct send carries no
    // offsets, and backendClockMs() = "ASAP".
    return backend->send (devices[(size_t) index].identifier, events, backendClockMs(), 48000.0);
}

void MidiOutputBank::queueRt (int port, const dusk::MidiBuffer& events, double sampleRate) noexcept
{
    const auto w = writeCount.load (std::memory_order_relaxed);
    if (w - readCount.load (std::memory_order_acquire) >= (std::uint32_t) kQueueSlots)
        return;   // pump stalled - drop the block

    auto& slot = queue[(size_t) (w % (std::uint32_t) kQueueSlots)];
    slot.events.clear();   // keeps the pre-sized capacity

    // A block past the pre-sized cap is dropped whole (same policy as
    // queue-full) rather than split: addEvent refuses to grow, so a partial copy
    // would tear paired events apart. The reserved slot is simply left
    // uncommitted.
    for (const auto meta : events)
    {
        if (! slot.events.addEvent (meta.data, meta.numBytes, meta.samplePosition))
        {
            slot.events.clear();
            return;
        }
    }

    slot.port       = port;
    slot.timeMs     = backendClockMs();
    slot.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    writeCount.store (w + 1, std::memory_order_release);
}

void MidiOutputBank::drainQueue()
{
    // The whole read cycle holds bankMutex - not just the send - so rebuild can
    // discard stale slots under the same mutex without racing a half-finished
    // drain (the queue's reader side is single-consumer). The audio thread only
    // touches the writer side and never takes this mutex.
    const std::lock_guard<std::mutex> lock (bankMutex);

    auto r = readCount.load (std::memory_order_relaxed);
    const auto w = writeCount.load (std::memory_order_acquire);
    while (r != w)
    {
        const auto& slot = queue[(size_t) (r % (std::uint32_t) kQueueSlots)];
        if (slot.port >= 0 && slot.port < (int) devices.size())
            (void) backend->send (devices[(size_t) slot.port].identifier, slot.events,
                                  slot.timeMs, slot.sampleRate);
        ++r;
    }
    readCount.store (r, std::memory_order_release);
}

void MidiOutputBank::pumpLoop()
{
    while (pumpRunning.load (std::memory_order_acquire))
    {
        drainQueue();
        pumpWake.wait (1);
    }
}

void MidiOutputBank::startPump()
{
    if (pumpRunning.exchange (true)) return;
    pump = std::thread ([this] { pumpLoop(); });
}

void MidiOutputBank::stopPump()
{
    if (! pumpRunning.exchange (false)) return;
    pumpWake.signal();
    if (pump.joinable()) pump.join();
}
} // namespace duskstudio::midi
