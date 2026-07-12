#include "DpAligner.h"

#include <juce_dsp/juce_dsp.h>
#include <cmath>

namespace duskstudio::dp
{
namespace
{
constexpr int kHop = 240;          // 5 ms at 48 kHz
constexpr int kFftOrder = 10;      // 1024-sample analysis window
constexpr int kDomExcludeFrames = 400;  // ~2 s at hop 240 / 48 kHz: dominance exclusion radius

// Decode an audio file to a mono float vector (channel-averaged). Returns
// empty on failure. Message-thread only.
std::vector<float> decodeMono (juce::AudioFormatManager& fm, const juce::File& f,
                               double& sampleRateOut)
{
    std::vector<float> out;
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (f));
    if (reader == nullptr) return out;
    const auto len = (std::int64_t) reader->lengthInSamples;
    const int  ch  = (int) reader->numChannels;
    if (len <= 0 || ch <= 0 || reader->sampleRate <= 0.0) return out;
    // Guard against absurd files eating all RAM. A stereo decode allocates
    // ~12 bytes/sample (the 2-ch buffer + the mono out), so this caps the
    // worst case near ~1 GB - generous for any real DP song (15 min @ 96 k /
    // 30 min @ 48 k) while refusing a pathological multi-hour file.
    if (len > 96000ll * 60ll * 15ll) return out;

    sampleRateOut = reader->sampleRate;
    // The AudioBuffer read overload only fills up to 2 channels (left/right
    // flags), so mix from at most the first two. DP fragments are mono and
    // mixdowns are stereo, so this covers every real input; for a >2-channel
    // file we mix L+R and ignore the rest rather than averaging in zeros.
    const int usedCh = juce::jmin (ch, 2);
    juce::AudioBuffer<float> buf (usedCh, (int) len);
    buf.clear();
    if (! reader->read (&buf, 0, (int) len, 0, true, usedCh > 1)) return out;

    out.resize ((size_t) len);
    if (usedCh == 1)
    {
        std::copy (buf.getReadPointer (0), buf.getReadPointer (0) + len, out.begin());
    }
    else
    {
        const float inv = 1.0f / (float) usedCh;
        for (std::int64_t i = 0; i < len; ++i)
        {
            float s = 0.0f;
            for (int c = 0; c < usedCh; ++c) s += buf.getSample (c, (int) i);
            out[(size_t) i] = s * inv;
        }
    }
    return out;
}
} // namespace

std::vector<float> onsetEnvelope (const float* x, int n, int hop, int fftOrder)
{
    const int fftSize = 1 << fftOrder;
    if (n < fftSize + hop) return {};
    // Frames whose full window fits: floor((n - fftSize) / hop) + 1.
    const int nfr = (n - fftSize + hop) / hop;
    if (nfr < 2) return {};

    juce::dsp::FFT fft (fftOrder);
    std::vector<float> window ((size_t) fftSize);
    for (int i = 0; i < fftSize; ++i)
        window[(size_t) i] = 0.5f * (1.0f - std::cos (2.0f * juce::MathConstants<float>::pi
                                                       * (float) i / (float) (fftSize - 1)));

    std::vector<float> fftbuf ((size_t) (2 * fftSize));
    std::vector<float> prevMag ((size_t) (fftSize / 2 + 1), 0.0f);
    std::vector<float> curMag  ((size_t) (fftSize / 2 + 1), 0.0f);
    std::vector<float> env ((size_t) nfr, 0.0f);

    for (int fr = 0; fr < nfr; ++fr)
    {
        const float* seg = x + fr * hop;
        std::fill (fftbuf.begin(), fftbuf.end(), 0.0f);
        for (int i = 0; i < fftSize; ++i)
            fftbuf[(size_t) i] = seg[i] * window[(size_t) i];

        fft.performRealOnlyForwardTransform (fftbuf.data(), true);

        float flux = 0.0f;
        for (int k = 0; k <= fftSize / 2; ++k)
        {
            const float re = fftbuf[(size_t) (2 * k)];
            const float im = fftbuf[(size_t) (2 * k + 1)];
            const float mag = std::sqrt (re * re + im * im);
            curMag[(size_t) k] = mag;
            if (fr > 0)
            {
                const float d = mag - prevMag[(size_t) k];
                if (d > 0.0f) flux += d;
            }
        }
        env[(size_t) fr] = flux;
        std::swap (prevMag, curMag);
    }

    // Normalise: zero-mean, unit-std.
    double mean = 0.0;
    for (float v : env) mean += v;
    mean /= (double) env.size();
    double var = 0.0;
    for (float& v : env) { v -= (float) mean; var += (double) v * v; }
    var /= (double) env.size();
    const float sd = (float) std::sqrt (var);
    if (sd > 0.0f) for (float& v : env) v /= sd;
    return env;
}

