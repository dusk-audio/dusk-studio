#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/ChannelStrip.h"
#include "engine/PdcMath.h"
#include "engine/PluginManager.h"

#include <array>
#include <cmath>

using Catch::Matchers::WithinAbs;
using duskstudio::ChannelStrip;
using duskstudio::pdc::computeCompensations;

namespace
{
constexpr int kBlock = 64;
constexpr std::array<std::array<int, 2>, 4> kRetargets {{ { 0, 96 }, { 64, 192 }, { 192, 32 }, { 192, 0 } }};
using Block = std::array<float, kBlock>;

struct StripPdcHarness
{
    duskstudio::ChannelStripParams params;
    ChannelStrip strip;
    Block left {}, right {}, masterL {}, masterR {};
    std::array<Block, ChannelStrip::kNumBuses> busL {}, busR {};
    std::array<Block, ChannelStrip::kNumAuxSends> auxL {}, auxR {};
    std::array<float*, ChannelStrip::kNumBuses> busLPtrs {}, busRPtrs {};
    std::array<float*, ChannelStrip::kNumAuxSends> auxLPtrs {}, auxRPtrs {};
    bool stereo;

    StripPdcHarness (bool useStereo, int delay) : stereo (useStereo)
    {
        params.eqEnabled.store (false);
        params.compEnabled.store (false);
        params.faderDb.store (0.0f);
        params.pan.store (0.0f);
        strip.bind (params);
        strip.insertMode.store (ChannelStrip::kInsertEmpty);
        strip.prepare (48000.0, kBlock, 1);
        strip.setPdcCompensationSamples (delay);
        for (std::size_t i = 0; i < busL.size(); ++i)
        {
            busLPtrs[i] = busL[i].data();
            busRPtrs[i] = busR[i].data();
        }
        for (std::size_t i = 0; i < auxL.size(); ++i)
        {
            auxLPtrs[i] = auxL[i].data();
            auxRPtrs[i] = auxR[i].data();
        }
    }

    void run (bool gate = true, bool frozen = false)
    {
        masterL.fill (0.0f); masterR.fill (0.0f);
        for (auto& bus : busL) bus.fill (0.0f);
        for (auto& bus : busR) bus.fill (0.0f);
        for (auto& aux : auxL) aux.fill (0.0f);
        for (auto& aux : auxR) aux.fill (0.0f);
        juce::MidiBuffer midi;
        strip.processAndAccumulate (left.data(), stereo ? right.data() : nullptr, midi, false,
                                    masterL.data(), masterR.data(), busLPtrs, busRPtrs,
                                    auxLPtrs, auxRPtrs, kBlock, gate,
                                    nullptr, 0, nullptr, 0, frozen);
    }

    void signal (int block)
    {
        for (int i = 0; i < kBlock; ++i)
        {
            const int frame = block * kBlock + i;
            left[(std::size_t) i] = 0.2f + 0.001f * (float) ((frame * 17) % 101);
            right[(std::size_t) i] = -0.1f - 0.0013f * (float) ((frame * 29) % 73);
        }
    }

    void silence() { left.fill (0.0f); right.fill (0.0f); }

    void expectImpulseAt (int delay)
    {
        std::array<float, 2> peaks {};
        std::array<int, 2> positions { -1, -1 };
        for (int block = 0; block < 8; ++block)
        {
            silence();
            if (block == 0) { left[0] = 0.5f; right[0] = -0.25f; }
            run();
            for (std::size_t c = 0; c < peaks.size(); ++c)
            {
                const auto& output = c == 0 ? masterL : masterR;
                for (int i = 0; i < kBlock; ++i)
                    if (std::abs (output[(std::size_t) i]) > peaks[c])
                    {
                        peaks[c] = std::abs (output[(std::size_t) i]);
                        positions[c] = block * kBlock + i;
                    }
            }
        }
        for (std::size_t c = 0; c < peaks.size(); ++c)
        {
            CAPTURE (c, delay);
            REQUIRE (peaks[c] > 0.05f);
            REQUIRE (positions[c] == delay);
        }
    }
};

class TransparentDelay final : public juce::AudioPluginInstance
{
public:
    using juce::AudioPluginInstance::processBlock;

    explicit TransparentDelay (int delay)
        : AudioPluginInstance (BusesProperties()
            .withInput ("Input", juce::AudioChannelSet::stereo(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true)), latency (delay) {}

    const juce::String getName() const override { return "Transparent delay"; }
    void prepareToPlay (double, int) override
    {
        for (auto& line : lines) line.setMaximumDelayInSamples (ChannelStrip::kMaxPdcSamples);
        setDelay (latency);
    }
    void setDelay (int delay)
    {
        latency = delay;
        for (auto& line : lines) line.setDelay (delay);
        setLatencySamples (delay);
    }
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& audio, juce::MidiBuffer&) override
    {
        for (int c = 0; c < audio.getNumChannels(); ++c)
            for (int i = 0; i < audio.getNumSamples(); ++i)
            {
                lines[(std::size_t) c].pushSample (audio.getSample (c, i));
                audio.setSample (c, i, lines[(std::size_t) c].popSample());
            }
        if (bypassAfterProcess != nullptr)
        {
            bypassAfterProcess->setBypassed (true);
            bypassAfterProcess = nullptr;
        }
    }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}
    void fillInPluginDescription (juce::PluginDescription& description) const override
    {
        description.name = getName();
        description.pluginFormatName = "Test";
        description.fileOrIdentifier = "transparent-delay";
    }

