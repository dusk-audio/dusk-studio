#pragma once

#include <DuskWidgets.hpp>

namespace duskstudio::imgui
{
// The console palette, as the widget kit's theme tokens plus the accents the kit has
// no opinion about (band colours, aux fills, transport states).
//
// The values are the ones DuskStudioLookAndFeel paints with. They are written out here
// rather than read from it because that class is a JUCE LookAndFeel_V4 and this side of
// the tower is JUCE-free: reading it would couple a new file to the framework the tower
// is removing. The constant each value comes from is named beside it, so a palette
// change has one place to be mirrored to until the JUCE strip is gone and this becomes
// the only copy.
struct ConsolePalette
{
    DuskWidgets::Theme widgets;

    unsigned int stripFill;
    unsigned int stripBorder;
    unsigned int consoleBack;
    unsigned int focusRing;
    unsigned int trackColour;

    unsigned int eqFill;
    unsigned int eqAccent;
    unsigned int compFill;
    unsigned int compGold;
    unsigned int sendFill;
    unsigned int sendPurple;

    unsigned int hfRed;
    unsigned int hmGreen;
    unsigned int lmBlue;
    unsigned int lfGraphite;
    unsigned int filterWhite;
    unsigned int lfGreen;
    unsigned int panRed;
    unsigned int panCyan;

    unsigned int auxFill[4];
    unsigned int auxPreOutline;
    unsigned int busColour[4];

    unsigned int armOn;
    unsigned int armOffText;
    unsigned int muteOn;
    unsigned int soloOn;
    unsigned int phaseOn;
    unsigned int printOn;
    unsigned int insertText;
    unsigned int ioText;
    unsigned int autoReadOn;
    unsigned int autoReadText;
    unsigned int eqTypeChip;
    unsigned int compLabel;
};

const ConsolePalette& consolePalette();
} // namespace duskstudio::imgui
