#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>
#include "AtomicSnapshot.h"
#include "MidiBindings.h"

namespace duskstudio
{
// Console-style automation. Parameters are recorded as performance gestures
// in Write/Touch mode and replayed in Read mode - no curve drawing UI ever
// (CLAUDE.md constraint #4). Values are stored normalized 0..1 so the same
// lane format covers dB-ranged faders, pan, send levels, and binary mute /
// solo states; per-param normalize / denormalize bridges the storage.
enum class AutomationMode : int { Off = 0, Read = 1, Write = 2, Touch = 3 };

// The fixed set of automatable per-channel parameters. EQ, Comp, HPF and bus
// assigns are deliberately NOT automatable per Dusk Studio.md - the console
// metaphor wants automation on dynamics gestures (level / position / on-off),
// not on tone-shaping. AuxSend1..4 maps onto the existing 4 aux-send knobs.
enum class AutomationParam : int
{
    FaderDb = 0,
    Pan,
    Mute,
    Solo,
    AuxSend1,
    AuxSend2,
    AuxSend3,
    AuxSend4,
    kCount
};

constexpr int kNumAutomationParams = (int) AutomationParam::kCount;

// Continuous params (fader / pan / sends) are linearly interpolated on read
// and RDP-thinned on write. Discrete params (mute / solo) have no
// interpolation and skip RDP because intermediate values would break the
// binary semantic. Used by the thinning pipeline that lands in 3c-iii.
constexpr bool isContinuousParam (AutomationParam p) noexcept
{
    return p != AutomationParam::Mute && p != AutomationParam::Solo;
}

struct AutomationPoint
{
    juce::int64 timeSamples   = 0;
    float       value         = 0.0f;     // normalized 0..1 (see normalize/denormalize helpers)
    float       recordedAtBPM = 120.0f;   // canonical BPM for retime; on-load default = session BPM
};

// One lane per automatable param, per track. Points are kept sorted by
// timeSamples; binary-search at lookup. For 3c-i this vector is mutated
// only at session-load time (transport stopped), so the audio thread reads
// it without synchronisation. When 3c-ii adds Write, the writer thread will
// assemble a new vector off-thread and swap via an atomic pointer (the
// PluginSlot::currentInstance pattern) so the audio thread always sees a
// consistent snapshot.
struct AutomationLane
{
    std::vector<AutomationPoint> points;
};

// Returns the denormalized value (dB / -1..+1 / 0-or-1 / dB+sentinel) at
// the given timeline sample. Linear interpolation between bracketing points
// for continuous params; step (hold-previous) for discrete params. Outside
// the lane's range, holds the first / last point's value. An empty lane is
// the caller's signal to skip the override; this function returns 0 in that
// case but callers must gate on `lane.points.empty()` first.
float evaluateLane (const AutomationLane& lane, juce::int64 t,
                    AutomationParam param) noexcept;

// External hardware-insert routing. These fields change together when
// the user flips the Output / Input dropdown in the HardwareInsertEditor,
// so the audio thread must observe them as one consistent snapshot.
// Wrapped in AtomicSnapshot<> inside HardwareInsertParams below.
struct HardwareInsertRouting
{
    int outputChL      = -1;   // -1 = unassigned (insert SEND disabled)
    int outputChR      = -1;
    int inputChL       = -1;   // -1 = unassigned (insert RETURN silent)
    int inputChR       = -1;
    int latencySamples =  0;   // round-trip, samples
    int format         =  0;   // 0 = Stereo, 1 = Mid/Side
};

// Per-strip hardware-insert state. Sibling of ChannelStripParams - the
// channel insert slot is "either a plugin OR a hardware insert", with
// the live mode chosen by the strip's atomic insertMode flag (added in
// Phase 3). Audio-thread reads lock-free via plain atomic loads (scalar
// knobs) and AtomicSnapshot acquire (routing). Message thread writes
// via plain stores + AtomicSnapshot::publish.
struct HardwareInsertParams
{
    std::atomic<bool>                     enabled { false };
    AtomicSnapshot<HardwareInsertRouting> routing;

    std::atomic<float> outputGainDb { 0.0f };   // SEND trim
    std::atomic<float> inputGainDb  { 0.0f };   // RETURN trim
    std::atomic<float> dryWet       { 1.0f };   // 0 = dry only, 1 = wet only

    // Ping handshake. UI thread sets pingPending = true; audio thread
    // runs the chirp + capture + correlation, writes the measured sample
    // lag to pingResult, then clears pingPending. Both fields are
    // mutable so the audio thread can update them through a
    // const HardwareInsertParams* reference (same pattern as the meter
    // atomics on Bus / Master params).
    mutable std::atomic<bool> pingPending { false };
    // -1 = correlation peak below threshold (cable connected but signal
    // too weak / noisy); -2 = capture stall (no input samples ever
    // arrived — likely no routing / device unplugged); >= 0 = measured
    // lag in samples. Default -1 is a no-op until the first ping
    // completes; UI gates on pingPending falling edge before reading.
    mutable std::atomic<int>  pingResult  { -1 };
};

// Phase 1a-minimal channel strip parameters: fader, pan, mute, solo. The full
// strip (HPF, 4-band EQ, FET/Opto compressor, sends, bus assigns) lands in
// later chunks once the DSP cores are lifted out of the Dusk plugins repo.
struct ChannelStripParams
{
    std::atomic<float> faderDb { 0.0f };   // -inf via -100 sentinel, +12 dB headroom
    std::atomic<float> pan     { 0.0f };   // -1 = full L, 0 = center, +1 = full R
    std::atomic<bool>  mute    { false };
    std::atomic<bool>  solo    { false };
    std::atomic<bool>  phaseInvert { false };  // Ø - flips polarity at strip input

    // Fader group id. 0 = ungrouped; 1..N = members of group N. When the
    // user drags a fader on a track in a group, every other track sharing
    // the same group id gets the same relative-dB delta applied. Anchor
    // values are captured at drag-start so the relative offset between
    // group members is preserved end-to-end.
    std::atomic<int>   faderGroupId { 0 };

    // Per-channel bus assigns. Bit i is true => post-fader signal also sums
    // into BUS (i+1) at unity. Channel always sums to master regardless of
    // bus assigns. Knob-style FX sends (with level) will be a separate
    // surface added later.
    static constexpr int kNumBuses = 4;
    std::array<std::atomic<bool>, kNumBuses> busAssign {};

    // Per-channel AUX sends. Distinct from bus assigns - AUXes feed the
    // 4 aux strips' plugin chains (reverb / delay / etc.), each with a
    // continuous send level and a pre/post-fader tap point. -100 dB sentinel
    // = no send (audio thread can short-circuit). Defaults: -inf, post-fader
    // (matches the typical "send to reverb" workflow).
    static constexpr int   kNumAuxSends = 4;
    static constexpr float kAuxSendMinDb = -60.0f;
    static constexpr float kAuxSendMaxDb =   6.0f;
    static constexpr float kAuxSendOffDb = -100.0f;   // sentinel: knob fully CCW
    std::array<std::atomic<float>, kNumAuxSends> auxSendDb {
        std::atomic<float>{ kAuxSendOffDb }, std::atomic<float>{ kAuxSendOffDb },
        std::atomic<float>{ kAuxSendOffDb }, std::atomic<float>{ kAuxSendOffDb }
    };
    std::array<std::atomic<bool>, kNumAuxSends> auxSendPreFader {};

    // 4K EQ surface: HPF + 4-band parametric (LF/LM/HM/HF) + Brown/Black mode.
    // Maps into BritishEQProcessor::Parameters (multi-q) when read on the audio thread.
    std::atomic<bool>  hpfEnabled { false };
    std::atomic<float> hpfFreq    { 20.0f };
    std::atomic<float> lfGainDb   { 0.0f };
    std::atomic<float> lfFreq     { 100.0f };
    std::atomic<float> lmGainDb   { 0.0f };
    std::atomic<float> lmFreq     { 600.0f };
    std::atomic<float> lmQ        { 0.7f };
    std::atomic<float> hmGainDb   { 0.0f };
    std::atomic<float> hmFreq     { 2000.0f };
    std::atomic<float> hmQ        { 0.7f };
    std::atomic<float> hfGainDb   { 0.0f };
    std::atomic<float> hfFreq     { 8000.0f };
    std::atomic<bool>  eqBlackMode { false };  // false = Brown (E-series), true = Black (G-series)

