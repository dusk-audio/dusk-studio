#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/builtin/DafUnitInstance.h"

#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// The host a DAF plug-in runs under as a built-in unit, driven here through a
// stand-in plug-in that records what reaches it. Tape Echo 2 itself is covered in
// builtin_tape_echo_2.cpp; these pin the host's own contract: parameters at the
// plug-in's indices, writes that reach the plug-in only on the audio thread and in
// order, the mirror, transport, latency and the session blob.

using namespace duskstudio;
using namespace duskstudio::builtin;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int    kBlock      = 64;
constexpr const char* kId    = "dusk.builtin.fake";

enum FakeParam : std::uint32_t { kGain = 0, kSteps, kAge, kOn, kLegacy, kMeter, kNumFakeParams };

class FakePlugin final : public DafPlugin
{
public:
    struct Write { std::uint32_t index; float value; };

    FakePlugin()
    {
        descs.resize (kNumFakeParams);
        auto set = [this] (FakeParam i, const char* symbol, float lo, float hi, float def)
        {
            descs[i].symbol = symbol;
            descs[i].name = symbol;
            descs[i].minValue = lo;
            descs[i].maxValue = hi;
            descs[i].defaultValue = def;
        };
        set (kGain, "gain", 0.0f, 2.0f, 1.0f);
        set (kSteps, "steps", 1.0f, 4.0f, 1.0f);
        descs[kSteps].isInteger = true;
        set (kAge, "age", 0.0f, 1.0f, 0.5f);
        descs[kAge].enumValues = { 0.0f, 0.5f, 1.0f };
        descs[kAge].enumLabels = { "New", "Used", "Old" };
        set (kOn, "on", 0.0f, 1.0f, 1.0f);
        descs[kOn].isBoolean = descs[kOn].isInteger = true;
        set (kLegacy, "legacy", 0.0f, 1.0f, 0.0f);
        descs[kLegacy].isHidden = true;
        set (kMeter, "meter", 0.0f, 4.0f, 0.0f);
        descs[kMeter].isOutput = true;

        for (std::size_t i = 0; i < descs.size(); ++i)
            held[i] = descs[i].defaultValue;
    }

    const std::vector<DafParamDesc>& params() const noexcept override { return descs; }
    int numInputs() const noexcept override  { return 2; }
    int numOutputs() const noexcept override { return 2; }

    void activate (double, int) override { ++activations; activeNow = true; }
    void deactivate() override { activeNow = false; }

    float getParameterValue (std::uint32_t index) const noexcept override { return held[index]; }
    void setParameterValue (std::uint32_t index, float value) noexcept override
    {
        held[index] = value;
        log.push_back ({ index, value });
    }

    void setTimePosition (const dusk::TransportPosition& position) noexcept override
    {
        ++transportCalls;
        lastBpm = position.bpm;
    }

    void run (const float* const* in, float* const* out, std::uint32_t frames) noexcept override
    {
        float peak = 0.0f;
        for (std::uint32_t c = 0; c < 2; ++c)
            for (std::uint32_t i = 0; i < frames; ++i)
            {
                out[c][i] = in[c][i] * held[kGain];
                peak = std::max (peak, std::abs (out[c][i]));
            }
        held[kMeter] = peak;
        ++runs;
    }

    int latencySamples() const noexcept override { return latency; }

    // The stand-in has no editor; the editor host is covered in daf_editor_host.cpp.
    bool hasEditor() const noexcept override { return false; }
    std::uint32_t editorWidth() const noexcept override  { return 0; }
    std::uint32_t editorHeight() const noexcept override { return 0; }
    std::unique_ptr<duskstudio::builtin::DafEditor> createEditor (
        std::uintptr_t, std::uint32_t, std::uint32_t, double,
        duskstudio::builtin::DafEditorCallbacks, std::string& errorOut) override
    {
        errorOut = "the stand-in plug-in has no editor.";
        return nullptr;
    }

    std::vector<DafParamDesc> descs;
    std::array<float, kNumFakeParams> held {};
    std::vector<Write> log;
    int activations = 0;
    bool activeNow = false;
    int transportCalls = 0;
    double lastBpm = 0.0;
    int runs = 0;
    int latency = 0;
};

