#include "MasteringEqView.h"
#include "DuskTheme.h"
#include "../../dsp/MasteringChain.h"
#include "../../foundation/Decibels.h"
#include "../../foundation/Fft.h"
#include "../../session/Session.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace duskstudio::imgui
{
namespace
{
namespace dw = DuskWidgets;

// MasteringEqEditor's geometry and palette, in design pixels.
constexpr float kOuterInset = 8.0f;
constexpr float kHeaderH = 20.0f;
constexpr float kHeaderGap = 4.0f;
constexpr float kCurveGap = 4.0f;
constexpr float kBandLabelH = 14.0f;
constexpr float kMinPanelW = 300.0f;
constexpr float kMinPanelH = 340.0f;

constexpr float kFreqMinHz = 20.0f;
constexpr float kFreqMaxHz = 20000.0f;
constexpr float kCurveDbRange = 18.0f;

// Spectrum overlay vertical scale (dBFS). The whole plot height maps this range; the
// EQ curve sits on top in its own scale.
constexpr float kSpecFloorDb = -96.0f;
constexpr float kSpecTopDb = 6.0f;

constexpr int kFftOrder = 11;
constexpr int kFftSize = 1 << kFftOrder;
constexpr int kNumBins = kFftSize / 2;

// One spectrum tick per JUCE timer tick, so the peak-attack / slow-release ballistics
// below land on the same shape they did at 30 Hz.
constexpr float kSpectrumInterval = 1.0f / 30.0f;

constexpr int kCurvePoints = 220;

constexpr unsigned int kPanelFill = 0x20202aff;
constexpr unsigned int kPanelBorder = 0x3a3a46ff;
constexpr unsigned int kPlotFill = 0x0d0d11ff;
constexpr unsigned int kGridLine = 0x20202aff;
constexpr unsigned int kZeroLine = 0x404048ff;
constexpr unsigned int kDecadeText = 0x606068ff;
constexpr unsigned int kScaleText = 0x909094ff;
constexpr unsigned int kBypassText = 0x707074ff;
constexpr unsigned int kSpecFill = 0x39c0c829;
constexpr unsigned int kSpecLine = 0x5fd6dd73;
constexpr unsigned int kTotalFill = 0x5a8ad02e;
constexpr unsigned int kTotalLine = 0x8aafeeff;
constexpr unsigned int kHeaderAccent = 0x5fa8d0ff;
constexpr unsigned int kSpecOnFill = 0x2a4a60ff;
constexpr unsigned int kSpecOnText = 0x8aceeeff;
constexpr unsigned int kSpecOffFill = 0x202028ff;
constexpr unsigned int kSpecOffText = 0x707078ff;
constexpr unsigned int kDotOutline = 0x0a0a0aff;
constexpr unsigned int kBadgeFill = 0x0d0d11ff;
constexpr unsigned int kBadgeText = 0xf0f0f0ff;

// Same hue family as the per-channel-strip EQ colours, so the eye-mapping carries
// between mixing and mastering.
constexpr unsigned int kBandColours[5] = {
    0x60c060ff,   // low shelf  - green
    0xe0c050ff,   // low mid    - amber
    0xe09050ff,   // mid        - orange
    0xd07070ff,   // high mid   - rose
    0x5a8ad0ff,   // high shelf - blue
};
const char* const kBandNames[5] = { "Low", "Lo-Mid", "Mid", "Hi-Mid", "High" };

// Overlapping ranges so a band can be tuned either side of its nominal centre, which
// is typical for a mastering EQ.
struct BandRange { float minHz, maxHz, defaultHz; };
constexpr BandRange kBandRanges[5] = {
    {   20.0f,   400.0f,    50.0f },
    {   60.0f,  1500.0f,   250.0f },
    {  200.0f,  6000.0f,  1000.0f },
    {  800.0f, 12000.0f,  4000.0f },
    { 2000.0f, 20000.0f, 12000.0f },
};

ImU32 rgba (unsigned int hex)
{
    return IM_COL32 ((hex >> 24) & 0xff, (hex >> 16) & 0xff, (hex >> 8) & 0xff, hex & 0xff);
}

float clampf (float lo, float hi, float value)
{
    return std::clamp (value, lo, std::max (lo, hi));
}

bool isShelfBand (int index) { return index == 0 || index == 4; }

// The compact readout the JUCE frequency knob's text box carried: 12000 reads "12k"
// rather than "12.0k", and anything under a kilohertz is a bare integer.
void formatBandFrequency (char* out, std::size_t size, float hz)
{
    if (hz >= 10000.0f)
        std::snprintf (out, size, "%dk", static_cast<int> (std::lround (hz / 1000.0f)));
    else if (hz >= 1000.0f)
        std::snprintf (out, size, "%.1fk", hz / 1000.0f);
    else
        std::snprintf (out, size, "%d", static_cast<int> (std::lround (hz)));
}

class MasteringEqViewImpl final : public DuskPanelView
{
public:
    MasteringEqViewImpl (MasteringParams& p, MasteringChain* c) : params (p), chain (c)
    {
        specDb.fill (kSpecFloorDb);
        dusk::audio::Fft::fillHannWindow (fftWindow.data(), kFftSize);
        rebuildBandCache();
    }

    ImVec2 preferredSize() const override { return ImVec2 (kMinPanelW, kMinPanelH); }
    bool wantsPlate() const override { return false; }

    void draw (dw::Context& ctx, ImVec2 origin, ImVec2 size) override
    {
        const ImVec2 br (origin.x + size.x, origin.y + size.y);
        ctx.dl->AddRectFilled (origin, br, rgba (kPanelFill));
        ctx.dl->AddRect (ImVec2 (origin.x + 0.5f, origin.y + 0.5f),
                         ImVec2 (br.x - 0.5f, br.y - 0.5f), rgba (kPanelBorder), 0.0f, 0,
                         ctx.s (1.0f));

        tickSpectrum();
        // The curve is evaluated from the cached values rather than from the atomics, so
        // a drag and the audio path cannot disagree about which shape is on screen.
        if (rebuildBandCache())
            curvesDirty = true;

        const float inset = ctx.s (kOuterInset);
        const float left = origin.x + inset;
        const float right = br.x - inset;
        const float width = right - left;
        if (width <= ctx.s (40.0f))
            return;

        float y = origin.y + inset;
        drawHeader (ctx, ImVec2 (left, y), width);
        y += ctx.s (kHeaderH) + ctx.s (kHeaderGap);

        // The curve takes what the controls row leaves. Its share is the JUCE editor's:
        // 48% of the remaining height, held between 130 and 210 design pixels.
        const float remaining = (br.y - inset) - y;
        const float controlsH = clampf (ctx.s (130.0f), ctx.s (210.0f), remaining * 0.48f);
        const float curveH = remaining - controlsH - ctx.s (kCurveGap);
        if (curveH <= ctx.s (40.0f))
            return;

        const Rect wanted { left + ctx.s (1.0f), y + ctx.s (1.0f), right - ctx.s (1.0f),
                            y + curveH - ctx.s (1.0f) };
        // Both point caches are in screen coordinates, so a panel that moved or resized
        // invalidates them as surely as a band that changed.
        if (! sameRect (wanted, plot))
        {
            plot = wanted;
            curvesDirty = true;
            spectrumDirty = true;
        }
        drawCurve (ctx);
        drawBandControls (ctx, ImVec2 (left, y + curveH + ctx.s (kCurveGap)),
                          ImVec2 (right, br.y - inset));
    }

private:
    struct Rect { float x0, y0, x1, y1; };

    static bool sameRect (const Rect& a, const Rect& b)
    {
        const auto same = [] (float l, float r) { return ! (l < r) && ! (r < l); };
        return same (a.x0, b.x0) && same (a.y0, b.y0) && same (a.x1, b.x1)
            && same (a.y1, b.y1);
    }

    // True when anything the curve is drawn from moved. The response is 1,100 biquad
    // magnitude evaluations, so it is rebuilt on a change rather than every frame.
    bool rebuildBandCache()
    {
        bool changed = false;
        const auto take = [&changed] (float& cached, float value)
        {
            if (cached < value || value < cached)
            {
                cached = value;
                changed = true;
            }
        };

        for (int i = 0; i < 5; ++i)
        {
            take (freqHz[i], params.eqBandFreq[i].load (std::memory_order_relaxed));
            take (gainDb[i], params.eqBandGainDb[i].load (std::memory_order_relaxed));
            take (q[i], params.eqBandQ[i].load (std::memory_order_relaxed));
        }

        const bool nowEnabled = params.eqEnabled.load (std::memory_order_relaxed);
        if (nowEnabled != enabled)
        {
            enabled = nowEnabled;
            changed = true;
        }

        // The response is evaluated at the chain's scope rate, so a device-rate switch
        // with no parameter change still leaves the drawn curve stale.
        const double rate = chain != nullptr ? chain->getScopeSampleRate() : 0.0;
        if (rate < scopeSampleRate || scopeSampleRate < rate)
        {
            scopeSampleRate = rate;
            changed = true;
        }
        return changed;
    }

    void tickSpectrum()
    {
        spectrumClock += ImGui::GetIO().DeltaTime;
        if (spectrumClock < kSpectrumInterval)
            return;
        spectrumClock = 0.0f;

        if (! showSpectrum || chain == nullptr)
            return;
        if (chain->readScopeLatest (fftScratch.data(), kFftSize) < kFftSize)
            return;   // not enough audio buffered yet
        spectrumDirty = true;

        std::copy (fftScratch.begin(), fftScratch.end(), fftWork.begin());
        for (int i = 0; i < kFftSize; ++i)
            fftWork[static_cast<std::size_t> (i)] *= fftWindow[static_cast<std::size_t> (i)];

        static_assert (kNumBins <= kFftSize / 2 + 1, "spectrum reads past the magnitude bins");
        fft.performFrequencyOnlyForwardTransform (fftWork.data());

        // 4/N: single-sided scaling (2/N) times the 2x that compensates the Hann
        // window's 0.5 coherent gain, so a bin-centred 0 dBFS sine reads near 0 dBFS.
        constexpr float kRef = 4.0f / static_cast<float> (kFftSize);
        for (int i = 0; i < kNumBins; ++i)
        {
            const float db = clampf (kSpecFloorDb, kSpecTopDb,
                                     dusk::audio::gainToDecibels (
                                         fftWork[static_cast<std::size_t> (i)] * kRef,
                                         kSpecFloorDb));
            float& smoothed = specDb[static_cast<std::size_t> (i)];
            smoothed = db > smoothed ? db : smoothed + (db - smoothed) * 0.25f;
        }
    }

    float bandResponseDb (int index, float atHz) const
    {
        // The DSP runs the biquads oversampled, so the curve is evaluated at the same
        // oversampled rate: that is the de-crammed response the audio actually applies.
        double base = chain != nullptr ? chain->getScopeSampleRate() : 48000.0;
        if (! (base > 0.0) || ! std::isfinite (base))
            base = 48000.0;
        return MasteringDigitalEq::magnitudeDb (
            index, base * MasteringDigitalEq::kOversample, freqHz[index], q[index],
            gainDb[index], atHz);
    }

    float dbToY (float db) const
    {
        const float midY = (plot.y0 + plot.y1) * 0.5f;
        return midY - (db / kCurveDbRange) * (plot.y1 - plot.y0) * 0.5f;
    }

    float yToDb (float atY) const
    {
        const float midY = (plot.y0 + plot.y1) * 0.5f;
        const float halfH = (plot.y1 - plot.y0) * 0.5f;
        if (halfH <= 0.0f)
            return 0.0f;
        return clampf (-kCurveDbRange, kCurveDbRange, (midY - atY) / halfH * kCurveDbRange);
    }

    float freqToX (float hz) const
    {
        const float frac = (std::log10 (hz) - std::log10 (kFreqMinHz))
                         / (std::log10 (kFreqMaxHz) - std::log10 (kFreqMinHz));
        return plot.x0 + frac * (plot.x1 - plot.x0);
    }

    float xToFreq (float atX) const
    {
        const float width = plot.x1 - plot.x0;
        if (width <= 0.0f)
            return kFreqMinHz;
        const float frac = clampf (0.0f, 1.0f, (atX - plot.x0) / width);
        return std::pow (10.0f, std::log10 (kFreqMinHz)
                                    + frac * (std::log10 (kFreqMaxHz) - std::log10 (kFreqMinHz)));
    }

    float dotY (int band) const
    {
        float total = 0.0f;
        for (int i = 0; i < 5; ++i)
            total += bandResponseDb (i, freqHz[band]);
        return clampf (plot.y0, plot.y1, dbToY (total));
    }

    int hitTestBandDot (ImVec2 at) const
    {
        constexpr float kHitRadius = 14.0f;
        int best = -1;
        float bestDistance = kHitRadius;
        for (int b = 0; b < 5; ++b)
        {
            const float dx = at.x - freqToX (freqHz[b]);
            const float dy = at.y - dotY (b);
            const float distance = std::sqrt (dx * dx + dy * dy);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = b;
            }
        }
        return best;
    }

    void drawHeader (dw::Context& ctx, ImVec2 at, float width)
    {
        const auto pill = dw::modulePill (ctx, "##mastering-eq-header", at,
                                          ImVec2 (at.x + width, at.y + ctx.s (kHeaderH)),
                                          "MASTERING EQ", enabled, rgba (kHeaderAccent));
        if (pill.toggled || pill.labelClicked)
        {
            enabled = ! enabled;
            params.eqEnabled.store (enabled, std::memory_order_relaxed);
        }
    }

    void drawCurve (dw::Context& ctx)
    {
        auto& dl = *ctx.dl;
        const float midY = (plot.y0 + plot.y1) * 0.5f;

        dl.AddRectFilled (ImVec2 (plot.x0, plot.y0), ImVec2 (plot.x1, plot.y1),
                          rgba (kPlotFill), ctx.s (3.0f));

        for (float db : { -12.0f, -6.0f, 6.0f, 12.0f })
            dl.AddLine (ImVec2 (plot.x0, dbToY (db)), ImVec2 (plot.x1, dbToY (db)),
                        rgba (kGridLine), ctx.s (1.0f));
        dl.AddLine (ImVec2 (plot.x0, midY), ImVec2 (plot.x1, midY), rgba (kZeroLine),
                    ctx.s (1.0f));

        for (float hz : { 100.0f, 1000.0f, 10000.0f })
            dl.AddLine (ImVec2 (freqToX (hz), plot.y0), ImVec2 (freqToX (hz), plot.y1),
                        rgba (kGridLine), ctx.s (1.0f));

        const float decadeSize = ctx.s (9.0f);
        for (const auto& entry : { std::pair<float, const char*> { 100.0f, "100" },
                                   std::pair<float, const char*> { 1000.0f, "1k" },
                                   std::pair<float, const char*> { 10000.0f, "10k" } })
            dw::text (ctx, ctx.fonts->caption, decadeSize,
                      ImVec2 (freqToX (entry.first) - ctx.s (18.0f),
                              plot.y1 - ctx.s (11.0f)),
                      ctx.s (36.0f), rgba (kDecadeText), entry.second);

        if (showSpectrum)
            drawSpectrum (ctx);

        drawResponse (ctx, midY);
        drawScaleLabels (ctx);

        if (! enabled)
            dw::text (ctx, ctx.fonts->pill, ctx.s (10.0f),
                      ImVec2 (plot.x0, plot.y0 + ctx.s (6.0f)), plot.x1 - plot.x0,
                      rgba (kBypassText), "BYPASS");

        // The toggle is submitted before the plot's own hit area: Dear ImGui gives an
        // overlap to whichever item was submitted first, so the other order would let
        // the plot swallow every click on the button sitting inside it.
        drawSpectrumToggle (ctx);
        handleDrag (ctx);
        drawBandDots (ctx);
    }

    void drawSpectrum (dw::Context& ctx)
    {
        const double sampleRate = chain != nullptr ? chain->getScopeSampleRate() : 48000.0;
        if (! (sampleRate > 0.0))
            return;

        const float binHz = static_cast<float> (sampleRate) / static_cast<float> (kFftSize);
        const float plotH = plot.y1 - plot.y0;
        const auto specToY = [&] (float db)
        {
            const float frac = clampf (0.0f, 1.0f,
                                       (db - kSpecFloorDb) / (kSpecTopDb - kSpecFloorDb));
            return plot.y1 - frac * plotH;
        };

        if (spectrumDirty)
        {
            spectrumDirty = false;
            spectrumPoints.clear();
            for (int i = 1; i < kNumBins; ++i)
            {
                const float hz = static_cast<float> (i) * binHz;
                if (hz < kFreqMinHz)
                    continue;
                if (hz > kFreqMaxHz)
                    break;
                spectrumPoints.push_back (
                    ImVec2 (freqToX (hz), specToY (specDb[static_cast<std::size_t> (i)])));
            }
        }
        if (spectrumPoints.size() < 2)
            return;

        // A column per segment rather than one filled path: the contour is not convex,
        // and Dear ImGui's filled-polygon path assumes it is.
        auto& dl = *ctx.dl;
        for (std::size_t i = 1; i < spectrumPoints.size(); ++i)
            dl.AddRectFilled (ImVec2 (spectrumPoints[i - 1].x, spectrumPoints[i].y),
                              ImVec2 (spectrumPoints[i].x + ctx.s (0.5f), plot.y1),
                              rgba (kSpecFill));
        dl.AddPolyline (spectrumPoints.data(), static_cast<int> (spectrumPoints.size()),
                        rgba (kSpecLine), 0, ctx.s (1.0f));
    }

    void rebuildCurves()
    {
        for (int i = 0; i < kCurvePoints; ++i)
        {
            const float t = static_cast<float> (i) / static_cast<float> (kCurvePoints - 1);
            const float hz = std::pow (10.0f,
                                       std::log10 (kFreqMinHz)
                                           + t * (std::log10 (kFreqMaxHz)
                                                  - std::log10 (kFreqMinHz)));
            const float x = freqToX (hz);

            float total = 0.0f;
            for (int b = 0; b < 5; ++b)
            {
                const float db = bandResponseDb (b, hz);
                total += db;
                bandCurves[static_cast<std::size_t> (b)][static_cast<std::size_t> (i)] =
                    ImVec2 (x, clampf (plot.y0, plot.y1, dbToY (db)));
            }
            totalCurve[static_cast<std::size_t> (i)] =
                ImVec2 (x, clampf (plot.y0, plot.y1, dbToY (total)));
        }
    }

    void drawResponse (dw::Context& ctx, float midY)
    {
        if (curvesDirty)
        {
            curvesDirty = false;
            rebuildCurves();
        }

        auto& dl = *ctx.dl;
        // Drawn whether or not the EQ is engaged, dimmed when it is not, so a shape can
        // be dialled in and watched before the section is switched on.
        const float alpha = enabled ? 1.0f : 0.35f;

        for (int b = 0; b < 5; ++b)
            dl.AddPolyline (bandCurves[static_cast<std::size_t> (b)].data(), kCurvePoints,
                            dw::withAlpha (rgba (kBandColours[b]), 0.30f * alpha), 0,
                            ctx.s (1.2f));

        // A column per segment rather than one filled path, for the same reason the
        // spectrum uses one: the area under the response is not a convex polygon.
        const ImU32 fill = dw::withAlpha (rgba (kTotalFill), alpha);
        for (int i = 1; i < kCurvePoints; ++i)
        {
            const auto& point = totalCurve[static_cast<std::size_t> (i)];
            dl.AddRectFilled (ImVec2 (totalCurve[static_cast<std::size_t> (i - 1)].x,
                                      std::min (midY, point.y)),
                              ImVec2 (point.x + ctx.s (0.5f), std::max (midY, point.y)), fill);
        }
        dl.AddPolyline (totalCurve.data(), kCurvePoints,
                        dw::withAlpha (rgba (kTotalLine), alpha), 0, ctx.s (1.6f));
    }

    void drawScaleLabels (dw::Context& ctx)
    {
        const float size = ctx.s (9.5f);
        for (float db : { 12.0f, 6.0f, 0.0f, -6.0f, -12.0f })
        {
            char label[8];
            std::snprintf (label, sizeof label, "%s%d", db > 0.0f ? "+" : "",
                           static_cast<int> (db));
            dw::text (ctx, ctx.fonts->caption, size,
                      ImVec2 (plot.x1 - ctx.s (30.0f), dbToY (db) - size * 0.5f),
                      ctx.s (28.0f), rgba (kScaleText), label, dw::Align::right);
        }
    }

    void handleDrag (dw::Context& ctx)
    {
        const bool overPlot = dw::hitArea (ctx, "##mastering-eq-plot",
                                           ImVec2 (plot.x0, plot.y0),
                                           ImVec2 (plot.x1, plot.y1));
        const ImVec2 mouse = ImGui::GetIO().MousePos;

        if (overPlot && ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left))
        {
            const int band = hitTestBandDot (mouse);
            if (band >= 0)
            {
                params.eqBandGainDb[band].store (0.0f, std::memory_order_relaxed);
                gainDb[band] = 0.0f;
            }
            draggingBand = -1;
            return;
        }

        if (! ImGui::IsItemActive())
        {
            draggingBand = -1;
            return;
        }
        if (ImGui::IsItemActivated())
            draggingBand = hitTestBandDot (mouse);
        if (draggingBand < 0)
            return;

        const int band = draggingBand;
        const float gain = clampf (-12.0f, 12.0f, yToDb (mouse.y));
        params.eqBandGainDb[band].store (gain, std::memory_order_relaxed);
        gainDb[band] = gain;

        // Clamped to the band's own range so a drag cannot teleport bands out of order.
        const float hz = clampf (kBandRanges[band].minHz, kBandRanges[band].maxHz,
                                 xToFreq (mouse.x));
        params.eqBandFreq[band].store (hz, std::memory_order_relaxed);
        freqHz[band] = hz;
    }

    void drawBandDots (dw::Context& ctx)
    {
        auto& dl = *ctx.dl;
        const float radius = ctx.s (8.0f);

        for (int b = 0; b < 5; ++b)
        {
            const ImVec2 centre (freqToX (freqHz[b]), dotY (b));
            const bool dragging = b == draggingBand;
            const ImU32 fill = dw::withAlpha (rgba (kBandColours[b]), enabled ? 0.95f : 0.55f);

            if (dragging)
                dl.AddCircleFilled (centre, radius + ctx.s (4.0f),
                                    dw::withAlpha (rgba (kBandColours[b]), 0.30f), 20);

            dl.AddCircleFilled (centre, radius, fill, 20);
            dl.AddCircle (centre, radius, rgba (kDotOutline), 20,
                          dragging ? ctx.s (1.6f) : ctx.s (1.0f));

            char number[4];
            std::snprintf (number, sizeof number, "%d", b + 1);
            const float numberSize = ctx.s (10.5f);
            dw::text (ctx, ctx.fonts->pill, numberSize,
                      ImVec2 (centre.x - radius, centre.y - numberSize * 0.5f), radius * 2.0f,
                      rgba (kDotOutline), number);

            if (! dragging)
                continue;

            const ImVec2 badgeTl (centre.x - ctx.s (28.0f), centre.y - radius - ctx.s (22.0f));
            const ImVec2 badgeBr (badgeTl.x + ctx.s (56.0f), badgeTl.y + ctx.s (16.0f));
            dl.AddRectFilled (badgeTl, badgeBr, rgba (kBadgeFill), ctx.s (3.0f));
            dl.AddRect (badgeTl, badgeBr, dw::withAlpha (rgba (kBandColours[b]), 0.85f),
                        ctx.s (3.0f), 0, ctx.s (0.8f));

            char readout[16];
            std::snprintf (readout, sizeof readout, "%+.1f dB",
                           static_cast<double> (gainDb[b]));
            const float readoutSize = ctx.s (10.5f);
            dw::text (ctx, ctx.fonts->pill, readoutSize,
                      ImVec2 (badgeTl.x, badgeTl.y + (ctx.s (16.0f) - readoutSize) * 0.5f),
                      badgeBr.x - badgeTl.x, rgba (kBadgeText), readout);
        }
    }

    void drawSpectrumToggle (dw::Context& ctx)
    {
        dw::ButtonStyle style;
        style.offFill = rgba (kSpecOffFill);
        style.onFill = rgba (kSpecOnFill);
        style.offText = rgba (kSpecOffText);
        style.onText = rgba (kSpecOnText);
        style.fontSize = 9.0f;
        style.rounding = 2.0f;
        if (dw::textButton (ctx, "##mastering-eq-fft",
                            ImVec2 (plot.x0 + ctx.s (5.0f), plot.y0 + ctx.s (4.0f)),
                            ImVec2 (plot.x0 + ctx.s (39.0f), plot.y0 + ctx.s (19.0f)),
                            "FFT", showSpectrum, style).clicked)
            showSpectrum = ! showSpectrum;
    }

    void drawBandControls (dw::Context& ctx, ImVec2 tl, ImVec2 br)
    {
        const float columnW = (br.x - tl.x) / 5.0f;
        const float rowsTop = tl.y + ctx.s (kBandLabelH + 2.0f);
        // One dial size for every column, taken from the three-row bands, so the shelf
        // columns' two knobs are the same size as the rest rather than larger. The row
        // has to carry the value readout under the dial as well.
        const float knobRadius = clampf (ctx.s (15.0f), ctx.s (28.0f),
                                         ((br.y - rowsTop) / 3.0f - ctx.s (14.0f)) * 0.5f);

        for (int b = 0; b < 5; ++b)
        {
            const float columnX = tl.x + static_cast<float> (b) * columnW;
            const ImU32 accent = rgba (kBandColours[b]);
            dw::text (ctx, ctx.fonts->pill, ctx.s (12.5f), ImVec2 (columnX, tl.y), columnW,
                      dw::brighter (accent, 0.25f), kBandNames[b]);

            const int rows = isShelfBand (b) ? 2 : 3;
            const float rowH = (br.y - rowsTop) / static_cast<float> (rows);
            const float centreX = columnX + columnW * 0.5f;
            const float knobTop = rowsTop + knobRadius + ctx.s (2.0f);

            char id[32];
            char readout[16];
            dw::KnobStyle style;
            style.fill = accent;
            style.value = readout;

            formatBandFrequency (readout, sizeof readout, freqHz[b]);
            std::snprintf (id, sizeof id, "##eq-freq%d", b);
            const auto range = dw::Range::withMidPoint (kBandRanges[b].minHz,
                                                        kBandRanges[b].maxHz,
                                                        kBandRanges[b].defaultHz);
            auto result = dw::knob (ctx, id, ImVec2 (centreX, knobTop), knobRadius,
                                    freqHz[b], range, kBandRanges[b].defaultHz, style);
            if (result.changed)
            {
                freqHz[b] = clampf (kBandRanges[b].minHz, kBandRanges[b].maxHz, result.value);
                params.eqBandFreq[b].store (freqHz[b], std::memory_order_relaxed);
            }

            std::snprintf (readout, sizeof readout, "%.1f dB",
                           static_cast<double> (gainDb[b]));
            std::snprintf (id, sizeof id, "##eq-gain%d", b);
            result = dw::knob (ctx, id, ImVec2 (centreX, knobTop + rowH), knobRadius,
                               gainDb[b], dw::Range (-12.0f, 12.0f), 0.0f, style);
            if (result.changed)
            {
                gainDb[b] = clampf (-12.0f, 12.0f, result.value);
                params.eqBandGainDb[b].store (gainDb[b], std::memory_order_relaxed);
            }

            // Shelf bands hide their Q: the dial is not musically useful for the gentle
            // slopes mastering uses, though the response evaluator still reads it.
            if (isShelfBand (b))
                continue;

            std::snprintf (readout, sizeof readout, "%.2f", static_cast<double> (q[b]));
            std::snprintf (id, sizeof id, "##eq-q%d", b);
            result = dw::knob (ctx, id, ImVec2 (centreX, knobTop + rowH * 2.0f), knobRadius,
                               q[b], dw::Range::withMidPoint (0.3f, 6.0f, 1.0f), 1.0f, style);
            if (result.changed)
            {
                q[b] = clampf (0.3f, 6.0f, result.value);
                params.eqBandQ[b].store (q[b], std::memory_order_relaxed);
            }
        }
    }

    MasteringParams& params;
    MasteringChain* chain = nullptr;

    dusk::audio::Fft fft { kFftOrder };
    std::array<float, kFftSize> fftWindow {};
    std::array<float, kFftSize> fftScratch {};
    std::array<float, kFftSize * 2> fftWork {};
    std::array<float, kNumBins> specDb {};
    std::vector<ImVec2> spectrumPoints;
    std::array<std::array<ImVec2, kCurvePoints>, 5> bandCurves {};
    std::array<ImVec2, kCurvePoints> totalCurve {};

    Rect plot {};
    float freqHz[5] {};
    float gainDb[5] {};
    float q[5] {};
    double scopeSampleRate = 0.0;
    bool enabled = false;
    bool showSpectrum = true;
    bool curvesDirty = true;
    bool spectrumDirty = true;
    int draggingBand = -1;
    float spectrumClock = 0.0f;
};
} // namespace

std::unique_ptr<DuskPanelView> makeMasteringEqView (MasteringParams& params,
                                                    MasteringChain* chain)
{
    return std::unique_ptr<DuskPanelView> (new MasteringEqViewImpl (params, chain));
}
} // namespace duskstudio::imgui
