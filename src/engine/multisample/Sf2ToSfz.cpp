#include "Sf2ToSfz.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <map>

namespace duskstudio
{
namespace
{
using GenMap = std::map<uint16_t, int16_t>;

// Flatten a zone's generators into a map (later opers win, matching how
// a zone lists each oper once but we stay robust to duplicates).
void mergeZone(GenMap& m, const Sf2Zone& z)
{
    for (const auto& g : z.gens)
        m[g.oper] = (int16_t) g.amount;
}

int genOr(const GenMap& m, uint16_t oper, int fallback)
{
    auto it = m.find(oper);
    return it != m.end() ? (int) it->second : fallback;
}

bool hasGen(const Sf2Zone& z, uint16_t oper)
{
    return z.find(oper) != nullptr;
}

// A zone is "global" when it carries no terminal generator: for an
// instrument that's the absence of sampleID (53); for a preset, the
// absence of instrument (41).
constexpr uint16_t kGenInstrument = 41;

juce::String sanitize(const juce::String& s)
{
    juce::String out;
    for (auto c : s)
        out += (juce::CharacterFunctions::isLetterOrDigit(c) ? c : '_');
    return out.isEmpty() ? juce::String("s") : out;
}

// Extract one mono sample [start,end) of 16-bit PCM from the smpl chunk
// into a WAV at the sample's own rate. Returns false on any I/O issue.
bool writeSampleWav(juce::FileInputStream& in,
                     juce::int64           smplOffset,
                     const Sf2Sample&      s,
                     const juce::File&     outWav)
{
    if (s.end <= s.start) return false;
    const int numFrames = (int) (s.end - s.start);

    in.setPosition(smplOffset + (juce::int64) s.start * 2);
    juce::HeapBlock<int16_t> pcm ((size_t) numFrames);
    const int wanted = numFrames * 2;
    if (in.read(pcm.getData(), wanted) != wanted)
        return false;

    juce::AudioBuffer<float> buf (1, numFrames);
    auto* w = buf.getWritePointer(0);
    for (int i = 0; i < numFrames; ++i)
    {
        // SF2 PCM is little-endian signed 16-bit; FileInputStream::read
        // gave us native-order int16 on LE hosts. Normalize to float.
        const int16_t v = (int16_t) juce::ByteOrder::swapIfBigEndian((juce::uint16) pcm[i]);
        w[i] = (float) v / 32768.0f;
    }

    outWav.deleteFile();
    std::unique_ptr<juce::FileOutputStream> os (outWav.createOutputStream());
    if (os == nullptr) return false;

    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        fmt.createWriterFor(os.get(),
                             (double) (s.sampleRate > 0 ? s.sampleRate : 44100),
                             1, 16, {}, 0));
    if (writer == nullptr) { os.reset(); outWav.deleteFile(); return false; }
    os.release();
    const bool ok = writer->writeFromAudioSampleBuffer(buf, 0, numFrames);
    writer.reset();
    return ok;
}
} // namespace

