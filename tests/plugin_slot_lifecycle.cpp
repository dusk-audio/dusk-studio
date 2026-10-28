#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestTempDirectory.h"
#include "engine/PluginManager.h"
#include "engine/PluginSlot.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <optional>
#include <thread>

#if ! defined (_WIN32)
 #include <csignal>
 #include <sys/types.h>
#endif

using namespace duskstudio;

namespace
{
class LifecyclePluginInstance : public juce::AudioPluginInstance
{
public:
    using juce::AudioPluginInstance::processBlock;

    LifecyclePluginInstance()
        : AudioPluginInstance (BusesProperties()
            .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
    }

    const juce::String getName() const override { return "Lifecycle test"; }

    void prepareToPlay (double, int) override
    {
        ++prepareCalls;
        setLatencySamples (latencyOnPrepare);
    }
    void releaseResources() override          { ++releaseCalls; }

    void processBlock (juce::AudioBuffer<float>&,
                       juce::MidiBuffer&) override
    {
        ++processCalls;
    }

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override                      { return false; }
    bool acceptsMidi() const override                    { return false; }
    bool producesMidi() const override                   { return false; }
    double getTailLengthSeconds() const override         { return 0.0; }

    int getNumPrograms() override                        { return 1; }
    int getCurrentProgram() override                     { return 0; }
    void setCurrentProgram (int) override                {}
    const juce::String getProgramName (int) override     { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override    {}

    void fillInPluginDescription (juce::PluginDescription& description) const override
    {
        description.name = getName();
        description.pluginFormatName = formatName;
        description.fileOrIdentifier = "lifecycle-test";
    }

    // The format the slot takes this plug-in for: "VST3" and "LV2" get the
    // treatment JUCE's hosts for those formats need.
    juce::String formatName { "Test" };
    int prepareCalls = 0;
    int releaseCalls = 0;
    int processCalls = 0;
    int latencyOnPrepare = 0;
};

struct LifecycleGainParameter final : juce::HostedAudioProcessorParameter
{
    float getValue() const override { return value; }
    float get() const { return value; }
    void setValue (float v) override { value = v; }
    float getDefaultValue() const override { return 0.25f; }
    juce::String getName (int) const override { return "Gain"; }
    juce::String getLabel() const override { return {}; }
    juce::String getParameterID() const override { return "gain"; }
    float getValueForText (const juce::String& text) const override { return text.getFloatValue(); }
    float value = 0.25f;
};

class StatefulLifecyclePluginInstance final : public LifecyclePluginInstance
{
public:
    StatefulLifecyclePluginInstance()
    {
        auto parameter = std::make_unique<LifecycleGainParameter>();
        gain = parameter.get();
        addHostedParameter (std::move (parameter));
    }

    int getNumPrograms() override { return 2; }
    int getCurrentProgram() override { return program; }
    void setCurrentProgram (int value) override
    {
        program = value;
        updateHostDisplay (ChangeDetails {}.withProgramChanged (true));
    }
    void changeOpaqueState (std::int32_t value)
    {
        opaque = value;
        updateHostDisplay (ChangeDetails {}.withNonParameterStateChanged (true));
    }
    void getStateInformation (juce::MemoryBlock& state) override
    {
        state.reset();
        const float value = gain->get();
        state.append (&value, sizeof (value));
        state.append (&program, sizeof (program));
        state.append (&opaque, sizeof (opaque));
    }

    LifecycleGainParameter* gain = nullptr;
private:
    std::int32_t program = 0;
    std::int32_t opaque = 0;
};

struct LifecycleMeterParameter final : juce::HostedAudioProcessorParameter
{
    float getValue() const override { return value; }
    void setValue (float v) override { value = v; }
    float getDefaultValue() const override { return 0.0f; }
    juce::String getName (int) const override { return "Meter"; }
    juce::String getLabel() const override { return {}; }
    juce::String getParameterID() const override { return "meter"; }
    float getValueForText (const juce::String& text) const override { return text.getFloatValue(); }
    bool isAutomatable() const override { return false; }
    float value = 0.0f;
};

// Reports a meter from every block, and on request moves its own gain there too.
class MeteringLifecyclePluginInstance final : public LifecyclePluginInstance
{
public:
    MeteringLifecyclePluginInstance()
    {
        auto gainParameter = std::make_unique<LifecycleGainParameter>();
        gain = gainParameter.get();
        addHostedParameter (std::move (gainParameter));
        auto meterParameter = std::make_unique<LifecycleMeterParameter>();
        meter = meterParameter.get();
        addHostedParameter (std::move (meterParameter));
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        LifecyclePluginInstance::processBlock (buffer, midi);
        meter->setValueNotifyingHost (static_cast<float> (processCalls % 7) / 7.0f);
        if (moveGain) gain->setValueNotifyingHost (0.9f);
    }

    LifecycleGainParameter* gain = nullptr;
    LifecycleMeterParameter* meter = nullptr;
    bool moveGain = false;
};

// Notes any call that lands while a test's stand-in audio block holds the slot.
class OverlapProbePluginInstance final : public LifecyclePluginInstance
{
public:
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        note();
        LifecyclePluginInstance::processBlock (buffer, midi);
    }
    void releaseResources() override { note(); LifecyclePluginInstance::releaseResources(); }
    void getStateInformation (juce::MemoryBlock&) override { note(); }

