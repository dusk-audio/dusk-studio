#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/hosting/INativeInstance.h"
#include "engine/hosting/PortBuffers.h"
#include "engine/hosting/PortLayout.h"

#include <algorithm>
#include <string>
#include <vector>

// The shared, host-agnostic native-host foundation (PortLayout / PortBuffers /
// INativeInstance). This pins the descriptor semantics the InsertAdapter and every
// host rely on, and proves INativeInstance is a complete, implementable interface.

using namespace duskstudio::hosting;

namespace
{
BusInfo audioBus (BusInfo::Direction dir, BusInfo::Role role, int chans, const char* name)
{
    BusInfo b;
    b.kind = BusInfo::Kind::Audio;
    b.dir = dir;
    b.role = role;
    b.channelCount = chans;
    b.active = true;
    b.name = name;
    return b;
}

BusInfo eventBus (BusInfo::Direction dir, const char* name)
{
    BusInfo b;
    b.kind = BusInfo::Kind::Event;
    b.dir = dir;
    b.role = BusInfo::Role::Main;
    b.channelCount = 0;
    b.active = true;
    b.carriesMidi = true;
    b.name = name;
    return b;
}
} // namespace

TEST_CASE ("PortLayout describes a plain stereo insert effect", "[hosting][ports]")
{
    PortLayout l;
    l.inputs.push_back  (audioBus (BusInfo::Direction::Input,  BusInfo::Role::Main, 2, "In"));
    l.outputs.push_back (audioBus (BusInfo::Direction::Output, BusInfo::Role::Main, 2, "Out"));
    l.mainInIndex = 0;
    l.mainOutIndex = 0;

    REQUIRE (l.mainInChannels()  == 2);
    REQUIRE (l.mainOutChannels() == 2);
    REQUIRE_FALSE (l.hasSidechain());
    REQUIRE_FALSE (l.acceptsMidi());
    REQUIRE_FALSE (l.emitsMidi());
    REQUIRE_FALSE (l.isInstrument);
}

TEST_CASE ("PortLayout describes a mono effect", "[hosting][ports]")
{
    PortLayout l;
    l.inputs.push_back  (audioBus (BusInfo::Direction::Input,  BusInfo::Role::Main, 1, "In"));
    l.outputs.push_back (audioBus (BusInfo::Direction::Output, BusInfo::Role::Main, 1, "Out"));
    l.mainInIndex = 0;
    l.mainOutIndex = 0;

    // The InsertAdapter will fold the stereo insert feed to mono and broadcast
    // the mono result back to L/R; the layout just reports the true shape.
    REQUIRE (l.mainInChannels()  == 1);
    REQUIRE (l.mainOutChannels() == 1);
    REQUIRE_FALSE (l.hasSidechain());
}

TEST_CASE ("PortLayout describes a sidechain compressor", "[hosting][ports]")
{
    PortLayout l;
    l.inputs.push_back  (audioBus (BusInfo::Direction::Input,  BusInfo::Role::Main,      2, "In"));
    l.inputs.push_back  (audioBus (BusInfo::Direction::Input,  BusInfo::Role::Sidechain, 2, "Sidechain"));
    l.outputs.push_back (audioBus (BusInfo::Direction::Output, BusInfo::Role::Main,      2, "Out"));
    l.mainInIndex = 0;
    l.sidechainInIndex = 1;
    l.mainOutIndex = 0;

    REQUIRE (l.hasSidechain());
    REQUIRE (l.sidechainInIndex == 1);
    REQUIRE (l.inputs[(size_t) l.sidechainInIndex].role == BusInfo::Role::Sidechain);
    REQUIRE (l.mainInChannels() == 2);
}

TEST_CASE ("PortLayout describes a MIDI instrument", "[hosting][ports]")
{
    PortLayout l;
    l.inputs.push_back  (eventBus  (BusInfo::Direction::Input, "MIDI In"));
    l.outputs.push_back (audioBus  (BusInfo::Direction::Output, BusInfo::Role::Main, 2, "Out"));
    l.eventInIndex = 0;
    l.mainOutIndex = 0;
    l.isInstrument = true;

    REQUIRE (l.acceptsMidi());
    REQUIRE (l.isInstrument);
    REQUIRE (l.mainInChannels() == 0);   // no audio input — the plugin is the source
    REQUIRE (l.mainOutChannels() == 2);
}

// A minimal INativeInstance to prove the interface is complete + implementable
// and drives a PortBuffers without a real plugin. Stands in for the concrete
// Vst3/Lv2/Clap instances that arrive in later phases.
namespace
{
class GainStub final : public INativeInstance
{
public:
    const PortLayout& portLayout() const noexcept override { return layout; }

    bool activate (double, int maxBlockFrames, std::string&) override
    {
        maxFrames = maxBlockFrames;
        active = true;
        return true;
    }
    void deactivate() override { active = false; }
    bool reactivate (double sr, int mb, std::string& e) override { deactivate(); return activate (sr, mb, e); }
    bool isActive() const noexcept override { return active; }

    void processBlock (const PortBuffers& io) noexcept override
    {
        if (! active || io.numFrames <= 0 || io.numFrames > maxFrames) return;
        const int ch = std::min (io.mainInChannels, io.mainOutChannels);
        for (int c = 0; c < ch; ++c)
            for (int i = 0; i < io.numFrames; ++i)
                io.mainOut[c][i] = io.mainIn[c][i] * 0.5f;
    }

    bool saveState (std::vector<uint8_t>& out) const override { out = { 1, 2, 3 }; return true; }
    bool loadState (const std::vector<uint8_t>& in) override  { return in.size() == 3; }
    int  getLatencySamples() const noexcept override { return 0; }

    PortLayout layout;
    bool active = false;
    int  maxFrames = 0;
};
} // namespace

TEST_CASE ("INativeInstance is implementable and processes a PortBuffers", "[hosting][instance]")
{
    GainStub stub;
    stub.layout.inputs.push_back  (audioBus (BusInfo::Direction::Input,  BusInfo::Role::Main, 2, "In"));
    stub.layout.outputs.push_back (audioBus (BusInfo::Direction::Output, BusInfo::Role::Main, 2, "Out"));
    stub.layout.mainInIndex = 0;
    stub.layout.mainOutIndex = 0;

    INativeInstance& inst = stub;   // drive through the base interface

    std::string err;
    REQUIRE (inst.activate (48000.0, 64, err));
    REQUIRE (inst.isActive());

    constexpr int n = 16;
    std::vector<float> inL (n, 1.0f), inR (n, -1.0f), outL (n, 0.0f), outR (n, 0.0f);
    float* inArr[2]  = { inL.data(), inR.data() };
    float* outArr[2] = { outL.data(), outR.data() };

    PortBuffers io;
    io.mainIn = inArr;   io.mainInChannels = 2;
    io.mainOut = outArr; io.mainOutChannels = 2;
    io.numFrames = n;

    inst.processBlock (io);

    using Catch::Matchers::WithinAbs;
    REQUIRE_THAT (outL[0], WithinAbs ( 0.5, 1e-6));
    REQUIRE_THAT (outR[0], WithinAbs (-0.5, 1e-6));

    std::vector<uint8_t> blob;
    REQUIRE (inst.saveState (blob));
    REQUIRE (inst.loadState (blob));
    REQUIRE (inst.getLatencySamples() == 0);

    // Out-of-range block is a no-op (guards the audio-thread contract).
    outL[0] = 99.0f;
    io.numFrames = 999;
    inst.processBlock (io);
    REQUIRE_THAT (outL[0], WithinAbs (99.0, 1e-6));
}