    duskstudio::PluginSlot* bypassAfterProcess = nullptr;

private:
    std::array<dusk::audio::IntDelayLine, 2> lines;
    int latency;
};

struct InsertPdcHarness : StripPdcHarness
{
    duskstudio::PluginManager manager;
    duskstudio::HardwareInsertParams hardwareParams;
    TransparentDelay* plugin = nullptr;

    InsertPdcHarness (bool useStereo, int latency, bool hardware = false)
        : StripPdcHarness (useStereo, 0)
    {
        strip.insertMode.store (hardware ? ChannelStrip::kInsertHardware : ChannelStrip::kInsertPlugin);
        if (hardware)
        {
            hardwareParams.routing.mutate ([latency] (auto& routing) { routing.latencySamples = latency; });
            strip.bindHardwareInsert (hardwareParams);
        }
        strip.prepare (48000.0, kBlock, 1);
        if (hardware)
            strip.getHardwareInsertSlot().setOfflineRender (true);
        else
        {
            strip.bindPluginManager (manager);
            auto instance = std::make_unique<TransparentDelay> (latency);
            plugin = instance.get();
            REQUIRE (strip.getPluginSlot().installInProcessInstanceForTest (std::move (instance)));
        }
    }

    void setLatency (int latency)
    {
        if (plugin != nullptr)
        {
            plugin->setDelay (latency);
            REQUIRE (strip.getPluginSlot().refreshLatencyIfChanged());
        }
        else
        {
            hardwareParams.routing.mutate ([latency] (auto& routing) { routing.latencySamples = latency; });
        }
    }
};

void expectSameOutput (const StripPdcHarness& actual, const StripPdcHarness& reference)
{
    for (std::size_t i = 0; i < actual.masterL.size(); ++i)
    {
        CAPTURE (i);
        REQUIRE_THAT (actual.masterL[i], WithinAbs (reference.masterL[i], 1e-6f));
        REQUIRE_THAT (actual.masterR[i], WithinAbs (reference.masterR[i], 1e-6f));
    }
}

void warm (StripPdcHarness& actual, StripPdcHarness& reference)
{
    for (int block = 0; block < 40; ++block)
    {
        actual.signal (block); reference.signal (block);
        actual.run(); reference.run();
    }
}
}

TEST_CASE ("PDC: every track compensates to the deepest latency", "[pdc]")
{
    // Track 1 is the deepest (512). Each track delays by deepest - own so all
    // line up; the deepest track itself adds zero.
    const std::array<int, 4> latency { 0, 512, 128, 64 };
    std::array<int, 4> comp {};
    const int deepest = computeCompensations (latency.data(), comp.data(), 4);

    REQUIRE (deepest == 512);
    REQUIRE (comp[0] == 512);   // no latency → delayed by the full amount
    REQUIRE (comp[1] == 0);     // the deepest track is the reference
    REQUIRE (comp[2] == 384);
    REQUIRE (comp[3] == 448);

    // Post-condition: own latency + compensation is identical for every track.
    for (int i = 0; i < 4; ++i)
        REQUIRE (latency[(size_t) i] + comp[(size_t) i] == deepest);
}

TEST_CASE ("PDC: all-zero latency yields no compensation", "[pdc]")
{
    const std::array<int, 3> latency { 0, 0, 0 };
    std::array<int, 3> comp { -1, -1, -1 };
    const int deepest = computeCompensations (latency.data(), comp.data(), 3);

    REQUIRE (deepest == 0);
    for (int c : comp) REQUIRE (c == 0);
}

TEST_CASE ("PDC: a single latency source delays everyone else", "[pdc]")
{
    std::array<int, 5> latency { 0, 0, 300, 0, 0 };
    std::array<int, 5> comp {};
    const int deepest = computeCompensations (latency.data(), comp.data(), 5);

    REQUIRE (deepest == 300);
    REQUIRE (comp[2] == 0);
    for (int i : { 0, 1, 3, 4 }) REQUIRE (comp[(size_t) i] == 300);
}

