#include "CompParams.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace duskstudio
{
namespace
{
#define DUSK_COMP_BAND(n)                                   \
    { "mb_threshold_" #n, -60.0f,   0.0f, -20.0f,        false }, \
    { "mb_ratio_" #n,       0.0f,   1.0f, 0.397359699f,  false }, \
    { "mb_attack_" #n,      0.0f,   1.0f, 0.396668643f,  false }, \
    { "mb_release_" #n,     0.0f,   1.0f, 0.383215368f,  false }, \
    { "mb_makeup_" #n,    -12.0f,  12.0f,   0.0f,        false }, \
    { "mb_bypass_" #n,      0.0f,   1.0f,   0.0f,        true  }, \
    { "mb_solo_" #n,        0.0f,   1.0f,   0.0f,        true  }, \
    { "mb_enabled_" #n,     0.0f,   1.0f,   1.0f,        true  }

// Host ranges and defaults are Multi-Comp 2's (daf-plugin/MultiCompParams.hpp
// kParams, kExtensionParams and bandParam through hostMin, hostMax and
// hostDefault). A tapered control's default is plainToHost of its plain
// default: (def - min) / (max - min) raised to the skew for the skewed ones,
// the two-segment line through its midpoint for bus_fade_rate.
constexpr std::array<CompParamSpec, kCompParamCount> kSpecs {{
    { "mix",                   0.0f, 100.0f, 100.0f,        false },
    { "sidechain_hp",          0.0f,   1.0f,   0.0f,        false },
    { "auto_makeup",           0.0f,   1.0f,   0.0f,        true  },
    { "opto_peak_reduction",   0.0f, 100.0f,   0.0f,        false },
    { "opto_gain",             0.0f, 100.0f,  23.4092255f,  false },
    { "opto_limit",            0.0f,   1.0f,   0.0f,        true  },
    { "opto_meter",            0.0f,   2.0f,   0.0f,        true  },
    { "fet_input",           -20.0f,  40.0f,   0.0f,        false },
    { "fet_output",          -20.0f,  20.0f,   0.0f,        false },
    { "fet_attack",            0.0f,   1.0f,   0.16057846f, false },
    { "fet_release",          50.0f, 1100.0f, 400.0f,       false },
    { "fet_ratio",             0.0f,   4.0f,   0.0f,        true  },
    { "fet_threshold",       -60.0f,   0.0f, -10.0f,        false },
    { "fet_meter",             0.0f,   2.0f,   0.0f,        true  },
    { "vca_threshold",       -55.0f,   0.0f, -27.0f,        false },
    { "vca_compression",       0.0f, 100.0f,  50.4944f,     false },
    { "vca_output",          -20.0f,  20.0f,   0.0f,        false },
    { "bus_threshold",       -30.0f,  15.0f,   0.0f,        false },
    { "bus_ratio",             0.0f,   2.0f,   0.0f,        true  },
    { "bus_attack",            0.0f,   5.0f,   2.0f,        true  },
    { "bus_release",           0.0f,   4.0f,   1.0f,        true  },
    { "bus_makeup",            0.0f,  20.0f,   0.0f,        false },
    { "bus_mix",               0.0f, 100.0f, 100.0f,        false },
    { "bus_headroom",          0.0f,   6.0f,   3.0f,        true  },
    { "bus_fade_rate",         0.0f,   1.0f,   0.674074054f, false },
    { "bus_fade",              0.0f,   1.0f,   0.0f,        true  },
    { "mb_crossover_1",        0.0f,   1.0f,   0.675480008f, false },
    { "mb_crossover_2",        0.0f,   1.0f,   0.675480008f, false },
    { "mb_crossover_3",        0.0f,   1.0f,   0.71254003f, false },
    { "mb_mix",                0.0f, 100.0f, 100.0f,        false },
    { "mb_output",           -24.0f,  24.0f,   0.0f,        false },
    DUSK_COMP_BAND (0),
    DUSK_COMP_BAND (1),
    DUSK_COMP_BAND (2),
    DUSK_COMP_BAND (3),
}};
#undef DUSK_COMP_BAND

using Id = CompParamId;

constexpr Id kStripParams[] {
    Id::Mix, Id::SidechainHp, Id::AutoMakeup,
    Id::OptoPeakReduction, Id::OptoGain, Id::OptoLimit, Id::OptoMeter,
    Id::FetInput, Id::FetOutput, Id::FetAttack, Id::FetRelease, Id::FetRatio, Id::FetThreshold, Id::FetMeter,
    Id::VcaThreshold, Id::VcaCompression, Id::VcaOutput,
};

constexpr Id kBusParams[] {
    Id::SidechainHp, Id::AutoMakeup,
    Id::BusThreshold, Id::BusRatio, Id::BusAttack, Id::BusRelease, Id::BusMakeup, Id::BusMix,
    Id::BusHeadroom, Id::BusFadeRate, Id::BusFade,
};

constexpr std::array<Id, 6 + kCompBandCount * kCompBandFieldCount> multibandParams() noexcept
{
    std::array<Id, 6 + kCompBandCount * kCompBandFieldCount> ids {};
    ids[0] = Id::AutoMakeup;
    ids[1] = Id::MbCrossover1;
    ids[2] = Id::MbCrossover2;
    ids[3] = Id::MbCrossover3;
    ids[4] = Id::MbMix;
    ids[5] = Id::MbOutput;
    for (int i = 0; i < kCompBandCount * kCompBandFieldCount; ++i)
        ids[(std::size_t) (6 + i)] = (Id) ((int) Id::MbBandFirst + i);
    return ids;
}

constexpr auto kMultibandParams = multibandParams();
} // namespace

const CompParamSpec& compParamSpec (CompParamId id) noexcept
{
    return kSpecs[(std::size_t) std::clamp ((int) id, 0, (int) kCompParamCount - 1)];
}

float clampCompParam (CompParamId id, float value) noexcept
{
    const auto& spec = compParamSpec (id);
    if (! std::isfinite (value)) return spec.hostDefault;
    const float clamped = std::clamp (value, spec.hostMin, spec.hostMax);
    return spec.integer ? std::round (clamped) : clamped;
}

CompParamList compStageParams (CompStage stage) noexcept
{
    switch (stage)
    {
        case CompStage::Strip:     return { kStripParams, std::size (kStripParams) };
        case CompStage::Bus:       return { kBusParams, std::size (kBusParams) };
        case CompStage::Multiband: break;
    }
    return { kMultibandParams.data(), kMultibandParams.size() };
}

CompParams::CompParams() noexcept
{
    for (std::size_t i = 0; i < kCompParamCount; ++i)
        atoms[i].store (kSpecs[i].hostDefault, std::memory_order_relaxed);
}

void CompParams::resetToDefaults (CompStage stage) noexcept
{
    for (const auto id : compStageParams (stage))
        atom (id).store (compParamSpec (id).hostDefault, std::memory_order_relaxed);
}

void CompParams::copyFrom (const CompParams& other) noexcept
{
    for (std::size_t i = 0; i < kCompParamCount; ++i)
        atoms[i].store (other.atoms[i].load (std::memory_order_relaxed), std::memory_order_relaxed);
}

CompParams::Values CompParams::values() const noexcept
{
    Values out {};
    for (std::size_t i = 0; i < kCompParamCount; ++i)
        out[i] = atoms[i].load (std::memory_order_relaxed);
    return out;
}

void CompParams::setValues (const Values& values) noexcept
{
    for (std::size_t i = 0; i < kCompParamCount; ++i)
        atoms[i].store (values[i], std::memory_order_relaxed);
}
} // namespace duskstudio