    std::atomic<bool>* blockInFlight = nullptr;
    std::atomic<bool> overlapped { false };

private:
    void note()
    {
        if (blockInFlight != nullptr && blockInFlight->load (std::memory_order_acquire))
            overlapped.store (true, std::memory_order_release);
    }
};

float capturedGain (const juce::String& stateBase64)
{
    juce::MemoryBlock state;
    REQUIRE (state.fromBase64Encoding (stateBase64));
    REQUIRE (state.getSize() == sizeof (float) + 2 * sizeof (std::int32_t));
    float value = 0.0f;
    std::memcpy (&value, state.getData(), sizeof (value));
    return value;
}
} // namespace

TEST_CASE ("PluginSlot tracks content revisions independently of its loaded owner", "[plugin][state][lifecycle]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    PluginManager manager;
    PluginSlot slot;
    slot.setManager (manager);
    slot.prepareToPlay (48000.0, 64);
    CHECK (slot.stateRevision() == 0);
    auto instance = std::make_unique<StatefulLifecyclePluginInstance>();
    auto* stateful = instance.get();
    REQUIRE (slot.installInProcessInstanceForTest (std::move (instance)));
    const auto generation = slot.generation();
    auto revision = slot.stateRevision();
    stateful->gain->beginChangeGesture();
    stateful->gain->endChangeGesture();
    CHECK (slot.stateRevision() == revision);
    stateful->gain->setValueNotifyingHost (0.75f);
    CHECK (slot.stateRevision() > revision);
    CHECK (slot.getLastTouchedParamIndex() == 0);
    revision = slot.stateRevision();
    stateful->setCurrentProgram (1);
    CHECK (slot.stateRevision() > revision);
    revision = slot.stateRevision();
    stateful->changeOpaqueState (23);
    CHECK (slot.stateRevision() > revision);
    revision = slot.stateRevision();
    stateful->setLatencySamples (64);
    stateful->updateHostDisplay (juce::AudioProcessorListener::ChangeDetails {}.withParameterInfoChanged (true));
    CHECK (slot.stateRevision() == revision);
    CHECK (slot.refreshLatencyIfChanged());
    CHECK (slot.getLatencySamples() == 64);
    slot.setBypassed (true);
    slot.setBypassed (false);
    slot.releaseResources();
    slot.prepareToPlay (48000.0, 128);
    CHECK_THAT (capturedGain (slot.getStateBase64ForSave (0)), Catch::Matchers::WithinAbs (0.75f, 1.0e-6f));
    CHECK (slot.stateRevision() == revision);
    CHECK (slot.generation() == generation);
    slot.unload();
    REQUIRE (slot.generation() != generation);
    REQUIRE (slot.installInProcessInstanceForTest (std::make_unique<StatefulLifecyclePluginInstance>()));
    CHECK (slot.generation() != generation);
    CHECK (slot.stateRevision() == revision);
}

TEST_CASE ("PluginSlot captures queued writes before its parameter timer runs", "[plugin][state][lifecycle]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    PluginManager manager;
    PluginSlot slot;
    slot.setManager (manager);
    slot.prepareToPlay (48000.0, 64);
    auto instance = std::make_unique<StatefulLifecyclePluginInstance>();
    auto* stateful = instance.get();
    REQUIRE (slot.installInProcessInstanceForTest (std::move (instance)));
    const auto generation = slot.generation();
    const auto beforeQueue = slot.stateRevision();
    slot.setParamNormalised (0, 0.625f);
    slot.setParamNormalised (0, 0.875f);
    CHECK_THAT (stateful->gain->get(), Catch::Matchers::WithinAbs (0.25f, 1.0e-6f));
    CHECK (slot.stateRevision() > beforeQueue);
    bool replaced = false;
    SECTION ("active owner") {}
    SECTION ("released owner") { slot.releaseResources(); }
    SECTION ("writes from the previous owner are discarded")
    {
        slot.unload();
        REQUIRE (slot.installInProcessInstanceForTest (std::make_unique<StatefulLifecyclePluginInstance>()));
        CHECK (slot.generation() != generation);
        replaced = true;
    }
    const auto queuedRevision = slot.stateRevision();
    std::string error;
    const auto state = slot.getStateBase64ForSave (0, &error);
    CHECK (error.empty());
    CHECK_THAT (capturedGain (state), Catch::Matchers::WithinAbs (replaced ? 0.25f : 0.875f, 1.0e-6f));
    if (replaced) CHECK (slot.stateRevision() == queuedRevision);
    else CHECK (slot.stateRevision() > queuedRevision);
}

TEST_CASE ("PluginSlot refuses reentrant state capture during queued parameter notification", "[plugin][state][lifecycle]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    PluginManager manager;
    PluginSlot slot;
    slot.setManager (manager);
    slot.prepareToPlay (48000.0, 64);
    auto instance = std::make_unique<StatefulLifecyclePluginInstance>();
    auto* stateful = instance.get();
    REQUIRE (slot.installInProcessInstanceForTest (std::move (instance)));
    struct Listener final : juce::AudioProcessorParameter::Listener
    {
        Listener (juce::AudioProcessorParameter& p, PluginSlot& s) : parameter (p), slot (s)
            { parameter.addListener (this); }
        ~Listener() override { parameter.removeListener (this); }
        void parameterValueChanged (int, float) override
        {
            if (++calls == 1) (void) slot.getStateBase64ForSave (0, &error);
        }
        void parameterGestureChanged (int, bool) override {}
        juce::AudioProcessorParameter& parameter;
        PluginSlot& slot;
        int calls = 0;
        std::string error;
    } listener (*stateful->gain, slot);
    slot.setParamNormalised (0, 0.875f);
    std::string error;
    CHECK_THAT (capturedGain (slot.getStateBase64ForSave (0, &error)), Catch::Matchers::WithinAbs (0.875f, 1.0e-6f));
    CHECK (error.empty());
    CHECK (listener.calls == 1);
    CHECK_FALSE (listener.error.empty());
    CHECK_THAT (capturedGain (slot.getStateBase64ForSave (0)), Catch::Matchers::WithinAbs (0.875f, 1.0e-6f));
    CHECK (listener.calls == 1);
}

