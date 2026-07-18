#include "JuceMidiBackend.h"

#include <map>
#include <utility>
#include <vector>

namespace duskstudio::midi
{
namespace
{
juce::AudioDeviceManager* deviceManager = nullptr;

class JuceMidiInput final : public IMidiInputBackend,
                            private juce::MidiInputCallback
{
public:
    ~JuceMidiInput() override { stop(); }

    std::vector<BackendDeviceInfo> enumerate() override
    {
        std::vector<BackendDeviceInfo> out;
        for (const auto& d : juce::MidiInput::getAvailableDevices())
            out.push_back ({ d.name.toStdString(), d.identifier.toStdString() });
        return out;
    }

    void setReceiver (Receiver r) override { receiver = std::move (r); }

    // JUCE's identifiers are what earlier sessions already hold on the platforms
    // this backend serves, so there is nothing to migrate.
    std::string migrateIdentifier (const std::string&) override { return {}; }

    bool enable (const std::string& identifier) override
    {
        if (deviceManager == nullptr) return false;
        const juce::String id (identifier);
        // setMidiInputDeviceEnabled returns void - re-query to verify.
        deviceManager->setMidiInputDeviceEnabled (id, true);
        if (! deviceManager->isMidiInputDeviceEnabled (id)) return false;
        enabled.push_back (identifier);
        return true;
    }

    void disableAll() override
    {
        if (deviceManager != nullptr)
            for (const auto& id : enabled)
                deviceManager->setMidiInputDeviceEnabled (juce::String (id), false);
        enabled.clear();
    }

    void start() override
    {
        // Empty identifier = every enabled input fans in here, demuxed by source.
        if (deviceManager != nullptr)
            deviceManager->addMidiInputDeviceCallback ({}, this);
    }

    void stop() override
    {
        // JUCE's remove joins the MIDI dispatch side before returning, which is
        // the detach fence the seam relies on.
        if (deviceManager != nullptr)
            deviceManager->removeMidiInputDeviceCallback ({}, this);
    }

private:
    void handleIncomingMidiMessage (juce::MidiInput* source,
                                    const juce::MidiMessage& message) override
    {
        if (source == nullptr || ! receiver) return;
        // Stamped with the seam's clock, not the message's own timestamp: the
        // collector this feeds drains against backendClockMs(), and JUCE's
        // timestamp epoch is a different one.
        receiver (source->getIdentifier().toStdString(),
                  message.getRawData(), message.getRawDataSize(), backendClockMs());
    }

    Receiver                 receiver;
    std::vector<std::string> enabled;
};

class JuceMidiOutput final : public IMidiOutputBackend
{
public:
    std::vector<BackendDeviceInfo> enumerate() override
    {
        std::vector<BackendDeviceInfo> out;
        for (const auto& d : juce::MidiOutput::getAvailableDevices())
            out.push_back ({ d.name.toStdString(), d.identifier.toStdString() });
        return out;
    }

    std::string migrateIdentifier (const std::string&) override { return {}; }

    bool open (const std::string& identifier) override
    {
        if (outputs.count (identifier)) return true;   // lazy open is idempotent
        auto out = juce::MidiOutput::openDevice (juce::String (identifier));
        if (out == nullptr) return false;
        // Background thread so sendBlockOfMessages enqueues without blocking the
        // pump on the OS port.
        out->startBackgroundThread();
        outputs[identifier] = std::move (out);
        return true;
    }

    void closeAll() override { outputs.clear(); }

    bool isOpen (const std::string& identifier) const override
    {
        return outputs.count (identifier) > 0;
    }

    bool send (const std::string& identifier, const dusk::MidiBuffer& events,
               double baseTimeMs, double sampleRate) override
    {
        const auto it = outputs.find (identifier);
        if (it == outputs.end() || it->second == nullptr) return false;

        scratch.clear();
        for (const auto meta : events)
            scratch.addEvent (meta.data, meta.numBytes, meta.samplePosition);

        // baseTimeMs comes from the seam's clock; sendBlockOfMessages schedules
        // against JUCE's, a different epoch. The pump's ~1 ms cadence makes
        // "now" the right base anyway - the same immediate-send policy the
        // native backend uses.
        (void) baseTimeMs;
        it->second->sendBlockOfMessages (scratch,
                                         juce::Time::getMillisecondCounterHiRes(),
                                         sampleRate > 0.0 ? sampleRate : 48000.0);
        return true;
    }

private:
    std::map<std::string, std::unique_ptr<juce::MidiOutput>> outputs;
    juce::MidiBuffer scratch;   // pump thread only
};
} // namespace

void setJuceMidiDeviceManager (juce::AudioDeviceManager& dm) { deviceManager = &dm; }

std::unique_ptr<IMidiInputBackend>  makeJuceMidiInputBackend()  { return std::make_unique<JuceMidiInput>(); }
std::unique_ptr<IMidiOutputBackend> makeJuceMidiOutputBackend() { return std::make_unique<JuceMidiOutput>(); }
} // namespace duskstudio::midi
