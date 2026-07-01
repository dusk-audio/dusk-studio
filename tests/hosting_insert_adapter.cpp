#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/hosting/InsertAdapter.h"

#include <string>
#include <vector>

// InsertAdapter reconciles a plugin's arbitrary PortLayout to the mixer's fixed
// stereo-in/stereo-out insert. These tests pin every fold rule (mono sum, mono-out
// broadcast, multi-out drop, sidechain fill/silence, instrument source) using a
// stub instance whose processBlock is a known transform, so a fold regression is a
// hard assertion failure.

using namespace duskstudio::hosting;
using Catch::Matchers::WithinAbs;

namespace
{
BusInfo audioBus (BusInfo::Direction dir, BusInfo::Role role, int chans)
{
    BusInfo b;
    b.kind = BusInfo::Kind::Audio; b.dir = dir; b.role = role;
    b.channelCount = chans; b.active = true;
    return b;
}

PortLayout effectLayout (int inCh, int outCh, int scCh = 0)
{
    PortLayout l;
    l.inputs.push_back (audioBus (BusInfo::Direction::Input, BusInfo::Role::Main, inCh));
    l.mainInIndex = 0;
    if (scCh > 0)
    {
        l.inputs.push_back (audioBus (BusInfo::Direction::Input, BusInfo::Role::Sidechain, scCh));
        l.sidechainInIndex = 1;
    }
    l.outputs.push_back (audioBus (BusInfo::Direction::Output, BusInfo::Role::Main, outCh));
    l.mainOutIndex = 0;
    return l;
}

// Writes gain*input into each output channel that has a matching input; output
// channels beyond the input count (and instrument outputs) get `fill`. Records
// the first sidechain sample it was handed so silence-vs-tap is observable.
class FoldStub final : public INativeInstance
{
public:
    explicit FoldStub (PortLayout l, float gain = 1.0f, float fill = 0.0f)
        : layout_ (std::move (l)), gain_ (gain), fill_ (fill) {}

    const PortLayout& portLayout() const noexcept override { return layout_; }
    bool activate (double, int, std::string&) override { active_ = true; return true; }
    void deactivate() override { active_ = false; }
    bool reactivate (double, int, std::string&) override { return true; }
    bool isActive() const noexcept override { return active_; }

    void processBlock (const PortBuffers& io) noexcept override
    {
        sawSidechain = (io.sidechainIn != nullptr && io.sidechainInChannels > 0)
                         ? io.sidechainIn[0][0] : -999.0f;

        for (int c = 0; c < io.mainOutChannels; ++c)
            for (int i = 0; i < io.numFrames; ++i)
                io.mainOut[c][i] = (c < io.mainInChannels) ? io.mainIn[c][i] * gain_ : fill_;
    }

    bool saveState (std::vector<uint8_t>&) const override { return true; }
    bool loadState (const std::vector<uint8_t>&) override { return true; }
    int  getLatencySamples() const noexcept override { return 0; }

    float sawSidechain = -999.0f;

private:
    PortLayout layout_;
    float gain_, fill_;
    bool  active_ = false;
};

constexpr int kN = 8;
std::vector<float> filled (float v) { return std::vector<float> ((size_t) kN, v); }
} // namespace

TEST_CASE ("InsertAdapter: stereo→stereo passes L/R straight through", "[hosting][adapter]")
{
    FoldStub stub (effectLayout (2, 2), /*gain*/ 0.5f);
    std::string e; stub.activate (48000.0, kN, e);
    InsertAdapter adapter; adapter.prepare (stub.portLayout(), kN);

    auto L = filled (1.0f), R = filled (-1.0f);
    adapter.process (stub, L.data(), R.data(), kN);

    REQUIRE_THAT (L[0], WithinAbs ( 0.5, 1e-6));
    REQUIRE_THAT (R[0], WithinAbs (-0.5, 1e-6));
}

TEST_CASE ("InsertAdapter: mono plugin sums the stereo feed and broadcasts back", "[hosting][adapter]")
{
    FoldStub stub (effectLayout (1, 1));   // 1-in / 1-out, unity gain
    std::string e; stub.activate (48000.0, kN, e);
    InsertAdapter adapter; adapter.prepare (stub.portLayout(), kN);

    auto L = filled (1.0f), R = filled (3.0f);   // sum/2 = 2.0
    adapter.process (stub, L.data(), R.data(), kN);

    REQUIRE_THAT (L[0], WithinAbs (2.0, 1e-6));   // broadcast to both sides
    REQUIRE_THAT (R[0], WithinAbs (2.0, 1e-6));
}