// A save marks the slot clean against the revision its state reflects. The
// writes the capture delivers are in that state, so they must not leave the slot
// looking edited, while a change after the read must.
TEST_CASE ("PluginSlot reports the revision its captured state reflects", "[plugin][state][lifecycle]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    PluginManager manager;
    PluginSlot slot;
    slot.setManager (manager);
    slot.prepareToPlay (48000.0, 64);
    auto instance = std::make_unique<StatefulLifecyclePluginInstance>();
    auto* stateful = instance.get();
    REQUIRE (slot.installInProcessInstanceForTest (std::move (instance)));
    slot.setParamNormalised (0, 0.875f);
    const auto queued = slot.stateRevision();
    SECTION ("active owner") {}
    SECTION ("released owner") { slot.releaseResources(); }
    std::string error;
    std::uint64_t captured = 0;
    CHECK_THAT (capturedGain (slot.getStateBase64ForSave (0, &error, &captured)),
                Catch::Matchers::WithinAbs (0.875f, 1.0e-6f));
    CHECK (error.empty());
    CHECK (captured > queued);
    CHECK (slot.stateRevision() == captured);
    stateful->changeOpaqueState (5);
    CHECK (slot.stateRevision() > captured);
}

// JUCE's VST3 host reports every restartComponent as a program change, whatever
// the flags - a latency re-announcement, a bus change, new parameter titles - so
// for that format the flag says nothing about state. Its real program changes
// arrive as a parameter change.
TEST_CASE ("PluginSlot does not count a VST3 restart notification as a state change", "[plugin][state][lifecycle]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    PluginManager manager;
    PluginSlot slot;
    slot.setManager (manager);
    slot.prepareToPlay (48000.0, 64);
    auto instance = std::make_unique<StatefulLifecyclePluginInstance>();
    auto* stateful = instance.get();
    stateful->formatName = "VST3";
    REQUIRE (slot.installInProcessInstanceForTest (std::move (instance)));
    const auto revision = slot.stateRevision();
    stateful->updateHostDisplay (juce::AudioProcessorListener::ChangeDetails {}
                                     .withProgramChanged (true).withParameterInfoChanged (true));
    CHECK (slot.stateRevision() == revision);
    stateful->changeOpaqueState (3);
    CHECK (slot.stateRevision() > revision);
}

// A plug-in reports its meters from inside processBlock, every block. None is an
// edit; a parameter the user could automate that the plug-in moves there is.
TEST_CASE ("PluginSlot does not count meters a plug-in reports from its own blocks", "[plugin][state][lifecycle]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    PluginManager manager;
    PluginSlot slot;
    slot.setManager (manager);
    slot.prepareToPlay (48000.0, 64);
    auto instance = std::make_unique<MeteringLifecyclePluginInstance>();
    auto* metering = instance.get();
    REQUIRE (slot.installInProcessInstanceForTest (std::move (instance)));
    const auto revision = slot.stateRevision();
    float left[64] {};
    float right[64] {};
    juce::MidiBuffer midi;
    for (int block = 0; block < 4; ++block)
        slot.processStereoBlock (left, right, 64, midi);
    CHECK (metering->processCalls == 4);
    CHECK (slot.stateRevision() == revision);
    metering->moveGain = true;
    slot.processStereoBlock (left, right, 64, midi);
    CHECK (slot.stateRevision() > revision);
}

// With no device the slot has never been prepared, and an inactive plug-in takes
// no block, so a value JUCE's VST3 or LV2 host is holding cannot reach its state.
// The capture must not vouch for it.
TEST_CASE ("PluginSlot does not vouch for a held parameter change it could not deliver", "[plugin][state][lifecycle]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    PluginManager manager;
    PluginSlot slot;
    slot.setManager (manager);
    auto instance = std::make_unique<StatefulLifecyclePluginInstance>();
    auto* stateful = instance.get();
    stateful->formatName = "LV2";
    REQUIRE (slot.installInProcessInstanceForTest (std::move (instance)));
    REQUIRE (slot.getInstance() == stateful);
    stateful->gain->setValueNotifyingHost (0.5f);

    std::string error;
    std::uint64_t captured = 0;
    (void) slot.getStateBase64ForSave (0, &error, &captured);
    CHECK (error.empty());
    CHECK (stateful->processCalls == 0);
    CHECK (slot.stateRevision() > captured);

    slot.prepareToPlay (48000.0, 64);
    (void) slot.getStateBase64ForSave (0, &error, &captured);
    CHECK (error.empty());
    CHECK (stateful->processCalls == 1);
    CHECK (slot.stateRevision() == captured);
}

