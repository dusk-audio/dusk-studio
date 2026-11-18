#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace duskstudio
{
// Multi-Comp 2's parameters as Dusk Studio stores them, in the plug-in's host
// domain and under its own symbols. A tapered control (sidechain_hp,
// fet_attack, bus_fade_rate, the crossovers and the band ratio, attack and
// release) is its 0..1 knob position there, not its plain value.
constexpr int kCompBandCount = 4;
constexpr int kCompBandFieldCount = 8;

enum class CompBandField : int { Threshold, Ratio, Attack, Release, Makeup, Bypass, Solo, Enabled };

enum class CompParamId : int
{
    Mix,
    SidechainHp,
    AutoMakeup,
    OptoPeakReduction,
    OptoGain,
    OptoLimit,
    OptoMeter,
    FetInput,
    FetOutput,
    FetAttack,
    FetRelease,
    FetRatio,
    FetThreshold,
    FetMeter,
    VcaThreshold,
    VcaCompression,
    VcaOutput,
    BusThreshold,
    BusRatio,
    BusAttack,
    BusRelease,
    BusMakeup,
    BusMix,
    BusHeadroom,
    BusFadeRate,
    BusFade,
    MbCrossover1,
    MbCrossover2,
    MbCrossover3,
    MbMix,
    MbOutput,
    // Band b's field f is MbBandFirst + b * kCompBandFieldCount + f.
    MbBandFirst,
    kCount = MbBandFirst + kCompBandCount * kCompBandFieldCount
};

constexpr std::size_t kCompParamCount = (std::size_t) CompParamId::kCount;

constexpr CompParamId compBandParam (int band, CompBandField field) noexcept
{
    return (CompParamId) ((int) CompParamId::MbBandFirst
                          + band * kCompBandFieldCount + (int) field);
}

struct CompParamSpec
{
    const char* symbol;
    float hostMin;
    float hostMax;
    float hostDefault;
    bool  integer;
};

const CompParamSpec& compParamSpec (CompParamId id) noexcept;

// Clamped to the spec range, rounded when integer; a non-finite value is the
// default.
float clampCompParam (CompParamId id, float value) noexcept;

// Which parameters a kind of stage stores. Mode and bypass are not among them:
// the strip's mode is its own field, bypass is compEnabled inverted, and the
// other stages run one fixed mode.
enum class CompStage { Strip, Bus, Multiband };

struct CompParamList
{
    const CompParamId* first;
    std::size_t size;
    const CompParamId* begin() const noexcept { return first; }
    const CompParamId* end() const noexcept { return first + size; }
};

CompParamList compStageParams (CompStage stage) noexcept;

class CompParams
{
public:
    using Values = std::array<float, kCompParamCount>;

    CompParams() noexcept;
    CompParams (const CompParams&) = delete;
    CompParams& operator= (const CompParams&) = delete;

    float get (CompParamId id) const noexcept
    {
        return atoms[(std::size_t) id].load (std::memory_order_relaxed);
    }

    void set (CompParamId id, float value) noexcept
    {
        atoms[(std::size_t) id].store (clampCompParam (id, value), std::memory_order_relaxed);
    }

    std::atomic<float>& atom (CompParamId id) noexcept { return atoms[(std::size_t) id]; }
    const std::atomic<float>& atom (CompParamId id) const noexcept { return atoms[(std::size_t) id]; }

    void resetToDefaults (CompStage stage) noexcept;
    void copyFrom (const CompParams& other) noexcept;

    Values values() const noexcept;
    void setValues (const Values& values) noexcept;

private:
    std::array<std::atomic<float>, kCompParamCount> atoms {};
};
} // namespace duskstudio
