#include "AnalogVuMeter.h"
#include <cmath>

namespace duskstudio
{
namespace
{
const juce::Colour kFaceTop        { 0xfff5f5f0 };
const juce::Colour kFaceBottom     { 0xffe2e2dc };
const juce::Colour kBezelOuter     { 0xff1a1410 };  // warm dark brown - TEAC look
const juce::Colour kBezelInner     { 0xff2a2218 };  // mid brown highlight ring
const juce::Colour kInk            { 0xff111114 };
const juce::Colour kInkSoft        { 0xff5a5a60 };
const juce::Colour kRedZone        { 0xffb02020 };
const juce::Colour kNeedle         { 0xff141418 };
const juce::Colour kNeedleR        { 0xff8a2828 };
const juce::Colour kPivotHousing   { 0xff1c1c20 };
const juce::Colour kPeakLedOff     { 0xff5a1a1a };
const juce::Colour kPeakLedOn      { 0xffff3030 };

// Warm-cream "rich" palette (TEAC/Tascam look), used only when richStyle is on.
const juce::Colour kCreamCore      { 0xfff6f1e3 };  // lit ivory centre
const juce::Colour kCreamRim       { 0xffe6dcc4 };  // aged cream at the edges
const juce::Colour kVignette       { 0x33000000 };  // corner darken (aging)
const juce::Colour kBezelHi        { 0xff4a4038 };  // top/left frame lip (lit)
const juce::Colour kBezelLo        { 0xff120d09 };  // bottom/right frame lip
const juce::Colour kGlassSheen     { 0xffffffff };  // specular reflection (low alpha)
const juce::Colour kHubHi          { 0xffe8e8ee };  // chrome dome highlight
const juce::Colour kHubMid         { 0xff8e8e96 };
const juce::Colour kHubLo          { 0xff2a2a30 };  // chrome dome shadow
const juce::Colour kRedWashCol     { 0xffb02020 };  // ink-wash under the 0..+3 arc

// Scale endpoints in VU dB (mirrors TapeMachine clamp).
constexpr float kVuMin = -20.0f;
constexpr float kVuMax =   3.0f;

// IEC 60268-17 ballistics (300 ms rise to 99 % via 65 ms RC) + mechanical
// spring overshoot. Same constants TapeMachine uses so the needles share
// dynamic behaviour, not just scale.
constexpr float kRefreshHz         = 60.0f;
constexpr float kVuTimeConstantMs  = 65.0f;
constexpr float kOvershootDamping  = 0.78f;
constexpr float kOvershootStiff    = 180.0f;

// Below this face width the numeric scale labels are suppressed - they
// collide on narrow bus strips, ticks alone read clearly. Matches the
// existing width-aware pattern.
constexpr int   kLabelMinWidth   = 88;
constexpr float kPeakHoldMs      = 600.0f;
constexpr float kPeakTriggerVuDb = 4.0f;

// Scale anchor: where on the swept arc a given VU dB lands. angleFrac is
// [-1, +1] = leftmost to rightmost edge of the half-sweep. Mirrors the
// non-linear spacing of a real analog VU face: the −20..−10 stretch
// covers a lot of arc, the run from −3 to 0 is much tighter, the red
// 0..+3 zone takes up the right ~⅓ of the sweep. The needle reads the
// same anchors so its rest-to-peak motion lands on the right labels.
struct ScaleAnchor { float vuDb; float angleFrac; };
const ScaleAnchor kAnchors[] = {
    { -20.0f, -1.000f },
    { -10.0f, -0.560f },
    {  -7.0f, -0.380f },
    {  -5.0f, -0.230f },
    {  -3.0f, -0.080f },
    {   0.0f,  0.210f },
    {   3.0f,  1.000f },
};
constexpr int kNumAnchors = sizeof (kAnchors) / sizeof (kAnchors[0]);

float vuDbToAngleFrac (float vuDb) noexcept
{
    if (vuDb <= kAnchors[0].vuDb)                 return kAnchors[0].angleFrac;
    if (vuDb >= kAnchors[kNumAnchors - 1].vuDb)   return kAnchors[kNumAnchors - 1].angleFrac;
    for (int i = 1; i < kNumAnchors; ++i)
    {
        if (vuDb <= kAnchors[i].vuDb)
        {
            const auto& a = kAnchors[i - 1];
            const auto& b = kAnchors[i];
            const float t = (vuDb - a.vuDb) / (b.vuDb - a.vuDb);
            return a.angleFrac + t * (b.angleFrac - a.angleFrac);
        }
    }
    return kAnchors[kNumAnchors - 1].angleFrac;
}

struct ScaleMark
{
    float       vuDb;       // tap point; angleFrac derived through vuDbToAngleFrac
    const char* label;      // nullptr = unlabelled minor tick
    bool        red;        // 0 VU and above paints red
    bool        primary;    // primary = numbered, longer + thicker tick
};

// Major (numbered) ticks + minor sub-ticks. The minor ticks fill out the
// scale between numbered marks the way a hardware face does. Reds turn on
// at 0 VU; everything below stays black.
const ScaleMark kMarks[] =
{
    { -20.0f, "-20", false, true  },
    { -15.0f, nullptr, false, false },
    { -12.0f, nullptr, false, false },
    { -10.0f, "10",  false, true  },
    {  -8.5f, nullptr, false, false },
    {  -7.0f,  "7",  false, true  },
    {  -6.0f, nullptr, false, false },
    {  -5.0f,  "5",  false, true  },
    {  -4.0f, nullptr, false, false },
    {  -3.0f,  "3",  false, true  },
    {  -2.0f, nullptr, false, false },
    {  -1.0f, nullptr, false, false },
    {   0.0f,  "0",  false, true  },   // "0" numeral is black; the red zone arc still starts here
    {   1.0f, nullptr, true,  false },
    {   2.0f, nullptr, true,  false },
    {   3.0f,  "+3", true,  true  },
};
} // namespace

AnalogVuMeter::AnalogVuMeter (const std::atomic<float>* l, const std::atomic<float>* r)
    : leftRms (l), rightRms (r)
{
    setInterceptsMouseClicks (false, false);
    setOpaque (false);
    startTimerHz ((int) kRefreshHz);
}

AnalogVuMeter::~AnalogVuMeter() = default;

void AnalogVuMeter::setReferenceLevelDb (float refDb)
{
    if (std::abs (referenceDbFs - refDb) < 0.01f) return;
    referenceDbFs = refDb;
    rebuildCachedFace();
    repaint();
}

void AnalogVuMeter::setCompactScale (bool compact)
{
    if (compactScale == compact) return;
    compactScale = compact;
    rebuildCachedFace();
    repaint();
}

void AnalogVuMeter::setRichStyle (bool rich)
{
    if (richStyle == rich) return;
    richStyle = rich;
    rebuildCachedFace();
    repaint();
}

namespace
{
// Display scale the cache should rasterise at, so the baked face stays crisp on
// HiDPI screens (the cache was previously allocated in logical px and softened
// when the OS upscaled it). Uses the scale of the monitor the component is
// actually on (not the primary) so a meter dragged to a different-DPI screen
// re-rasterises at the right resolution. Falls back to the primary, then 1x.
float currentDisplayScale (const juce::Component& comp) noexcept
{
    auto& displays = juce::Desktop::getInstance().getDisplays();
    if (auto* d = displays.getDisplayForRect (comp.getScreenBounds()))
        return juce::jlimit (1.0f, 4.0f, (float) d->scale);
    if (auto* d = displays.getPrimaryDisplay())
        return juce::jlimit (1.0f, 4.0f, (float) d->scale);
    return 1.0f;
}
} // namespace

void AnalogVuMeter::resized()
{
    // Aspect-locked dial with a wide hardware-style sweep (150° total).
    // Pivot at the bottom-centre of a tight safe area; arcRadius is solved
    // from the sweep angle so the extreme labels just clear the safe edges
    // horizontally and the arc top clears the safe top. Always circular —
    // single scalar radius driving every element.
    const auto safe = getLocalBounds().toFloat().reduced (6.0f, 4.0f);
    pivot.x = safe.getCentreX();
    pivot.y = safe.getBottom() - 4.0f;
    // Shallow arc - matches Sifam/TEAC hardware VU geometry where the
    // arc occupies a thin band near the top of the face and most of
    // the visible area below the arc is empty (mechanical galvo).
    // 55-60° half-sweep gives the right vertical depth = r * (1 - cos)
    // ≈ 35-50 % of the radius. Tighter sweep on the master so its
    // numbered labels keep their breathing room.
    halfArcDeg = compactScale ? 60.0f : 55.0f;

    const float halfArcRad = juce::degreesToRadians (halfArcDeg);
    const float sinH = std::sin (halfArcRad);
    // Label margin reserved past arc tips. Numeric labels need ~14 px;
    // compact-mode "-" / "+" endpoint glyphs need only ~4 px. Wider
    // labels are also suppressed under kLabelMinWidth on the master
    // face for narrow strips.
    const float lblMargin = compactScale
                              ? 4.0f
                              : (getWidth() >= kLabelMinWidth ? 14.0f : 2.0f);
    const float rByWidth  = (safe.getWidth() * 0.5f - lblMargin)
                             / juce::jmax (0.001f, sinH);
    // The arc's TOP (angleFrac=0) is exactly arcRadius above pivot - it
    // is NOT divided by cosH. Earlier division by cosH let the dial
    // extend past the top edge whenever the face was short relative to
    // its width. Reserve a band at the top: bigger when the face draws
    // numbered labels (master), smaller for the compact "−" / "+" face
    // on bus strips.
    const float topHeadroom = compactScale ? 6.0f : 14.0f;
    const float rByHeight   = juce::jmax (1.0f, safe.getHeight() - topHeadroom);
    arcRadius = juce::jmax (10.0f, juce::jmin (rByWidth, rByHeight));
    rebuildCachedFace();
}

void AnalogVuMeter::timerCallback()
{
    if (leftRms == nullptr) return;

    const float dt      = 1.0f / kRefreshHz;
    const float vuCoeff = 1.0f - std::exp (-1000.0f * dt / kVuTimeConstantMs);
    const float critDamp = kOvershootDamping * 2.0f * std::sqrt (kOvershootStiff);

    // Returns clamped VU dB for the needle AND writes the pre-clamp value
    // into `outRawVuDb` so the PEAK LED can fire above the scale's visible
    // ceiling (the needle pegs at +3 long before the user actually wants
    // to see a peak warning).
    auto rmsToVuDb = [this] (const std::atomic<float>* atom, float& outRawVuDb)
    {
        const float rms = juce::jmax (1.0e-6f, atom->load (std::memory_order_relaxed));
        const float dbFs = 20.0f * std::log10 (rms);
        const float rawVu = dbFs - referenceDbFs;
        outRawVuDb = rawVu;
        return juce::jlimit (kVuMin, kVuMax, rawVu);
    };

    auto tickChannel = [&] (const std::atomic<float>* atom, float& pos, float& vel, float& rawVu)
    {
        const float target = vuDbToAngleFrac (rmsToVuDb (atom, rawVu));

        const float springForce  = (target - pos) * kOvershootStiff;
        const float dampingForce = -vel * critDamp;
        vel += (springForce + dampingForce) * dt;
        pos += vel * dt;
        pos += vuCoeff * (target - pos) * 0.3f;
        pos  = juce::jlimit (-1.0f, 1.0f, pos);

        if (std::abs (vel) < 0.001f && std::abs (target - pos) < 0.001f)
            vel = 0.0f;
    };

    float rawL = kVuMin, rawR = kVuMin;
    tickChannel (leftRms, needlePosL, needleVelL, rawL);
    if (rightRms != nullptr)
        tickChannel (rightRms, needlePosR, needleVelR, rawR);

    const float maxRaw = juce::jmax (rawL, rawR);
    if (maxRaw >= kPeakTriggerVuDb)
        peakHoldUntilMs = juce::Time::getMillisecondCounter() + (juce::uint32) kPeakHoldMs;

    // Re-bake the cache if the window moved to a different-DPI monitor so the
    // face stays crisp (resize already rebuilds for size changes).
    if (cachedFace.isValid() && std::abs (currentDisplayScale (*this) - lastScale) > 0.01f)
        rebuildCachedFace();

    repaint();
}

void AnalogVuMeter::rebuildCachedFace()
{
    const auto w = getWidth();
    const auto h = getHeight();
    if (w <= 0 || h <= 0)
    {
        cachedFace = juce::Image();
        return;
    }

    // Rasterise the cache at the display's physical resolution, then draw in
    // logical coords via a scale transform. paint() blits it back 1:1 so the
    // baked face (ticks, labels, gradients, glass) stays sharp on HiDPI.
    lastScale = currentDisplayScale (*this);
    const int pw = juce::jmax (1, juce::roundToInt ((float) w * lastScale));
    const int ph = juce::jmax (1, juce::roundToInt ((float) h * lastScale));
    cachedFace = juce::Image (juce::Image::ARGB, pw, ph, true);
    juce::Graphics g (cachedFace);
    g.addTransform (juce::AffineTransform::scale (lastScale));
    const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h);

