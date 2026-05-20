#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>

namespace duskstudio
{
// Standard fader scale ticks - the dB values that get a horizontal mark on
// every vertical fader's track and a numeric label in the strip's scale
// column. Shared between the LookAndFeel's track drawing and the strip
// paint() functions so the labels and ticks always line up.
struct FaderTick { float db; const char* label; };
inline constexpr std::array<FaderTick, 9> kFaderTicks {{
    {  6.0f,  "+6" },
    {  3.0f,  "+3" },
    {  0.0f,  "0"  },
    { -3.0f,  "3"  },
    { -6.0f,  "6"  },
    { -12.0f, "12" },
    { -24.0f, "24" },
    { -40.0f, "40" },
    { -90.0f, "90" },
}};

// Y-coord, in the FADER's PARENT coordinate system, for a given dB value.
// fader.getBounds() returns parent-relative bounds, so the result is meant
// to be drawn from the parent's paint() (next to the fader). Do NOT call
// from inside the slider's own paint() or after g.setOrigin() shifts the
// graphics origin - the math won't match. Uses the slider's NormalisableRange
// so SkewFactorFromMidPoint(-12) etc. are respected, and the 6-px padding
// matches drawLinearSlider's track-padding so labels align with the ticks.
// Slider::valueToProportionOfLength is non-const in JUCE (mutable cache),
// so the parameter is non-const here too even though we don't mutate it.
// Not noexcept - valueToProportionOfLength can call user-supplied
// NormalisableRange lambdas which JUCE doesn't promise are noexcept.
inline float faderYForDb (juce::Slider& fader, float dB)
{
    const auto b = fader.getBounds();
    const float prop = (float) fader.valueToProportionOfLength (dB);
    // 6 px padding top + bottom matches drawLinearSlider's track rect.
    return (float) b.getY() + (float) b.getHeight() - 6.0f
            - prop * ((float) b.getHeight() - 12.0f);
}

// Console-style rotary knob look: dark grey body with a soft inner gradient
// plus a colored "cap" indicator that points to the current value, modelled
// on the SSL 4K / Harrison Mixbus knob aesthetic.
//
// Per-knob accent comes from the slider's `rotarySliderFillColourId` - set
// that to the band/section color and the cap takes it.
class DuskStudioLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    DuskStudioLookAndFeel()
    {
        setColour (juce::Slider::rotarySliderFillColourId,    juce::Colour (0xff4a7c9e));
        setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff404048));
        setColour (juce::Slider::thumbColourId,               juce::Colour (0xffe0e0e0));
        setColour (juce::Slider::trackColourId,               juce::Colour (0xff303034));
        setColour (juce::Slider::backgroundColourId,          juce::Colour (0xff1a1a1c));
    }

    // ComboBox + PopupMenu fonts. JUCE's defaults clamp the combo font at
    // 15 px and the popup-menu font at 17 px - on dense console-style UIs
    // both feel cramped, especially the long MIDI device names ("PANORAMA
    // T6 Plugin" etc.) which truncate to "PANO..." in the closed combo.
    // Explicit overrides make the closed combo legible and the open
    // dropdown's items comfortable to read.
    juce::Font getComboBoxFont (juce::ComboBox&) override
    {
        return juce::Font (juce::FontOptions (14.0f));
    }
    juce::Font getPopupMenuFont() override
    {
        return juce::Font (juce::FontOptions (16.5f));
    }

    // Console-style fader cap for vertical linear sliders. The fader rides on
    // a thin centered track; the thumb is a wide horizontal cap with a soft
    // gradient and grip lines, modelled on a real motorized fader handle.
    void drawLinearSlider (juce::Graphics& g,
                           int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           const juce::Slider::SliderStyle style,
                           juce::Slider& slider) override
    {
        if (style != juce::Slider::LinearVertical && style != juce::Slider::LinearBarVertical)
        {
            juce::LookAndFeel_V4::drawLinearSlider (g, x, y, width, height,
                                                     sliderPos, minSliderPos, maxSliderPos,
                                                     style, slider);
            return;
        }

        const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
        const float cx = bounds.getCentreX();

        // Track - thin vertical channel with a faint unity-gain mark.
        const float trackW = juce::jmin (4.0f, bounds.getWidth() * 0.18f);
        const auto trackRect = juce::Rectangle<float> (cx - trackW * 0.5f, bounds.getY() + 6.0f,
                                                        trackW, bounds.getHeight() - 12.0f);
        g.setColour (juce::Colour (0xff0a0a0c));
        g.fillRoundedRectangle (trackRect, trackW * 0.5f);
        // Inner shadow at the top so the track reads as recessed into the
        // strip - a subtle vertical fade just inside the top edge.
        {
            juce::ColourGradient innerShadow (juce::Colour (0x80000000),
                                                trackRect.getX(), trackRect.getY(),
                                                juce::Colour (0x00000000),
                                                trackRect.getX(),
                                                trackRect.getY() + 6.0f, false);
            g.setGradientFill (innerShadow);
            g.fillRoundedRectangle (trackRect, trackW * 0.5f);
        }
        g.setColour (juce::Colour (0xff2a2a2e));
        g.drawRoundedRectangle (trackRect, trackW * 0.5f, 0.6f);

        // dB tick marks across the track. The 0 dB / unity mark gets a
        // brighter, slightly longer line; the others are dim guides for
        // setting levels by ear. Range-aware via NormalisableRange so the
        // skew (e.g. SkewFactorFromMidPoint(-12)) places ticks correctly.
        const auto range = slider.getNormalisableRange();
        const float padTopBot = 6.0f;
        const float trackH = bounds.getHeight() - padTopBot * 2.0f;
        for (const auto& t : kFaderTicks)
        {
            // Skip ticks outside the slider's range (e.g. -90 on a 0..+12 slider).
            if (t.db < range.start - 0.01f || t.db > range.end + 0.01f) continue;
            const float prop = (float) range.convertTo0to1 (t.db);
            const float tickY = bounds.getBottom() - padTopBot - prop * trackH;
            const bool isZero = (std::abs (t.db) < 0.01f);
            const float xOver = isZero ? 6.0f : 3.0f;
            g.setColour (isZero ? juce::Colour (0x90ffffff) : juce::Colour (0x40ffffff));
            g.drawLine (trackRect.getX() - xOver, tickY,
                         trackRect.getRight() + xOver, tickY,
                         isZero ? 1.2f : 0.7f);
        }

        // Cap.
        const float capW = juce::jmin (bounds.getWidth() - 6.0f, 38.0f);
        const float capH = 22.0f;
        const auto cap = juce::Rectangle<float> (cx - capW * 0.5f, sliderPos - capH * 0.5f, capW, capH);

        // Soft drop shadow under the cap. Real Gaussian blur via
        // juce::DropShadow looks like the cap is hovering above the
        // track; the previous 2-px translated rectangle read as a
        // graphic-design shadow rather than a physical one. Drawn first
        // so the body gradient sits cleanly on top.
        juce::DropShadow (juce::Colours::black.withAlpha (0.55f), 6,
                            juce::Point<int> (0, 2))
            .drawForRectangle (g, cap.toNearestInt());

        // Body gradient (top-bright, bottom-dark).
        juce::ColourGradient body (juce::Colour (0xffd0c8b0), cap.getX(), cap.getY(),
                                    juce::Colour (0xff3a3530), cap.getX(), cap.getBottom(), false);
        body.addColour (0.45, juce::Colour (0xff908878));
        body.addColour (0.55, juce::Colour (0xff605850));
        g.setGradientFill (body);
        g.fillRoundedRectangle (cap, 3.0f);

        // Bright top edge.
        g.setColour (juce::Colour (0x60ffffff));
        g.drawHorizontalLine ((int) cap.getY() + 1, cap.getX() + 2.0f, cap.getRight() - 2.0f);

        // Outer rim.
        g.setColour (juce::Colour (0xff0a0a0a));
        g.drawRoundedRectangle (cap, 3.0f, 1.0f);

        // Center grip groove (where the engineer's finger sits).
        g.setColour (juce::Colour (0xff20201c));
        g.fillRoundedRectangle (juce::Rectangle<float> (cap.getX() + 4.0f, cap.getCentreY() - 1.0f,
                                                          cap.getWidth() - 8.0f, 2.0f), 1.0f);
        g.setColour (juce::Colour (0x30ffffff));
        g.drawHorizontalLine ((int) cap.getCentreY() - 2,
                               cap.getX() + 4.0f, cap.getRight() - 4.0f);
    }

    void drawRotarySlider (juce::Graphics& g,
                           int x, int y, int width, int height,
                           float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider& slider) override
    {
        const float cx = x + width  * 0.5f;
        const float cy = y + height * 0.5f;
        const float radius = juce::jmin (width, height) * 0.5f - 2.0f;
        if (radius <= 2.0f) return;

        const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
        const auto fill = slider.findColour (juce::Slider::rotarySliderFillColourId);

        // Soft drop shadow under the body.
        {
            juce::ColourGradient shadow (juce::Colour (0x60000000), cx, cy,
                                         juce::Colour (0x00000000), cx, cy + radius + 6.0f, true);
            g.setGradientFill (shadow);
            g.fillEllipse (cx - radius - 2.0f, cy - radius, (radius + 2.0f) * 2.0f, (radius + 2.0f) * 2.0f);
        }

        // Body - fully coloured in the slider's accent (SSL-style).
        // Radial gradient: top-left highlight, bottom-right shadow.
        {
            juce::ColourGradient body (fill.brighter (0.15f), cx - radius * 0.55f, cy - radius * 0.55f,
                                       fill.darker  (0.55f),  cx + radius * 0.55f, cy + radius * 0.55f, true);
            g.setGradientFill (body);
            g.fillEllipse (cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);
        }

        // Subtle plastic-sheen highlight at the top.
        g.setColour (juce::Colour (0x20ffffff));
        g.fillEllipse (cx - radius * 0.85f, cy - radius * 0.95f, radius * 1.7f, radius * 0.7f);

        // Crisp dark rim.
        g.setColour (juce::Colour (0xff0a0a0a));
        g.drawEllipse (cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, 1.2f);

        // Indicator - white line from inner radius to near the rim, plus a
        // small white dot at the tip. Replaces the old coloured cap.
        const float dx = std::cos (angle - juce::MathConstants<float>::halfPi);
        const float dy = std::sin (angle - juce::MathConstants<float>::halfPi);
        const float lineInnerR = radius * 0.30f;
        const float lineOuterR = radius * 0.92f;
        const float tipR       = juce::jmax (1.5f, radius * 0.12f);
        const float lineX1 = cx + lineInnerR * dx;
        const float lineY1 = cy + lineInnerR * dy;
        const float lineX2 = cx + lineOuterR * dx;
        const float lineY2 = cy + lineOuterR * dy;

        g.setColour (juce::Colours::white);
        g.drawLine (lineX1, lineY1, lineX2, lineY2, 2.0f);

        // Tip dot.
        const float tipX = cx + (lineOuterR - tipR * 0.4f) * dx;
        const float tipY = cy + (lineOuterR - tipR * 0.4f) * dy;
        g.setColour (juce::Colours::white);
        g.fillEllipse (tipX - tipR, tipY - tipR, tipR * 2.0f, tipR * 2.0f);
        g.setColour (juce::Colour (0x80000000));
        g.drawEllipse (tipX - tipR, tipY - tipR, tipR * 2.0f, tipR * 2.0f, 0.5f);
    }
};