// A capture parks the audio pointer and, with a park sleep, merely waits for a
// block to pass. A block that loaded the pointer just before and runs longer is
// still inside the plug-in when the capture's empty block and state read begin,
// and JUCE's LV2 host would run the plug-in twice at once. The audio side holds
// the process lock for the whole block, so the capture has to take it too.
TEST_CASE ("PluginSlot does not capture state while an audio block is still running", "[plugin][state][lifecycle]")
{
    using namespace std::chrono_literals;
    juce::ScopedJuceInitialiser_GUI juceInit;
    PluginManager manager;
    PluginSlot slot;
    slot.setManager (manager);
    slot.prepareToPlay (48000.0, 64);
    auto instance = std::make_unique<OverlapProbePluginInstance>();
    auto* probe = instance.get();
    probe->formatName = "LV2";
    std::atomic<bool> blockInFlight { false };
    probe->blockInFlight = &blockInFlight;
    REQUIRE (slot.installInProcessInstanceForTest (std::move (instance)));

    std::thread audioThread ([&]
    {
        const juce::SpinLock::ScopedLockType processGuard (slot.getProcessLock());
        blockInFlight.store (true, std::memory_order_release);
        std::this_thread::sleep_for (100ms);
        blockInFlight.store (false, std::memory_order_release);
    });
    while (! blockInFlight.load (std::memory_order_acquire))
        std::this_thread::yield();

    std::string error;
    (void) slot.getStateBase64ForSave (0, &error);
    audioThread.join();
    CHECK (error.empty());
    CHECK (probe->processCalls == 1);
    CHECK_FALSE (probe->overlapped.load());
}

TEST_CASE ("PluginSlot republishes an in-process instance after release and prepare")
{
    PluginManager manager;
    PluginSlot slot;
    slot.setManager (manager);
    slot.prepareToPlay (48000.0, 64);

    auto instance = std::make_unique<LifecyclePluginInstance>();
    auto* lifecycle = instance.get();
    REQUIRE (slot.installInProcessInstanceForTest (std::move (instance)));

    slot.releaseResources();
    slot.prepareToPlay (48000.0, 64);

    float left[64] {};
    float right[64] {};
    juce::MidiBuffer midi;
    slot.processStereoBlock (left, right, 64, midi);

    CHECK (lifecycle->releaseCalls == 2);
    CHECK (lifecycle->prepareCalls == 2);
    CHECK (lifecycle->processCalls == 1);
}

// A save reads a plug-in's state between a release and a prepare, and a plug-in
// can settle on another latency when it is prepared, as a look-ahead limiter
// does after its look-ahead has been changed. PDC has to follow the latency the
// plug-in has once the save is done, not the one it had before.
TEST_CASE ("PluginSlot re-reads a plugin's latency when a save re-prepares it", "[plugin]")
{
    PluginManager manager;
    PluginSlot slot;
    slot.setManager (manager);
    slot.prepareToPlay (48000.0, 64);

    auto instance = std::make_unique<LifecyclePluginInstance>();
    auto* lifecycle = instance.get();
    lifecycle->latencyOnPrepare = 32;
    REQUIRE (slot.installInProcessInstanceForTest (std::move (instance)));
    REQUIRE (slot.getLatencySamples() == 32);

    lifecycle->latencyOnPrepare = 128;
    std::string captureError = "previous capture failure";
    juce::MemoryBlock savedState;
    CHECK (savedState.fromBase64Encoding (slot.getStateBase64ForSave (0, &captureError)));
    CHECK (savedState.getSize() == 0);
    CHECK (captureError.empty());
    CHECK (lifecycle->releaseCalls == 1);
    CHECK (lifecycle->prepareCalls == 2);
    CHECK (slot.getLatencySamples() == 128);

    // And the slot is back on the audio path, re-prepared.
    float left[64] {};
    float right[64] {};
    juce::MidiBuffer midi;
    slot.processStereoBlock (left, right, 64, midi);
    CHECK (lifecycle->processCalls == 1);
}

// With no audio device open nothing has given the slot a sample rate or block
// size, so a user load is refused the way the native hosts refuse theirs, before
// any binary is opened and without disturbing what the slot already holds. The
// paths below do not exist: a refusal for the right reason carries the
// not-prepared message, not a file-not-found one.
TEST_CASE ("PluginSlot refuses in-process loads until a device prepares it",
           "[plugin][device]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const juce::File missing = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                   .getChildFile ("dusk-unprepared-slot-never-opened.vst3");
    PluginDescriptor descriptor;
    descriptor.name = "Never opened";
    descriptor.formatName = "VST3";
    descriptor.location = missing.getFullPathName().toStdString();

    PluginManager manager;
    PluginSlot slot;
    slot.setManager (manager);

    auto instance = std::make_unique<LifecyclePluginInstance>();
    auto* lifecycle = instance.get();
    REQUIRE (slot.installInProcessInstanceForTest (std::move (instance)));
    REQUIRE (lifecycle->prepareCalls == 0);

    const auto keptPrevious = [&]
    {
        CHECK (slot.isLoaded());
        CHECK (slot.getInstance() == lifecycle);
        CHECK (slot.getLoadedName() == "Lifecycle test");
        CHECK_FALSE (slot.isOffline());
    };

    juce::String error;
    CHECK_FALSE (slot.loadFromFile (missing, error));
    CHECK (error == "plugin slot not prepared");
    keptPrevious();

    error.clear();
    CHECK_FALSE (slot.loadFromDescriptor (descriptor, error));
    CHECK (error == "plugin slot not prepared");
    keptPrevious();

    bool completed = false;
    bool succeeded = true;
    juce::String asyncError;
    slot.loadFromDescriptorAsync (descriptor, [&] (bool ok, juce::String err)
    {
        completed = true;
        succeeded = ok;
        asyncError = err;
    });
    CHECK (completed);
    CHECK_FALSE (succeeded);
    CHECK (asyncError == "plugin slot not prepared");
    keptPrevious();

    // The kept plugin is prepared once a device arrives, and from then on the
    // same load reaches the loader and fails on its own terms.
    slot.prepareToPlay (48000.0, 64);
    CHECK (lifecycle->prepareCalls == 1);

    error.clear();
    CHECK_FALSE (slot.loadFromFile (missing, error));
    CHECK (error.isNotEmpty());
    CHECK (error != "plugin slot not prepared");
}