    // Compressor surface: UniversalCompressor exposes a different parameter
    // set per mode (Opto/FET/VCA model real hardware), so each mode keeps its
    // own atomic state and the UI swaps which controls are visible.
    std::atomic<bool>  compEnabled    { false };
    std::atomic<int>   compMode       { 0 };       // 0=Opto, 1=FET, 2=VCA

    // Opto (LA-2A style): peak-reduction + gain + optional limit mode.
    std::atomic<float> compOptoPeakRed { 30.0f };  // 0..100 %
    std::atomic<float> compOptoGain    { 50.0f };  // 0..100 % (50 = unity)
    std::atomic<bool>  compOptoLimit   { false };

    // FET (1176 style): input drives compression, ratio is a discrete choice.
    std::atomic<float> compFetInput   { 0.0f };    // -20..40 dB
    std::atomic<float> compFetOutput  { 0.0f };    // -20..20 dB
    std::atomic<float> compFetAttack  { 0.2f };    // 0.02..80 ms
    std::atomic<float> compFetRelease { 400.0f };  // 50..1100 ms
    std::atomic<int>   compFetRatio   { 0 };       // 0=4:1, 1=8:1, 2=12:1, 3=20:1, 4=All

    // VCA (classic): the textbook threshold/ratio/attack/release/output.
    std::atomic<float> compVcaThreshDb { 0.0f };    // -38..12 dB
    std::atomic<float> compVcaRatio    { 4.0f };    // 1..120
    std::atomic<float> compVcaAttack   { 1.0f };    // 0.1..50 ms
    std::atomic<float> compVcaRelease  { 100.0f };  // 10..5000 ms
    std::atomic<float> compVcaOutput   { 0.0f };    // -20..20 dB

    // Legacy unified threshold - still driven by the comp meter strip's drag
    // handle. Mirrors the *current mode's* primary compression knob so the
    // drag stays usable regardless of which mode is active. The audio thread
    // does NOT read this directly any more - see ChannelStrip::updateCompParameters.
    std::atomic<float> compThresholdDb { 0.0f };

    // Unified makeup-intent across modes. The comp editor's MAKEUP knob writes
    // here AS WELL AS to the appropriate per-mode atom; FET threshold reads
    // this back when recomputing compFetOutput so threshold and makeup compose
    // (without it, threshold's chain compensation overwrites the user's makeup).
    // VCA / Opto don't need this dance - their makeup parameter is independent
    // of threshold - but we still write here for consistency / save-state.
    std::atomic<float> compMakeupDb    { 0.0f };

    // Effective live fader value (dB) AFTER any automation routing. Written
    // by AudioEngine at the top of every block: in Off mode it mirrors
    // `faderDb`; in Read mode it carries the automated value from the lane.
    // ChannelStrip reads THIS, not faderDb, so the Off/Read hand-off stays
    // local to the engine. The UI's fader-display timer also polls this
    // atom, giving free motor-fader animation in Read mode without extra
    // wiring. `mutable` so a const ChannelStripParams* (the audio-thread
    // const-ref) can still write it (same pattern as the meter atomics).
    mutable std::atomic<float> liveFaderDb { 0.0f };

    // True while the user has the fader slider grabbed (between drag-start
    // and drag-end). Drives Touch-mode behavior: the audio thread reads
    // the lane normally, but switches to manual faderDb whenever
    // faderTouched is true (and the UI captures into the lane during the
    // touch). On release, the smoother's existing 20 ms ramp gives a
    // natural glide back to the lane value.
    std::atomic<bool> faderTouched { false };

    // Live pan value AFTER any automation routing. Same pattern as
    // liveFaderDb / faderTouched but for the pan knob. Range -1..+1.
    mutable std::atomic<float> livePan { 0.0f };
    std::atomic<bool> panTouched { false };

    // Live aux send levels (dB) AFTER automation routing. Same pattern
    // as liveFaderDb but per-send. ChannelStrip reads liveAuxSendDb when
    // computing aux-send gains; the manual auxSendDb stays the user's
    // persisted setpoint.
    mutable std::array<std::atomic<float>, kNumAuxSends> liveAuxSendDb {
        std::atomic<float>{ kAuxSendOffDb }, std::atomic<float>{ kAuxSendOffDb },
        std::atomic<float>{ kAuxSendOffDb }, std::atomic<float>{ kAuxSendOffDb }
    };
    std::array<std::atomic<bool>, kNumAuxSends> auxSendTouched {};

    // Live mute / solo AFTER automation routing. Discrete (no
    // interpolation) and the button click is instantaneous (no
    // grab/release), so unlike the continuous params there's no
    // touched flag - Touch mode for these simply means "read lane,
    // and any click writes a new transition point into the lane".
    // The audio thread reads liveMute / liveSolo for both the
    // strip's gain decision and the engine's per-strip routing
    // flags. Session::anyTrackSoloed() also scans liveSolo, so
    // automated solos correctly trigger the global SIP-mute logic.
    mutable std::atomic<bool> liveMute { false };
    mutable std::atomic<bool> liveSolo { false };

    static constexpr float kFaderMinDb       = -100.0f;
    static constexpr float kFaderMaxDb       =  12.0f;
    static constexpr float kFaderInfThreshDb = -90.0f;  // below this we hard-mute

    // EQ control ranges
    static constexpr float kHpfMinHz   = 20.0f;
    static constexpr float kHpfMaxHz   = 300.0f;
    static constexpr float kHpfOffHz   = 20.0f;   // at min, HPF is effectively bypassed
    static constexpr float kLfFreqMin  = 20.0f,   kLfFreqMax = 400.0f;
    static constexpr float kLmFreqMin  = 100.0f,  kLmFreqMax = 4000.0f;
    static constexpr float kHmFreqMin  = 600.0f,  kHmFreqMax = 13000.0f;
    static constexpr float kHfFreqMin  = 1000.0f, kHfFreqMax = 20000.0f;
    static constexpr float kBandGainMin = -15.0f, kBandGainMax = 15.0f;
    static constexpr float kBandQMin = 0.4f, kBandQMax = 4.0f;  // mid-band Q range

