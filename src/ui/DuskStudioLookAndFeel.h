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

// Track padding above/below the visible scale. Set to the fader cap's
// half-height (cap = 36 px tall, see drawLinearSlider) so the cap at min
// or max sits entirely within the slider's bounds — no clipping into the
// pan knob above or the textBoxBelow value label.
inline constexpr float kFaderTrackPad = 18.0f;

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
    // Mirror DuskStudioLookAndFeel::getSliderLayout (no minYSpace reduce
    // — we override the V2 default 8 px reduce so the visual track and
    // the drag math share the same Y range). Just carve any textbox.
    const auto pos = fader.getTextBoxPosition();
    const int textBoxH = (pos == juce::Slider::TextBoxAbove
                          || pos == juce::Slider::TextBoxBelow)
                           ? fader.getTextBoxHeight() : 0;
    const int textBoxAboveH = (pos == juce::Slider::TextBoxAbove) ? textBoxH : 0;
    const float trackY = (float) (b.getY() + textBoxAboveH);
    const float trackH = (float) (b.getHeight() - textBoxH);
    return trackY + trackH - kFaderTrackPad - prop * (trackH - kFaderTrackPad * 2.0f);
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
        return juce::Font (juce::FontOptions (16.0f));
    }
    juce::Font getPopupMenuFont() override
    {
        return juce::Font (juce::FontOptions (18.5f));
    }

    // Anchor tooltips to the TOP-CENTER of the active main window
    // (instead of JUCE's default "follow the cursor"). With dense
    // dynamic UIs (compressor mode picker, EQ band cells, etc.) the
    // cursor-follow tooltip kept landing over controls the user was
    // about to interact with. Top-center is a stable, predictable
    // location — engineers know where to look without the tooltip
    // ever obscuring the very element it's describing.
    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText,
                                              juce::Point<int> /*screenPos*/,
                                              juce::Rectangle<int> parentArea) override
    {
        // Anchor the tooltip in the top menu-bar row (same row as
        // File / Settings) so the help text sits in the empty black
        // strip beside the loaded-session label — high-visibility,
        // never overlaps the control being described, and stays put
        // as the cursor moves. Forced single-line: measure the tip
        // text without wrap and size the rect to the natural width.
        //
        // Coordinate space: when the TooltipWindow has a parent
        // Component (MainComponent owns ours), `parentArea` is in
        // local-to-parent coords, NOT screen coords. Use parentArea
        // directly — TopLevelWindow::getScreenBounds() returns the
        // WRONG space and the resulting Y gets clamped back inside
        // parentArea, which is why the tooltip lands below the tab
        // row instead of in the menu bar.
        constexpr float kFontPx = 13.0f;
        const juce::Font font { juce::FontOptions (kFontPx) };
        const auto flat = tipText.replaceCharacter ('\n', ' ');
        const int textW = (int) std::ceil (font.getStringWidthFloat (flat)) + 32;   // +padding (must match drawTooltip's natural width)
        const int textH = (int) std::ceil (kFontPx) + 10;
        // Top menu bar row is ~28 px tall; centre the tooltip
        // vertically in that band.
        constexpr int kMenuRowH = 28;
        const int w = juce::jmin (textW, juce::jmax (60, parentArea.getWidth() - 16));
        const int h = textH;
        const int x = parentArea.getCentreX() - w / 2;
        const int y = parentArea.getY() + juce::jmax (0, (kMenuRowH - h) / 2);
        return juce::Rectangle<int> (x, y, w, h).constrainedWithin (parentArea);
    }

    void drawTooltip (juce::Graphics& g, const juce::String& text,
                       int width, int height) override
    {
        // JUCE's LookAndFeel_V4::drawTooltip routes through an internal
        // LayoutHelpers::layoutTooltipText that hard-caps the TextLayout
        // wrap width at ~400 px, so long tooltips wrap to 2-3 lines and
        // their lower lines get clipped against the menu-bar row's
        // single-line height. Render the layout ourselves with
        // word-wrap disabled so the tooltip stays on one line at the
        // full `width` getTooltipBounds reserved for it.
        const juce::Rectangle<float> bounds (0.0f, 0.0f, (float) width, (float) height);
        g.setColour (findColour (juce::TooltipWindow::backgroundColourId));
        g.fillRect (bounds);
        g.setColour (findColour (juce::TooltipWindow::outlineColourId));
        g.drawRect (bounds, 1.0f);

        const auto flat = text.replaceCharacter ('\n', ' ');
        juce::AttributedString as;
        as.setJustification (juce::Justification::centred);
        as.setWordWrap (juce::AttributedString::none);
        as.append (flat,
                    juce::Font (juce::FontOptions (13.0f)),
                    findColour (juce::TooltipWindow::textColourId));
        juce::TextLayout layout;
        layout.createLayout (as, (float) width);
        layout.draw (g, bounds);
    }

    // Override LookAndFeel_V2::getSliderLayout's default 8 px top/bottom
    // reduce on vertical sliders. Default reduce makes the slider's drag
    // math (mouse-Y → value) span a SMALLER rect than the visual track
    // drawn by drawLinearSlider (which uses its own padTopBot=6), so the
    // cap couldn't reach the visible bottom tick. With this override the
    // visual + drag math share identical bounds and the cap can travel
    // the full range from "+6" tick at the top to "off" tick at the bottom.
    juce::Slider::SliderLayout getSliderLayout (juce::Slider& slider) override
    {
        juce::Slider::SliderLayout layout;
        layout.sliderBounds = slider.getLocalBounds();
        // Carve textbox from one edge if attached (preserves the V2
        // behaviour the rest of the UI relies on).
        const auto pos = slider.getTextBoxPosition();
        const int textBoxH = (pos == juce::Slider::TextBoxAbove
                               || pos == juce::Slider::TextBoxBelow)
                                ? slider.getTextBoxHeight() : 0;
        const int textBoxW = (pos == juce::Slider::TextBoxLeft
                               || pos == juce::Slider::TextBoxRight)
                                ? slider.getTextBoxWidth()  : 0;
        if (pos == juce::Slider::TextBoxBelow)
        {
            layout.textBoxBounds = layout.sliderBounds.removeFromBottom (textBoxH);
        }
        else if (pos == juce::Slider::TextBoxAbove)
        {
            layout.textBoxBounds = layout.sliderBounds.removeFromTop (textBoxH);
        }
        else if (pos == juce::Slider::TextBoxRight)
        {
            layout.textBoxBounds = layout.sliderBounds.removeFromRight (textBoxW);
        }
        else if (pos == juce::Slider::TextBoxLeft)
        {
            layout.textBoxBounds = layout.sliderBounds.removeFromLeft (textBoxW);
        }
        // For LINEAR VERTICAL sliders, carve kFaderTrackPad from the top and
        // bottom of layout.sliderBounds. JUCE's getPositionOfValue uses this
        // rect to compute sliderPos — at min/max the cap centres on the
        // sliderBounds edge. Without this carve the cap (36 px tall, drawn
        // centred on sliderPos) would clip half its height at the slider's
        // top/bottom. With the carve the sliderPos travel range lands the
        // cap entirely inside the slider's actual component bounds (so it
        // can also visually overlap an adjacent pan knob via z-order).
        const auto style = slider.getSliderStyle();
        if (style == juce::Slider::LinearVertical
            || style == juce::Slider::LinearBarVertical)
        {
            const int pad = (int) kFaderTrackPad;
            if (layout.sliderBounds.getHeight() > pad * 2)
                layout.sliderBounds.reduce (0, pad);
        }
        return layout;
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
        // bounds here is layout.sliderBounds, which getSliderLayout already
        // shrank by kFaderTrackPad top + bottom so JUCE's sliderPos travel
        // range matches the cap geometry. No extra inset needed.
        const float trackW = juce::jmin (4.0f, bounds.getWidth() * 0.18f);
        const auto trackRect = juce::Rectangle<float> (cx - trackW * 0.5f, bounds.getY(),
                                                        trackW, bounds.getHeight());
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
        const float padTopBot = 0.0f;   // bounds is already the inset track rect (see getSliderLayout)
        const float trackH = bounds.getHeight() - padTopBot * 2.0f;
        // Per-slider opt-in: faders that want hardware-style scale labels
        // drawn alongside the ticks set the "dusk_drawFaderScaleLabels"
        // property. When true, the ticks extend further out and the dB
        // number is drawn on the LEFT of the tick line ("off" replaces
        // "90" at the bottom for that hardware-fader grammar).
        const bool drawScaleLabels = (bool) slider.getProperties()
            .getWithDefault ("dusk_drawFaderScaleLabels", false);

        for (const auto& t : kFaderTicks)
        {
            // Skip ticks outside the slider's range (e.g. -90 on a 0..+12 slider).
            if (t.db < range.start - 0.01f || t.db > range.end + 0.01f) continue;
            const float prop = (float) range.convertTo0to1 (t.db);
            const float tickY = bounds.getBottom() - padTopBot - prop * trackH;
            const bool isZero = (std::abs (t.db) < 0.01f);
            const float xOver = drawScaleLabels ? (isZero ? 24.0f : 20.0f)
                                                : (isZero ? 6.0f  : 3.0f);
            g.setColour (isZero ? juce::Colour (0x90ffffff) : juce::Colour (0x40ffffff));
            g.drawLine (trackRect.getX() - xOver, tickY,
                         trackRect.getRight() + xOver, tickY,
                         isZero ? 1.2f : 0.7f);

            // Label text is intentionally NOT drawn here. The slider's
            // bounds are too narrow on the channel strip to host labels
            // without clipping; the strip paints them itself in its own
            // paint() using faderYForDb for vertical alignment.
        }

        // Cap — tall, narrow, satin metal with horizontal grip grooves.
        // Portrait orientation matches the reference hardware fader cap.
        // Width capped at ~20 px so it stays slim against the thin track.
        const float capW = juce::jmin (bounds.getWidth() - 6.0f, 20.0f);
        const float capH = 36.0f;
        // Cap CENTRE sits exactly on the value's Y — at max value cap
        // centres on the top tick, at min on the bottom tick. Matches
        // standard hardware-fader grammar (cap straddles the value line).
        const float capCy = sliderPos;
        const auto cap = juce::Rectangle<float> (cx - capW * 0.5f,
                                                   capCy - capH * 0.5f,
                                                   capW, capH);

        // Soft drop shadow under the cap.
        juce::DropShadow (juce::Colours::black.withAlpha (0.55f), 6,
                            juce::Point<int> (0, 2))
            .drawForRectangle (g, cap.toNearestInt());

        // Brushed-satin gradient: bright at top + bottom, mid-grey in the
        // middle so the cap reads as a metal cylinder catching light from
        // above and below the centre grip lines.
        juce::ColourGradient body (juce::Colour (0xffe2dccb), cap.getX(), cap.getY(),
                                    juce::Colour (0xffb8b0a0), cap.getX(), cap.getBottom(), false);
        body.addColour (0.30, juce::Colour (0xffcfc8b8));
        body.addColour (0.50, juce::Colour (0xff9d958a));
        body.addColour (0.70, juce::Colour (0xffcfc8b8));
        g.setGradientFill (body);
        g.fillRoundedRectangle (cap, 2.5f);

        // Bright top + bottom edges so the cap reads as 3-D.
        g.setColour (juce::Colour (0x70ffffff));
        g.drawHorizontalLine ((int) cap.getY() + 1, cap.getX() + 2.0f, cap.getRight() - 2.0f);
        g.setColour (juce::Colour (0x40000000));
        g.drawHorizontalLine ((int) cap.getBottom() - 2, cap.getX() + 2.0f, cap.getRight() - 2.0f);

        // Outer rim.
        g.setColour (juce::Colour (0xff0a0a0a));
        g.drawRoundedRectangle (cap, 2.5f, 1.0f);

        // Three horizontal grip grooves through the centre (matches the
        // reference cap's finger-grip detail).
        const float gripX0  = cap.getX() + 3.0f;
        const float gripX1  = cap.getRight() - 3.0f;
        for (int i = -1; i <= 1; ++i)
        {
            const float yC = cap.getCentreY() + (float) i * 4.0f;
            g.setColour (juce::Colour (0xff202018));
            g.fillRect (juce::Rectangle<float> (gripX0, yC - 0.8f, gripX1 - gripX0, 1.6f));
            g.setColour (juce::Colour (0x35ffffff));
            g.drawHorizontalLine ((int) (yC + 0.9f), gripX0, gripX1);
        }
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
    inline constexpr juce::uint32 kHpfBlue = 0xff4a7c9e;   // legacy, retained for any callsite still using it
    // SSL 9000 J top-section filter knobs are white-faced. HPF + LPF
    // share this colour so the user reads them as a pair.
    inline constexpr juce::uint32 kFilterWhite = 0xffe0e0e4;
}
} // namespace duskstudio