TEST_CASE ("Channel strip PDC keeps its applied delay while audio is continuous", "[pdc][channel-strip]")
{
    const bool stereo = GENERATE (false, true);
    const auto change = kRetargets[(std::size_t) GENERATE (0, 1, 2, 3)];
    CAPTURE (stereo, change[0], change[1]);
    StripPdcHarness actual { stereo, change[0] }, reference { stereo, change[0] };
    warm (actual, reference);
    actual.strip.setPdcCompensationSamples (change[1]);
    for (int block = 40; block < 52; ++block)
    {
        CAPTURE (block);
        actual.signal (block); reference.signal (block);
        actual.run(); reference.run();
        expectSameOutput (actual, reference);
    }
}

TEST_CASE ("Channel strip PDC preserves its tail and retargets after silence", "[pdc][channel-strip]")
{
    const bool stereo = GENERATE (false, true);
    const auto change = kRetargets[(std::size_t) GENERATE (0, 1, 2, 3)];
    CAPTURE (stereo, change[0], change[1]);
    StripPdcHarness actual { stereo, change[0] }, reference { stereo, change[0] };
    warm (actual, reference);
    actual.strip.setPdcCompensationSamples (change[1]);
    actual.silence(); reference.silence();
    for (int block = 0; block < (change[0] + kBlock - 1) / kBlock; ++block)
    {
        actual.run(); reference.run();
        expectSameOutput (actual, reference);
    }
    for (int block = 0; block < 8; ++block) actual.run();
    REQUIRE (actual.strip.getLastProcessedSamples() == 0);
    actual.expectImpulseAt (change[1]);
}

TEST_CASE ("Channel strip PDC retargets after an inactive gate", "[pdc][channel-strip]")
{
    const bool stereo = GENERATE (false, true);
    const auto change = kRetargets[(std::size_t) GENERATE (0, 1, 2, 3)];
    CAPTURE (stereo, change[0], change[1]);
    StripPdcHarness actual { stereo, change[0] }, reference { stereo, change[0] };
    warm (actual, reference);
    actual.strip.setPdcCompensationSamples (change[1]);
    actual.run (false);
    REQUIRE (actual.strip.getLastProcessedSamples() == 0);
    actual.expectImpulseAt (change[1]);
}

TEST_CASE ("Channel insert LED preserves the wet path timing through both transitions", "[pdc][channel-strip][insert-led]")
{
    const bool stereo = GENERATE (false, true);
    const int latency = GENERATE (96, 193, ChannelStrip::kMaxPdcSamples);
    CAPTURE (stereo, latency);
    InsertPdcHarness actual { stereo, latency }, reference { stereo, latency };
    for (int block = 0; block < latency / kBlock + 40; ++block)
    {
        actual.signal (block); reference.signal (block);
        actual.run(); reference.run();
    }
    for (const bool bypassed : { true, false })
    {
        actual.params.insertBypassed.store (bypassed);
        for (int block = 40; block < 72; ++block)
        {
            CAPTURE (bypassed, block);
            actual.signal (block); reference.signal (block);
            actual.run(); reference.run();
            expectSameOutput (actual, reference);
        }
    }
    actual.params.insertBypassed.store (true);
    warm (actual, reference);
    actual.plugin->bypassAfterProcess = &actual.strip.getPluginSlot();
    reference.plugin->bypassAfterProcess = &reference.strip.getPluginSlot();
    for (int block = 72; block < 76; ++block)
    {
        actual.signal (block); reference.signal (block);
        actual.run(); reference.run();
        REQUIRE (actual.strip.getPluginSlot().getLatencySamples() == 0);
        expectSameOutput (actual, reference);
    }
}

TEST_CASE ("Channel insert LED follows changed plugin latency with warm dry history", "[pdc][channel-strip][insert-led]")
{
    const bool stereo = GENERATE (false, true);
    const int latency = GENERATE (0, 96, 257);
    CAPTURE (stereo, latency);
    InsertPdcHarness actual { stereo, 193 }, reference { stereo, 193 };
    warm (actual, reference);
    actual.params.insertBypassed.store (true);
    for (int block = 40; block < 88; ++block)
    {
        if (block == 60) { actual.setLatency (latency); reference.setLatency (latency); }
        if (block == 68) actual.params.insertBypassed.store (false);
        actual.signal (block); reference.signal (block);
        actual.run(); reference.run();
        CAPTURE (block);
        expectSameOutput (actual, reference);
    }
}

TEST_CASE ("Channel insert LED aligns hardware dry audio including frozen playback", "[pdc][channel-strip][insert-led]")
{
    const bool frozen = GENERATE (false, true);
    const bool stereo = GENERATE (false, true);
    CAPTURE (frozen, stereo);
    InsertPdcHarness actual { stereo, 193, true }, reference { stereo, 193, true };
    for (int block = 0; block < 120; ++block)
    {
        if (block == 40 || block == 80) actual.params.insertBypassed.store (true);
        if (block == 60 || block == 100) actual.params.insertBypassed.store (false);
        if (block == 85) { actual.setLatency (96); reference.setLatency (96); }
        actual.signal (block); reference.signal (block);
        actual.run (true, frozen); reference.run (true, frozen);
        CAPTURE (block);
        expectSameOutput (actual, reference);
    }
}