    // Compressor parameter ranges
    static constexpr float kCompThreshMin =  -60.0f, kCompThreshMax =   0.0f;
    static constexpr float kCompRatioMin  =    1.0f, kCompRatioMax  =  20.0f;
    static constexpr float kCompAttackMin =    0.1f, kCompAttackMax = 200.0f;
    static constexpr float kCompReleaseMin=   10.0f, kCompReleaseMax= 2000.0f;
    static constexpr float kCompMakeupMin =  -12.0f, kCompMakeupMax =  24.0f;
};

// One slot in a region's take history. Stores everything needed to swap a
// previously-recorded version of this region's audio back into the live
// region: the WAV path on disk, where in that WAV the playable slice starts,
// and how many samples the slice runs for. Timeline position is NOT stored -
// rotating takes preserves the region's timelineStart so the user always
// hears the alternate take in the same spot in the song.
struct TakeRef
{
    juce::File file;
    juce::int64 sourceOffset    = 0;
    juce::int64 lengthInSamples = 0;
};

// Standard MIDI tick resolution. Pulses-per-quarter-note = 480 matches
// every modern DAW + .mid file convention; high enough that 64th-note
// triplets quantize exactly (480 / 24 = 20). Conversion helpers below
// turn between sample positions (audio-domain) and tick positions
// (musical-domain) at a given tempo and sample rate. Tempo is read from
// the session at conversion time - changes to BPM rebuild the
// scheduled-sample mapping; the tick positions on the events stay stable.
constexpr int kMidiTicksPerQuarter = 480;

inline juce::int64 samplesToTicks (juce::int64 samples,
                                   double sampleRate,
                                   float bpm) noexcept
{
    if (sampleRate <= 0.0 || bpm <= 0.0f) return 0;
    return (juce::int64) std::llround (
        (double) samples * (double) bpm * (double) kMidiTicksPerQuarter
            / (sampleRate * 60.0));
}

inline juce::int64 ticksToSamples (juce::int64 ticks,
                                   double sampleRate,
                                   float bpm) noexcept
{
    if (sampleRate <= 0.0 || bpm <= 0.0f) return 0;
    return (juce::int64) std::llround (
        (double) ticks * sampleRate * 60.0
            / ((double) bpm * (double) kMidiTicksPerQuarter));
}

// Time-display modes. Stored as int in Session::timeDisplayMode so it
// can be a plain atomic; cast at use sites. Match the order with the
// transport-bar's toggle button (Bars first since it's the default).
enum class TimeDisplayMode : int { Bars = 0, Time = 1 };

// Ardour-style edit mode. Click + drag semantics on the arrange surface
// (TapeStrip) and inside the region editor branch on this. Grab is the
// historical default — same gestures Dusk Studio had before the mode palette
// landed. Range, Cut, Grid, Draw are layered on top: each one specialises
// what a body-click does. Stretch is deliberately absent (Dusk Studio spec
// forbids time-stretching).
enum class EditMode : int
{
    Grab  = 0,   // default: click = cursor, body drag = move region
    Range = 1,   // any body drag = time-range selection
    Cut   = 2,   // single click on region body = split at cursor
    Grid  = 3,   // ruler click adds/edits a tempo-map event (Phase 3c)
    Draw  = 4    // MIDI: note pencil; Audio: gain envelope breakpoints (3d)
};

// Snap denomination. The Ardour-style snap UX is two orthogonal controls:
//   • session.snapToGrid (bool, declared below) — on/off master switch.
//     SnapHelpers no-op when this is false regardless of the resolution.
//   • session.snapResolution (this enum) — grid size used when snapping
//     is on. Musical resolutions (Bar..1/128) are relative to the current
//     tempo + time-signature. Triplets are ×2/3 of the base, dotted are
//     ×3/2. Non-musical modes — Timecode (25 fps frame), MinSec (1 second),
//     CD-Frames (75 fps frame) — ignore tempo entirely. Quarter is the
//     default and matches the historical "one beat" behaviour.
enum class SnapResolution : int
{
    Bar              =  0,
    Half             =  1,
    Quarter          =  2,
    Eighth           =  3,
    Sixteenth        =  4,
    ThirtySecond     =  5,
    SixtyFourth      =  6,
    OneTwentyEighth  =  7,
    HalfTriplet      =  8,
    QuarterTriplet   =  9,
    EighthTriplet    = 10,
    SixteenthTriplet = 11,
    ThirtySecondTrip = 12,
    HalfDotted       = 13,
    QuarterDotted    = 14,
    EighthDotted     = 15,
    SixteenthDotted  = 16,
    Timecode         = 17,
    MinSec           = 18,
    CDFrames         = 19
};

// Shared sample-position formatter. Every surface that displays a
// playhead / cursor position routes through this so flipping
// Session::timeDisplayMode swaps the whole app's representation
// atomically. Bars mode emits "bar.beat.tick" (1-based bar/beat,
// tick in 0..ticksPerBeat-1, zero-padded to 3 digits). Time mode
// emits "mm:ss.mmm" (zero-padded minutes, seconds, milliseconds).
inline juce::String formatSamplePosition (juce::int64 samples,
                                            double sampleRate,
                                            float bpm,
                                            int beatsPerBar,
                                            TimeDisplayMode mode) noexcept
{
    if (samples < 0) samples = 0;
    if (mode == TimeDisplayMode::Time)
    {
        if (sampleRate <= 0.0) return juce::String ("00:00.000");
        const double totalSec = (double) samples / sampleRate;
        const int mins   = (int) (totalSec / 60.0);
        const int secs   = (int) totalSec % 60;
        const int millis = (int) std::round ((totalSec - std::floor (totalSec)) * 1000.0);
        return juce::String::formatted ("%02d:%02d.%03d", mins, secs,
                                          millis >= 1000 ? 999 : millis);
    }
    // Bars / Beats / Tick.
    if (sampleRate <= 0.0 || bpm <= 0.0f || beatsPerBar <= 0)
        return juce::String ("1.1.000");
    const double samplesPerBeat = sampleRate * 60.0 / (double) bpm;
    const double samplesPerBar  = samplesPerBeat * (double) beatsPerBar;
    const int bar  = (int) ((double) samples / samplesPerBar);
    const double remBar = (double) samples - (double) bar * samplesPerBar;
    const int beat = (int) (remBar / samplesPerBeat);
    const double remBeat = remBar - (double) beat * samplesPerBeat;
    const int tick = (int) (remBeat * (double) kMidiTicksPerQuarter / samplesPerBeat);
    return juce::String (bar + 1) + "."
         + juce::String (beat + 1) + "."
         + juce::String (tick).paddedLeft ('0', 3);
}

// One MIDI note in tick time, anchored relative to its containing region's
// start. Off events are folded into `lengthInTicks` so the model never has
// dangling note-on / note-off bookkeeping. Negative or zero length is the
// "all-notes-off across the region" signal that scheduler should skip.
struct MidiNote
{
    int  channel       = 1;     // 1..16
    int  noteNumber    = 60;    // 0..127
    int  velocity      = 100;   // 1..127 (MIDI vel 0 = note-off; recorded notes always >= 1)
    juce::int64 startTick     = 0;
    juce::int64 lengthInTicks = 0;
};

// CC / pitch-bend / sustain / etc. - non-note MIDI events stored generically.
// `controller` doubles as the message type discriminator: 0..127 is a
// regular CC; we'll add named sentinels for pitch-bend / aftertouch in 4c
// when the piano roll surfaces them. For 4b only CCs are captured.
struct MidiCc
{
    int  channel    = 1;     // 1..16
    int  controller = 64;    // 0..127 (sustain pedal = 64)
    int  value      = 0;     // 0..127
    juce::int64 atTick = 0;
};

// MIDI region - the MIDI counterpart of AudioRegion. Anchored on the
// timeline at `timelineStart` (samples), holds events in ticks relative
// to that anchor. lengthInSamples is the rendered length at session
// tempo (cached so the timeline-rect math doesn't reconvert per draw);
// PlaybackEngine recomputes it whenever tempoBpm changes. lengthInTicks
// is the source of truth for musical length. previousTakes mirrors the
// AudioRegion take-history pattern - overdubbed takes stack here when
// the new take's range fully contains an existing region.
struct MidiTakeRef
{
    juce::int64           lengthInTicks = 0;
    std::vector<MidiNote> notes;
    std::vector<MidiCc>   ccs;
};

struct MidiRegion
{
    juce::int64 timelineStart    = 0;   // sample position on the timeline
    juce::int64 lengthInSamples  = 0;   // cached at session tempo
    juce::int64 lengthInTicks    = 0;   // source of truth for musical length

    std::vector<MidiNote> notes;
    std::vector<MidiCc>   ccs;

    // Older takes that occupied this region's timeline range, captured by
    // the recorder when a new take's range fully contains an existing
    // region. Front of the vector = next to surface on a cycle, same
    // semantics as AudioRegion::previousTakes.
    std::vector<MidiTakeRef> previousTakes;

    // Per-region accent colour override + user-supplied label. Same
    // semantics as AudioRegion::customColour / label - default values
    // mean "use track defaults / no label", the painter respects
    // both, and SessionSerializer round-trips them.
    juce::Colour customColour;
    juce::String label;

    // Per-region mute - same semantics as AudioRegion::muted. The
    // disk-MIDI scheduler in AudioEngine skips notes / CCs from
    // muted regions, so toggling takes effect on the next audio
    // block (no playback rebuild needed - MIDI regions are lock-
    // free-readable from the audio thread via AtomicSnapshot).
    bool muted = false;

    // Per-region lock - same semantics as AudioRegion::locked.
    // Affects right-click delete and (when piano roll handlers
    // grow lock-awareness) edit gestures. Doesn't affect
    // playback or note scheduling.
    bool locked = false;
};

// Fade curve shape applied to a region's fade-in / fade-out (and to the
// implicit crossfade across overlapping regions on the same track). All
// shapes map t in [0,1] -> gain in [0,1] with shape(0)=0 and shape(1)=1.
// Linear is the historical default; EqualPower is constant-power and
// preserves perceived loudness across crossfade overlaps.
enum class FadeShape : int
{
    Linear     = 0,
    EqualPower = 1,
    Sigmoid    = 2,
    Exp        = 3,
    Log        = 4
};

// Apply the shape function. Caller normalises t into [0,1]. Out-of-range
// inputs are clamped. Used by PlaybackEngine for audio rendering and by
// AudioRegionEditor for envelope painting; keep in sync.
inline float applyFadeShape (float t, FadeShape s) noexcept
{
    t = juce::jlimit (0.0f, 1.0f, t);
    switch (s)
    {
        case FadeShape::Linear:     return t;
        case FadeShape::EqualPower: return std::sin (t * juce::MathConstants<float>::halfPi);
        case FadeShape::Sigmoid:    return t * t * (3.0f - 2.0f * t);
        case FadeShape::Exp:        return t * t;
        case FadeShape::Log:        return 1.0f - (1.0f - t) * (1.0f - t);
    }
    return t;
}

// Audio region - references a mono WAV file on disk. Multiple recordings
// over the same timeline range stack into `previousTakes`; the live take's
// fields (file/sourceOffset/lengthInSamples) plus that vector form a
// rotating history the user can cycle through via the badge UI.
struct AudioRegion
{
    juce::File file;                  // absolute path to the WAV
    juce::int64 timelineStart = 0;    // sample position on the timeline
    juce::int64 lengthInSamples = 0;
    juce::int64 sourceOffset = 0;     // future: trim from left
    int          numChannels    = 1;  // 1 = mono WAV, 2 = stereo WAV. Set by
                                      // RecordManager when committing the take
                                      // (matches the writer's channel count)
                                      // and persisted in the session.json.