    // Bezel + face. Two-tone outline + cream paper fill with a vertical
    // gradient so the face has a faint analog patina.
    const float corner = juce::jmin (3.0f, juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.06f);
    g.setColour (kBezelOuter);
    g.fillRoundedRectangle (bounds, corner);
    g.setColour (kBezelInner);
    g.fillRoundedRectangle (bounds.reduced (1.0f), corner * 0.85f);

    auto faceRect = bounds.reduced (2.0f);
    if (richStyle)
    {
        // Warm cream lit from just above the pivot: ivory core radiating out to
        // an aged-cream rim, then a second radial pass darkening the corners
        // (aging vignette). TEAC/Tascam look.
        juce::ColourGradient cream (kCreamCore, pivot.x, pivot.y - arcRadius * 0.35f,
                                     kCreamRim, faceRect.getX(), faceRect.getY(), true);
        g.setGradientFill (cream);
        g.fillRoundedRectangle (faceRect, corner * 0.7f);

        juce::ColourGradient vig (juce::Colours::transparentBlack,
                                   faceRect.getCentreX(), faceRect.getCentreY(),
                                   kVignette, faceRect.getX(), faceRect.getBottom(), true);
        g.setGradientFill (vig);
        g.fillRoundedRectangle (faceRect, corner * 0.7f);

        // Recessed inner shadow along the top/left so the face reads as set
        // behind the bezel lip.
        juce::ColourGradient innerTop (kBezelLo.withAlpha (0.45f), faceRect.getX(), faceRect.getY(),
                                        juce::Colours::transparentBlack,
                                        faceRect.getX(), faceRect.getY() + arcRadius * 0.18f, false);
        g.setGradientFill (innerTop);
        g.fillRoundedRectangle (faceRect, corner * 0.7f);
    }
    else
    {
        juce::ColourGradient grad (kFaceTop, faceRect.getX(), faceRect.getY(),
                                    kFaceBottom, faceRect.getX(), faceRect.getBottom(), false);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (faceRect, corner * 0.7f);
    }

