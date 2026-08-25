#include "DuskTheme.h"

#include <cstdint>

namespace duskstudio::imgui
{
namespace
{
constexpr unsigned int argb (std::uint32_t value)
{
    return IM_COL32 ((value >> 16) & 0xff, (value >> 8) & 0xff, value & 0xff,
                     (value >> 24) & 0xff);
}

ConsolePalette buildPalette()
{
    ConsolePalette p {};

    // Panels and shell: ChannelStripComponent::paint() and the look-and-feel's
    // Slider::backgroundColourId / outline pair.
    p.stripFill   = argb (0xff1a1a1c);
    p.stripBorder = argb (0xff2a2a2e);
    p.consoleBack = argb (0xff121214);
    p.focusRing   = argb (0xffd0a050);
    p.trackColour = argb (0xffc06848);

    // Module panels.
    p.eqFill      = argb (0xff1f231e);
    p.eqAccent    = argb (0xff80c090);
    p.compFill    = argb (0xff241f1c);
    p.compGold    = argb (0xffd09060);  // fourKColors::kCompGold
    p.sendFill    = argb (0xff1f1d24);
    p.sendPurple  = argb (0xff9080c0);  // fourKColors::kSendPurple

    // EQ band bodies: sslEqColors.
    p.hfRed       = argb (0xffc44444);
    p.hmGreen     = argb (0xff5fa55f);
    p.lmBlue      = argb (0xff5878b0);
    p.lfGraphite  = argb (0xff5a5a62);
    p.filterWhite = argb (0xffe0e0e4);
    p.lfGreen     = argb (0xff5c9a5c);  // fourKColors::kLfGreen
    p.panRed      = argb (0xffc04040);
    p.panCyan     = argb (0xff70b8c0);  // fourKColors::kPanCyan

    p.auxFill[0]    = argb (0xff5a8ad0);
    p.auxFill[1]    = argb (0xff9080c0);
    p.auxFill[2]    = argb (0xffe0c050);
    p.auxFill[3]    = argb (0xff60c060);
    p.auxPreOutline = argb (0xffffc060);

    p.busColour[0] = argb (0xff5a8ad0);
    p.busColour[1] = argb (0xffe0c050);
    p.busColour[2] = argb (0xff60c060);
    p.busColour[3] = argb (0xffc06888);

    p.armOn        = argb (0xffd03030);
    p.armOffText   = argb (0xffd06060);
    p.muteOn       = argb (0xffff4500);
    p.soloOn       = argb (0xffcccc00);
    p.phaseOn      = argb (0xff70c0d0);
    p.printOn      = argb (0xffd09060);
    p.insertText   = argb (0xff9080c0);
    p.ioText       = argb (0xffa0a8b8);
    p.autoReadOn   = argb (0xff20603a);
    p.autoReadText = argb (0xffd0e8d0);
    p.eqTypeChip   = argb (0xff5a3a20);
    p.compLabel    = argb (0xffb07050);

    auto& w = p.widgets;
    w.panelFill   = p.stripFill;
    w.panelBorder = p.stripBorder;
    w.background  = p.consoleBack;

    w.knobFill          = p.filterWhite;
    w.knobOutline       = argb (0xff404048);
    w.knobTick          = argb (0xffc8c8d2);
    w.knobPointer       = argb (0xffffffff);
    w.knobPointerShadow = argb (0xff0c0c0e);

    w.textDim      = argb (0xff8e9298);
    w.textValue    = argb (0xffd8d8d8);
    w.textBright   = argb (0xffffffff);
    w.textOn       = argb (0xff121214);
    w.textBypassed = argb (0xff77777f);

    w.buttonOff   = argb (0xff202024);
    w.buttonPanel = argb (0xff222226);

    w.pillFill    = argb (0xff202024);
    w.pillBorder  = argb (0xff55555c);
    w.pillDivider = argb (0xff4a4a50);
    w.ledRing     = argb (0xff09090b);
    w.ledOff      = argb (0xff29292e);

    w.meterBack    = argb (0xff060608);
    w.meterBorder  = argb (0xff2a2a30);
    w.meterSegment = argb (0xff020203);
    w.meterLow     = argb (0xff20d040);
    w.meterMid     = argb (0xfff0e020);
    w.meterHigh    = argb (0xffff2020);
    w.peakTick     = argb (0xfff0f0f0);
    w.peakTickHot  = argb (0xffff8080);

    w.trackFill   = argb (0xff0a0a0c);
    w.trackBorder = p.stripBorder;
    w.capTop      = argb (0xffe2dccb);
    w.capUpper    = argb (0xffcfc8b8);
    w.capMid      = argb (0xff9d958a);
    w.capBottom   = argb (0xffb8b0a0);
    w.capGroove   = argb (0xff202018);
    w.capRim      = argb (0xff0a0a0a);
    w.tickLabel   = argb (0xffb8b8c0);

    w.grBack   = argb (0xff141418);
    w.grLow    = DuskWidgets::brighter (p.compGold, 0.25f);
    w.grHigh   = DuskWidgets::brighter (p.hfRed, 0.10f);
    w.grHandle = p.compGold;

    w.fieldFill = w.buttonOff;
    w.fieldText = w.textBright;

    return p;
}
} // namespace

const ConsolePalette& consolePalette()
{
    static const ConsolePalette palette = buildPalette();
    return palette;
}
} // namespace duskstudio::imgui