// 4K-derived band/section accent colors. Re-used wherever we need the
// canonical SSL palette. Names also drive the track colour-picker labels.
namespace fourKColors
{
    inline constexpr juce::uint32 kHpfBlue   = 0xff4a7c9e;
    inline constexpr juce::uint32 kLfGreen   = 0xff5c9a5c;
    inline constexpr juce::uint32 kLmAmber   = 0xffd9a35a;
    inline constexpr juce::uint32 kHmOrange  = 0xffc47a44;
    inline constexpr juce::uint32 kHfRed     = 0xffc44444;
    inline constexpr juce::uint32 kCompGold  = 0xffd09060;
    inline constexpr juce::uint32 kSendPurple= 0xff9080c0;
    inline constexpr juce::uint32 kPanCyan   = 0xff70b8c0;
    inline constexpr juce::uint32 kMasterTan = 0xffd0a060;
}

// SSL 9000J band colours - used only for the EQ knob bodies. Kept in a
// separate namespace so the track colour-picker (driven by fourKColors) keeps
// matching its labels (Red / Orange / Amber / Green).
namespace sslEqColors
{
    inline constexpr juce::uint32 kHfRed   = 0xffc44444;
    inline constexpr juce::uint32 kHmGreen = 0xff5fa55f;
    inline constexpr juce::uint32 kLmBlue  = 0xff5878b0;
    inline constexpr juce::uint32 kLfBlack = 0xff353538;
    inline constexpr juce::uint32 kHpfBlue = 0xff4a7c9e;
}
} // namespace duskstudio