    auto angleFracToScreenAngleRad = [this] (float angleFrac)
    {
        const float deg = angleFrac * halfArcDeg - 90.0f;
        return juce::degreesToRadians (deg);
    };
    auto pointOnArc = [&] (float angleFrac, float radius)
    {
        const float a = angleFracToScreenAngleRad (angleFrac);
        return juce::Point<float> (pivot.x + std::cos (a) * radius,
                                     pivot.y + std::sin (a) * radius);
    };

    const float jucePi = juce::MathConstants<float>::pi;

    // ALL element radii are fractions of the single arcRadius scalar so
    // the dial stays circular when the host strip changes aspect ratio.
    // Layout (smallest → largest from pivot):
    //   baselineRad   curved scale arc + bottom of every tick
    //   tick top       baselineRad + tickLenMaj  (numbered) / + tickLenMin (sub)
    //   labelRad       baselineRad + tickLenMaj + labelInset
    //
    // Compact-scale mode (bus VUs, Mixbus / Sifam-style): force-off
    // numeric labels + shrink ticks regardless of face width so the bus
    // face reads as the simpler member of the visual family. The
    // master VU keeps the full numbered scale.
    const bool  showLabels  = (! compactScale) && (w >= kLabelMinWidth);
    const float labelFont   = juce::jlimit (6.0f, 11.0f, arcRadius * 0.13f);
    const float tickLenMaj  = arcRadius * 0.12f;
    const float tickLenMin  = arcRadius * 0.06f;
    baselineRad             = arcRadius * 0.78f;