struct Rig
{
    Rig()
    {
        auto owned = std::make_unique<FakePlugin>();
        fake = owned.get();
        unit = std::make_unique<DafUnitInstance> (kId, std::move (owned));
    }

    void activate()
    {
        std::string error;
        REQUIRE (unit->activate (kSampleRate, kBlock, error));
    }

    // One block of a constant signal; returns the left output's first sample.
    float process (float in, const dusk::TransportPosition* transport = nullptr)
    {
        std::fill (inL.begin(), inL.end(), in);
        std::fill (inR.begin(), inR.end(), in);
        float* ins[2]  = { inL.data(), inR.data() };
        float* outs[2] = { outL.data(), outR.data() };
        hosting::PortBuffers io;
        io.mainIn = ins;
        io.mainInChannels = 2;
        io.mainOut = outs;
        io.mainOutChannels = 2;
        io.numFrames = kBlock;
        io.transport = transport;
        unit->processBlock (io);
        return outL[0];
    }

    FakePlugin* fake = nullptr;
    std::unique_ptr<DafUnitInstance> unit;
    std::array<float, kBlock> inL {}, inR {}, outL {}, outR {};
};

std::vector<std::uint8_t> blob (const std::string& text)
{
    return { text.begin(), text.end() };
}
} // namespace

TEST_CASE ("a DAF unit presents the plug-in's parameters at the plug-in's indices",
           "[builtin][daf]")
{
    Rig rig;
    REQUIRE (rig.unit->paramCount() == (int) kNumFakeParams);

    const auto* gain = rig.unit->paramInfo (kGain);
    REQUIRE (gain != nullptr);
    REQUIRE (std::string (gain->id) == "gain");
    REQUIRE (gain->kind == ParamKind::Continuous);
    REQUIRE_FALSE (gain->hidden);
    REQUIRE_THAT (gain->maxValue, WithinAbs (2.0, 1e-9));
    REQUIRE_THAT (gain->defaultValue, WithinAbs (1.0, 1e-9));

    const auto* steps = rig.unit->paramInfo (kSteps);
    REQUIRE (steps->kind == ParamKind::Choice);
    REQUIRE (steps->choiceCount == 4);
    REQUIRE (std::string (steps->choices[0]) == "1");
    REQUIRE (std::string (steps->choices[3]) == "4");

    REQUIRE (rig.unit->paramInfo (kOn)->kind == ParamKind::Toggle);
    REQUIRE (rig.unit->paramInfo (kAge)->kind == ParamKind::Continuous);
    REQUIRE (rig.unit->paramInfo (kLegacy)->hidden);
    REQUIRE (rig.unit->paramInfo (kMeter)->hidden);
    REQUIRE (rig.unit->paramInfo (kNumFakeParams) == nullptr);

    REQUIRE (rig.unit->portLayout().inputs.size() == 1);
    REQUIRE (rig.unit->portLayout().outputs.size() == 1);
    REQUIRE (rig.unit->portLayout().outputs[0].channelCount == 2);
}

TEST_CASE ("a DAF unit's parameter writes reach the plug-in only on the audio thread",
           "[builtin][daf]")
{
    Rig rig;
    rig.activate();
    rig.fake->log.clear();

    rig.unit->setParamValue (kGain, 0.5f);
    rig.unit->setParamValue (kSteps, 3.0f);
    rig.unit->setParamValue (kGain, 0.25f);

    // Queued, not applied: the message thread never calls into the plug-in.
    REQUIRE (rig.fake->log.empty());
    REQUIRE_THAT (rig.unit->getParamValue (kGain), WithinAbs (0.25, 1e-9));

    // Drained at the top of the block, in the order they were made, before run.
    REQUIRE_THAT (rig.process (1.0f), WithinAbs (0.25, 1e-6));
    REQUIRE (rig.fake->log.size() == 3);
    REQUIRE (rig.fake->log[0].index == kGain);
    REQUIRE_THAT (rig.fake->log[0].value, WithinAbs (0.5, 1e-9));
    REQUIRE (rig.fake->log[1].index == kSteps);
    REQUIRE (rig.fake->log[2].index == kGain);
    REQUIRE_THAT (rig.fake->log[2].value, WithinAbs (0.25, 1e-9));

    rig.fake->log.clear();
    rig.process (1.0f);
    REQUIRE (rig.fake->log.empty());
}

