#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>

namespace duskstudio
{
// Shared by LookAndFeel track drawing + strip paint() so labels and
// ticks always line up.
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

// = cap half-height (cap 36 px in drawLinearSlider) so the cap at min /
// max sits entirely inside the slider's bounds.
inline constexpr float kFaderTrackPad = 18.0f;

// PARENT-coord Y for a given dB. Call from the parent's paint(), NOT
// from the slider's own paint() or after g.setOrigin() — math won't
// match. Honours NormalisableRange skew. Non-const + non-noexcept
// because valueToProportionOfLength caches internally and can call
// user-supplied lambdas.
inline float faderYForDb (juce::Slider& fader, float dB)
{
    const auto b = fader.getBounds();
    const float prop = (float) fader.valueToProportionOfLength (dB);
    // Mirror getSliderLayout — we override V2's 8 px reduce so visual
    // track and drag math share the same Y. Just carve the textbox.
    const auto pos = fader.getTextBoxPosition();
    const int textBoxH = (pos == juce::Slider::TextBoxAbove
                          || pos == juce::Slider::TextBoxBelow)
                           ? fader.getTextBoxHeight() : 0;
    const int textBoxAboveH = (pos == juce::Slider::TextBoxAbove) ? textBoxH : 0;
    const float trackY = (float) (b.getY() + textBoxAboveH);
    const float trackH = (float) (b.getHeight() - textBoxH);
    return trackY + trackH - kFaderTrackPad - prop * (trackH - kFaderTrackPad * 2.0f);
}

// SSL 4K / Harrison Mixbus knob aesthetic. Per-knob accent =
// rotarySliderFillColourId on the slider.
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

    // JUCE defaults clamp combo at 15 px / popup at 17 px — long MIDI
    // device names truncate ("PANO..."). Bump to 16 / 18.5 px.
    juce::Font getComboBoxFont (juce::ComboBox&) override
    {
        return juce::Font (juce::FontOptions (16.0f));
    }
    juce::Font getPopupMenuFont() override
    {
        return juce::Font (juce::FontOptions (18.5f));
    }

    // The menu-row band that tooltips live in, and the stage-tab block they
    // must NOT cover. MainComponent::resized() keeps these current; both are
    // in MainComponent-local coords (the space getTooltipBounds returns).
    void setTooltipPlacement (juce::Rectangle<int> row, juce::Rectangle<int> avoid) noexcept
    {
        tooltipRow_   = row;
        tooltipAvoid_ = avoid;
    }