    // Curved baseline arc — one continuous black stroke spanning the full
    // sweep, with a red overlay over the 0..+3 segment. Drawing black
    // underneath gives the red a clean start without a rounded-end bulge
    // at the junction (which is what made the two-stroke version look
    // crooked at 0 VU).
    const float arcStrokeW = juce::jmax (1.0f, arcRadius * 0.025f);
    const float angleZero  = vuDbToAngleFrac (0.0f);
    {
        juce::Path arcBlack;
        arcBlack.addCentredArc (pivot.x, pivot.y, baselineRad, baselineRad, 0.0f,
                                  angleFracToScreenAngleRad (-1.0f) + jucePi * 0.5f,
                                  angleFracToScreenAngleRad ( 1.0f) + jucePi * 0.5f,
                                  true);
        g.setColour (juce::Colours::black);
        g.strokePath (arcBlack, juce::PathStrokeType (arcStrokeW,
                                                        juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::rounded));

        juce::Path arcRed;
        arcRed.addCentredArc (pivot.x, pivot.y, baselineRad, baselineRad, 0.0f,
                                angleFracToScreenAngleRad (angleZero) + jucePi * 0.5f,
                                angleFracToScreenAngleRad ( 1.0f)     + jucePi * 0.5f,
                                true);
        g.setColour (juce::Colours::red);
        g.strokePath (arcRed, juce::PathStrokeType (arcStrokeW,
                                                       juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
    }

    // Radial tick marks. Each tick line runs along the radius from the
    // pivot, sitting flush on the baseline arc and growing outward by
    // tickLenMaj (numbered) or tickLenMin (sub-tick). Drawn after the
    // arc so they paint on top of the baseline.
    for (const auto& m : kMarks)
    {
        const float af   = vuDbToAngleFrac (m.vuDb);
        const float len  = m.primary ? tickLenMaj : tickLenMin;
        const auto pIn   = pointOnArc (af, baselineRad);
        const auto pOut  = pointOnArc (af, baselineRad + len);

        g.setColour (m.red ? juce::Colours::red : juce::Colours::black);
        const float thickness = m.primary ? 1.5f : 1.0f;
        g.drawLine (juce::Line<float> (pIn, pOut), thickness);
    }

    // Numeric labels — upright sans-serif horizontally centred above the
    // tick outer end (Justification::centredBottom anchors the text just
    // above the tick). Suppressed under kLabelMinWidth.
    if (showLabels)
    {
        g.setFont (juce::Font (juce::FontOptions (labelFont, juce::Font::bold)));
        const float boxW = labelFont * 2.6f;
        const float boxH = labelFont * 1.25f;
        for (const auto& m : kMarks)
        {
            if (m.label == nullptr) continue;
            const float af = vuDbToAngleFrac (m.vuDb);
            const auto pTickTop = pointOnArc (af, baselineRad + tickLenMaj);
            const float textCx  = pTickTop.x;
            const float textBaseY = pTickTop.y - 1.5f;
            g.setColour (m.red ? juce::Colours::red : juce::Colours::black);
            g.drawText (juce::String (m.label),
                         juce::Rectangle<float> (textCx - boxW * 0.5f,
                                                    textBaseY - boxH,
                                                    boxW, boxH),
                         juce::Justification::centredBottom, false);
        }
    }

    // Compact-scale (bus VU) endpoint glyphs: "-" at the left endstop and
    // "+" at the right. Mixbus / Sifam meters use these as a minimal
    // sense-of-direction cue in place of full numbered scales.
    if (compactScale)
    {
        const float glyphFont = juce::jlimit (7.0f, 12.0f, arcRadius * 0.18f);
        g.setFont (juce::Font (juce::FontOptions (glyphFont, juce::Font::bold)));
        const float boxW = glyphFont * 1.6f;
        const float boxH = glyphFont * 1.2f;
        auto drawEnd = [&] (float af, const juce::String& glyph, juce::Colour col)
        {
            const auto pTickTop = pointOnArc (af, baselineRad + tickLenMaj);
            g.setColour (col);
            g.drawText (glyph,
                         juce::Rectangle<float> (pTickTop.x - boxW * 0.5f,
                                                    pTickTop.y - boxH,
                                                    boxW, boxH),
                         juce::Justification::centredBottom, false);
        };
        // U+2212 MINUS SIGN. Must construct via CharPointer_UTF8 - juce::String's
        // const char* ctor uses the system locale, which mangles multi-byte
        // UTF-8 into Latin-1 garbage on Linux ("â\x86" instead of "−").
        drawEnd (-1.0f, juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92")),
                 juce::Colours::black);
        drawEnd ( 1.0f, juce::String ("+"), juce::Colours::red);
    }

    // Semicircular pivot hub centred exactly on (pivot.x, pivot.y) — the
    // same point every other element radiates from. Top half of a circle
    // of radius arcRadius * 0.10, so it scales with the dial without
    // dominating narrow bus faces.
    {
        const float hubR = juce::jmax (4.0f, arcRadius * 0.10f);
        juce::Path hub;
        hub.startNewSubPath (pivot.x - hubR, pivot.y);
        hub.addArc (pivot.x - hubR, pivot.y - hubR,
                     hubR * 2.0f, hubR * 2.0f,
                     -jucePi * 0.5f, jucePi * 0.5f, false);
        hub.lineTo (pivot.x + hubR, pivot.y);
        hub.closeSubPath();
        if (richStyle)
        {
            // Chrome screw dome: radial steel highlight -> shadow.
            juce::ColourGradient chrome (kHubHi, pivot.x - hubR * 0.4f, pivot.y - hubR * 0.7f,
                                          kHubLo, pivot.x + hubR * 0.7f, pivot.y, true);
            chrome.addColour (0.55, kHubMid);
            g.setGradientFill (chrome);
            g.fillPath (hub);
            g.setColour (kHubLo);
            g.strokePath (hub, juce::PathStrokeType (0.8f));
        }
        else
        {
            g.setColour (kPivotHousing);
            g.fillPath (hub);
            g.setColour (juce::Colour (0xff3a3a40));
            g.strokePath (hub, juce::PathStrokeType (0.8f));
        }
    }

    // PEAK indicator — small LED in the upper-right corner of the face,
    // matching the SSL-style placement. Caption dropped since the dot is
    // self-explanatory in that corner and there's no clean room for text
    // without crowding the +3 label.
    {
        const float ledR = juce::jmax (2.0f, arcRadius * 0.05f);
        const float margin = juce::jmax (4.0f, arcRadius * 0.06f);
        const float ledCx = faceRect.getRight()  - margin - ledR;
        const float ledCy = faceRect.getY()      + margin + ledR;
        peakLedRect = juce::Rectangle<float> (ledCx - ledR, ledCy - ledR,
                                                ledR * 2.0f, ledR * 2.0f);

        // Faint recessed well so the LED reads as a physical hole even when
        // unlit (visible against the bright face).
        g.setColour (juce::Colour (0xff909094));
        g.fillEllipse (peakLedRect.expanded (1.0f));
        g.setColour (kPeakLedOff);
        g.fillEllipse (peakLedRect);
        g.setColour (juce::Colour (0xff201010));
        g.drawEllipse (peakLedRect, 0.5f);
    }

    // "VU" badge — plain sans-serif above the pivot hub. Suppressed on
    // narrow bus faces (same threshold as the numeric labels) where the
    // badge would collide with the hub and the +3 label.
    if (showLabels)
    {
        const float vuFont = juce::jlimit (10.0f, 18.0f, arcRadius * 0.18f);
        g.setFont (juce::Font (juce::FontOptions (vuFont, juce::Font::plain)));
        g.setColour (juce::Colours::black);
        const float vuBoxW = vuFont * 2.6f;
        const float vuBoxH = vuFont * 1.15f;
        const float hubR   = juce::jmax (4.0f, arcRadius * 0.10f);
        const float vuCx = pivot.x;
        const float vuCy = pivot.y - hubR - vuBoxH * 0.5f - arcRadius * 0.04f;
        g.drawText ("VU",
                     juce::Rectangle<float> (vuCx - vuBoxW * 0.5f,
                                                vuCy - vuBoxH * 0.5f,
                                                vuBoxW, vuBoxH),
                     juce::Justification::centred, false);

        // Rich master face adds the small "OUTPUT LEVEL" sub-caption under the
        // VU badge, matching the TEAC/Tascam face.
        if (richStyle)
        {
            const float subFont = juce::jlimit (5.0f, 9.0f, arcRadius * 0.085f);
            g.setFont (juce::Font (juce::FontOptions (subFont, juce::Font::plain)));
            g.setColour (kInkSoft);
            g.drawText ("OUTPUT LEVEL",
                         juce::Rectangle<float> (vuCx - vuBoxW,
                                                    vuCy + vuBoxH * 0.5f,
                                                    vuBoxW * 2.0f, subFont * 1.4f),
                         juce::Justification::centred, false);
        }
    }

    // Glass specular — drawn LAST so the printed scale reads as if under convex
    // glass. Rich face only; baked into the cache (zero per-frame cost).
    if (richStyle)
    {
        g.saveState();
        g.reduceClipRegion (faceRect.toNearestInt());
        juce::Path sweep;
        sweep.startNewSubPath (faceRect.getX(), faceRect.getY());
        sweep.lineTo (faceRect.getX() + faceRect.getWidth()  * 0.62f, faceRect.getY());
        sweep.lineTo (faceRect.getX(), faceRect.getY() + faceRect.getHeight() * 0.62f);
        sweep.closeSubPath();
        juce::ColourGradient sheen (kGlassSheen.withAlpha (0.18f), faceRect.getX(), faceRect.getY(),
                                     juce::Colours::transparentBlack,
                                     faceRect.getCentreX(), faceRect.getCentreY(), false);
        g.setGradientFill (sheen);
        g.fillPath (sweep);
        g.setColour (kGlassSheen.withAlpha (0.12f));   // top meniscus streak
        g.drawLine (faceRect.getX() + corner, faceRect.getY() + 1.2f,
                     faceRect.getRight() - corner, faceRect.getY() + 1.2f, 1.0f);
        g.restoreState();
    }
}

void AnalogVuMeter::paint (juce::Graphics& g)
{
    // Blit the physical-resolution cache back to logical bounds so it stays crisp on
    // HiDPI. Derive the inverse scale from the cache's ACTUAL pixel dimensions vs the
    // logical size — the cache size was rounded to integer px in rebuildCachedFace, so
    // 1/lastScale would be slightly off at fractional DPI and soften the blit.
    if (cachedFace.isValid() && getWidth() > 0 && getHeight() > 0)
        g.drawImageTransformed (cachedFace, juce::AffineTransform::scale (
            (float) getWidth()  / (float) cachedFace.getWidth(),
            (float) getHeight() / (float) cachedFace.getHeight()));

    auto drawNeedle = [&] (float angleFrac, juce::Colour c, float thickness)
    {
        const float deg = angleFrac * halfArcDeg - 90.0f;
        const float rad  = juce::degreesToRadians (deg);
        // Needle starts EXACTLY at the pivot and radiates outward to the
        // baseline. No baseR offset — using a non-zero base distance made
        // the line look like it continued past the pivot when the segment
        // was extrapolated visually by the eye through the hub.
        const float tipR  = baselineRad - arcRadius * 0.02f;
        const auto base = pivot;
        const auto tip  = juce::Point<float> (pivot.x + std::cos (rad) * tipR,
                                                 pivot.y + std::sin (rad) * tipR);
        g.setColour (c);
        g.drawLine (juce::Line<float> (base, tip), thickness);
    };

    // Rich needle: a tapered blade (wide at the hub, fine point at the tip)
    // with a soft offset shadow so it reads as a physical pointer above the
    // face. Built into the hoisted needlePath (clear()'d each frame) and
    // rotated with a single AffineTransform — allocation- and trig-per-point-
    // free. Canonical needle points straight up (angleFrac 0); rotate by the
    // angle so the tip lands on the scale.
    auto drawRichNeedle = [&] (float angleFrac, juce::Colour c)
    {
        const float tipR  = baselineRad - arcRadius * 0.02f;
        const float halfW = juce::jmax (1.1f, arcRadius * 0.020f);
        const float px = pivot.x, py = pivot.y;
        needlePath.clear();
        needlePath.startNewSubPath (px, py - tipR);   // fine tip
        needlePath.lineTo (px + halfW, py);           // base right
        needlePath.lineTo (px - halfW, py);           // base left
        needlePath.closeSubPath();
        const auto rot = juce::AffineTransform::rotation (
            juce::degreesToRadians (angleFrac * halfArcDeg), px, py);
        g.setColour (juce::Colour (0x33000000));      // soft shadow
        g.fillPath (needlePath, rot.followedBy (juce::AffineTransform::translation (1.0f, 1.5f)));
        g.setColour (c);                              // body
        g.fillPath (needlePath, rot);
    };

    // Right needle slightly behind the left one (drawn first) so a centred-
    // pan stereo signal shows the mono read-out cleanly with the L on top.
    if (richStyle)
    {
        if (rightRms != nullptr) drawRichNeedle (needlePosR, kNeedleR);
        drawRichNeedle (needlePosL, kNeedle);

        // Chrome hub cap over the needle bases (dynamic counterpart of the
        // baked dome) so the bases tuck under a polished pivot.
        const float capR = juce::jmax (3.0f, arcRadius * 0.085f);
        juce::ColourGradient capG (kHubHi, pivot.x - capR * 0.4f, pivot.y - capR * 0.5f,
                                    kHubLo, pivot.x + capR * 0.5f, pivot.y, true);
        capG.addColour (0.55, kHubMid);
        g.setGradientFill (capG);
        g.fillEllipse (pivot.x - capR, pivot.y - capR, capR * 2.0f, capR * 2.0f);
        g.setColour (kHubLo);
        g.drawEllipse (pivot.x - capR, pivot.y - capR, capR * 2.0f, capR * 2.0f, 0.7f);
        g.setColour (kGlassSheen.withAlpha (0.6f));   // specular dot
        g.fillEllipse (pivot.x - capR * 0.35f, pivot.y - capR * 0.55f, capR * 0.5f, capR * 0.5f);
    }
    else
    {
        if (rightRms != nullptr)
            drawNeedle (needlePosR, kNeedleR, 1.2f);
        drawNeedle (needlePosL, kNeedle, 1.4f);
    }

    // PEAK LED overlay - lit while the hold timer is still in the future.
    // Wrap-safe compare: juce::Time::getMillisecondCounter wraps every ~49
    // days; a plain `now < peakHoldUntilMs` would briefly lie when the
    // deadline straddles the wrap. Casting the diff to int32 gives a
    // signed remaining-ms that handles the wrap correctly.
    if (peakHoldUntilMs != 0 && ! peakLedRect.isEmpty())
    {
        const auto now = juce::Time::getMillisecondCounter();
        if ((juce::int32) (peakHoldUntilMs - now) > 0)
        {
            g.setColour (kPeakLedOn);
            g.fillEllipse (peakLedRect);
            // Soft glow so the lit state pops against the cream face.
            g.setColour (kPeakLedOn.withAlpha (0.35f));
            g.fillEllipse (peakLedRect.expanded (peakLedRect.getWidth() * 0.25f));
        }
    }
}
} // namespace duskstudio