CorrResult crossCorrelate (const std::vector<float>& longEnv,
                           const std::vector<float>& shortEnv)
{
    CorrResult r { 0, 0.0f, 0.0f };
    const int La = (int) longEnv.size();
    const int Lb = (int) shortEnv.size();
    if (La <= Lb || Lb < 2) return r;

    int order = 0;
    while ((1 << order) < La + Lb) ++order;
    const int N = 1 << order;

    std::vector<juce::dsp::Complex<float>> A ((size_t) N), B ((size_t) N), C ((size_t) N),
                                           Af ((size_t) N), Bf ((size_t) N), Cf ((size_t) N);
    for (int i = 0; i < N; ++i)
    {
        A[(size_t) i] = { i < La ? longEnv[(size_t) i]  : 0.0f, 0.0f };
        B[(size_t) i] = { i < Lb ? shortEnv[(size_t) i] : 0.0f, 0.0f };
    }
    juce::dsp::FFT fft (order);
    // perform() must NOT alias input and output. macOS vDSP tolerates in-place,
    // but the FFTFallback engine (Linux / any non-vDSP build) writes garbage
    // when input == output - which silently zeroed the whole correlation and
    // left every fragment unplaced. Keep the in/out buffers distinct.
    fft.perform (A.data(), Af.data(), false);
    fft.perform (B.data(), Bf.data(), false);
    for (int i = 0; i < N; ++i)
        Cf[(size_t) i] = Af[(size_t) i] * std::conj (Bf[(size_t) i]);
    fft.perform (Cf.data(), C.data(), true);

    const int maxLag = La - Lb;
    // Peak + std over the valid lag range.
    int bestLag = 0;
    float peak = -1.0e30f;
    double sum = 0.0, sumSq = 0.0;
    for (int t = 0; t <= maxLag; ++t)
    {
        const float v = C[(size_t) t].real();
        sum += v; sumSq += (double) v * v;
        if (v > peak) { peak = v; bestLag = t; }
    }
    const int count = maxLag + 1;
    const double mean = sum / count;
    const double var  = sumSq / count - mean * mean;
    const float sd = (float) std::sqrt (var > 0.0 ? var : 0.0);

    // Dominance: peak vs the best peak at least kDomExcludeFrames away.
    float second = -1.0e30f;
    bool  hasCompetitor = false;
    for (int t = 0; t <= maxLag; ++t)
    {
        if (std::abs (t - bestLag) <= kDomExcludeFrames) continue;
        hasCompetitor = true;
        const float v = C[(size_t) t].real();
        if (v > second) second = v;
    }

    r.lagFrames = bestLag;
    r.sigma     = sd > 0.0f ? (float) ((peak - mean) / sd) : 0.0f;
    // No competitor at all (fragment nearly mix-length -> the whole valid range
    // sits inside the exclusion radius) means we have NO evidence of dominance,
    // so report 1.0 (below any gate) rather than a misleading high value.
    r.dominance = ! hasCompetitor ? 1.0f
                : (second > 0.0f ? (peak / second) : (peak > 0.0f ? 99.0f : 0.0f));
    return r;
}

std::vector<Alignment> alignToMixdown (const juce::File& mixdown,
                                       const std::vector<juce::File>& sources,
                                       float dominanceGate)
{
    std::vector<Alignment> result ((size_t) sources.size());

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();

    double mixSr = 0.0;
    const auto mix = decodeMono (fm, mixdown, mixSr);
    if (mix.empty() || mixSr <= 0.0) return result;   // all unplaced
    const auto mixEnv = onsetEnvelope (mix.data(), (int) mix.size(), kHop, kFftOrder);
    if (mixEnv.size() < 2) return result;

    for (size_t i = 0; i < sources.size(); ++i)
    {
        Alignment a;
        double fragSr = 0.0;
        const auto frag = decodeMono (fm, sources[i], fragSr);
        if (frag.empty()) { result[i] = a; continue; }

        // Full-length take (spans the whole song) -> sits at song start.
        if ((std::int64_t) frag.size() >= (std::int64_t) mix.size())
        {
            a.placed = true; a.fullLength = true;
            a.timelineStartSamples = 0; a.positionSeconds = 0.0;
            result[i] = a; continue;
        }

        const auto fe = onsetEnvelope (frag.data(), (int) frag.size(), kHop, kFftOrder);
        if (fe.size() < 2 || fe.size() >= mixEnv.size()) { result[i] = a; continue; }

        const auto cc = crossCorrelate (mixEnv, fe);
        a.sigma      = cc.sigma;
        a.dominance  = cc.dominance;
        a.positionSeconds      = (double) cc.lagFrames * kHop / mixSr;
        a.timelineStartSamples = (std::int64_t) cc.lagFrames * kHop;
        a.placed     = (cc.dominance >= dominanceGate);
        result[i] = a;
    }
    return result;
}
} // namespace duskstudio::dp