TEST_CASE ("a DAF unit whose write ring fills pushes the whole mirror instead",
           "[builtin][daf]")
{
    Rig rig;
    rig.activate();

    for (std::uint32_t i = 0; i < DafUnitInstance::kWriteRingSize + 50; ++i)
    {
        rig.unit->setParamValue (kGain, (float) (i % 7) * 0.25f);
        rig.unit->setParamValue (kSteps, (float) (1 + i % 4));
    }
    rig.unit->setParamValue (kGain, 1.5f);

    rig.process (1.0f);
    REQUIRE_THAT (rig.fake->held[kGain], WithinAbs (1.5, 1e-9));
    REQUIRE_THAT (rig.fake->held[kSteps],
                  WithinAbs (rig.unit->getParamValue (kSteps), 1e-9));
    REQUIRE_THAT (rig.process (1.0f), WithinAbs (1.5, 1e-6));
}

TEST_CASE ("a DAF unit's mirror holds what the plug-in would", "[builtin][daf]")
{
    Rig rig;

    rig.unit->setParamValue (kGain, 7.0f);
    REQUIRE_THAT (rig.unit->getParamValue (kGain), WithinAbs (2.0, 1e-9));

    rig.unit->setParamValue (kSteps, 2.6f);
    REQUIRE_THAT (rig.unit->getParamValue (kSteps), WithinAbs (3.0, 1e-9));

    rig.unit->setParamValue (kOn, 0.4f);
    REQUIRE_THAT (rig.unit->getParamValue (kOn), WithinAbs (0.0, 1e-9));
    rig.unit->setParamValue (kOn, 0.5f);
    REQUIRE_THAT (rig.unit->getParamValue (kOn), WithinAbs (1.0, 1e-9));

    // A restricted enumeration snaps, and a tie goes to the higher value.
    rig.unit->setParamValue (kAge, 0.2f);
    REQUIRE_THAT (rig.unit->getParamValue (kAge), WithinAbs (0.0, 1e-9));
    rig.unit->setParamValue (kAge, 0.25f);
    REQUIRE_THAT (rig.unit->getParamValue (kAge), WithinAbs (0.5, 1e-9));
    rig.unit->setParamValue (kAge, 0.9f);
    REQUIRE_THAT (rig.unit->getParamValue (kAge), WithinAbs (1.0, 1e-9));

    rig.unit->setParamValue (kGain, std::nanf (""));
    REQUIRE_THAT (rig.unit->getParamValue (kGain), WithinAbs (2.0, 1e-9));

    // An output belongs to the plug-in.
    rig.unit->setParamValue (kMeter, 3.0f);
    REQUIRE_THAT (rig.unit->getParamValue (kMeter), WithinAbs (0.0, 1e-9));
}

TEST_CASE ("a DAF unit copies its output parameters into the mirror each block",
           "[builtin][daf]")
{
    Rig rig;
    rig.activate();
    rig.unit->setParamValue (kGain, 2.0f);
    rig.process (0.75f);
    REQUIRE_THAT (rig.unit->getParamValue (kMeter), WithinAbs (1.5, 1e-6));
}

TEST_CASE ("a DAF unit pushes writes made while inactive when it activates",
           "[builtin][daf]")
{
    Rig rig;
    rig.unit->setParamValue (kGain, 0.5f);
    REQUIRE_THAT (rig.fake->held[kGain], WithinAbs (1.0, 1e-9));

    rig.activate();
    REQUIRE (rig.fake->activations == 1);
    REQUIRE_THAT (rig.fake->held[kGain], WithinAbs (0.5, 1e-9));

    // Reactivating keeps the plug-in and every value it holds.
    std::string error;
    REQUIRE (rig.unit->reactivate (96000.0, kBlock, error));
    REQUIRE (rig.fake->activations == 2);
    REQUIRE_THAT (rig.process (1.0f), WithinAbs (0.5, 1e-6));
}

TEST_CASE ("a DAF unit processes nothing while inactive or oversized", "[builtin][daf]")
{
    Rig rig;
    rig.process (1.0f);
    REQUIRE (rig.fake->runs == 0);

    rig.activate();
    rig.process (1.0f);
    REQUIRE (rig.fake->runs == 1);

    rig.unit->deactivate();
    REQUIRE_FALSE (rig.fake->activeNow);
    rig.process (1.0f);
    REQUIRE (rig.fake->runs == 1);
}

