#include "Sf2ToSfz.h"

#include "../audiofile/FileWriter.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

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

std::string sanitize(const std::string& s)
{
    std::string out;
    for (const char ch : s)
    {
        const auto c = (unsigned char) ch;
        if (c >= 0x80)                 // pass UTF-8 continuation/lead bytes through
            out += (char) c;
        else if (std::isalnum(c))
            out += (char) c;
        else
            out += '_';
    }
    return out.empty() ? std::string("s") : out;
}

// Fixed decimal-place formatting with trailing-zero padding for the SFZ
// numeric fields. snprintf's decimal separator follows the C locale (which
// hosted plugins are known to flip); SFZ requires '.', so normalize.
std::string fmtF(double v, int dp)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", dp, v);
    for (char* c = buf; *c != '\0'; ++c)
        if (*c == ',') *c = '.';
    return std::string(buf);
}

// Extract one mono sample [start,end) of 16-bit PCM from the smpl chunk
// into a WAV at the sample's own rate. Returns false on any I/O issue.
bool writeSampleWav(juce::FileInputStream& in,
                     std::int64_t           smplOffset,
                     std::int64_t           smplSize,
                     const Sf2Sample&      s,
                     const juce::File&     outWav)
{
    // shdr start/end are file-declared and untrusted: validate against the
    // smpl chunk's actual byte size before sizing any allocation (a crafted
    // end of 0x7FFFFFFF would otherwise request a ~4 GB buffer, and
    // frames*2 in int is signed-overflow UB).
    if (s.end <= s.start) return false;
    const std::int64_t frames64 = (std::int64_t) s.end - (std::int64_t) s.start;
    if (smplSize <= 0 || (std::int64_t) s.end * 2 > smplSize) return false;
    if (frames64 > 0x10000000) return false;   // 256 M frames ≈ 512 MB - no real instrument sample
    const int numFrames = (int) frames64;

    in.setPosition(smplOffset + (std::int64_t) s.start * 2);
    juce::HeapBlock<int16_t> pcm ((size_t) numFrames);
    const std::int64_t wanted = frames64 * 2;
    if (in.read(pcm.getData(), (int) wanted) != (int) wanted)
        return false;

    juce::AudioBuffer<float> buf (1, numFrames);
    auto* w = buf.getWritePointer(0);
    for (int i = 0; i < numFrames; ++i)
    {
        // SF2 PCM is little-endian signed 16-bit; FileInputStream::read
        // gave us native-order int16 on LE hosts. Normalize to float.
        const int16_t v = (int16_t) juce::ByteOrder::swapIfBigEndian((std::uint16_t) pcm[i]);
        w[i] = (float) v / 32768.0f;
    }

    outWav.deleteFile();
    dusk::audio::WriteSpec spec;
    spec.sampleRate    = (double) (s.sampleRate > 0 ? s.sampleRate : 44100);
    spec.numChannels   = 1;
    spec.bitsPerSample = 16;
    spec.format        = dusk::audio::WriteSpec::Format::Wav;
    auto writer = dusk::audio::FileWriter::create (
        std::filesystem::u8path (outWav.getFullPathName().toStdString()), spec);
    if (writer == nullptr) { outWav.deleteFile(); return false; }
    const bool ok = writer->write (buf.getArrayOfReadPointers(), 1, numFrames);
    writer.reset();
    return ok;
}
} // namespace

