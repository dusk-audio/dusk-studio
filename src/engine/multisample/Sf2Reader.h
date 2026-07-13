#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>
#include <string>
#include <vector>

namespace duskstudio
{
// Minimal SoundFont 2 (.sf2) reader. Parses the pdta metadata records
// (presets / instruments / samples + their generator zones) and records
// the byte range of the sdta sample chunk so a later pass can extract
// the PCM. Modulators, ROM samples, and the INFO block are ignored -
// they don't affect the SF2 -> SFZ note-mapping this feeds.
//
// Layout follows the SoundFont 2.04 spec: a RIFF('sfbk') containing
// LIST('INFO'), LIST('sdta') and LIST('pdta'). pdta holds fixed-size
// records (phdr/pbag/pgen/inst/ibag/igen/shdr) with terminal sentinel
// entries ("EOP"/"EOI"/"EOS") used only to bound the preceding record's
// index ranges.
//
// This header is pure data + juce_core; no audio engine, so it unit-
// tests cleanly against a real .sf2 fixture.

// SF2 generator operators we care about. Values are from the spec's
// SFGenerator enum; the full set is larger but the converter only reads
// these. Kept as plain ints so unhandled opers pass through untouched.
enum Sf2Gen : uint16_t
{
    kGenStartAddrsOffset      = 0,
    kGenInitialFilterFc       = 8,    // lowpass cutoff, absolute cents
    kGenInitialFilterQ        = 9,    // resonance, centibels
    kGenPan                   = 17,
    kGenDelayVolEnv           = 33,
    kGenAttackVolEnv          = 34,
    kGenHoldVolEnv            = 35,
    kGenDecayVolEnv           = 36,
    kGenSustainVolEnv         = 37,
    kGenReleaseVolEnv         = 38,
    kGenKeyRange              = 43,
    kGenVelRange              = 44,
    kGenInitialAttenuation    = 48,
    kGenCoarseTune            = 51,
    kGenFineTune              = 52,
    kGenSampleID              = 53,
    kGenSampleModes           = 54,
    kGenScaleTuning           = 56,
    kGenExclusiveClass        = 57,   // self-choking group (drum hi-hats)
    kGenOverridingRootKey     = 58,
};

struct Sf2Generator
{
    uint16_t oper   { 0 };
    uint16_t amount { 0 };   // raw genAmount word; interpret per oper

    // Range generators (keyRange/velRange) pack lo in the low byte and
    // hi in the high byte.
    uint8_t lo() const noexcept { return (uint8_t) (amount & 0xff); }
    uint8_t hi() const noexcept { return (uint8_t) ((amount >> 8) & 0xff); }
    int16_t asSigned() const noexcept { return (int16_t) amount; }
};

struct Sf2Zone
{
    std::vector<Sf2Generator> gens;

    // Returns a pointer to the first generator with this oper, or
    // nullptr. SF2 zones list each oper at most once.
    const Sf2Generator* find(uint16_t oper) const noexcept;
};

struct Sf2Instrument
{
    std::string          name;
    std::vector<Sf2Zone> zones;   // first zone may be a global zone (no sampleID)
};

struct Sf2Preset
{
    std::string          name;
    uint16_t             preset { 0 };   // MIDI program
    uint16_t             bank   { 0 };
    std::vector<Sf2Zone> zones;          // first zone may be global; others ref instruments
};

struct Sf2Sample
{
    std::string  name;
    uint32_t     start          { 0 };   // sample frames into the smpl chunk
    uint32_t     end            { 0 };
    uint32_t     startLoop      { 0 };
    uint32_t     endLoop        { 0 };
    uint32_t     sampleRate     { 0 };
    uint8_t      originalPitch  { 60 };
    int8_t       pitchCorrection{ 0 };   // cents
    uint16_t     sampleLink     { 0 };   // index of the paired sample for stereo
    uint16_t     sampleType     { 1 };   // 1=mono,2=right,4=left,8=linked,0x8000=ROM
};

struct Sf2File
{
    std::vector<Sf2Preset>     presets;
    std::vector<Sf2Instrument> instruments;
    std::vector<Sf2Sample>     samples;

    // Byte offset + length of the sdta 'smpl' chunk PCM (16-bit LE) in
    // the source file, for on-demand sample extraction. sm24 is the
    // optional 8-bit LSB extension for 24-bit samples (0 size = absent).
    std::int64_t smplOffset { 0 };
    std::int64_t smplSize   { 0 };
    std::int64_t sm24Offset { 0 };
    std::int64_t sm24Size   { 0 };

    bool         ok    { false };
    std::string  error;
};

// Parse an .sf2 file. On failure returns an Sf2File with ok=false and a
// populated error string. Does NOT read sample PCM - only metadata + the
// smpl chunk location.
Sf2File readSf2(const juce::File& file);
} // namespace duskstudio