    // Crossfade ramps in samples. PlaybackEngine applies the configured
    // shape across [timelineStart, +fadeInSamples) for the in ramp and
    // across [end - fadeOutSamples, end) for the out ramp. Zero (default) =
    // no fade, hard edge. Set by RecordManager when committing a punched
    // take so the new region crossfades against trimmed outer fragments
    // of the existing region without an audible click at the punch boundary.
    //
    // Invariants (enforced by RecordManager when these are written, and
    // defensively re-clamped by PlaybackEngine::preparePlayback):
    //   • fadeInSamples >= 0 && fadeOutSamples >= 0
    //   • fadeInSamples + fadeOutSamples <= lengthInSamples
    // Without the second invariant, the fade-in and fade-out ramps would
    // overlap in the middle of the region and the multiplied gain would
    // produce a notch instead of a flat 1.0 hold.
    juce::int64 fadeInSamples  = 0;
    juce::int64 fadeOutSamples = 0;

    // Per-region fade curve shape. Default Linear matches the original
    // hard-coded ramp so existing sessions stay byte-identical when no
    // shape was persisted. EqualPower is the default crossfade choice
    // (constant power across the overlap when two regions abut). Sigmoid
    // is an S-curve (smooth at both ends). Exp / Log bias the energy
    // toward the unity / silent end respectively.
    FadeShape fadeInShape  = FadeShape::Linear;
    FadeShape fadeOutShape = FadeShape::Linear;

    // True when this fade was created by the editor's auto-crossfade
    // path (i.e. a move-drag produced an overlap with a neighbour).
    // The next move re-syncs these auto fades to the current overlap
    // length and clears them entirely when the overlap is gone, so a
    // region dragged out of overlap doesn't keep a stray ramp. Cleared
    // whenever the user manually drags the fade handle, picks a shape
    // explicitly, or runs "Reset fades" — those gestures promote the
    // fade to user-owned and pin it against future auto-retraction.
    bool fadeInAuto  = false;
    bool fadeOutAuto = false;

    // Per-region gain in dB. Default 0.0 = unity (no change). Applied
    // multiplicatively in PlaybackEngine's per-sample loop, on top of
    // the fade envelope, BEFORE the channel strip's processing - so
    // strip EQ / compression sees the user's chosen level instead of
    // the raw take. Range is enforced at the UI (Alt-drag clamps to
    // [-24, +12] dB); the field itself is just a float.
    float gainDb = 0.0f;

    // Optional per-region colour override. Default-constructed
    // (transparent) means "use the parent track's colour". When the
    // user picks a custom colour from the right-click region menu,
    // this gets set and the tape-strip painter uses it instead of
    // session.track(t).colour. Lets the user mark sections (verse /
    // chorus / etc.) without changing the whole track's tint.
    juce::Colour customColour;

    // Optional user-supplied label painted at the top-left of the
    // region body. Empty string = no label. Set via the right-click
    // "Rename..." action; persists per-region in session.json.
    juce::String label;

    // Per-region mute. true = skip during playback (the audio is
    // still on disk and the region's other state - fades, gain,
    // edits - are preserved; toggling unmute brings the region
    // back without re-recording). Audio takes effect on the next
    // preparePlayback (i.e. next stop+play); the painter dims
    // muted regions immediately.
    bool muted = false;

    // Per-region lock. true = the region rejects drag / resize /
    // delete / split / nudge / gain operations. Useful for marking
    // a finalised take so a stray drag doesn't move it. Doesn't
    // affect playback - locked regions still play through every
    // signal-flow path. Toggle from the right-click menu; the
    // painter shows a small lock badge.
    bool locked = false;

    // Older takes that occupied this region's timeline range, captured by
    // RecordManager::stopRecording when the new take's range fully contains
    // an existing region. Front of the vector = next to surface on a cycle.
    std::vector<TakeRef> previousTakes;
};

struct Track
{
    // Per-track signal mode. Selects how the strip records and plays back:
    //   Mono   - one audio input -> mono WAV -> mono playback (legacy default)
    //   Stereo - two audio inputs (L + R) -> 2-ch WAV -> stereo playback
    //   Midi   - one MIDI input -> .mid sequence -> drives the strip's
    //            hosted plugin which produces the audible audio
    // Phase 1 ships only the model + the UI selector. Stereo audio capture
    // and MIDI capture/playback land in subsequent phases.
    enum class Mode : int { Mono = 0, Stereo = 1, Midi = 2 };

    juce::String name;
    juce::Colour colour;
    ChannelStripParams strip;

    // Hardware-insert side of the channel insert slot. The strip's
    // insert is "either a plugin OR a hardware insert", chosen at audio
    // time by an atomic mode flag added in Phase 3. The two slots
    // co-exist as zero-cost-when-idle DSP objects so swapping between
    // them needs no allocation.
    HardwareInsertParams hardwareInsert;

    std::atomic<int> mode { (int) Mode::Mono };  // store as int for atomic-ness

    // Recording surface
    std::atomic<bool> recordArmed { false };
    std::atomic<bool> inputMonitor { false };  // off by default - engineer engages IN explicitly when monitoring
                                              // (track still records and meters when armed)
    std::atomic<bool> printEffects { false };  // when true, the recorded WAV captures the post-EQ/post-comp
                                              // signal - "print effects on the way in". Default off so the
                                              // user can re-EQ/re-comp at mix time.
    std::atomic<int>  inputSource { -2 };  // mono mode + L of stereo mode:
                                            // -2 = follow track index (default),
                                            // -1 = no input,
                                            // 0..N = audio device input N
    std::atomic<int>  inputSourceR { -2 }; // R of stereo mode (same encoding;
                                            // -2 = inputSource + 1, paired adjacent)
    std::atomic<int>  midiInputIndex { -1 }; // -1 = none; 0..N = AudioDeviceManager
                                              // MIDI input index. Only used in Midi mode.
    // Stable device identifier (juce::MidiDeviceInfo::identifier) for the
    // selected MIDI input. Persisted alongside midiInputIndex; on load
    // we look this up in the current `juce::MidiInput::getAvailableDevices()`
    // to recompute the right index, so a session saved with one device
    // ordering still resolves correctly when the user reloads after
    // unplugging/replugging USB MIDI gear. Empty when no input is
    // selected. Message-thread only - the audio thread reads the int
    // atom above, never this string.
    juce::String      midiInputIdentifier;

    // External MIDI output. When set on a Midi-mode track, the per-track
    // MIDI buffer (recorded notes + live input + automation) is also
    // sent to this hardware MIDI port every block - so the user can
    // drive an external synth/drum machine. The synth's audio comes
    // back on a separate audio track via the user's audio interface.
    // -1 = no external output (default; only the loaded instrument
    // plugin sees the MIDI). 0..N = AudioEngine MIDI output index.
    // Identifier-based persistence mirrors midiInputIdentifier above.
    std::atomic<int>  midiOutputIndex { -1 };
    juce::String      midiOutputIdentifier;
    std::atomic<int>  midiChannel    { 0 };  // 0 = omni (all channels accepted),
                                              // 1..16 = filter to that MIDI channel only.
    // Audio thread sets to true when a MIDI message routes to this track in
    // a given block. UI timer polls + clear-on-read for the activity LED so a
    // continuous stream still flashes (clear-then-rise per timer tick).
    mutable std::atomic<bool> midiActivity { false };