Sf2Conversion convertSf2Preset(const juce::File& sf2,
                                int               presetIndex,
                                const juce::File& outDir)
{
    Sf2Conversion conv;

    auto parsed = readSf2(std::filesystem::u8path(sf2.getFullPathName().toStdString()));
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

    presetIndex = std::clamp(presetIndex, 0, (int) parsed.presets.size() - 1);
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

    std::vector<std::string> regions;
    std::map<int, std::string> extracted;   // sampleIndex -> wav filename

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

            // Key / velocity ranges (intersect with preset layer)
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
                loKey = std::max(loKey, it->second & 0xff);
                hiKey = std::min(hiKey, (it->second >> 8) & 0xff);
            }
            if (auto it = presetLayer.find(kGenVelRange); it != presetLayer.end())
            {
                loVel = std::max(loVel, it->second & 0xff);
                hiVel = std::min(hiVel, (it->second >> 8) & 0xff);
            }
            if (loKey > hiKey || loVel > hiVel) continue;   // empty after intersect

            // Pitch
            const int rootKey = genOr(eff, kGenOverridingRootKey, smp.originalPitch);
            const int coarse  = genOr(eff, kGenCoarseTune, 0)
                              + genOr(presetLayer, kGenCoarseTune, 0);
            const int fine    = genOr(eff, kGenFineTune, 0)
                              + genOr(presetLayer, kGenFineTune, 0)
                              + smp.pitchCorrection;

            // Level / pan
            const int attenCb = genOr(eff, kGenInitialAttenuation, 0)
                              + genOr(presetLayer, kGenInitialAttenuation, 0);
            const double volumeDb = -((double) attenCb) / 10.0;

            // Pan: explicit generator wins (-500..500 -> -100..100). When
            // absent, derive from the sample's stereo type so linked
            // left/right samples land hard L/R as the SF2 intends.
            const bool hasPanGen = (eff.count(kGenPan) || presetLayer.count(kGenPan));
            double pan = 0.0;
            if (hasPanGen)
            {
                const int panRaw = genOr(eff, kGenPan, 0) + genOr(presetLayer, kGenPan, 0);
                pan = std::clamp((double) panRaw / 5.0, -100.0, 100.0);
            }
            else
            {
                const int type = smp.sampleType & 0x7fff;   // strip ROM flag
                if      (type == 4) pan = -100.0;            // left
                else if (type == 2) pan =  100.0;            // right
            }

            // Loop
            const int sampleModes = genOr(eff, kGenSampleModes, 0);
            const bool loops = (sampleModes == 1 || sampleModes == 3);

            // Volume envelope (timecents -> seconds; cB -> % sustain)
            auto tcToSec = [](int tc) { return std::pow(2.0, (double) tc / 1200.0); };
            std::string env;
            if (auto it = eff.find(kGenDelayVolEnv);   it != eff.end())
                env += " ampeg_delay="   + fmtF(tcToSec(it->second), 4);
            if (auto it = eff.find(kGenAttackVolEnv);  it != eff.end())
                env += " ampeg_attack="  + fmtF(tcToSec(it->second), 4);
            if (auto it = eff.find(kGenHoldVolEnv);    it != eff.end())
                env += " ampeg_hold="    + fmtF(tcToSec(it->second), 4);
            if (auto it = eff.find(kGenDecayVolEnv);   it != eff.end())
                env += " ampeg_decay="   + fmtF(tcToSec(it->second), 4);
            if (auto it = eff.find(kGenSustainVolEnv); it != eff.end())
            {
                // SF2 sustain is centibels of attenuation from full level.
                const double pct = std::clamp(100.0 * std::pow(10.0, -((double) it->second) / 200.0),
                                              0.0, 100.0);
                env += " ampeg_sustain=" + fmtF(pct, 2);
            }
            if (auto it = eff.find(kGenReleaseVolEnv); it != eff.end())
                env += " ampeg_release=" + fmtF(tcToSec(it->second), 4);

            // Lowpass filter
            // initialFilterFc is absolute cents (8.176 Hz reference);
            // default 13500 cents (~20 kHz) = effectively open, skip.
            const int fcCents = genOr(eff, kGenInitialFilterFc, 13500)
                              + genOr(presetLayer, kGenInitialFilterFc, 0);
            if (fcCents < 13500)
            {
                const double hz = 8.176 * std::pow(2.0, (double) fcCents / 1200.0);
                env += " cutoff=" + fmtF(std::clamp(hz, 20.0, 22000.0), 1)
                     + " fil_type=lpf_2p";
            }
            const int resCb = genOr(eff, kGenInitialFilterQ, 0)
                            + genOr(presetLayer, kGenInitialFilterQ, 0);
            if (resCb > 0)
                env += " resonance=" + fmtF((double) resCb / 10.0, 2);

            // Key tracking (scaleTuning, cents/key; default 100)
            if (auto it = eff.find(kGenScaleTuning); it != eff.end() && it->second != 100)
                env += " pitch_keytrack=" + std::to_string((int) it->second);

            // Exclusive class -> self-choking group (drum hi-hats)
            if (auto it = eff.find(kGenExclusiveClass); it != eff.end() && it->second != 0)
                env += " group=" + std::to_string((int) it->second)
                     + " off_by=" + std::to_string((int) it->second);

            // Extract the sample once, reuse the WAV across regions.
            auto exIt = extracted.find(sampleIdx);
            if (exIt == extracted.end())
            {
                const std::string fname = "s" + std::to_string(sampleIdx) + "_"
                                        + sanitize(smp.name) + ".wav";
                const auto wav = outDir.getChildFile(fname);
                if (! writeSampleWav(in, parsed.smplOffset, parsed.smplSize, smp, wav))
                    continue;
                exIt = extracted.emplace(sampleIdx, fname).first;
            }
            const auto& wavName = exIt->second;

            // Emit region
            std::string r = "<region> sample=" + wavName
                          + " lokey=" + std::to_string(loKey) + " hikey=" + std::to_string(hiKey)
                          + " lovel=" + std::to_string(loVel) + " hivel=" + std::to_string(hiVel)
                          + " pitch_keycenter=" + std::to_string(rootKey);
            if (coarse != 0) r += " transpose=" + std::to_string(coarse);
            if (fine   != 0) r += " tune=" + std::to_string(std::clamp(fine, -100, 100));
            if (std::abs(volumeDb) > 0.01) r += " volume=" + fmtF(volumeDb, 2);
            if (std::abs(pan) > 0.01)      r += " pan=" + fmtF(pan, 1);
            if (loops && smp.endLoop > smp.startLoop && smp.startLoop >= smp.start
                && smp.endLoop <= smp.end)
            {
                r += " loop_mode=loop_continuous";
                r += " loop_start=" + std::to_string((int) (smp.startLoop - smp.start));
                r += " loop_end="   + std::to_string((int) (smp.endLoop   - smp.start - 1));
            }
            r += env;
            regions.push_back(r);
        }
    }

    if (regions.empty())
    {
        conv.error = "SF2 preset \"" + preset.name + "\" produced no playable regions";
        return conv;
    }

    std::string sfz = "// Auto-generated from " + sf2.getFileName().toStdString()
                    + " preset \"" + preset.name + "\"\n"
                    + "<control>\n"
                    + "<global>\n";
    for (const auto& r : regions)
        sfz += r + "\n";

    conv.sfzText   = sfz;
    conv.sampleDir = outDir;
    conv.ok        = true;
    return conv;
}
} // namespace duskstudio