TEST_CASE ("a DAF unit hands the plug-in the transport only when one is supplied",
           "[builtin][daf]")
{
    Rig rig;
    rig.activate();

    rig.process (0.0f);
    REQUIRE (rig.fake->transportCalls == 0);

    dusk::TransportPosition position;
    position.bpm = 93.0;
    rig.process (0.0f, &position);
    REQUIRE (rig.fake->transportCalls == 1);
    REQUIRE_THAT (rig.fake->lastBpm, WithinAbs (93.0, 1e-9));
}

TEST_CASE ("a DAF unit reports the plug-in's latency only while active", "[builtin][daf]")
{
    Rig rig;
    rig.fake->latency = 128;
    REQUIRE (rig.unit->getLatencySamples() == 0);
    rig.activate();
    REQUIRE (rig.unit->getLatencySamples() == 128);
    rig.unit->deactivate();
    REQUIRE (rig.unit->getLatencySamples() == 0);
}

TEST_CASE ("a DAF unit's session blob is version 2, keyed by parameter symbol",
           "[builtin][daf]")
{
    Rig saver;
    saver.unit->setParamValue (kGain, 0.3f);
    saver.unit->setParamValue (kSteps, 4.0f);
    saver.unit->setParamValue (kLegacy, 0.7f);

    std::vector<std::uint8_t> state;
    REQUIRE (saver.unit->saveState (state));
    const std::string text (state.begin(), state.end());
    REQUIRE (text.find ("\"version\":2") != std::string::npos);
    REQUIRE (text.find ("\"gain\"") != std::string::npos);
    REQUIRE (text.find ("\"legacy\"") != std::string::npos);
    REQUIRE (text.find ("\"meter\"") == std::string::npos);

    Rig loader;
    loader.activate();
    REQUIRE (loader.unit->loadState (state));
    REQUIRE_THAT (loader.unit->getParamValue (kGain), WithinAbs (0.3, 1e-6));
    REQUIRE_THAT (loader.unit->getParamValue (kSteps), WithinAbs (4.0, 1e-9));
    REQUIRE_THAT (loader.unit->getParamValue (kLegacy), WithinAbs (0.7, 1e-6));
    REQUIRE_THAT (loader.process (1.0f), WithinAbs (0.3, 1e-6));

    SECTION ("a missing key restores that parameter's default")
    {
        REQUIRE (loader.unit->loadState (blob (
            R"({"id":"dusk.builtin.fake","version":2,"params":{"steps":2}})")));
        REQUIRE_THAT (loader.unit->getParamValue (kGain), WithinAbs (1.0, 1e-9));
        REQUIRE_THAT (loader.unit->getParamValue (kSteps), WithinAbs (2.0, 1e-9));
    }
}

TEST_CASE ("a DAF unit restores a version-1 blob under its id as its defaults",
           "[builtin][daf]")
{
    Rig rig;
    rig.unit->setParamValue (kGain, 0.3f);
    rig.unit->setParamValue (kSteps, 4.0f);

    REQUIRE (rig.unit->loadState (blob (
        R"({"id":"dusk.builtin.fake","version":1,"params":{"gain":0.6,"echo":1.0}})")));
    for (int i = 0; i < (int) kNumFakeParams; ++i)
        if (i != (int) kMeter)
            REQUIRE_THAT (rig.unit->getParamValue (i),
                          WithinAbs (rig.unit->paramInfo (i)->defaultValue, 1e-9));
}

TEST_CASE ("a DAF unit refuses a blob it cannot read", "[builtin][daf]")
{
    Rig rig;
    rig.unit->setParamValue (kGain, 0.3f);

    REQUIRE_FALSE (rig.unit->loadState ({}));
    REQUIRE_FALSE (rig.unit->loadState (blob ("not json")));
    REQUIRE_FALSE (rig.unit->loadState (blob (
        R"({"id":"dusk.builtin.other","version":2,"params":{"gain":1.5}})")));
    REQUIRE_FALSE (rig.unit->loadState (blob (
        R"({"id":"dusk.builtin.fake","version":3,"params":{"gain":1.5}})")));
    REQUIRE_THAT (rig.unit->getParamValue (kGain), WithinAbs (0.3, 1e-6));
}