    // Recorded regions for this track. Audio tracks (Mode::Mono /
    // Mode::Stereo) write to `regions`; MIDI tracks (Mode::Midi) write to
    // `midiRegions`. The other vector stays empty for the wrong mode - we
    // don't switch storage on a mode flip, so a Mono->Midi conversion
    // preserves the audio takes underneath (a future "convert to MIDI" UX
    // would explicitly clear them).
    //
    // `regions` is mutated only at session-load and is read by the audio
    // thread indirectly through PlaybackEngine's pre-built region-streams
    // snapshot, not directly - so a plain vector is fine.
    //
    // `midiRegions` is mutated at session-load AND at recording-stop
    // (RecordManager::stopRecording push_backs the freshly-captured take).
    // The audio thread reads it directly during MIDI playback (AudioEngine
    // hanging-note flush + scheduling). It's wrapped in AtomicSnapshot so
    // the recording-stop swap is lock-free for the audio thread.
    std::vector<AudioRegion>                regions;
    AtomicSnapshot<std::vector<MidiRegion>> midiRegions;

    // Per-channel plugin slot persistence. Populated from the live PluginSlot
    // immediately before SessionSerializer::save (via AudioEngine::publishPluginStateForSave)
    // and consumed immediately after SessionSerializer::load (via consumePluginStateAfterLoad).
    // Empty when no plugin is loaded. Both are message-thread-only fields -
    // not std::atomic - because plugin load/save runs message-thread.
    juce::String pluginDescriptionXml;  // juce::PluginDescription serialised as XML
    juce::String pluginStateBase64;     // raw plugin state (getStateInformation), base64-encoded

    // Metering surface - written from the audio thread, polled by the UI.
    std::atomic<float> meterGrDb     { 0.0f };     // peak gain reduction in dB (≤ 0)
    std::atomic<float> meterInputDb  { -100.0f };  // peak of channel source (live or playback) in dBFS
    // R-channel input peak. Written only when track.mode == Stereo and an R
    // input is resolved; left at -100 for mono / midi tracks. The UI uses
    // this to render a 2nd LED bar alongside the fader for stereo tracks.
    std::atomic<float> meterInputRDb { -100.0f };

    // Per-strip automation engagement. Stored as int because the atomic-bool
    // pattern doesn't extend to 4 states, and we keep the same lock-free
    // contract (UI mutates with relaxed; audio thread reads with relaxed).
    // 3c-i wires Off + Read; Write/Touch reserved for 3c-ii (UI greys them).
    std::atomic<int> automationMode { 0 };  // AutomationMode cast to int

    // Per-parameter automation lanes. Indexed by AutomationParam.
    //
    // Concurrency: reader and writer are partitioned by mode + per-control
    // touched flags, NOT by container atomicity. The audio thread reads
    // lane.points only when readsLane = (mode == Read) || (mode == Touch
    // && !touched). The message thread (Write/Touch capture in
    // ChannelStripComponent::captureWritePoint) appends only when
    // capturing = playing && (mode == Write || (mode == Touch && touched)).
    // The two predicates are mutually exclusive on (mode, touched) - one
    // side or the other touches the vector at any given time, never both.
    //
    // Synchronization is via release/acquire on automationMode (stored
    // release on mode-cycle clicks, acquired in AudioEngine before reading
    // the lane) and on each per-control *Touched flag (stored release on
    // drag start/end, acquired in AudioEngine when deciding readsLane).
    // Every message-thread append therefore happens-before the audio
    // thread's next read of the same lane.
    std::array<AutomationLane, kNumAutomationParams> automationLanes {};
};

// Aux bus surface. Phase 1a: fader + mute + solo + 3-band EQ + bus comp + pan
// + meter. Bus EQ uses 3 of BritishEQProcessor's 4 bands (LF / MID-as-LM / HF)
// with fixed musical frequencies. Bus comp uses UniversalCompressor in Bus
// mode (mode index 3).
struct BusParams
{
    std::atomic<float> faderDb { 0.0f };
    std::atomic<float> pan     { 0.0f };  // -1..1 (left..right balance)
    std::atomic<bool>  mute    { false };
    std::atomic<bool>  solo    { false };

    // 3-band EQ.
    std::atomic<bool>  eqEnabled  { false };
    std::atomic<float> eqLfGainDb { 0.0f };  // -15..+15
    std::atomic<float> eqMidGainDb{ 0.0f };
    std::atomic<float> eqHfGainDb { 0.0f };

    // Bus compressor.
    std::atomic<bool>  compEnabled   { false };
    std::atomic<float> compThreshDb  { 0.0f };    // -30..0
    std::atomic<float> compRatio     { 4.0f };    // 1..10
    std::atomic<float> compAttackMs  { 10.0f };
    std::atomic<float> compReleaseMs { 100.0f };
    // When true, the UniversalCompressor Bus mode runs its hardware-style
    // program-dependent release (150-450 ms) and the explicit release knob
    // is ignored. Default true because the donor's bus-comp choice param
    // clamps anything outside its discrete set to "Auto" anyway, so this
    // matches what existing sessions have actually been hearing.
    std::atomic<bool>  compReleaseAuto { true };
    std::atomic<float> compMakeupDb  { 0.0f };

    // Metering - written from the audio thread, read from UI. mutable for
    // const-DSP-pointer write semantics (same pattern as MasterBusParams).
    // Stereo L+R for the bus LED meter. dB atoms are peak-of-block, used by
    // LED meters. RMS atoms are linear amplitude smoothed at 300 ms tau on
    // the audio thread, used by the analog VU - same signal TapeMachine's
    // VU integrates internally so mixer + TapeMachine VUs track together.
    mutable std::atomic<float> meterPostBusLDb  { -100.0f };
    mutable std::atomic<float> meterPostBusRDb  { -100.0f };
    mutable std::atomic<float> meterPostBusRmsL { 0.0f };
    mutable std::atomic<float> meterPostBusRmsR { 0.0f };
    mutable std::atomic<float> meterGrDb        { 0.0f };
};

struct Bus
{
    juce::String name;
    juce::Colour colour;
    BusParams strip;
};

// AUX return lane. Distinct from Bus: a Bus is a subgroup that channels
// route INTO via the kNumBuses busAssign toggles (a 1/2/3/4 button group on
// each strip), while an AuxLane is an FX return that channels SEND TO via
// the kNumAuxSends auxSendDb knobs. Each lane hosts up to kMaxLanePlugins
// plugin slots (typically a reverb / delay) and mixes its processed output
// into the master after the bus pass.
struct AuxLaneParams
{
    // Single plugin per lane. The whole point of the AUX stage is to show
    // the plugin's full UI at a comfortable size; doubling up halves that
    // budget and was already squeezing fixed-size plugin editors.
    static constexpr int kMaxLanePlugins = 1;

    std::atomic<float> returnLevelDb { 0.0f };   // -inf via kFaderInfThreshDb sentinel
    std::atomic<bool>  mute           { false };

    // Live return-level + mute AFTER automation routing. Same pattern as
    // ChannelStripParams::liveFaderDb / liveMute: the audio thread reads
    // THESE (not returnLevelDb / mute) so the Off/Read/Write/Touch
    // hand-off happens in the engine's per-block routing block, with
    // AuxLaneStrip staying lane-agnostic. `mutable` so write-by-DSP
    // semantics still work through a const AuxLaneParams*.
    mutable std::atomic<float> liveReturnLevelDb { 0.0f };
    mutable std::atomic<bool>  liveMute          { false };

    // Touch-mode latch: true while the user has the fader slider grabbed.
    // Audio thread reads the lane normally except when faderTouched is
    // true, in which case it falls through to the manual returnLevelDb
    // and the UI captures into the lane during the grab.
    std::atomic<bool> faderTouched { false };

    // Per-aux automation engagement + lanes. Same shape as ChannelStripParams
    // (kNumAutomationParams slots) for code reuse, but only FaderDb + Mute
    // get populated — pan, solo, and aux sends aren't aux-bus concepts.
    std::atomic<int> automationMode { 0 };  // AutomationMode cast to int
    std::array<AutomationLane, kNumAutomationParams> automationLanes {};

    // Stereo output meters - mutable for write-by-DSP semantics.
    mutable std::atomic<float> meterPostL { -100.0f };
    mutable std::atomic<float> meterPostR { -100.0f };
};

// Timeline marker. Owned exclusively by the message thread; the audio
// thread never reads markers (it only cares about playhead, loop, punch).
// Markers are kept sorted by timelineSamples for predictable iteration in
// the ruler painter and for binary-search hit-testing.
struct Marker
{
    juce::String name;
    juce::int64  timelineSamples = 0;
    juce::Colour colour;
};

struct AuxLane
{
    juce::String name;
    juce::Colour colour;
    AuxLaneParams params;