#if defined(DUSKSTUDIO_FILE_STATE_LV2_FIXTURE_PATH) && JUCE_PLUGINHOST_LV2
namespace
{
class ScopedFixtureConfig
{
public:
    ScopedFixtureConfig()
    {
        if (const auto* value = std::getenv (dusk::fs::kConfigDirEnv)) previous = value;
        set (directory.path().u8string().c_str());
    }
    ~ScopedFixtureConfig() { set (previous ? previous->c_str() : nullptr); }

    bool seed() const
    {
        std::string error;
        return pluginpaths::save ({ DUSKSTUDIO_FILE_STATE_LV2_FIXTURE_PATH }, error,
                                   directory.path() / "lv2-custom-bundles.json");
    }

private:
    static void set (const char* value)
    {
       #if defined(_WIN32)
        ::_putenv_s (dusk::fs::kConfigDirEnv, value != nullptr ? value : "");
       #else
        if (value != nullptr) ::setenv (dusk::fs::kConfigDirEnv, value, 1);
        else                  ::unsetenv (dusk::fs::kConfigDirEnv);
       #endif
    }
    duskstudio::test::TempDirectory directory { "dusk-plugin-restore-config-" };
    std::optional<std::string> previous;
};

PluginDescriptor statefulStandardDescriptor()
{
    juce::LV2PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> descriptions;
    format.findAllTypesForFile (descriptions, DUSKSTUDIO_FILE_STATE_LV2_FIXTURE_PATH);
    juce::PluginDescription* chosen = nullptr;
    for (auto* description : descriptions)
        if (description->fileOrIdentifier == "urn:duskstudio:test:control-state")
            chosen = description;
    REQUIRE (chosen != nullptr);
    return PluginManager::descriptorFromJuceForTest (*chosen);
}

juce::String nondefaultStandardState (PluginManager& manager, const PluginDescriptor& descriptor)
{
    juce::String error;
    auto source = manager.createPluginInstance (descriptor, 48000.0, 64, error);
    REQUIRE (source != nullptr);
    source->prepareToPlay (48000.0, 64);
    bool changed = false;
    for (auto* param : source->getParameters())
        if (param->getName (100) == "Gain")
        {
            param->setValueNotifyingHost (0.625f);
            changed = true;
        }
    REQUIRE (changed);
    juce::AudioBuffer<float> audio (2, 64);
    audio.clear();
    juce::MidiBuffer midi;
    source->processBlock (audio, midi);
    juce::MemoryBlock state;
    source->getStateInformation (state);
    REQUIRE (state.getSize() > 0);
    return state.toBase64Encoding();
}
}

#if DUSKSTUDIO_HAS_OOP_PLUGINS
TEST_CASE ("PluginSlot restores saved state into an in-process sandbox fallback", "[plugin][ipc][restore]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    ScopedFixtureConfig fixtureConfig;
    REQUIRE (fixtureConfig.seed());
    duskstudio::test::TempDirectory temp ("dusk-plugin-fallback-");
    const auto hostPath = temp.path() / "unavailable-host";
    SECTION ("missing child") {}
    SECTION ("unlaunchable child") { std::ofstream (hostPath) << "not an executable"; }

    PluginManager manager;
    manager.setOopEnabled (true);
    manager.setHostExecutableOverride (hostPath.u8string(), "--ipc-host");
    const auto descriptor = statefulStandardDescriptor();
    const auto saved = nondefaultStandardState (manager, descriptor);
    PluginSlot slot;
    slot.setManager (manager);
    slot.prepareToPlay (48000.0, 64);
    juce::String error;
    REQUIRE (slot.restoreFromSavedState (descriptor, {}, saved, error));
    REQUIRE_FALSE (slot.isRemote());
    REQUIRE (slot.getInstance() != nullptr);
    CHECK_FALSE (slot.isOffline());
    bool checked = false;
    for (auto* param : slot.getInstance()->getParameters())
        if (param->getName (100) == "Gain")
        {
            CHECK_THAT (param->getValue(), Catch::Matchers::WithinAbs (0.625f, 1.0e-6f));
            checked = true;
        }
    CHECK (checked);
    CHECK (slot.getStateBase64ForSave (0) == saved);
}
#endif

