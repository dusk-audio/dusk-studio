#pragma once

#include <cstdint>

namespace duskspike
{
namespace theme
{

// ARGB literals lifted from DuskStudioLookAndFeel.h and ChannelStripComponent::paint(),
// converted to ImGui's ABGR at use through col().
constexpr std::uint32_t kStripFill      = 0xff1a1a1c;
constexpr std::uint32_t kStripBorder    = 0xff2a2a2e;
constexpr std::uint32_t kConsoleBack    = 0xff121214;
constexpr std::uint32_t kFocusRing      = 0xffd0a050;

constexpr std::uint32_t kEqFill         = 0xff1f231e;
constexpr std::uint32_t kEqAccent       = 0xff80c090;
constexpr std::uint32_t kCompFill       = 0xff241f1c;
constexpr std::uint32_t kCompGold       = 0xffd09060;
constexpr std::uint32_t kSendFill       = 0xff1f1d24;
constexpr std::uint32_t kSendPurple     = 0xff9080c0;

constexpr std::uint32_t kHfRed          = 0xffc44444;
constexpr std::uint32_t kHmGreen        = 0xff5fa55f;
constexpr std::uint32_t kLmBlue         = 0xff5878b0;
constexpr std::uint32_t kLfGraphite     = 0xff5a5a62;
constexpr std::uint32_t kFilterWhite    = 0xffe0e0e4;
constexpr std::uint32_t kLfGreen        = 0xff5c9a5c;
constexpr std::uint32_t kPanRed         = 0xffc04040;
constexpr std::uint32_t kPanCyan        = 0xff70b8c0;

constexpr std::uint32_t kAuxFill[4]     = { 0xff5a8ad0, 0xff9080c0, 0xffe0c050, 0xff60c060 };
constexpr std::uint32_t kAuxPreOutline  = 0xffffc060;
constexpr std::uint32_t kKnobOutline    = 0xff404048;

constexpr std::uint32_t kButtonOff      = 0xff202024;
constexpr std::uint32_t kButtonPanel    = 0xff222226;
constexpr std::uint32_t kTextDim        = 0xff8e9298;
constexpr std::uint32_t kTextValue      = 0xffd8d8d8;
constexpr std::uint32_t kTextBright     = 0xffffffff;
constexpr std::uint32_t kArmOn          = 0xffd03030;
constexpr std::uint32_t kArmOffText     = 0xffd06060;
constexpr std::uint32_t kMuteOn         = 0xffff4500;
constexpr std::uint32_t kSoloOn         = 0xffcccc00;
constexpr std::uint32_t kPhaseOn        = 0xff70c0d0;
constexpr std::uint32_t kPrintOn        = 0xffd09060;
constexpr std::uint32_t kInsertText     = 0xff9080c0;
constexpr std::uint32_t kIoText         = 0xffa0a8b8;
constexpr std::uint32_t kOnText         = 0xff121214;

constexpr std::uint32_t kShellFill      = 0xff202024;
constexpr std::uint32_t kShellBorder    = 0xff55555c;
constexpr std::uint32_t kShellDivider   = 0xff4a4a50;
constexpr std::uint32_t kLedRingDark    = 0xff09090b;
constexpr std::uint32_t kLedOffFill     = 0xff29292e;
constexpr std::uint32_t kLedOffRing     = 0xff66666e;
constexpr std::uint32_t kLabelBypassed  = 0xff77777f;

constexpr std::uint32_t kLedGreen       = 0xff20d040;
constexpr std::uint32_t kLedYellow      = 0xfff0e020;
constexpr std::uint32_t kLedRed         = 0xffff2020;
constexpr std::uint32_t kMeterBack      = 0xff060608;
constexpr std::uint32_t kMeterBorder    = 0xff2a2a30;
constexpr std::uint32_t kMeterSegment   = 0xff020203;
constexpr std::uint32_t kPeakTick       = 0xfff0f0f0;
constexpr std::uint32_t kPeakTickHot    = 0xffff8080;

constexpr std::uint32_t kTrackFill      = 0xff0a0a0c;
constexpr std::uint32_t kCapTop         = 0xffe2dccb;
constexpr std::uint32_t kCapMid         = 0xff9d958a;
constexpr std::uint32_t kCapBottom      = 0xffb8b0a0;
constexpr std::uint32_t kCapGroove      = 0xff202018;
constexpr std::uint32_t kCapRim         = 0xff0a0a0a;
constexpr std::uint32_t kKnobDot        = 0xffc8c8d2;
constexpr std::uint32_t kKnobRim        = 0xff0b0b0d;

constexpr std::uint32_t kGrBack         = 0xff141418;
constexpr std::uint32_t kTrackColour    = 0xffc06848;

} // namespace theme

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

struct FaderTick { float db; const char* label; };

inline constexpr FaderTick kFaderTicks[] = {
    {   6.0f, "+6" }, {  3.0f, "+3" }, { 0.0f, "0" }, { -3.0f, "3" }, { -6.0f, "6" },
    { -12.0f, "12" }, { -24.0f, "24" }, { -40.0f, "40" }, { -90.0f, "\xe2\x88\x9e" }
};

} // namespace duskspike