    // Per-slot plugin state. Populated by AudioEngine::publishPluginStateForSave
    // before serialisation, consumed by consumePluginStateAfterLoad. Empty
    // strings = no plugin loaded in that slot.
    std::array<juce::String, AuxLaneParams::kMaxLanePlugins> pluginDescriptionXml;
    std::array<juce::String, AuxLaneParams::kMaxLanePlugins> pluginStateBase64;

    // Hardware-insert side of each lane slot. Parallel to the plugin
    // state above - the slot's audio mode (Plugin vs Hardware) lives on
    // the strip; Session just persists the parameter values so a saved
    // hardware-insert configuration survives a reload.
    std::array<HardwareInsertParams, AuxLaneParams::kMaxLanePlugins> hardwareInserts;
};

struct MasterBusParams
{
    std::atomic<float> faderDb     { 0.0f };

    // Live master fader AFTER automation routing. MasterBus DSP reads
    // THIS (not faderDb) so the Off/Read/Write/Touch hand-off lives in
    // the engine. `mutable` keeps the write-by-DSP path clean.
    mutable std::atomic<float> liveFaderDb { 0.0f };

    // Touch-mode latch on the master fader slider.
    std::atomic<bool> faderTouched { false };

    // Master-bus automation engagement + lanes. Spec only lists the
    // master fader as automatable; the lanes array reuses the per-track
    // shape for symmetry but only FaderDb is meaningful here.
    std::atomic<int> automationMode { 0 };  // AutomationMode cast to int
    std::array<AutomationLane, kNumAutomationParams> automationLanes {};

    // Tape on/off + HQ. Internal saturation drive / bias / formulation are
    // fixed in the DSP; the user toggles only the engagement and the HQ
    // (4x oversampling) flag. Mirror the TapeMachine plugin's preset values
    // requested in the spec.
    std::atomic<bool>  tapeEnabled { false };
    std::atomic<bool>  tapeHQ      { false };

    // TapeMachine plugin state (APVTS XML, base64-encoded). Populated by
    // AudioEngine::publishPluginStateForSave from the live processor's
    // getStateInformation() and consumed by consumePluginStateAfterLoad
    // back into setStateInformation. Empty string = use the donor's
    // built-in defaults. Mirrors the per-track / per-aux-slot string.
    juce::String tapeStateBase64;

    // Pultec-style Tube EQ ("Pultec" in the spec, exposed as "Tube" mode in
    // multi-q). Minimal control surface: LF boost, HF boost, tube drive,
    // output gain - the four most musical Pultec controls - plus enable.
    // Frequencies and bandwidth are set to musical defaults internally.
    std::atomic<bool>  eqEnabled       { false };
    std::atomic<float> eqLfBoost       { 0.0f };   // 0..10 (Pultec gain knob)
    std::atomic<float> eqHfBoost       { 0.0f };   // 0..10
    std::atomic<float> eqHfAtten       { 0.0f };   // 0..10 (HF cut shelf)
    std::atomic<float> eqTubeDrive     { 0.3f };   // 0..1 (saturation amount)
    std::atomic<float> eqOutputGainDb  { 0.0f };   // -12..+12 dB

    // Master bus compressor - UniversalCompressor in Bus mode (mode index 3).
    // Minimal control surface mirroring a stock SSL-style bus comp.
    std::atomic<bool>  compEnabled    { false };
    std::atomic<float> compThreshDb   { 0.0f };    // -30..0
    std::atomic<float> compRatio      { 4.0f };    // 1..10
    std::atomic<float> compAttackMs   { 10.0f };   // 0.1..50
    std::atomic<float> compReleaseMs  { 100.0f };  // 50..1000
    // See BusParams::compReleaseAuto for why this defaults to true.
    std::atomic<bool>  compReleaseAuto { true };
    std::atomic<float> compMakeupDb   { 0.0f };    // -10..+20

    // Metering - written from the audio thread each block, polled by UI.
    // `mutable` so DSP code holding a `const MasterBusParams*` can update
    // these even through a const surface (write-by-DSP semantics). Stereo
    // L+R so the master LED can render two channels side by side. dB atoms
    // are peak-of-block (LED meter); RMS atoms are linear amplitude
    // smoothed at 300 ms tau on the audio thread (analog VU - matches the
    // TapeMachine plugin's internal VU integrator so meters track together).
    mutable std::atomic<float> meterPostMasterLDb  { -100.0f };
    mutable std::atomic<float> meterPostMasterRDb  { -100.0f };
    mutable std::atomic<float> meterPostMasterRmsL { 0.0f };
    mutable std::atomic<float> meterPostMasterRmsR { 0.0f };
    mutable std::atomic<float> meterGrDb           { 0.0f };
};

// Mastering-stage chain parameters. Independent of MasterBusParams so the
// user can dial in different EQ / comp on the bounced mix vs. the live mix.
// Persisted to / from session.json. Source file is the WAV being mastered;
// the path is absolute so projects moved across machines need a relink.
struct MasteringParams
{
    juce::File sourceFile;  // empty when no mix has been loaded yet

    // Five-band digital EQ for the mastering stage. Band 0 is a low shelf,
    // bands 1-3 are peaking bells, band 4 is a high shelf. Defaults match
    // MasteringDigitalEq::prepare() so loading a fresh session matches
    // the DSP's idle state.
    static constexpr int kNumEqBands = 5;
    std::atomic<bool>  eqEnabled       { false };
    std::atomic<float> eqBandFreq[kNumEqBands]  { 80.0f, 250.0f, 1000.0f, 4000.0f, 12000.0f };
    std::atomic<float> eqBandGainDb[kNumEqBands]{ 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    std::atomic<float> eqBandQ[kNumEqBands]     { 0.7f, 1.0f, 1.0f, 1.0f, 0.7f };

    // Legacy Tube-EQ atomics - kept for session.json backward compatibility
    // (sessions saved before the digital-EQ swap). Not driven by the DSP
    // any longer.
    std::atomic<float> eqLfBoost       { 0.0f };
    std::atomic<float> eqHfBoost       { 0.0f };
    std::atomic<float> eqHfAtten       { 0.0f };
    std::atomic<float> eqTubeDrive     { 0.3f };
    std::atomic<float> eqOutputGainDb  { 0.0f };

    // Bus comp - same shape as MasterBusParams. Mastering tends to use
    // slower release + softer ratio; defaults reflect that.
    std::atomic<bool>  compEnabled    { false };
    std::atomic<float> compThreshDb   { 0.0f };
    std::atomic<float> compRatio      { 2.0f };
    std::atomic<float> compAttackMs   { 30.0f };
    std::atomic<float> compReleaseMs  { 250.0f };
    // See BusParams::compReleaseAuto.
    std::atomic<bool>  compReleaseAuto { true };
    std::atomic<float> compMakeupDb   { 0.0f };

    // Brickwall limiter (final stage). Default ceiling -0.3 dB matches
    // common streaming-platform headroom.
    std::atomic<bool>  limiterEnabled  { true };
    std::atomic<float> limiterDriveDb  { 0.0f };
    std::atomic<float> limiterCeilingDb{ -0.3f };
    std::atomic<float> limiterReleaseMs{ 100.0f };

    // Output meters - mutable for write-by-DSP semantics. dB atoms = peak
    // (LED), RMS atoms = linear amplitude smoothed at 300 ms tau on the
    // audio thread (analog VU - matches TapeMachine's internal integrator).
    mutable std::atomic<float> meterPostMasterLDb  { -100.0f };
    mutable std::atomic<float> meterPostMasterRDb  { -100.0f };
    mutable std::atomic<float> meterPostMasterRmsL { 0.0f };
    mutable std::atomic<float> meterPostMasterRmsR { 0.0f };
    mutable std::atomic<float> meterCompGrDb     { 0.0f };
    mutable std::atomic<float> meterLimiterGrDb  { 0.0f };

    // BS.1770 / EBU R128 loudness - written from the mastering chain's
    // post-limiter LoudnessMeter. M = momentary (400 ms), S = short-term
    // (3 s), I = integrated (program-wide, gated). True peak is in dBTP
    // (4× oversampled per ITU BS.1770 Annex 2).
    mutable std::atomic<float> meterMomentaryLufs   { -100.0f };
    mutable std::atomic<float> meterShortTermLufs   { -100.0f };
    mutable std::atomic<float> meterIntegratedLufs  { -100.0f };
    mutable std::atomic<float> meterTruePeakDb      { -100.0f };

    // Streaming-platform target preset. Drives the colour-coding of the
    // I-LUFS / true-peak cells in MasteringView. 0 = Off (neutral display);
    // see kMasteringTargets in RegionEditActions.cpp / MasteringView for
    // the canonical preset table. Persisted with the session.
    std::atomic<int> targetPresetIndex { 0 };
};

class Session
{
public:
    static constexpr int kNumTracks   = 16;
    static constexpr int kNumBuses    = 4;
    static constexpr int kNumAuxLanes = 4;   // matches ChannelStripParams::kNumAuxSends
    static constexpr int kBankSize    = 8;
    static constexpr int kNumBanks    = kNumTracks / kBankSize;