TEST_CASE ("PluginSlot rejects malformed saved state and preserves its original reference", "[plugin][ipc][restore]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    ScopedFixtureConfig fixtureConfig;
    REQUIRE (fixtureConfig.seed());
    duskstudio::test::TempDirectory temp ("dusk-plugin-corrupt-state-");
    PluginManager manager;
    manager.setHostExecutableOverride ((temp.path() / "missing-host").u8string(), "--ipc-host");
    SECTION ("in-process") { manager.setOopEnabled (false); }
    SECTION ("sandbox requested") { manager.setOopEnabled (true); }
    auto descriptor = statefulStandardDescriptor();
    const auto originalLocation = descriptor.location;
    for (const char* malformed : { "not a blob", "1.", "9.+C" })
    {
        PluginSlot slot;
        slot.setManager (manager);
        slot.prepareToPlay (48000.0, 64);
        juce::String error;
        CHECK_FALSE (slot.restoreFromSavedState (descriptor, {}, malformed, error));
        CHECK (error.isNotEmpty());
        CHECK_FALSE (slot.isLoaded());
        CHECK (slot.isOffline());
        REQUIRE (slot.getDescriptorForSave (0).has_value());
        CHECK (*slot.getDescriptorForSave (0) == descriptor);
        CHECK (slot.getDescriptorForSave (0)->location == originalLocation);
        CHECK (slot.getStateBase64ForSave (0) == malformed);
    }
    for (const char* empty : { "", "0." })
    {
        PluginSlot slot;
        slot.setManager (manager);
        slot.prepareToPlay (48000.0, 64);
        juce::String error;
        CHECK (slot.restoreFromSavedState (descriptor, {}, empty, error));
        CHECK_FALSE (slot.isOffline());
    }
}

// JUCE's LV2 and VST3 hosts hold a parameter change until the next process call
// writes it into the plug-in, and a capture parks the audio thread first. The
// slot has to deliver the change before it reads state or the save keeps the
// value the plug-in had before the edit.
TEST_CASE ("PluginSlot captures a hosted plug-in's parameter change before its next block", "[plugin][state][lifecycle]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    ScopedFixtureConfig fixtureConfig;
    REQUIRE (fixtureConfig.seed());
    PluginManager manager;
    manager.setOopEnabled (false);
    const auto descriptor = statefulStandardDescriptor();
    const auto edited = nondefaultStandardState (manager, descriptor);
    PluginSlot slot;
    slot.setManager (manager);
    slot.prepareToPlay (48000.0, 64);
    juce::String error;
    REQUIRE (slot.loadFromDescriptor (descriptor, error));
    REQUIRE (slot.getInstance() != nullptr);
    int gainIndex = -1;
    const auto& parameters = slot.getInstance()->getParameters();
    for (int index = 0; index < parameters.size(); ++index)
        if (parameters[index]->getName (100) == "Gain") gainIndex = index;
    REQUIRE (gainIndex >= 0);
    SECTION ("a write the slot queued") { slot.setParamNormalised (gainIndex, 0.625f); }
    SECTION ("an edit made on the plug-in") { parameters[gainIndex]->setValueNotifyingHost (0.625f); }
    std::string captureError;
    CHECK (slot.getStateBase64ForSave (0, &captureError) == edited);
    CHECK (captureError.empty());
}
#endif

#if DUSKSTUDIO_HAS_OOP_PLUGINS
namespace
{
PluginDescriptor sandboxTestDescriptor()
{
    PluginDescriptor descriptor;
    descriptor.name = "sandbox stub";
    descriptor.formatName = "VST3";
    descriptor.location = "/nonexistent/sandbox-stub.vst3";
    return descriptor;
}

// The child is normally resolved beside the running executable; under ctest that
// is the Catch2 binary, so the sandboxed branch is unreachable without pointing
// the slot at the built child and one of its stub modes.
void useSandboxStub (PluginManager& manager, const char* modeArg)
{
    manager.setOopEnabled (true);
    manager.setHostExecutableOverride (DUSKSTUDIO_PLUGIN_HOST_PATH, modeArg);
}

#if ! defined (__APPLE__)
// Runs the dispatch loop until `done` holds or the deadline passes. This JUCE
// build has no runDispatchLoopUntil, and stopDispatchLoop latches the quit flag
// for the life of the MessageManager, so a test gets exactly one pump: a loop
// of short pumps would dispatch nothing after the first.
struct LoopStopper final : dusk::Timer
{
    std::function<bool()> done;
    std::chrono::steady_clock::time_point deadline;

    void timerCallback() override
    {
        if (! done() && std::chrono::steady_clock::now() < deadline) return;
        stopTimer();
        juce::MessageManager::getInstance()->stopDispatchLoop();
    }
};

void pumpUntil (std::function<bool()> done, std::chrono::milliseconds timeout)
{
    LoopStopper stopper;
    stopper.done = std::move (done);
    stopper.deadline = std::chrono::steady_clock::now() + timeout;
    stopper.startTimer (10);
    juce::MessageManager::getInstance()->runDispatchLoop();
}
#endif
} // namespace