TEST_CASE ("InsertAdapter: multi-out plugin keeps only channels 0/1", "[hosting][adapter]")
{
    // 2-in / 4-out: channels 2,3 are filled with a poison value that must NOT
    // reach the mixer.
    FoldStub stub (effectLayout (2, 4), /*gain*/ 1.0f, /*fill*/ 999.0f);
    std::string e; stub.activate (48000.0, kN, e);
    InsertAdapter adapter; adapter.prepare (stub.portLayout(), kN);

    auto L = filled (1.0f), R = filled (2.0f);
    adapter.process (stub, L.data(), R.data(), kN);

    REQUIRE_THAT (L[0], WithinAbs (1.0, 1e-6));
    REQUIRE_THAT (R[0], WithinAbs (2.0, 1e-6));   // poison ch2/3 dropped
}

TEST_CASE ("InsertAdapter: sidechain is fed from the tap, or silence when unrouted", "[hosting][adapter]")
{
    FoldStub stub (effectLayout (2, 2, /*sidechain*/ 2));
    std::string e; stub.activate (48000.0, kN, e);
    InsertAdapter adapter; adapter.prepare (stub.portLayout(), kN);
    REQUIRE (adapter.sidechainChannels() == 2);

    auto L = filled (0.0f), R = filled (0.0f);

    SECTION ("routed")
    {
        auto scL = filled (7.0f), scR = filled (7.0f);
        adapter.process (stub, L.data(), R.data(), kN, scL.data(), scR.data());
        REQUIRE_THAT (stub.sawSidechain, WithinAbs (7.0, 1e-6));
    }
    SECTION ("unrouted → silence, never a null pointer")
    {
        adapter.process (stub, L.data(), R.data(), kN, nullptr, nullptr);
        REQUIRE_THAT (stub.sawSidechain, WithinAbs (0.0, 1e-6));
    }
}

TEST_CASE ("InsertAdapter: instrument output replaces the dry signal", "[hosting][adapter]")
{
    PortLayout l;   // no audio input; MIDI in + stereo out
    l.outputs.push_back (audioBus (BusInfo::Direction::Output, BusInfo::Role::Main, 2));
    l.mainOutIndex = 0;
    l.isInstrument = true;

    FoldStub stub (l, /*gain*/ 1.0f, /*fill*/ 0.25f);   // 0 in → every out = fill
    std::string e; stub.activate (48000.0, kN, e);
    InsertAdapter adapter; adapter.prepare (stub.portLayout(), kN);
    REQUIRE (adapter.inputChannels() == 0);

    auto L = filled (9.0f), R = filled (9.0f);   // dry content must be overwritten
    adapter.process (stub, L.data(), R.data(), kN);

    REQUIRE_THAT (L[0], WithinAbs (0.25, 1e-6));
    REQUIRE_THAT (R[0], WithinAbs (0.25, 1e-6));
}

TEST_CASE ("InsertAdapter: oversized block and inactive instance are dry passthrough", "[hosting][adapter]")
{
    FoldStub stub (effectLayout (2, 2), 0.5f);
    InsertAdapter adapter; adapter.prepare (stub.portLayout(), kN);

    SECTION ("inactive → untouched")
    {
        auto L = filled (1.0f), R = filled (1.0f);
        adapter.process (stub, L.data(), R.data(), kN);   // never activated
        REQUIRE_THAT (L[0], WithinAbs (1.0, 1e-6));
        REQUIRE_THAT (R[0], WithinAbs (1.0, 1e-6));
    }
    SECTION ("numFrames > prepared max → untouched")
    {
        std::string e; stub.activate (48000.0, kN, e);
        auto L = filled (1.0f), R = filled (1.0f);
        adapter.process (stub, L.data(), R.data(), kN + 1);
        REQUIRE_THAT (L[0], WithinAbs (1.0, 1e-6));
        REQUIRE_THAT (R[0], WithinAbs (1.0, 1e-6));
    }
}
