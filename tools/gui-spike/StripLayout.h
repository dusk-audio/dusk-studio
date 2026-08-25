#pragma once

namespace duskspike
{
// Layout constants, all in design pixels at scale 1.0, mirroring
// ChannelStripComponent::resized() and ConsoleLayout.h.
namespace layout
{
constexpr float kStripWidth      = 188.0f;
constexpr float kOuterInset      = 4.0f;
constexpr float kTopPad          = 6.0f;
constexpr float kNameH           = 20.0f;
constexpr float kIoButtonH       = 18.0f;
constexpr float kRowH            = 18.0f;

constexpr float kKnobSize        = 26.0f;
constexpr float kValueLabelH     = 12.0f;
constexpr float kKnobBlockH      = kKnobSize + kValueLabelH + 2.0f;
constexpr float kEqHeaderH       = 16.0f;
constexpr float kFilterLabelH    = 10.0f;
constexpr float kEqHpfRowH       = kFilterLabelH + kKnobBlockH;
constexpr float kFilterBandGap   = 5.0f;
constexpr float kColumnHeaderH   = 10.0f;
constexpr float kEqBandRowH      = kKnobBlockH;
constexpr float kEqBandGap       = 2.0f;
constexpr float kRowLabelW       = 28.0f;
constexpr int   kEqBandCount     = 4;
constexpr float kEqPanelH        = kEqHeaderH + kEqHpfRowH + kFilterBandGap + kColumnHeaderH
                                   + kEqBandCount * kEqBandRowH + (kEqBandCount - 1) * kEqBandGap;

constexpr float kCompKnobSize    = 24.0f;
constexpr float kCompKnobBlockH  = kCompKnobSize + kValueLabelH + 2.0f;
constexpr float kCompKnobLabelH  = 10.0f;
constexpr float kCompKnobRowH    = kCompKnobLabelH + kCompKnobBlockH;
constexpr float kCompPanelH      = 16.0f + 2.0f + kCompKnobRowH + 4.0f;

constexpr float kAuxKnobSize     = 24.0f;
constexpr float kAuxStaggerY     = 10.0f;
constexpr float kAuxValueH       = 10.0f;
constexpr float kAuxBlockH       = kAuxStaggerY + kAuxKnobSize + 2.0f + kAuxValueH;
constexpr float kAuxPanelH       = 3.0f + 11.0f + 1.0f + kAuxBlockH;

constexpr float kPanKnobSize     = 26.0f;
constexpr float kPanBlockH       = kPanKnobSize + kValueLabelH + 2.0f;
constexpr float kPanLabelH       = 11.0f;
constexpr float kPanBlockW       = 56.0f;
constexpr float kPanFaderGap     = 4.0f;

constexpr float kPeakLabelH      = 18.0f;
constexpr float kAutoH           = 16.0f;
constexpr float kMsRowH          = 20.0f;

constexpr float kMeterWidth      = 12.0f;
constexpr float kBusColumnW      = 18.0f;
constexpr float kBusButtonH      = 20.0f;
constexpr float kGrLedW          = 20.0f;
constexpr float kFaderTrackPad   = 18.0f;
constexpr float kFaderCapH       = 36.0f;
constexpr float kFaderCapW       = 20.0f;

// Fixed height above and below the fader column, from the resized() walk.
constexpr float kAboveFader      = 481.0f;
constexpr float kBelowFader      = 66.0f;
constexpr float kMinStripHeight  = 820.0f;
}

} // namespace duskspike