// Connecting to the plugin host waits up to 5 s for the handshake and its
// LoadPlugin RPC up to 30 s, so running either on the message thread hands a
// plugin that stalls in the child the power to freeze the editor for over half
// a minute - the failure the sandbox exists to contain. The load has to be
// handed off, which shows up here as the completion never arriving inside the
// call and the slot only going remote once the message loop runs again.
#if ! defined (__APPLE__)
TEST_CASE ("PluginSlot completes an out-of-process load off the message thread", "[plugin][ipc]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    PluginManager manager;
    useSandboxStub (manager, "--ipc-load-reply-stub");

    PluginSlot slot;
    slot.setManager (manager);
    slot.prepareToPlay (48000.0, 64);

    bool completed = false;
    bool succeeded = false;
    slot.loadFromDescriptorAsync (sandboxTestDescriptor(),
                                  [&] (bool ok, juce::String)
    {
        completed = true;
        succeeded = ok;
    });

    CHECK_FALSE (completed);
    CHECK_FALSE (slot.isRemote());

    // Generous: a sanitizer build spawns the child slowly, and the loop stops
    // as soon as the completion lands.
    pumpUntil ([&] { return completed; }, std::chrono::seconds (15));

    CHECK (completed);
    CHECK (succeeded);
    CHECK (slot.isRemote());
}

// A sandboxed plug-in has no latency query once it has loaded, so Re-enable
// after a stall brings back the latency its load reported rather than dropping
// it. The load stub answers no block command, so the first block stalls into the
// bypass while the child stays alive.
TEST_CASE ("PluginSlot keeps a sandboxed plugin's load latency across a stall and Re-enable",
           "[plugin][ipc]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    PluginManager manager;
    useSandboxStub (manager, "--ipc-load-reply-stub");

    PluginSlot slot;
    slot.setManager (manager);
    slot.prepareToPlay (48000.0, 64);

    bool completed = false;
    bool succeeded = false;
    slot.loadFromDescriptorAsync (sandboxTestDescriptor(),
                                  [&] (bool ok, juce::String)
    {
        completed = true;
        succeeded = ok;
    });
    pumpUntil ([&] { return completed; }, std::chrono::seconds (15));

    REQUIRE (succeeded);
    REQUIRE (slot.isRemote());
    REQUIRE (slot.getLatencySamples() == ipc::kLoadStubLatencySamples);

    float left[64] {};
    float right[64] {};
    juce::MidiBuffer midi;
    slot.processStereoBlock (left, right, 64, midi);
    REQUIRE (slot.wasAutoBypassed());
    CHECK_FALSE (slot.wasCrashed());
    CHECK (slot.getLatencySamples() == 0);

    slot.clearAutoBypass();
    CHECK (slot.isRemote());
    CHECK (slot.getLatencySamples() == ipc::kLoadStubLatencySamples);
}

#if ! defined (_WIN32)
// A sandboxed plug-in's latency is in delay compensation while its child runs
// it. A child that dies takes the plug-in out of the signal path, and Re-enable
// on the crashed slot drops the dead child, so neither leaves a latency behind
// for the other tracks to be delayed by.
TEST_CASE ("PluginSlot reports no latency for a crashed sandbox child before or after Re-enable",
           "[plugin][ipc]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    PluginManager manager;
    useSandboxStub (manager, "--ipc-load-reply-stub");

    PluginSlot slot;
    slot.setManager (manager);
    slot.prepareToPlay (48000.0, 64);

    bool completed = false;
    bool succeeded = false;
    bool killed = false;
    int latencyWhileRunning = -1;
    slot.loadFromDescriptorAsync (sandboxTestDescriptor(),
                                  [&] (bool ok, juce::String)
    {
        completed = true;
        succeeded = ok;
    });

    // The crash is noticed by the slot's reaper timer, so the whole case runs
    // inside the one dispatch loop a test gets.
    pumpUntil ([&]
    {
        if (! completed) return false;
        if (! succeeded || ! slot.isRemote()) return true;
        if (! killed)
        {
            latencyWhileRunning = slot.getLatencySamples();
            killed = ::kill ((pid_t) slot.getRemoteChildPid(), SIGKILL) == 0;
            return ! killed;
        }
        return slot.wasCrashed();
    }, std::chrono::seconds (20));

    REQUIRE (succeeded);
    REQUIRE (killed);
    REQUIRE (slot.wasCrashed());
    CHECK (latencyWhileRunning == ipc::kLoadStubLatencySamples);
    CHECK (slot.wasAutoBypassed());
    CHECK (slot.getLatencySamples() == 0);

    slot.clearAutoBypass();
    CHECK_FALSE (slot.wasCrashed());
    CHECK_FALSE (slot.isRemote());
    CHECK (slot.getLatencySamples() == 0);
}
#endif

// With no child binary where the loader looks, the sandbox is simply not
// available: the load must still take the in-process path rather than fail or
// wait on a host that will never answer.
TEST_CASE ("PluginSlot falls back to in-process when the host binary is missing", "[plugin][ipc]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    PluginManager manager;
    manager.setOopEnabled (true);
    manager.setHostExecutableOverride (
        (juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile ("dusk-studio-plugin-host-that-is-not-there")
            .getFullPathName()).toStdString(),
        "--ipc-load-reply-stub");

    PluginSlot slot;
    slot.setManager (manager);
    slot.prepareToPlay (48000.0, 64);

    bool completed = false;
    slot.loadFromDescriptorAsync (sandboxTestDescriptor(),
                                  [&] (bool, juce::String) { completed = true; });

    pumpUntil ([&] { return completed; }, std::chrono::seconds (15));

    CHECK (completed);
    // The descriptor names a plugin that does not exist either, so the
    // in-process load it fell back to fails on its own terms. What matters is
    // that it never went remote and never waited for a handshake.
    CHECK_FALSE (slot.isRemote());
}

