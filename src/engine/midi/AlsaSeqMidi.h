#pragma once

#include "MidiBackend.h"

#include <memory>

// Native ALSA sequencer (snd_seq_*) backend for the MIDI device seam on Linux,
// implementing IMidiInputBackend / IMidiOutputBackend against the same
// libasound the ALSA audio backend already links - no new dependency. ALSA
// types stay in the .cpp behind a pimpl, so this header is includable on any
// platform and adds no <alsa/...> include to its users; on non-Linux the .cpp
// is a stub.
//
// Identifier scheme is name-based ("alsa-seq:<client>:<port>[:<dup>]") because
// ALSA client / port numbers are not stable across reboot or replug - the
// string survives, the numbers do not. The seam owns index<->identifier; the
// backend only ever speaks identifiers, and resolves one to a live address by
// re-enumerating and matching the string. Caveat: the :<dup> suffix on
// identically named ports is an ordinal in the current enumeration order, not a
// fixed key, so two same-named devices can trade identifiers across a replug or
// reboot.
//
// Threading contract (matches the seam's detach/rebuild/attach fence, same as
// the JUCE backing it replaces): enumerate / enable / disableAll / open /
// closeAll / send are called on the message or pump thread while the input
// poll thread is stopped. start() launches the poll thread; stop() joins it, so
// no Receiver callback is in flight once stop() returns.
namespace duskstudio::midi
{
class AlsaSeqMidiInput final : public IMidiInputBackend
{
public:
    AlsaSeqMidiInput();
    ~AlsaSeqMidiInput() override;

    std::vector<BackendDeviceInfo> enumerate() override;
    void setReceiver (Receiver r) override;
    bool enable (const std::string& identifier) override;
    void disableAll() override;
    void start() override;
    void stop() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

class AlsaSeqMidiOutput final : public IMidiOutputBackend
{
public:
    AlsaSeqMidiOutput();
    ~AlsaSeqMidiOutput() override;

    std::vector<BackendDeviceInfo> enumerate() override;
    bool open (const std::string& identifier) override;
    void closeAll() override;
    bool isOpen (const std::string& identifier) const override;
    bool send (const std::string& identifier, const dusk::MidiBuffer& events,
               double baseTimeMs, double sampleRate) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace duskstudio::midi