    Session();

    juce::File getSessionDirectory() const noexcept { return sessionDir; }
    juce::File getAudioDirectory()   const noexcept { return sessionDir.getChildFile ("audio"); }
    void setSessionDirectory (const juce::File& dir);

    Track& track (int i) noexcept             { jassert (i >= 0 && i < kNumTracks);   return tracks[(size_t) i]; }
    const Track& track (int i) const noexcept { jassert (i >= 0 && i < kNumTracks);   return tracks[(size_t) i]; }

    Bus& bus (int i) noexcept              { jassert (i >= 0 && i < kNumBuses); return buses[(size_t) i]; }
    const Bus& bus (int i) const noexcept  { jassert (i >= 0 && i < kNumBuses); return buses[(size_t) i]; }

    AuxLane&       auxLane (int i)       noexcept { jassert (i >= 0 && i < kNumAuxLanes); return auxLanes[(size_t) i]; }
    const AuxLane& auxLane (int i) const noexcept { jassert (i >= 0 && i < kNumAuxLanes); return auxLanes[(size_t) i]; }

    // Markers - message-thread-only timeline labels. addMarker returns the
    // inserted index; removeMarker / renameMarker are no-ops on out-of-
    // range indices. findMarkerNear returns -1 when nothing is within the
    // tolerance window. Markers are kept sorted by timelineSamples so the
    // ruler painter can iterate left-to-right without an extra sort.
    std::vector<Marker>&       getMarkers()       noexcept { return markers; }
    const std::vector<Marker>& getMarkers() const noexcept { return markers; }
    int  addMarker     (juce::int64 timelineSamples, const juce::String& name = {});
    void removeMarker  (int index);
    void renameMarker  (int index, const juce::String& name);
    int  findMarkerNear (juce::int64 timelineSamples, juce::int64 toleranceSamples) const noexcept;

    MasterBusParams& master() noexcept             { return masterParams; }
    const MasterBusParams& master() const noexcept { return masterParams; }

    MasteringParams&       mastering() noexcept       { return masteringParams; }
    const MasteringParams& mastering() const noexcept { return masteringParams; }

    // Audio-thread "any soloed / armed?" queries. O(1) - back-by atomic
    // counters that the UI bumps via the setters below. Bulk-write paths
    // (SessionSerializer / self-test) write the underlying atoms directly,
    // then call recomputeRtCounters() to resync. Without the counters these
    // queries scanned 16 / 4 / 16 atoms every callback before the per-track
    // loop even started.
    bool anyTrackSoloed() const noexcept;
    bool anyBusSoloed()   const noexcept;
    bool anyTrackArmed()  const noexcept;

    // Counter-aware setters. Atomically toggle the bool AND adjust the
    // corresponding count, so anyXSoloed() stays correct without scans.
    // No-op when the value didn't actually change. Call from message thread.
    void setTrackSoloed (int trackIndex, bool soloed) noexcept;
    void setBusSoloed   (int busIndex,   bool soloed) noexcept;
    void setTrackArmed  (int trackIndex, bool armed)  noexcept;

    // Re-scan every track / aux atom and rebuild the counters. Call after a
    // bulk path that wrote the atoms directly (session load, self-test) so
    // the audio thread's any-X-soloed reads are correct on the next callback.
    void recomputeRtCounters() noexcept;

    // Persisted transport settings - mirrored to/from Transport's atomics
    // by AudioEngine::publishTransportStateForSave / consume so they round
    // trip through SessionSerializer. Plain (non-atomic) because they are
    // touched only on the message thread, between save/load and the
    // engine's bookend calls.
    juce::int64 savedLoopStart    = 0;
    juce::int64 savedLoopEnd      = 0;
    bool        savedLoopEnabled  = false;
    juce::int64 savedPunchIn      = 0;
    juce::int64 savedPunchOut     = 0;
    bool        savedPunchEnabled = false;

    // Snap-to-grid is two orthogonal fields by design, matching the
    // Ardour Snap toggle + Snap-denomination dropdown UX:
    //   snapToGrid     - master enable/disable. SnapHelpers no-ops when
    //                    false regardless of the resolution. Toggled by
    //                    the EditModeToolbar "Snap" button.
    //   snapResolution - which grid the snap rounds to (Bar / Half /
    //                    Quarter / triplets / dotted / Timecode / etc.).
    //                    Lives independently so toggling the master off
    //                    and back on preserves the user's denomination
    //                    pick. Toggled by the EditModeToolbar dropdown.
    // Both touched only on the message thread.
    bool           snapToGrid     = false;
    SnapResolution snapResolution = SnapResolution::Quarter;

    // Piano-roll "snap to scale degree" toggle. Persists across editor
    // reopens; touched only on the message thread. Defaults to ON.
    bool           pianoRollKeySnap = true;

    // Ardour-style edit mode. Determines what a click/drag on the arrange
    // surface (TapeStrip) or the region editor does. Mouse handlers in
    // both components branch on this at the top of mouseDown. Persisted
    // in the transport block of session.json so the user's mode stays
    // across sessions. Touched only on the message thread.
    EditMode editMode = EditMode::Grab;

    // Global oversampling factor applied to effects that support it (master
    // tape sat, master/aux bus comp). Per-channel comp + EQ run at native
    // rate regardless. 1 = native (default - lowest CPU), 2 = 2× ox, 4 =
    // 4× ox. Read by MasterBus and BusStrip in prepare(); changing it
    // requires a re-prepare (the AudioSettingsPanel triggers one via
    // setAudioDeviceSetup). Stored as an atomic<int> so future audio-thread
    // reads (e.g. for per-block reactive ox) are lock-free, but today only
    // the message-thread prepare path consults it.
    std::atomic<int> oversamplingFactor { 4 };

    // Tempo + transport-grid metadata. Persisted with the session.
    //   tempoBpm        - beats per minute. 0 disables beat-grid behavior
    //                      (metronome stays silent, snap falls back to seconds).
    //   beatsPerBar     - numerator of the time signature (e.g. 4 in 4/4).
    //   beatUnit        - denominator (4 = quarter, 8 = eighth, etc.).
    //   metronomeEnabled - click on/off.
    //   metronomeVolDb  - click level relative to master output (negative).
    // BPM and metronomeEnabled are atomics so the audio thread can pick up
    // tempo changes without a lock.
    std::atomic<float> tempoBpm          { 120.0f };
    std::atomic<int>   beatsPerBar       { 4 };
    std::atomic<int>   beatUnit          { 4 };
    std::atomic<bool>  metronomeEnabled  { false };
    std::atomic<float> metronomeVolDb    { -12.0f };

    // Time display mode: 0 = Bars / Beats / Tick (musical, default),
    // 1 = mm:ss.ms (wall-clock time). One mode applies to every
    // surface that shows a position — transport clock, TapeStrip
    // ruler, region editor rulers, status-bar readouts — so the
    // user sees one coherent representation throughout the app.
    std::atomic<int>   timeDisplayMode   { 0 };

    // Count-in (pre-roll). When true, hitting Record shifts the playhead
    // backwards by one bar so the metronome ticks for one bar before
    // capture begins. The captured WAV's first sample still maps to the
    // playhead position at Record-press, not the negative pre-roll start.
    std::atomic<bool>  countInEnabled    { false };

    // Transport cluster state (Tascam-style REW/FFWD).
    //   lastRecordPointSamples - timeline position the most-recent record
    //     pass STARTED at; the FFWD-while-stopped tap (= TO LAST REC on
    //     the DP-24SD) jumps to it. Default 0 = haven't recorded yet, so
    //     the first such tap just stays at zero (reads as "do nothing").
    std::atomic<juce::int64> lastRecordPointSamples { 0 };