Sf2Conversion convertSf2Preset(const juce::File& sf2,
                                int               presetIndex,
                                const juce::File& outDir)
{
    Sf2Conversion conv;

    auto parsed = readSf2(sf2);
    if (! parsed.ok)
    {
        conv.error = parsed.error;
        return conv;
    }
    if (parsed.presets.empty())
    {
        conv.error = "SF2 has no presets";
        return conv;
    }

    presetIndex = juce::jlimit(0, (int) parsed.presets.size() - 1, presetIndex);
    const auto& preset = parsed.presets[(size_t) presetIndex];
    conv.presetName = preset.name;

    outDir.createDirectory();

    juce::FileInputStream in (sf2);
    if (! in.openedOk())
    {
        conv.error = "Could not reopen SF2 for sample extraction";
        return conv;
    }

    // Preset-global zone gens (preset zone[0] when it lacks an
    // instrument generator) apply to every instrument the preset uses.
    GenMap presetGlobal;
    size_t firstInstZone = 0;
    if (! preset.zones.empty() && ! hasGen(preset.zones[0], kGenInstrument))
    {
        mergeZone(presetGlobal, preset.zones[0]);
        firstInstZone = 1;
    }

    juce::StringArray regions;
    std::map<int, juce::String> extracted;   // sampleIndex -> wav filename

    for (size_t pz = firstInstZone; pz < preset.zones.size(); ++pz)
    {
        const auto& presetZone = preset.zones[pz];
        const auto* instGen = presetZone.find(kGenInstrument);
        if (instGen == nullptr) continue;
        const int instIdx = (int) instGen->amount;
        if (instIdx < 0 || instIdx >= (int) parsed.instruments.size()) continue;
        const auto& inst = parsed.instruments[(size_t) instIdx];

        // Per-preset-zone additive layer (preset-global + this zone).
        GenMap presetLayer = presetGlobal;
        mergeZone(presetLayer, presetZone);

        // Instrument-global zone defaults.
        GenMap instGlobal;
        size_t firstSampleZone = 0;
        if (! inst.zones.empty() && ! hasGen(inst.zones[0], kGenSampleID))
        {
            mergeZone(instGlobal, inst.zones[0]);
            firstSampleZone = 1;
        }

        for (size_t iz = firstSampleZone; iz < inst.zones.size(); ++iz)
        {
            const auto& instZone = inst.zones[iz];
            const auto* sidGen = instZone.find(kGenSampleID);
            if (sidGen == nullptr) continue;
            const int sampleIdx = (int) sidGen->amount;
            if (sampleIdx < 0 || sampleIdx >= (int) parsed.samples.size()) continue;
            const auto& smp = parsed.samples[(size_t) sampleIdx];

            // Effective instrument-level gens: global then zone override.
            GenMap eff = instGlobal;
            mergeZone(eff, instZone);

            // ── Key / velocity ranges (intersect with preset layer) ──
            int loKey = 0, hiKey = 127, loVel = 0, hiVel = 127;
            if (auto it = eff.find(kGenKeyRange); it != eff.end())
            {
                loKey = it->second & 0xff;
                hiKey = (it->second >> 8) & 0xff;
            }
            if (auto it = eff.find(kGenVelRange); it != eff.end())
            {
                loVel = it->second & 0xff;
                hiVel = (it->second >> 8) & 0xff;
            }
            if (auto it = presetLayer.find(kGenKeyRange); it != presetLayer.end())
            {
                loKey = juce::jmax(loKey, it->second & 0xff);
                hiKey = juce::jmin(hiKey, (it->second >> 8) & 0xff);
            }
            if (auto it = presetLayer.find(kGenVelRange); it != presetLayer.end())
            {
                loVel = juce::jmax(loVel, it->second & 0xff);
                hiVel = juce::jmin(hiVel, (it->second >> 8) & 0xff);
            }
            if (loKey > hiKey || loVel > hiVel) continue;   // empty after intersect

            // ── Pitch ──
            const int rootKey = genOr(eff, kGenOverridingRootKey, smp.originalPitch);
            const int coarse  = genOr(eff, kGenCoarseTune, 0)
                              + genOr(presetLayer, kGenCoarseTune, 0);
            const int fine    = genOr(eff, kGenFineTune, 0)
                              + genOr(presetLayer, kGenFineTune, 0)
                              + smp.pitchCorrection;

            // ── Level / pan ──
            const int attenCb = genOr(eff, kGenInitialAttenuation, 0)
                              + genOr(presetLayer, kGenInitialAttenuation, 0);
            const double volumeDb = -((double) attenCb) / 10.0;

            // Pan: explicit generator wins (-500..500 → -100..100). When
            // absent, derive from the sample's stereo type so linked
            // left/right samples land hard L/R as the SF2 intends.
            const bool hasPanGen = (eff.count(kGenPan) || presetLayer.count(kGenPan));
            double pan = 0.0;
            if (hasPanGen)
            {
                const int panRaw = genOr(eff, kGenPan, 0) + genOr(presetLayer, kGenPan, 0);
                pan = juce::jlimit(-100.0, 100.0, (double) panRaw / 5.0);
            }
            else
            {
                const int type = smp.sampleType & 0x7fff;   // strip ROM flag
                if      (type == 4) pan = -100.0;            // left
                else if (type == 2) pan =  100.0;            // right
            }

            // ── Loop ──
            const int sampleModes = genOr(eff, kGenSampleModes, 0);
            const bool loops = (sampleModes == 1 || sampleModes == 3);

            // ── Volume envelope (timecents → seconds; cB → % sustain) ──
            auto tcToSec = [](int tc) { return std::pow(2.0, (double) tc / 1200.0); };
            juce::String env;
            if (auto it = eff.find(kGenDelayVolEnv);   it != eff.end())
                env << " ampeg_delay="   << juce::String(tcToSec(it->second), 4);
            if (auto it = eff.find(kGenAttackVolEnv);  it != eff.end())
                env << " ampeg_attack="  << juce::String(tcToSec(it->second), 4);
            if (auto it = eff.find(kGenHoldVolEnv);    it != eff.end())
                env << " ampeg_hold="    << juce::String(tcToSec(it->second), 4);
            if (auto it = eff.find(kGenDecayVolEnv);   it != eff.end())
                env << " ampeg_decay="   << juce::String(tcToSec(it->second), 4);
            if (auto it = eff.find(kGenSustainVolEnv); it != eff.end())
            {
                // SF2 sustain is centibels of attenuation from full level.
                const double pct = juce::jlimit(0.0, 100.0,
                                                 100.0 * std::pow(10.0, -((double) it->second) / 200.0));
                env << " ampeg_sustain=" << juce::String(pct, 2);
            }
            if (auto it = eff.find(kGenReleaseVolEnv); it != eff.end())
                env << " ampeg_release=" << juce::String(tcToSec(it->second), 4);

            // ── Lowpass filter ──
            // initialFilterFc is absolute cents (8.176 Hz reference);
            // default 13500 cents (~20 kHz) = effectively open, skip.
            const int fcCents = genOr(eff, kGenInitialFilterFc, 13500)
                              + genOr(presetLayer, kGenInitialFilterFc, 0);
            if (fcCents < 13500)
            {
                const double hz = 8.176 * std::pow(2.0, (double) fcCents / 1200.0);
                env << " cutoff=" << juce::String(juce::jlimit(20.0, 22000.0, hz), 1)
                    << " fil_type=lpf_2p";
            }
            const int resCb = genOr(eff, kGenInitialFilterQ, 0)
                            + genOr(presetLayer, kGenInitialFilterQ, 0);
            if (resCb > 0)
                env << " resonance=" << juce::String((double) resCb / 10.0, 2);

            // ── Key tracking (scaleTuning, cents/key; default 100) ──
            if (auto it = eff.find(kGenScaleTuning); it != eff.end() && it->second != 100)
                env << " pitch_keytrack=" << (int) it->second;

            // ── Exclusive class → self-choking group (drum hi-hats) ──
            if (auto it = eff.find(kGenExclusiveClass); it != eff.end() && it->second != 0)
                env << " group=" << (int) it->second
                    << " off_by=" << (int) it->second;

            // Extract the sample once, reuse the WAV across regions.
            auto exIt = extracted.find(sampleIdx);
            if (exIt == extracted.end())
            {
                const auto fname = "s" + juce::String(sampleIdx) + "_"
                                 + sanitize(smp.name) + ".wav";
                const auto wav = outDir.getChildFile(fname);
                if (! writeSampleWav(in, parsed.smplOffset, smp, wav))
                    continue;
                exIt = extracted.emplace(sampleIdx, fname).first;
            }
            const auto& wavName = exIt->second;

            // ── Emit region ──
            juce::String r;
            r << "<region> sample=" << wavName
              << " lokey=" << loKey << " hikey=" << hiKey
              << " lovel=" << loVel << " hivel=" << hiVel
              << " pitch_keycenter=" << rootKey;
            if (coarse != 0) r << " transpose=" << coarse;
            if (fine   != 0) r << " tune=" << juce::jlimit(-100, 100, fine);
            if (std::abs(volumeDb) > 0.01) r << " volume=" << juce::String(volumeDb, 2);
            if (std::abs(pan) > 0.01)      r << " pan=" << juce::String(pan, 1);
            if (loops && smp.endLoop > smp.startLoop && smp.startLoop >= smp.start)
            {
                r << " loop_mode=loop_continuous"
                  << " loop_start=" << (int) (smp.startLoop - smp.start)
                  << " loop_end="   << (int) (smp.endLoop   - smp.start - 1);
            }
            r << env;
            regions.add(r);
        }
    }

    if (regions.isEmpty())
    {
        conv.error = "SF2 preset \"" + preset.name + "\" produced no playable regions";
        return conv;
    }

    juce::String sfz;
    sfz << "// Auto-generated from " << sf2.getFileName()
        << " preset \"" << preset.name << "\"\n"
        << "<control>\n"
        << "<global>\n";
    for (const auto& r : regions)
        sfz << r << "\n";

    conv.sfzText   = sfz;
    conv.sampleDir = outDir;
    conv.ok        = true;
    return conv;
}
} // namespace duskstudio
