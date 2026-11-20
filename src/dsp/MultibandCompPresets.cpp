#include "MultibandCompPresets.h"
#include "MultiCompStage.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>

namespace duskstudio::mbpresets
{
namespace
{
constexpr int kLow = 0, kLowMid = 1, kHighMid = 2, kHigh = 3;
// Three bands of eight fields, highmid's three switches, the three crossovers
// and Auto Makeup.
constexpr std::size_t kWrites = 3 * kCompBandFieldCount + 3 + 3 + 1;

// A chart number as typed text. Whole numbers carry no decimal point, which
// the plug-in's reader and the C library's would each read by their own
// locale; the chart's only fractions are tenths of a ratio, read in the
// classic locale.
std::string typed (float plain)
{
    const long tenths = std::lround (plain * 10.0f);
    const long magnitude = std::labs (tenths);
    std::string text = (tenths < 0 ? "-" : "") + std::to_string (magnitude / 10);
    if (magnitude % 10 != 0)
        text += "." + std::to_string (magnitude % 10);
    return text;
}
} // namespace

bool apply (int index, const MultiCompStage& stage, CompParams& comp)
{
    if (index < 0 || index >= kNumPresets) return false;
    const auto& preset = kPresets[(std::size_t) index];

    std::array<std::pair<CompParamId, float>, kWrites> plain {};
    std::size_t count = 0;
    const auto put = [&plain, &count] (CompParamId id, float value) { plain[count++] = { id, value }; };
    const auto band = [&put] (int b, const Band& values)
    {
        put (compBandParam (b, CompBandField::Threshold), values.thresholdDb);
        put (compBandParam (b, CompBandField::Ratio),     values.ratio);
        put (compBandParam (b, CompBandField::Attack),    values.attackMs);
        put (compBandParam (b, CompBandField::Release),   values.releaseMs);
        put (compBandParam (b, CompBandField::Makeup),    values.makeupDb);
        put (compBandParam (b, CompBandField::Enabled),   1.0f);
        put (compBandParam (b, CompBandField::Bypass),    0.0f);
        put (compBandParam (b, CompBandField::Solo),      0.0f);
    };
    band (kLow, preset.low);
    band (kLowMid, preset.mid);
    band (kHigh, preset.high);
    put (compBandParam (kHighMid, CompBandField::Enabled), 0.0f);
    put (compBandParam (kHighMid, CompBandField::Bypass),  0.0f);
    put (compBandParam (kHighMid, CompBandField::Solo),    0.0f);
    put (CompParamId::MbCrossover1, preset.lowXoverHz);
    put (CompParamId::MbCrossover2, std::round (std::sqrt (preset.lowXoverHz * preset.hiXoverHz)));
    put (CompParamId::MbCrossover3, preset.hiXoverHz);
    put (CompParamId::AutoMakeup, 0.0f);
    assert (count == kWrites);

    std::array<float, kWrites> host {};
    for (std::size_t i = 0; i < count; ++i)
        if (! stage.valueFromText (plain[i].first, typed (plain[i].second), host[i]))
            return false;
    for (std::size_t i = 0; i < count; ++i)
        comp.set (plain[i].first, host[i]);
    return true;
}
} // namespace duskstudio::mbpresets