    // Auto-punch pre-roll / post-roll. Active only when transport.punch
    // is enabled (otherwise the punch range is meaningless and these
    // settings stay dormant).
    //   preRollSeconds  - on Record-press, the playhead is rolled back
    //     this many seconds before punchIn. Existing material plays
    //     audibly during pre-roll; the audio callback's punch-window
    //     gate already prevents committing audio before punchIn.
    //   postRollSeconds - after the playhead crosses punchOut, the
    //     transport keeps rolling for this long, then auto-stops.
    //     0 = stay rolling indefinitely (matches the previous behaviour
    //     where punch never auto-stopped).
    std::atomic<float> preRollSeconds  { 0.0f };
    std::atomic<float> postRollSeconds { 0.0f };

    // Tuner. tuneTrackIndex = -1 disables; 0..15 selects which track's
    // device input feeds the pitch detector. tuneLatestHz / tuneLatestLevel
    // are written from the audio thread on every block (the detector runs
    // when an active track is selected) and polled by TunerOverlay's
    // 30 Hz timer to drive the note + cents readout. Not persisted - the
    // tuner is a transient utility, not a workflow setting.
    std::atomic<int>   tuneTrackIndex { -1 };
    std::atomic<float> tuneLatestHz   { 0.0f };
    std::atomic<float> tuneLatestLevel { 0.0f };

    // Currently-held MIDI notes across all inputs - the audio thread sets
    // a slot to true on Note On (vel > 0) and false on Note Off (or vel
    // 0 Note On). The chord-analyzer poll on the SystemStatusBar timer
    // walks the array, builds a vector, and runs ChordAnalyzer::analyze
    // off-thread. Per-note std::atomic<bool> rather than a single packed
    // word keeps the audio thread's update path lock-free without
    // needing CAS loops.
    static constexpr int kNumMidiNotes = 128;
    std::array<std::atomic<bool>, kNumMidiNotes> heldMidiNotes {};

    // MIDI controller bindings. Mutated only on the message thread (UI
    // learn-capture and explicit forget); audio thread reads it lock-free
    // via the AtomicSnapshot's acquire-load. Mutations build a fresh
    // vector copy and atomically swap it in (see TransportBar's Learn
    // capture timer and MidiBindings::showLearnMenu's Forget action).
    //
    // pendingTransportAction - audio thread sets this when a binding hits
    //   a transport target; TransportBar's 20 Hz timer drains it on the
    //   message thread (engine.play/stop/record aren't RT-safe).
    // midiLearnPending - UI sets a packed (target, index); audio thread
    //   captures the next matching MIDI source into midiLearnCapture;
    //   message-thread timer drains the capture and appends a binding.
    AtomicSnapshot<std::vector<MidiBinding>> midiBindings;
    std::atomic<int>         pendingTransportAction { (int) PendingTransportAction::None };
    std::atomic<int>         midiLearnPending       { -1 };
    std::atomic<juce::int64> midiLearnCapture       { 0 };

    // External MIDI sync. syncSourceInputIdx selects which entry in the
    // engine's midiInputDevices list drives clock chase; -1 = no sync,
    // tempo stays user-controlled. Persisted as a device identifier
    // string in session JSON; the engine resolves the string to an
    // index on load + on every hot-plug rebuild.
    // externalBpm is the smoothed BPM derived by MidiSyncReceiver. UI
    // polls this for the "EXT: N BPM" badge; the engine pushes the
    // value into tempoBpm when externalSyncFollowsTempo is true.
    // externalSyncRolling mirrors the master's Start/Stop transport
    // bytes (FA / FC). v1 is "tempo only" - the engine reads the BPM
    // but ignores the rolling flag.
    juce::String              syncSourceInputIdentifier;          // empty = off
    std::atomic<int>          syncSourceInputIdx      { -1 };
    mutable std::atomic<float> externalBpm            { 0.0f };
    mutable std::atomic<bool>  externalSyncRolling    { false };
    std::atomic<bool>          externalSyncFollowsTempo { true };
    // When true, FA/FB (Start/Continue) and FC (Stop) bytes drive the
    // engine's Transport state - Dusk Studio plays/stops in lock-step with
    // the external master. Off by default so v1's "tempo-only" mode
    // stays the baseline; user opts in via the Audio Settings panel.
    std::atomic<bool>          externalSyncChasesTransport { false };

    // MIDI Clock OUTPUT (master mode). syncOutputIdentifier picks one
    // entry from the engine's midiOutputDevices list; -1 idx = off.
    // syncOutputEmitClock toggles F8 + FA/FC emission to that port.
    // v1 emits to a single port; multi-port emission can layer on
    // later by widening syncOutputIdentifier to a comma-sep list.
    juce::String              syncOutputIdentifier;          // empty = off
    std::atomic<int>          syncOutputIdx        { -1 };
    std::atomic<bool>         syncOutputEmitClock  { false };

    // MTC (MIDI Time Code) — slave-side decoded state. Shares the
    // syncSourceInputIdx port with Clock (both QF and F8 multiplex on
    // the same input). externalTimeCodeFrames is the running absolute
    // SMPTE frame count from 00:00:00:00 (with the standard 2-frame
    // QF transmission-delay compensation already applied — i.e., this
    // matches what the master's display reads RIGHT NOW, not what its
    // QF sequence assembled 2 frames ago). externalTimeCodeReversed
    // is a UI cue when the master is scrubbing backward — chase
    // ignores this state (v1 doesn't support reverse chase).
    mutable std::atomic<juce::int64> externalTimeCodeFrames    { 0 };
    mutable std::atomic<bool>        externalTimeCodeRolling   { false };
    mutable std::atomic<bool>        externalTimeCodeReversed  { false };
    mutable std::atomic<int>         externalTimeCodeFrameRate { 3 };  // 0=24, 1=25, 2=29.97DF, 3=30

    // When true AND externalTimeCodeRolling is true, the audio engine
    // drives Transport state to follow the MTC master. Initial lock
    // on rolling-edge, freewheel within tolerance, soft re-locate on
    // sustained drift, stop on rolling-edge false. Off by default;
    // user opts in via the Audio Settings panel.
    std::atomic<bool> externalTimeCodeChasesTransport { false };

    // Pending transport playhead set, drained on the message thread.
    // -1 = nothing pending. Audio thread writes a target sample value;
    // a message-thread timer drains it, calls Transport::setPlayhead,
    // and stores -1 back. Mirrors pendingTransportAction's dispatch
    // pattern — Transport mutators aren't RT-safe.
    std::atomic<juce::int64> pendingTransportPlayhead { -1 };

    // MTC master-emit toggles. Reuses the existing syncOutputIdx /
    // syncOutputIdentifier port — Clock + MTC multiplex on the same
    // MidiOutput. syncOutputTimeCodeFrameRate selects the SMPTE rate
    // we encode (24 / 25 / 29.97DF / 30). syncOutputEmitTimeCode
    // toggles QF + full-frame sysex emission.
    std::atomic<bool> syncOutputEmitTimeCode     { false };
    std::atomic<int>  syncOutputTimeCodeFrameRate { 3 };

    // Resolve the audio device input channel that this track should read from.
    // -2 (default) means "follow the track index", -1 means "no input".
    int resolveInputForTrack (int trackIndex) const noexcept;
    // R-channel input resolver - mirrors resolveInputForTrack but reads
    // inputSourceR. Returns -1 when no R input is configured, or in Mono /
    // Midi mode (where the second channel is meaningless).
    int resolveInputRForTrack (int trackIndex) const noexcept;

private:
    std::array<Track, kNumTracks> tracks;
    std::array<Bus, kNumBuses> buses;
    std::array<AuxLane, kNumAuxLanes> auxLanes;
    std::vector<Marker> markers;
    MasterBusParams masterParams;
    MasteringParams masteringParams;
    juce::File sessionDir;

    // RT-perf: counter-backed any-X-soloed / -armed queries. Each is
    // incremented when a flag goes false->true and decremented true->false
    // by the matching setter; the audio thread does a single relaxed load
    // per callback instead of scanning all 16 / 4 atoms.
    std::atomic<int> soloTrackCount { 0 };
    std::atomic<int> soloBusCount   { 0 };
    std::atomic<int> armedTrackCount { 0 };
};
} // namespace duskstudio