// The same async load, with the device callback already running across it. A
// child spawned from the load worker dies with that worker, which the startup
// restore never sees because it spawns from the message thread. Needs the stub
// that answers block commands as well as control RPCs; the control-only one
// cannot keep any slot alive under a live callback.
TEST_CASE ("PluginSlot keeps its sandbox child when the graph is live across the load", "[plugin][ipc]")
{
    using namespace std::chrono_literals;

    juce::ScopedJuceInitialiser_GUI juceInit;

    PluginManager manager;
    useSandboxStub (manager, "--ipc-load-audio-stub");

    PluginSlot slot;
    slot.setManager (manager);
    slot.prepareToPlay (48000.0, 64);

    std::atomic<bool> stopAudio { false };
    std::atomic<int>  blocksRun { 0 };

    std::thread audioThread ([&]
    {
        float left[64] {};
        float right[64] {};
        juce::MidiBuffer midi;
        while (! stopAudio.load (std::memory_order_acquire))
        {
            slot.processStereoBlock (left, right, 64, midi);
            blocksRun.fetch_add (1, std::memory_order_relaxed);
            std::this_thread::sleep_for (1333us);
        }
    });

    while (blocksRun.load (std::memory_order_relaxed) < 8)
        std::this_thread::yield();

    std::atomic<bool> completed { false };
    std::atomic<bool> succeeded { false };
    slot.loadFromDescriptorAsync (sandboxTestDescriptor(),
                                  [&] (bool ok, juce::String)
    {
        succeeded.store (ok, std::memory_order_release);
        completed.store (true, std::memory_order_release);
    });

    // Keeps pumping past the completion: the child is lost about 200 ms after a
    // load that reported success, so stopping at the completion would pass.
    std::chrono::steady_clock::time_point settleUntil {};
    pumpUntil ([&]
    {
        if (! completed.load (std::memory_order_acquire)) return false;
        if (settleUntil.time_since_epoch().count() == 0)
            settleUntil = std::chrono::steady_clock::now() + 1500ms;
        return std::chrono::steady_clock::now() >= settleUntil;
    }, std::chrono::seconds (20));

    stopAudio.store (true, std::memory_order_release);
    audioThread.join();

    REQUIRE (completed.load (std::memory_order_acquire));
    REQUIRE (succeeded.load (std::memory_order_acquire));
    CHECK_FALSE (slot.wasCrashed());
    CHECK_FALSE (slot.wasAutoBypassed());
    CHECK (slot.isRemote());
}
#endif

// Quitting while a child stalls used to cost the destructor the whole handshake
// budget plus the whole LoadPlugin deadline, per slot, on the message thread.
// The worker is cancellable now, so both stalls unwind in about one poll slice
// plus the child teardown.
TEST_CASE ("PluginSlot destruction cancels a stalled out-of-process load", "[plugin][ipc]")
{
    using namespace std::chrono_literals;

    // Enough for the fork/exec and, where the child acks, the handshake - so
    // the worker really is parked in the wait this covers.
    constexpr auto kSettleTime = 200ms;
    constexpr auto kBound      = 1000ms;

    const char* modeArg = nullptr;
    SECTION ("stalled in the LoadPlugin reply wait") { modeArg = "--ipc-stub"; }
    SECTION ("stalled in the ready handshake")       { modeArg = "--ipc-mute-handshake-stub"; }

    PluginManager manager;
    useSandboxStub (manager, modeArg);

    std::chrono::steady_clock::duration elapsed {};
    {
        auto slot = std::make_unique<PluginSlot>();
        slot->setManager (manager);
        slot->prepareToPlay (48000.0, 64);
        slot->loadFromDescriptorAsync (sandboxTestDescriptor(),
                                       [] (bool, juce::String) {});

        std::this_thread::sleep_for (kSettleTime);

        const auto start = std::chrono::steady_clock::now();
        slot.reset();
        elapsed = std::chrono::steady_clock::now() - start;
    }

    INFO ("destructor took "
          << std::chrono::duration_cast<std::chrono::milliseconds> (elapsed).count()
          << " ms");
    CHECK (elapsed < kBound);
}

// The audio thread try-locks processLock and then stays inside
// RemotePluginConnection::processBlockSync for up to the OOP block timeout,
// reading the child's shared memory the whole time. Every message-thread path
// that rotates the deferred-destruction ring destroys the connection evicted
// from the far slot, which unmaps that shared memory - so the rotation has to
// happen with the lock held, not merely after currentRemote has been nulled.
TEST_CASE ("PluginSlot rotates the remote ring under the process lock", "[plugin][ipc]")
{
    using namespace std::chrono_literals;

    PluginManager manager;
    PluginSlot slot;
    slot.setManager (manager);
    slot.prepareToPlay (48000.0, 64);

    std::atomic<bool> holdingLock { false };
    std::atomic<bool> holdComplete { false };

    std::thread audioThread ([&]
    {
        const juce::SpinLock::ScopedLockType processGuard (slot.getProcessLock());
        holdingLock.store (true, std::memory_order_release);
        std::this_thread::sleep_for (150ms);
        holdComplete.store (true, std::memory_order_release);
    });

    while (! holdingLock.load (std::memory_order_acquire))
        std::this_thread::yield();

    slot.unload();
    const bool waitedForAudioThread = holdComplete.load (std::memory_order_acquire);

    audioThread.join();
    CHECK (waitedForAudioThread);
}
#endif