    // Anchor in the top menu-bar row (same row as File/Settings) so
    // tooltips never overlap the control they describe. JUCE's default
    // cursor-follow kept landing over UI the user was about to click.
    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText,
                                              juce::Point<int> /*screenPos*/,
                                              juce::Rectangle<int> parentArea) override
    {
        // parentArea is in local-to-parent (MainComponent owns the
        // TooltipWindow), NOT screen coords; TopLevelWindow::getScreenBounds
        // returns the wrong space and the resulting Y gets clamped back inside
        // parentArea (tooltip lands below the tab row otherwise).
        constexpr float kFontPx = 13.0f;
        constexpr int   kPadX   = 16;   // total inner horizontal padding
        constexpr int   kPadY   = 8;    // total inner vertical padding
        const juce::Font font { juce::FontOptions (kFontPx) };
        const auto flat = tipText.replaceCharacter ('\n', ' ');

        // Natural single-line width via TextLayout (Font::getStringWidthFloat is
        // deprecated in JUCE 8).
        const int textW = [&]
        {
            juce::AttributedString as;
            as.setWordWrap (juce::AttributedString::none);
            as.append (flat, font, juce::Colours::white);
            juce::TextLayout tl;
            tl.createLayout (as, 1.0e6f);
            return (int) std::ceil (tl.getWidth()) + kPadX;
        }();

        // Height needed to render `flat` word-wrapped inside a box `boxW` wide.
        // A long tip that won't fit one line wraps and grows DOWNWARD instead
        // of overflowing a single-line box (the bug: centred no-wrap text
        // spilled past both edges and got clipped middle-out). Must match
        // drawTooltip's layout width (boxW - kPadX).
        auto wrappedHeight = [&] (int boxW)
        {
            juce::AttributedString as;
            as.setWordWrap (juce::AttributedString::byWord);
            as.append (flat, font, juce::Colours::white);
            juce::TextLayout tl;
            tl.createLayout (as, (float) juce::jmax (1, boxW - kPadX));
            return (int) std::ceil (tl.getHeight()) + kPadY;
        };

        if (! tooltipRow_.isEmpty())
        {
            // Place inside the menu-row band, dodging the centred stage tabs:
            // a centred tip would always sit on top of them. Prefer the side
            // gap that fits the natural width (left/status gap first), else the
            // wider side, clamping width so the tip never overlaps the tabs.
            constexpr int gap = 12;
            int w = juce::jmin (textW, tooltipRow_.getWidth());
            int x = tooltipRow_.getCentreX() - w / 2;

            if (! tooltipAvoid_.isEmpty()
                  && juce::Rectangle<int> (x, tooltipRow_.getY(), w, tooltipRow_.getHeight())
                        .intersects (tooltipAvoid_))
            {
                const int leftRoom  = juce::jmax (0, tooltipAvoid_.getX() - gap - tooltipRow_.getX());
                const int rightRoom = juce::jmax (0, tooltipRow_.getRight() - (tooltipAvoid_.getRight() + gap));
                if (w <= leftRoom)              { x = tooltipRow_.getX(); }
                else if (w <= rightRoom)        { x = tooltipAvoid_.getRight() + gap; }
                else if (leftRoom >= rightRoom) { w = leftRoom;  x = tooltipRow_.getX(); }
                else                            { w = rightRoom; x = tooltipAvoid_.getRight() + gap; }
            }

            // Clamp w/h to the parent BEFORE measuring/returning so
            // constrainedWithin only repositions (never resizes) - a resize
            // would shrink the box below the width wrappedHeight used and
            // re-introduce clipping.
            w = juce::jlimit (1, parentArea.getWidth(), w);
            const int h = juce::jmin (wrappedHeight (w), parentArea.getHeight());
            // Single line: centre vertically in the row. Wrapped (taller than
            // the row): start at the row top and grow downward over the
            // transport row below - a transient tip may briefly cover content.
            const int y = (h <= tooltipRow_.getHeight())
                            ? tooltipRow_.getY() + (tooltipRow_.getHeight() - h) / 2
                            : tooltipRow_.getY();
            return juce::Rectangle<int> (x, y, w, h).constrainedWithin (parentArea);
        }

        // Fallback before the first layout pass sets a placement: centre the
        // tip in a top band the height of the original menu row.
        constexpr int kMenuRowH = 28;
        const int w = juce::jlimit (1, parentArea.getWidth(),
                                     juce::jmin (textW, juce::jmax (60, parentArea.getWidth() - 16)));
        const int h = juce::jmin (wrappedHeight (w), parentArea.getHeight());
        const int x = parentArea.getCentreX() - w / 2;
        const int y = parentArea.getY() + juce::jmax (0, (kMenuRowH - h) / 2);
        // w/h fit parentArea, so constrainedWithin only repositions (no resize
        // that would diverge from wrappedHeight's wrapping width).
        return juce::Rectangle<int> (x, y, w, h).constrainedWithin (parentArea);
    }

    void drawTooltip (juce::Graphics& g, const juce::String& text,
                       int width, int height) override
    {
        // Word-wrap ON: getTooltipBounds sized the box (incl. height) to the
        // wrapped text, so long tips flow onto multiple lines instead of
        // overflowing a single line and clipping middle-out. Layout width must
        // match getTooltipBounds' measure (width - kPadX, kPadX = 16).
        const juce::Rectangle<float> bounds (0.0f, 0.0f, (float) width, (float) height);
        g.setColour (findColour (juce::TooltipWindow::backgroundColourId));
        g.fillRect (bounds);
        g.setColour (findColour (juce::TooltipWindow::outlineColourId));
        g.drawRect (bounds, 1.0f);

        const auto flat = text.replaceCharacter ('\n', ' ');
        juce::AttributedString as;
        as.setJustification (juce::Justification::centred);
        as.setWordWrap (juce::AttributedString::byWord);
        as.append (flat,
                    juce::Font (juce::FontOptions (13.0f)),
                    findColour (juce::TooltipWindow::textColourId));
        juce::TextLayout layout;
        const float innerW = (float) juce::jmax (1, width - 16);
        layout.createLayout (as, innerW);
        const float ty = juce::jmax (0.0f, ((float) height - layout.getHeight()) * 0.5f);
        layout.draw (g, juce::Rectangle<float> (8.0f, ty, innerW, layout.getHeight()));
    }

    // Override V2's 8 px top/bottom reduce — default reduce makes the
    // drag math span a smaller rect than the visual track, so the cap
    // couldn't reach the bottom tick. Visual + drag math now identical.
    juce::Slider::SliderLayout getSliderLayout (juce::Slider& slider) override
    {
        juce::Slider::SliderLayout layout;
        layout.sliderBounds = slider.getLocalBounds();
        // Carve textbox from one edge (preserves V2 behaviour the
        // rest of the UI relies on).
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
        // Vertical sliders: carve kFaderTrackPad so the 36 px cap
        // (drawn centred on sliderPos) doesn't clip half its height
        // at the slider top/bottom. Carved range = full cap travel
        // inside component bounds.
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

    // Motorized fader handle look: thin centred track + tall narrow
    // cap with brushed-satin gradient + grip grooves.
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

        // bounds = layout.sliderBounds (already shrunk by kFaderTrackPad
        // in getSliderLayout). No extra inset needed.
        const float trackW = juce::jmin (4.0f, bounds.getWidth() * 0.18f);
        const auto trackRect = juce::Rectangle<float> (cx - trackW * 0.5f, bounds.getY(),
                                                        trackW, bounds.getHeight());
        g.setColour (juce::Colour (0xff0a0a0c));
        g.fillRoundedRectangle (trackRect, trackW * 0.5f);
        // Recessed look — vertical fade just inside the top edge.
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

        // dB ticks. 0 dB gets a brighter / longer line. Range-aware via
        // NormalisableRange so SkewFactorFromMidPoint(-12) places ticks
        // correctly.
        const auto range = slider.getNormalisableRange();
        const float padTopBot = 0.0f;   // bounds already inset
        const float trackH = bounds.getHeight() - padTopBot * 2.0f;
        // Per-slider opt-in via "dusk_drawFaderScaleLabels" property —
        // hardware-fader grammar: ticks extend further, dB drawn left of
        // tick, "off" replaces "90" at the bottom.
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

            // Labels not drawn here — strip is too narrow without
            // clipping; strip's own paint() uses faderYForDb.
        }

        // Cap CENTRE sits exactly on the value's Y — hardware-fader
        // grammar (cap straddles the value line).
        const float capW = juce::jmin (bounds.getWidth() - 6.0f, 20.0f);
        const float capH = 36.0f;
        const float capCy = sliderPos;
        const auto cap = juce::Rectangle<float> (cx - capW * 0.5f,
                                                   capCy - capH * 0.5f,
                                                   capW, capH);

        // Soft drop shadow under the cap.
        juce::DropShadow (juce::Colours::black.withAlpha (0.55f), 6,
                            juce::Point<int> (0, 2))
            .drawForRectangle (g, cap.toNearestInt());

        // Brushed satin: bright top + bottom, mid-grey middle so the
        // cap reads as a metal cylinder catching light around the grip.
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
        const float outerR = juce::jmin (width, height) * 0.5f - 2.0f;
        if (outerR <= 2.0f) return;

        const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
        const auto fill = slider.findColour (juce::Slider::rotarySliderFillColourId);

        // SSL-hardware EQ knob: a milled grey skirt around a smaller coloured
        // centre cap, instead of the glossy ball. Scoped by a per-slider property
        // so only EQ knobs get it (comp / pan / send knobs keep the ball).
        if ((bool) slider.getProperties().getWithDefault ("sslEqKnob", false))
        {
            drawSslEqKnob (g, cx, cy, outerR, angle, fill);
            return;
        }

        // Value ring in the annulus around the body — an at-a-glance value
        // readout that lifts knob contrast against the dark panels. Only on
        // larger knobs (mastering / master): tiny channel-strip knobs keep the
        // clean capped look so the ring doesn't crowd a 28 px body.
        float radius = outerR;
        const bool drawRing = outerR > 15.0f;
        if (drawRing)
        {
            const float ringR     = outerR - 1.0f;
            const float ringThick = juce::jmax (2.5f, outerR * 0.13f);
            juce::Path track;
            track.addCentredArc (cx, cy, ringR, ringR, 0.0f,
                                 rotaryStartAngle, rotaryEndAngle, true);
            g.setColour (juce::Colour (0xff31313c));
            g.strokePath (track, juce::PathStrokeType (ringThick, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
            juce::Path val;
            val.addCentredArc (cx, cy, ringR, ringR, 0.0f,
                               rotaryStartAngle, angle, true);
            g.setColour (fill.brighter (0.30f));
            g.strokePath (val, juce::PathStrokeType (ringThick, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
            radius = ringR - ringThick - 1.5f;   // body sits inside the ring
            if (radius <= 2.0f) return;
        }

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

    // SSL-console knob: a milled grey skirt (knurled rim) around a coloured
    // centre cap, with a pointer that spans the cap and the skirt. R is the full
    // radius; `fill` is the band accent (cap colour); `angle` the value angle.
    void drawSslEqKnob (juce::Graphics& g, float cx, float cy, float R,
                        float angle, juce::Colour fill)
    {
        // Soft drop shadow under the knob.
        {
            juce::ColourGradient shadow (juce::Colour (0x66000000), cx, cy,
                                         juce::Colour (0x00000000), cx, cy + R + 6.0f, true);
            g.setGradientFill (shadow);
            g.fillEllipse (cx - R - 2.0f, cy - R, (R + 2.0f) * 2.0f, (R + 2.0f) * 2.0f);
        }

        // Milled grey skirt — flat metal diagonal sheen (light top-left).
        {
            juce::ColourGradient skirt (juce::Colour (0xff74747e), cx - R, cy - R,
                                        juce::Colour (0xff26262b), cx + R, cy + R, false);
            g.setGradientFill (skirt);
            g.fillEllipse (cx - R, cy - R, R * 2.0f, R * 2.0f);
        }

        // Knurling — short radial teeth (alternating light edge / dark gap) around
        // the rim. Static (uniform), so it reads the same as a rotating knurl.
        // Skipped on tiny inline knobs where it would just be noise.
        if (R > 16.0f)
        {
            constexpr int teeth = 32;
            const float r0 = R * 0.84f, r1 = R * 0.99f;
            const float half = juce::MathConstants<float>::twoPi * 0.5f / (float) teeth;
            for (int i = 0; i < teeth; ++i)
            {
                const float a = juce::MathConstants<float>::twoPi * (float) i / (float) teeth;
                const float ca = std::cos (a), sa = std::sin (a);
                g.setColour (juce::Colour (0x40ffffff));
                g.drawLine (cx + r0 * ca, cy + r0 * sa, cx + r1 * ca, cy + r1 * sa, 1.0f);
                const float c2 = std::cos (a + half), s2 = std::sin (a + half);
                g.setColour (juce::Colour (0x55000000));
                g.drawLine (cx + r0 * c2, cy + r0 * s2, cx + r1 * c2, cy + r1 * s2, 1.0f);
            }
        }

        // Outer dark rim.
        g.setColour (juce::Colour (0xff0a0a0a));
        g.drawEllipse (cx - R, cy - R, R * 2.0f, R * 2.0f, 1.2f);

        // Recessed groove where the cap meets the skirt.
        const float capR = R * 0.58f;
        g.setColour (juce::Colour (0xff141417));
        g.drawEllipse (cx - capR - 1.5f, cy - capR - 1.5f,
                       (capR + 1.5f) * 2.0f, (capR + 1.5f) * 2.0f, 2.0f);

        // Coloured centre cap — slightly domed (radial highlight top-left).
        {
            juce::ColourGradient cap (fill.brighter (0.18f), cx - capR * 0.4f, cy - capR * 0.4f,
                                      fill.darker (0.50f),   cx + capR * 0.55f, cy + capR * 0.55f, true);
            g.setGradientFill (cap);
            g.fillEllipse (cx - capR, cy - capR, capR * 2.0f, capR * 2.0f);
        }
        g.setColour (juce::Colour (0x22ffffff));   // cap sheen
        g.fillEllipse (cx - capR * 0.7f, cy - capR * 0.85f, capR * 1.4f, capR * 0.6f);

        // Pointer — white line across the cap plus a thicker tick on the skirt at
        // the value angle, so the position reads on both (like a real SSL knob).
        const float dx = std::cos (angle - juce::MathConstants<float>::halfPi);
        const float dy = std::sin (angle - juce::MathConstants<float>::halfPi);
        g.setColour (juce::Colours::white);
        g.drawLine (cx + R * 0.12f * dx, cy + R * 0.12f * dy,
                    cx + capR * 0.95f * dx, cy + capR * 0.95f * dy, 2.0f);
        g.drawLine (cx + capR * 1.10f * dx, cy + capR * 1.10f * dy,
                    cx + R * 0.95f * dx, cy + R * 0.95f * dy,
                    juce::jmax (1.6f, R * 0.07f));
    }

private:
    juce::Rectangle<int> tooltipRow_;     // menu-row band tooltips live in
    juce::Rectangle<int> tooltipAvoid_;   // stage-tab block they must not cover
};

// 4K band/section palette. Names also drive the track colour-picker labels.
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

// SSL 9000J palette — EQ knob bodies only. Separate namespace so the
// track colour-picker (driven by fourKColors) keeps its labels (Red /
// Orange / Amber / Green).
namespace sslEqColors
{
    inline constexpr juce::uint32 kHfRed   = 0xffc44444;
    inline constexpr juce::uint32 kHmGreen = 0xff5fa55f;
    inline constexpr juce::uint32 kLmBlue  = 0xff5878b0;
    inline constexpr juce::uint32 kLfBlack = 0xff5a5a62;  // graphite, not true black — legible on the dark EQ panel while staying the darkest band
    inline constexpr juce::uint32 kHpfBlue = 0xff4a7c9e;
    // 9000J top-section filter knobs are white-faced — HPF + LPF
    // share this so they read as a pair.
    inline constexpr juce::uint32 kFilterWhite = 0xffe0e0e4;
}

// Tag a rotary slider so DuskStudioLookAndFeel::drawRotarySlider paints it as an
// SSL-console knob (milled grey skirt + coloured cap) instead of the glossy ball.
// Call on EQ knobs only.
inline void markEqKnob (juce::Slider& s) { s.getProperties().set ("sslEqKnob", true); }
} // namespace duskstudio
