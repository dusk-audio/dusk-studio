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
// Console-style automation. Performance gestures in Write/Touch, replay
// in Read — no curve drawing UI (CLAUDE.md constraint #4). Values stored
// normalized 0..1; per-param normalize/denormalize bridges to natural
// range (dB / pan / 0|1).
enum class AutomationMode : int { Off = 0, Read = 1, Write = 2, Touch = 3 };

// EQ, Comp, HPF and bus assigns are deliberately NOT automatable per
// DuskStudio.md — automation is for dynamics gestures, not tone shaping.
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

// Continuous params interp + RDP-thin; discrete (mute/solo) skip both —
// intermediate values break the binary semantic.
constexpr bool isContinuousParam (AutomationParam p) noexcept
{
    return p != AutomationParam::Mute && p != AutomationParam::Solo;
}

struct AutomationPoint
{
    juce::int64 timeSamples   = 0;
    float       value         = 0.0f;     // 0..1
    float       recordedAtBPM = 120.0f;
};

// Points sorted by timeSamples; binary search at lookup. Plain vector —
// session load (transport stopped) is the only mutator the audio thread
// races. Write mode (3c-ii) will swap via atomic ptr.
struct AutomationLane
{
    std::vector<AutomationPoint> points;
};

// Linear interp between bracketing points for continuous; step (hold
// previous) for discrete. Out-of-range holds the first / last value.
// Returns 0 on empty lane — callers gate on lane.points.empty() first.
float evaluateLane (const AutomationLane& lane, juce::int64 t,
                    AutomationParam param) noexcept;

float denormalizeAutomationValue (AutomationParam p, float v01) noexcept;
float normalizeAutomationValue   (AutomationParam p, float denormValue) noexcept;

// RDP thin in normalized 0..1 units (range-independent). 0.002 = 0.2%
// of vertical, the production default. No-op for discrete params.
// Synchronous — 10-min Write pass (~18k points) runs in < 1 ms.
void thinAutomationLane (AutomationLane& lane,
                            AutomationParam param,
                            double epsilon) noexcept;

class Session;
// Walks every lane (track / aux / master) and thins. UNSAFE concurrent
// with audio-thread lane reads; caller guarantees transport stopped +
// every automationMode == Off. Today utility-grade; future swap to
// AtomicSnapshot per lane would lift this.
void handleWritePassComplete (Session& s) noexcept;

// Retime every MidiRegion for a BPM change. Publish order to the audio
// thread is (1) per-track midiRegions republished, then (2)
// tempoBpm.store(release). Audio thread acquire-loads tempoBpm, so
// observing the new BPM implies new region positions are visible. The
// reverse window (new regions while BPM still old) is tens of ns
// between the two stores; spec requires transport stopped for BPM
// changes so the transient is accepted for 1.0.
//
// tempoLock=true  : timelineStart scales by old/new ratio,
//                   lengthInSamples re-derived from lengthInTicks.
//                   Musical position + duration preserved.
// tempoLock=false : sample positions stay, lengthInTicks re-derived
//                   from lengthInSamples. Region "floats" on the
//                   new grid (musical extent changes).
//
// Audio regions + markers are NOT retimed (no time-stretching per spec).
// Message-thread only. sampleRate <= 0 skips recompute (BPM still stores).
void applyTempoChange (Session& s, float newBpm, double sampleRate) noexcept;

// External hardware insert routing. Group changes together so the
// audio thread sees a consistent snapshot.
struct HardwareInsertRouting
{
    int outputChL      = -1;   // -1 = unassigned (SEND disabled)
    int outputChR      = -1;
    int inputChL       = -1;   // -1 = unassigned (RETURN silent)
    int inputChR       = -1;
    int latencySamples =  0;
    int format         =  0;   // 0 = Stereo, 1 = Mid/Side
};

// Slot is "plugin OR hardware insert", live mode chosen by atomic
// insertMode on the strip. Plain atomic loads on scalar knobs;
// AtomicSnapshot acquire on routing.
struct HardwareInsertParams
{
    std::atomic<bool>                     enabled { false };
    AtomicSnapshot<HardwareInsertRouting> routing;

    std::atomic<float> outputGainDb { 0.0f };   // SEND trim
    std::atomic<float> inputGainDb  { 0.0f };   // RETURN trim
    std::atomic<float> dryWet       { 1.0f };   // 0 = dry only, 1 = wet only

    // Ping handshake. UI sets pingPending; audio runs chirp + capture +
    // correlation, writes pingResult, clears pingPending. mutable so the
    // audio thread can update through a const ref.
    // -1 = correlation below threshold; -2 = capture stall; >=0 = lag.
    mutable std::atomic<bool> pingPending { false };
    mutable std::atomic<int>  pingResult  { -1 };
};

struct ChannelStripParams
{
    std::atomic<float> faderDb { 0.0f };   // -inf via -100 sentinel, +12 dB max
    std::atomic<float> pan     { 0.0f };   // -1..+1
    std::atomic<bool>  mute    { false };
    std::atomic<bool>  solo    { false };
    std::atomic<bool>  phaseInvert { false };

    // 0 = ungrouped; 1..N = group members. Drag deltas apply across
    // group; anchor values captured at drag-start preserve relative
    // offsets.
    std::atomic<int>   faderGroupId { 0 };

    // Bus assigns are EXCLUSIVE with the master send — see
    // ChannelStrip::processAndAccumulate for the routing (toMaster
    // gate by 1-maxBusG). Without this exclusivity the signal would
    // arrive at master twice (direct + via bus) and double by +3 dB.
    static constexpr int kNumBuses = 4;
    std::array<std::atomic<bool>, kNumBuses> busAssign {};

    // AUX sends feed the kNumAuxLanes aux strips' plugin chains.
    // kAuxSendOffDb is a sentinel: knob fully CCW, audio thread
    // short-circuits.
    static constexpr int   kNumAuxSends = 4;
    static constexpr float kAuxSendMinDb = -60.0f;
    static constexpr float kAuxSendMaxDb =   6.0f;
    static constexpr float kAuxSendOffDb = -100.0f;
    std::array<std::atomic<float>, kNumAuxSends> auxSendDb {
        std::atomic<float>{ kAuxSendOffDb }, std::atomic<float>{ kAuxSendOffDb },
        std::atomic<float>{ kAuxSendOffDb }, std::atomic<float>{ kAuxSendOffDb }
    };
    std::array<std::atomic<bool>, kNumAuxSends> auxSendPreFader {};

    // 4K-style EQ: HPF + 4-band parametric + LPF + Brown/Black mode.
    std::atomic<bool>  hpfEnabled { false };
    std::atomic<float> hpfFreq    { 20.0f };
    std::atomic<bool>  lpfEnabled { false };
    std::atomic<float> lpfFreq    { 20000.0f };
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
    // Auto-arms on first knob adjustment.
    std::atomic<bool>  eqEnabled   { false };

    // Comp: each mode (Opto / FET / VCA) keeps its own atomics and the
    // UI swaps visible controls.
    std::atomic<bool>  compEnabled    { false };
    std::atomic<int>   compMode       { 0 };       // 0=Opto, 1=FET, 2=VCA
    // True once the user picks a mode — UI shows "COMP" otherwise so
    // first-time users see a clear "click here" affordance. DSP still
    // uses compMode regardless.
    std::atomic<bool>  compModePicked { false };

    // Opto (LA-2A style).
    std::atomic<float> compOptoPeakRed { 0.0f };   // 0..100 %
    std::atomic<float> compOptoGain    { 50.0f };  // 0..100 % (50 = unity)
    std::atomic<bool>  compOptoLimit   { false };

    // FET (1176 style).
    std::atomic<float> compFetInput   { 0.0f };    // -20..40 dB (drive into detection)
    std::atomic<float> compFetOutput  { 0.0f };    // -20..20 dB
    std::atomic<float> compFetAttack  { 0.2f };    // 0.02..80 ms
    std::atomic<float> compFetRelease { 400.0f };  // 50..1100 ms
    std::atomic<int>   compFetRatio   { 0 };       // 0=4:1, 1=8:1, 2=12:1, 3=20:1, 4=All
    // Adjustable FET threshold — donor's original was a hardcoded -10
    // dBFS; defaulting here to the same value preserves saved-state
    // behaviour for sessions written before fet_threshold existed.
    std::atomic<float> compFetThresholdDb { -10.0f }; // -60..0 dB

    // VCA (textbook).
    std::atomic<float> compVcaThreshDb { 12.0f };   // -38..12 dB
    std::atomic<float> compVcaRatio    { 4.0f };    // 1..120
    std::atomic<float> compVcaAttack   { 1.0f };    // 0.1..50 ms
    std::atomic<float> compVcaRelease  { 100.0f };  // 10..5000 ms
    std::atomic<float> compVcaOutput   { 0.0f };    // -20..20 dB
    // dbx 160X OverEasy parabolic soft knee.
    std::atomic<bool>  compVcaOverEasy { false };
    // false = Adaptive (donor's 35→5 ms RMS TC); true = Classic (fixed 10 ms).
    std::atomic<bool>  compVcaDetectorClassic { false };

    // Legacy unified threshold — driven by the comp-meter drag handle,
    // mirrors the current mode's primary knob so drag stays usable.
    // Audio thread does NOT read directly; see ChannelStrip::updateCompParameters.
    std::atomic<float> compThresholdDb { 0.0f };

    // FET threshold reads this back when recomputing compFetOutput so
    // threshold + makeup compose (without it, threshold's chain
    // compensation overwrites makeup). VCA / Opto write here for
    // consistency / save state.
    std::atomic<float> compMakeupDb    { 0.0f };

    // Engine writes liveFaderDb at the top of every block: Off mode
    // mirrors faderDb; Read mode carries lane value. ChannelStrip
    // reads liveFaderDb, not faderDb, so Off/Read hand-off lives in
    // the engine. UI's fader timer polls this for free motor-fader
    // animation in Read mode. mutable so const-ref writes work.
    mutable std::atomic<float> liveFaderDb { 0.0f };

    // True while the user has the fader grabbed (drag-start..end).
    // Touch mode: audio reads the lane normally; switches to manual
    // faderDb when faderTouched. On release the smoother's 20 ms
    // ramp glides back to lane value.
    std::atomic<bool> faderTouched { false };

    mutable std::atomic<float> livePan { 0.0f };
    std::atomic<bool> panTouched { false };

    mutable std::array<std::atomic<float>, kNumAuxSends> liveAuxSendDb {
        std::atomic<float>{ kAuxSendOffDb }, std::atomic<float>{ kAuxSendOffDb },
        std::atomic<float>{ kAuxSendOffDb }, std::atomic<float>{ kAuxSendOffDb }
    };
    std::array<std::atomic<bool>, kNumAuxSends> auxSendTouched {};

    // Discrete — no touched flag (clicks are instantaneous). Touch
    // mode = read lane, any click writes a transition. Session::
    // anyTrackSoloed scans liveSolo so automated solos trigger the
    // global SIP-mute.
    mutable std::atomic<bool> liveMute { false };
    mutable std::atomic<bool> liveSolo { false };

    static constexpr float kFaderMinDb       = -100.0f;
    static constexpr float kFaderMaxDb       =  12.0f;
    static constexpr float kFaderInfThreshDb = -90.0f;  // below this = hard mute

    static constexpr float kHpfMinHz   = 20.0f;
    static constexpr float kHpfMaxHz   = 300.0f;
    static constexpr float kHpfOffHz   = 20.0f;
    static constexpr float kLpfMinHz   = 3000.0f;
    static constexpr float kLpfMaxHz   = 20000.0f;
    static constexpr float kLpfOffHz   = 20000.0f;
    static constexpr float kLfFreqMin  = 20.0f,   kLfFreqMax = 400.0f;
    static constexpr float kLmFreqMin  = 100.0f,  kLmFreqMax = 4000.0f;
    static constexpr float kHmFreqMin  = 600.0f,  kHmFreqMax = 13000.0f;
    static constexpr float kHfFreqMin  = 1000.0f, kHfFreqMax = 20000.0f;
    static constexpr float kBandGainMin = -15.0f, kBandGainMax = 15.0f;
    static constexpr float kBandQMin = 0.4f, kBandQMax = 4.0f;

    static constexpr float kCompThreshMin =  -60.0f, kCompThreshMax =   0.0f;
    static constexpr float kCompRatioMin  =    1.0f, kCompRatioMax  =  20.0f;
    static constexpr float kCompAttackMin =    0.1f, kCompAttackMax = 200.0f;
    static constexpr float kCompReleaseMin=   10.0f, kCompReleaseMax= 2000.0f;
    static constexpr float kCompMakeupMin =  -12.0f, kCompMakeupMax =  24.0f;
};

// Take-history slot — timeline position is NOT stored so rotating
// preserves the region's timelineStart (same spot in the song).
struct TakeRef
{
    juce::File file;
    juce::int64 sourceOffset    = 0;
    juce::int64 lengthInSamples = 0;
};

// 480 PPQN matches every modern DAW + .mid convention; high enough
// that 64th-note triplets quantize exactly (480/24=20). BPM changes
// rebuild the sample mapping; tick positions on events stay stable.
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

enum class TimeDisplayMode : int { Bars = 0, Time = 1 };

// Ardour-style. Stretch is deliberately absent — DuskStudio.md forbids
// time-stretching.
enum class EditMode : int
{
    Grab  = 0,   // default: click = cursor, body drag = move region
    Range = 1,   // body drag = time-range selection
    Cut   = 2,   // single click on body = split at cursor
    Grid  = 3,   // ruler click adds/edits tempo-map event
    Draw  = 4    // MIDI pencil / audio gain breakpoints
};

// Two orthogonal controls: session.snapToGrid (on/off) and
// session.snapResolution (grid size). SnapHelpers no-op when off
// regardless of resolution. Triplets are ×2/3 of the base, dotted ×3/2.
// Non-musical modes (Timecode/MinSec/CDFrames) ignore tempo.
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

// Every surface displaying a position routes through this so flipping
// Session::timeDisplayMode swaps the whole app atomically.
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

// Off events folded into lengthInTicks — no dangling on/off bookkeeping.
// Negative or zero length = "all-notes-off across region" sentinel.
struct MidiNote
{
    int  channel       = 1;     // 1..16
    int  noteNumber    = 60;    // 0..127
    int  velocity      = 100;   // 1..127 (recorded notes always >= 1)
    juce::int64 startTick     = 0;
    juce::int64 lengthInTicks = 0;
};

// `controller` doubles as message-type discriminator: 0..127 = CC.
// Sentinels for pitch-bend / aftertouch land when piano roll surfaces them.
struct MidiCc
{
    int  channel    = 1;
    int  controller = 64;       // sustain pedal
    int  value      = 0;
    juce::int64 atTick = 0;
};

struct MidiTakeRef
{
    juce::int64           lengthInTicks = 0;
    std::vector<MidiNote> notes;
    std::vector<MidiCc>   ccs;
};

struct MidiRegion
{
    juce::int64 timelineStart    = 0;
    juce::int64 lengthInSamples  = 0;   // cached at session tempo
    juce::int64 lengthInTicks    = 0;   // source of truth for musical length

    std::vector<MidiNote> notes;
    std::vector<MidiCc>   ccs;

    // Front = next to surface on cycle. Same semantics as AudioRegion.
    std::vector<MidiTakeRef> previousTakes;

    juce::Colour customColour;
    juce::String label;

    bool muted = false;
    bool locked = false;

    // tempoLock=true: musical position + length preserved across BPM
    // change (lengthInSamples re-derived from lengthInTicks).
    // tempoLock=false: sample positions preserved (region floats on
    // the new tempo grid).
    bool   tempoLock     = true;

    // Conversion anchor for tempo-locked retime.
    double recordedAtBPM = 120.0;
};

// All shapes: shape(0)=0, shape(1)=1. EqualPower is constant-power for
// crossfades. RaisedCosine has zero slope at both endpoints — the right
// choice for very-short click-mask fades (punch in/out).
enum class FadeShape : int
{
    Linear      = 0,
    EqualPower  = 1,
    Sigmoid     = 2,
    Exp         = 3,
    Log         = 4,
    RaisedCosine = 5
};

// Used by PlaybackEngine for audio + AudioRegionEditor for envelope
// painting — keep in sync.
inline float applyFadeShape (float t, FadeShape s) noexcept
{
    t = juce::jlimit (0.0f, 1.0f, t);
    switch (s)
    {
        case FadeShape::Linear:      return t;
        case FadeShape::EqualPower:  return std::sin (t * juce::MathConstants<float>::halfPi);
        case FadeShape::Sigmoid:     return t * t * (3.0f - 2.0f * t);
        case FadeShape::Exp:         return t * t;
        case FadeShape::Log:         return 1.0f - (1.0f - t) * (1.0f - t);
        case FadeShape::RaisedCosine:
            return 0.5f * (1.0f - std::cos (t * juce::MathConstants<float>::pi));
    }
    return t;
}

struct AudioRegion
{
    juce::File file;
    juce::int64 timelineStart = 0;
    juce::int64 lengthInSamples = 0;
    juce::int64 sourceOffset = 0;
    int          numChannels    = 1;  // 1 = mono WAV, 2 = stereo WAV

    // Invariants enforced by RecordManager + defensive re-clamp in
    // PlaybackEngine::preparePlayback:
    //   fadeInSamples >= 0 && fadeOutSamples >= 0
    //   fadeInSamples + fadeOutSamples <= lengthInSamples
    // Second invariant prevents the ramps overlapping mid-region and
    // multiplying to a notch instead of a flat 1.0 hold.
    juce::int64 fadeInSamples  = 0;
    juce::int64 fadeOutSamples = 0;

    FadeShape fadeInShape  = FadeShape::Linear;
    FadeShape fadeOutShape = FadeShape::Linear;

    // Auto-fade set by the editor's auto-crossfade path (move-drag
    // produces overlap). Next move re-syncs to current overlap and
    // clears entirely when overlap is gone. Cleared by manual handle
    // drag / explicit shape pick / Reset fades — those promote the
    // fade to user-owned.
    bool fadeInAuto  = false;
    bool fadeOutAuto = false;

    // dB. Applied multiplicatively in PlaybackEngine BEFORE strip
    // processing so strip EQ/comp sees the user's chosen level.
    float gainDb = 0.0f;

    // Default-constructed (transparent) = "use track colour".
    juce::Colour customColour;
    juce::String label;

    // Audio still on disk + other state (fades, gain, edits) preserved.
    // Takes effect on next preparePlayback (stop+play); painter dims
    // immediately.
    bool muted = false;

    // Rejects drag/resize/delete/split/nudge/gain ops. Doesn't affect
    // playback. Right-click menu toggles; painter shows lock badge.
    bool locked = false;

    // Front = next to surface on cycle.
    std::vector<TakeRef> previousTakes;
};

struct Track
{
    // Mono   : 1 audio in -> mono WAV -> mono playback
    // Stereo : L+R inputs -> 2-ch WAV -> stereo playback
    // Midi   : MIDI in -> .mid -> drives the strip's hosted plugin
    enum class Mode : int { Mono = 0, Stereo = 1, Midi = 2 };

    juce::String name;
    juce::Colour colour;
    ChannelStripParams strip;

    // The slot's audio mode (Plugin vs Hardware) lives on the strip;
    // this is just the param state, zero-cost-when-idle.
    HardwareInsertParams hardwareInsert;

    std::atomic<int> mode { (int) Mode::Mono };

    std::atomic<bool> recordArmed { false };
    // OFF by default — live input NEVER passes through to master
    // without explicit opt-in (feedback / hearing safety).
    std::atomic<bool> inputMonitor { false };
    // Recorded WAV captures post-EQ/post-comp signal. Off so the user
    // can re-EQ/re-comp at mix time.
    std::atomic<bool> printEffects { false };
    // -2 = follow track index (default); -1 = no input; 0..N = device input.
    std::atomic<int>  inputSource { -2 };
    // R of stereo. -2 = inputSource + 1 (paired adjacent).
    std::atomic<int>  inputSourceR { -2 };
    std::atomic<int>  midiInputIndex { -1 };
    // Stable identifier so saved sessions resolve correctly after
    // USB-MIDI replug. Message thread only.
    juce::String      midiInputIdentifier;

    // External MIDI out — drives a hardware synth/drum machine in
    // parallel with the loaded instrument plugin. Synth's audio
    // returns on a separate audio track.
    std::atomic<int>  midiOutputIndex { -1 };
    juce::String      midiOutputIdentifier;
    // 0 = omni, 1..16 = filter to that channel only.
    std::atomic<int>  midiChannel    { 0 };

    // Audio thread sets true on MIDI route; UI timer poll + clear so a
    // continuous stream still flashes.
    mutable std::atomic<bool> midiActivity { false };

    // Mode flip preserves the off-mode vector (no implicit clear). A
    // "convert to MIDI" UX would explicitly clear them.
    // regions: written only at session load, audio reads via the
    // PlaybackEngine snapshot. midiRegions: written at load AND at
    // recording-stop, audio reads directly during MIDI playback —
    // wrapped in AtomicSnapshot for the lock-free swap.
    std::vector<AudioRegion>                regions;
    AtomicSnapshot<std::vector<MidiRegion>> midiRegions;

    // Populated by AudioEngine::publishPluginStateForSave before save;
    // consumed by consumePluginStateAfterLoad. Empty = no plugin.
    juce::String pluginDescriptionXml;
    juce::String pluginStateBase64;

    std::atomic<float> meterGrDb     { 0.0f };     // <= 0
    std::atomic<float> meterInputDb  { -100.0f };
    // Stereo R-input peak. -100 for mono/midi.
    std::atomic<float> meterInputRDb { -100.0f };
    // Post-fader / post-pan output peak (stereo). The strip meter shows this
    // during playback; it switches to meterInput* above when the track is
    // monitoring its input (IN engaged), matching console / DP-24 metering.
    std::atomic<float> meterOutLDb   { -100.0f };
    std::atomic<float> meterOutRDb   { -100.0f };

    // int (not bool) — 4 states. Lock-free: UI relaxed-stores; audio
    // relaxed-loads. 3c-i wires Off + Read; Write/Touch reserved.
    std::atomic<int> automationMode { 0 };  // AutomationMode cast

    // Reader/writer partitioned by mode + touched flags, not container
    // atomicity. Audio reads when (mode==Read) || (mode==Touch && !touched).
    // Message writes when (playing && (mode==Write || (mode==Touch && touched))).
    // Predicates are mutually exclusive on (mode, touched).
    // Sync via release/acquire on automationMode and each *Touched flag.
    std::array<AutomationLane, kNumAutomationParams> automationLanes {};
};

// 3 EQ bands (LF / mid-as-LM / HF) at fixed musical freqs. Bus comp =
// UniversalCompressor in Bus mode.
struct BusParams
{
    std::atomic<float> faderDb { 0.0f };
    std::atomic<float> pan     { 0.0f };
    std::atomic<bool>  mute    { false };
    std::atomic<bool>  solo    { false };

    std::atomic<bool>  eqEnabled  { false };
    // Mixbus mix-bus Tone EQ spec: LF shelf 300 Hz / MID bell 800 Hz Q0.7 /
    // HF shelf 2 kHz, all +/-9 dB (freqs fixed in BusStrip::updateEqParameters).
    std::atomic<float> eqLfGainDb { 0.0f };  // -9..+9
    std::atomic<float> eqMidGainDb{ 0.0f };  // -9..+9
    std::atomic<float> eqHfGainDb { 0.0f };  // -9..+9

    std::atomic<bool>  compEnabled   { false };
    std::atomic<float> compThreshDb  { 0.0f };
    std::atomic<float> compRatio     { 4.0f };
    std::atomic<float> compAttackMs  { 10.0f };
    std::atomic<float> compReleaseMs { 100.0f };
    // Donor's bus-comp choice param clamps non-discrete values to
    // "Auto" anyway, so this matches what existing sessions hear.
    std::atomic<bool>  compReleaseAuto { true };
    std::atomic<float> compMakeupDb  { 0.0f };

    // Audio reads THESE (post-automation) so the Off/Read/Write/Touch
    // hand-off lives in the engine; BusStrip::updateGainTargets and the
    // engine's mute gate stay lane-agnostic. Only FaderDb / Pan / Mute are
    // meaningful for a bus (no aux sends; solo isn't automated). UI's timer
    // polls live* for motor-fader/pan animation in Read mode.
    mutable std::atomic<float> liveFaderDb { 0.0f };
    mutable std::atomic<float> livePan     { 0.0f };
    mutable std::atomic<bool>  liveMute    { false };

    // True while the user has the fader / pan grabbed (drag-start..end).
    std::atomic<bool> faderTouched { false };
    std::atomic<bool> panTouched   { false };

    // Same shape as ChannelStripParams for code reuse; only the FaderDb /
    // Pan / Mute lanes get populated. Sync via release/acquire on
    // automationMode and each *Touched flag (see the per-track block).
    std::atomic<int> automationMode { 0 };  // AutomationMode cast
    std::array<AutomationLane, kNumAutomationParams> automationLanes {};

    // dB = peak-of-block (LED). RMS = linear amplitude smoothed at
    // 300 ms tau (analog VU, matches TapeMachine's internal integrator).
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

// Channels SEND TO via kNumAuxSends auxSendDb knobs (vs Bus which
// channels ROUTE INTO via busAssign). Hosts a plugin (typically
// reverb / delay).
struct AuxLaneParams
{
    // 1 slot — full plugin UI at comfortable size; doubling up halved
    // that budget and squeezed fixed-size editors.
    static constexpr int kMaxLanePlugins = 1;

    std::atomic<float> returnLevelDb { 0.0f };
    std::atomic<bool>  mute           { false };

    // Audio reads THESE (not raw values) so Off/Read/Write/Touch
    // hand-off lives in the engine; AuxLaneStrip stays lane-agnostic.
    mutable std::atomic<float> liveReturnLevelDb { 0.0f };
    mutable std::atomic<bool>  liveMute          { false };

    std::atomic<bool> faderTouched { false };

    // Same shape as ChannelStripParams for code reuse, but only
    // FaderDb + Mute get populated — pan/solo/sends aren't aux concepts.
    std::atomic<int> automationMode { 0 };
    std::array<AutomationLane, kNumAutomationParams> automationLanes {};

    mutable std::atomic<float> meterPostL { -100.0f };
    mutable std::atomic<float> meterPostR { -100.0f };
};

// Message-thread only — audio thread doesn't read markers (only
// cares about playhead / loop / punch). Kept sorted by timelineSamples
// for ruler iteration + binary-search hit-testing.
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

    // Per-slot plugin state. Slot's audio mode (Plugin vs Hardware)
    // lives on the strip; Session just persists param values.
    std::array<juce::String, AuxLaneParams::kMaxLanePlugins> pluginDescriptionXml;
    std::array<juce::String, AuxLaneParams::kMaxLanePlugins> pluginStateBase64;

    std::array<HardwareInsertParams, AuxLaneParams::kMaxLanePlugins> hardwareInserts;
};

struct MasterBusParams
{
    std::atomic<float> faderDb     { 0.0f };

    // Zero the output bus. Cheaper than -inf fader because MasterBus
    // also skips the metering RMS smooth.
    std::atomic<bool>  mute        { false };

    // Collapse L+R to (L+R)*0.5 on both channels — mono compatibility
    // check via console monitor "Mono" button. Independent of stereo
    // width / pan; affects only the final stereo->mono fold.
    std::atomic<bool>  monoSum     { false };

    mutable std::atomic<float> liveFaderDb { 0.0f };
    std::atomic<bool> faderTouched { false };

    // Spec lists only the master fader as automatable; lanes array
    // reuses the per-track shape for symmetry but only FaderDb is
    // meaningful.
    std::atomic<int> automationMode { 0 };
    std::array<AutomationLane, kNumAutomationParams> automationLanes {};

    // Internal drive / bias / formulation fixed; user toggles only
    // engagement + HQ (4x ox).
    std::atomic<bool>  tapeEnabled { false };
    std::atomic<bool>  tapeHQ      { false };

    // APVTS XML (base64). Empty = donor defaults.
    juce::String tapeStateBase64;

    // Pultec-style Tube EQ. Minimal control surface: LF boost, HF
    // boost, drive, output gain. Frequencies + bandwidth are musical
    // defaults internally.
    std::atomic<bool>  eqEnabled       { false };
    std::atomic<float> eqLfBoost       { 0.0f };   // 0..10
    std::atomic<float> eqLfAtten       { 0.0f };   // 0..10 (LF cut shelf)
    std::atomic<float> eqLfFreq        { 60.0f };  // discrete: 20/30/60/100 Hz
    std::atomic<float> eqHfBoost       { 0.0f };
    std::atomic<float> eqHfBoostFreq   { 8000.0f }; // discrete: 3k/4k/5k/8k/10k/12k/16k
    std::atomic<float> eqHfBoostBandwidth { 0.5f }; // 0..10 Sharp->Broad (Q on boost peak)
    std::atomic<float> eqHfAtten       { 0.0f };   // 0..10 (HF cut shelf)
    std::atomic<float> eqHfAttenFreq   { 10000.0f }; // discrete: 5k/10k/20k Hz
    std::atomic<float> eqOutputGainDb  { 0.0f };

    // SSL-style bus comp.
    std::atomic<bool>  compEnabled    { false };
    std::atomic<float> compThreshDb   { 0.0f };
    std::atomic<float> compRatio      { 4.0f };
    std::atomic<float> compAttackMs   { 10.0f };
    std::atomic<float> compReleaseMs  { 100.0f };
    std::atomic<bool>  compReleaseAuto { true };
    std::atomic<float> compMakeupDb   { 0.0f };

    mutable std::atomic<float> meterPostMasterLDb  { -100.0f };
    mutable std::atomic<float> meterPostMasterRDb  { -100.0f };
    mutable std::atomic<float> meterPostMasterRmsL { 0.0f };
    mutable std::atomic<float> meterPostMasterRmsR { 0.0f };
    mutable std::atomic<float> meterGrDb           { 0.0f };
};

// Independent of MasterBusParams so the user can dial different EQ /
// comp on the bounced mix vs live mix.
struct MasteringParams
{
    juce::File sourceFile;  // empty = no mix loaded

    // Band 0 = low shelf, 1-3 = peaking, 4 = high shelf. Defaults match
    // MasteringDigitalEq::prepare idle state.
    static constexpr int kNumEqBands = 5;
    std::atomic<bool>  eqEnabled       { false };
    std::atomic<float> eqBandFreq[kNumEqBands]  { 80.0f, 250.0f, 1000.0f, 4000.0f, 12000.0f };
    std::atomic<float> eqBandGainDb[kNumEqBands]{ 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    std::atomic<float> eqBandQ[kNumEqBands]     { 0.7f, 1.0f, 1.0f, 1.0f, 0.7f };

    // Legacy Tube-EQ atoms — back-compat with sessions saved before
    // the digital-EQ swap. Not driven by the DSP any more.
    std::atomic<float> eqLfBoost       { 0.0f };
    std::atomic<float> eqHfBoost       { 0.0f };
    std::atomic<float> eqHfAtten       { 0.0f };
    std::atomic<float> eqTubeDrive     { 0.3f };
    std::atomic<float> eqOutputGainDb  { 0.0f };

    // Mastering tends slower / softer than bus comp; defaults reflect.
    std::atomic<bool>  compEnabled    { false };
    std::atomic<float> compThreshDb   { 0.0f };
    std::atomic<float> compRatio      { 2.0f };
    std::atomic<float> compAttackMs   { 30.0f };
    std::atomic<float> compReleaseMs  { 250.0f };
    std::atomic<bool>  compReleaseAuto { true };
    std::atomic<float> compMakeupDb   { 0.0f };

    // -0.3 dB ceiling matches streaming-platform headroom.
    std::atomic<bool>  limiterEnabled  { true };
    std::atomic<float> limiterDriveDb  { 0.0f };
    std::atomic<float> limiterCeilingDb{ -0.3f };
    std::atomic<float> limiterReleaseMs{ 100.0f };
    // 0 Modern, 1 Transparent, 2 Punchy — shapes hold/release around the
    // release knob. Stereo link on = matched L/R gain (preserves the image);
    // off = independent per-channel limiting.
    std::atomic<int>   limiterMode       { 0 };
    std::atomic<bool>  limiterStereoLink { true };

    mutable std::atomic<float> meterPostMasterLDb  { -100.0f };
    mutable std::atomic<float> meterPostMasterRDb  { -100.0f };
    mutable std::atomic<float> meterPostMasterRmsL { 0.0f };
    mutable std::atomic<float> meterPostMasterRmsR { 0.0f };
    mutable std::atomic<float> meterCompGrDb     { 0.0f };
    mutable std::atomic<float> meterLimiterGrDb  { 0.0f };

    // dBTP = 4× oversampled per ITU BS.1770 Annex 2.
    mutable std::atomic<float> meterMomentaryLufs   { -100.0f };
    mutable std::atomic<float> meterShortTermLufs   { -100.0f };
    mutable std::atomic<float> meterIntegratedLufs  { -100.0f };
    mutable std::atomic<float> meterTruePeakDb      { -100.0f };

    // Drives I-LUFS / true-peak cell colour-coding in MasteringView.
    // 0 = Off (neutral). See kMasteringTargets for the preset table.
    std::atomic<int> targetPresetIndex { 0 };
};

class Session
{
public:
    static constexpr int kNumTracks   = 24;  // three banks of 8
    static constexpr int kNumBuses    = 4;
    static constexpr int kNumAuxLanes = 4;
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

    // Markers kept sorted by timelineSamples. findMarkerNear returns
    // -1 outside tolerance. Message-thread only.
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

    // O(1) — counter-backed. Bulk write paths (SessionSerializer,
    // self-test) bypass the setters then call recomputeRtCounters.
    bool anyTrackSoloed() const noexcept;
    bool anyBusSoloed()   const noexcept;
    bool anyTrackArmed()  const noexcept;

    // Atomically toggle bool + adjust count. No-op when unchanged.
    // Message thread only.
    void setTrackSoloed (int trackIndex, bool soloed) noexcept;
    void setBusSoloed   (int busIndex,   bool soloed) noexcept;
    void setTrackArmed  (int trackIndex, bool armed)  noexcept;

    // Rebuild counters after a bulk path that wrote atoms directly.
    void recomputeRtCounters() noexcept;

    // Write a new fader dB to track `ti`. If the track belongs to a fader
    // group, shift every other member by the same dB delta so the group's
    // relative balance is preserved (DP-24 fader-group behaviour). RT-safe:
    // atomic loads/stores and a bounded loop only, no alloc/lock - callable
    // from the audio thread (MCU pitch-bend, MIDI-binding fader). The UI drag
    // path has its own gesture-anchored propagation and does NOT route here.
    void setTrackFaderGrouped (int ti, float newDb) noexcept;

    // Mirrored to/from Transport by publish/consume bookends. Plain
    // non-atomic — touched only on the message thread between bookends.
    juce::int64 savedLoopStart    = 0;
    juce::int64 savedLoopEnd      = 0;
    bool        savedLoopEnabled  = false;
    juce::int64 savedPunchIn      = 0;
    juce::int64 savedPunchOut     = 0;
    bool        savedPunchEnabled = false;

    // Two orthogonal fields: snapToGrid (master enable, SnapHelpers
    // no-op when false) + snapResolution (denomination). Toggling
    // master off and on preserves the denomination pick. Message
    // thread only.
    bool           snapToGrid     = false;
    SnapResolution snapResolution = SnapResolution::Quarter;

    // Persists across editor reopens. Message thread only.
    bool           pianoRollKeySnap = true;

    // TapeStrip / region-editor mouseDowns branch on this at the top.
    // Persisted in session.json transport block. Message thread only.
    EditMode editMode = EditMode::Grab;

    // 1 = native, 2 = 2× ox, 4 = 4× ox. Read by MasterBus + BusStrip in
    // prepare. Changing requires re-prepare. Atomic for future audio-
    // thread reads but today only message-thread prepare reads it.
    std::atomic<int> oversamplingFactor { 4 };

    // tempoBpm = 0 disables beat-grid (metronome silent, snap falls
    // back to seconds). Atomics so audio picks up changes lock-free.
    std::atomic<float> tempoBpm          { 120.0f };
    std::atomic<int>   beatsPerBar       { 4 };
    std::atomic<int>   beatUnit          { 4 };
    std::atomic<bool>  metronomeEnabled  { false };
    std::atomic<float> metronomeVolDb    { -12.0f };
    // metronomeOnlyDuringCountIn overrides clickWhileRecording for the
    // post-pre-roll portion. metronomePolyphonic: off by default to
    // match historical mono behaviour (long click bodies at high BPM
    // otherwise step on themselves).
    std::atomic<bool>  metronomeClickWhileRecording { true  };
    std::atomic<bool>  metronomeClickWhilePlaying   { false };
    std::atomic<bool>  metronomeOnlyDuringCountIn   { false };
    std::atomic<bool>  metronomePolyphonic          { false };

    // One mode for every position-display surface (transport clock,
    // TapeStrip ruler, region editors, status bar).
    std::atomic<int>   timeDisplayMode   { 0 };

    // Persisted across saves. New sessions default to Recording (0).
    // 0=Recording, 1=Mixing, 2=Aux, 3=Mastering.
    std::atomic<int>   uiStage           { 0 };

    // Tape-head behaviour on Stop, mirroring the appconfig enum:
    //   0 = PauseInPlace (default, leave the playhead where it landed)
    //   1 = ReturnToZero (rewind to 0 on every Stop)
    //   2 = ReturnToLastClicked (jump to lastClickedTimelineSample)
    // Pushed from MainComponent at startup + whenever the user changes
    // the Settings dropdown. AudioEngine::stop reads this on Stop.
    std::atomic<int>      stopBehavior              { 0 };
    // Last position the user clicked on the tape-strip ruler (samples).
    // -1 = never clicked / unknown — engine treats as PauseInPlace fall-
    // back when ReturnToLastClicked is selected.
    std::atomic<juce::int64> lastClickedTimelineSample { -1 };

    // Shifts playhead back one bar so metronome ticks pre-roll before
    // capture. WAV's first sample still maps to playhead-at-Record-press.
    std::atomic<bool>  countInEnabled    { false };

    // Timeline position the most recent record pass STARTED at; FFWD-
    // while-stopped jumps here. 0 = haven't recorded yet (first tap is
    // a no-op).
    std::atomic<juce::int64> lastRecordPointSamples { 0 };

    // Auto-punch pre/post-roll. preRoll: existing material plays during
    // pre-roll; the audio callback's punch-window gate prevents
    // committing audio before punchIn. postRoll = 0 = stay rolling
    // indefinitely.
    std::atomic<float> preRollSeconds  { 0.0f };
    std::atomic<float> postRollSeconds { 0.0f };
    // Enable toggles, so a roll can be bypassed without losing its seconds
    // value. Effective roll = enabled ? seconds : 0.
    std::atomic<bool>  preRollEnabled  { true };
    std::atomic<bool>  postRollEnabled { true };

    // tuneTrackIndex = -1 disables; 0..15 selects. Audio writes Hz/level
    // per block; TunerOverlay's 30 Hz timer reads. Not persisted.
    std::atomic<int>   tuneTrackIndex { -1 };
    std::atomic<float> tuneLatestHz   { 0.0f };
    std::atomic<float> tuneLatestLevel { 0.0f };

    // Audio thread sets on Note On / clears on Note Off. SystemStatusBar
    // timer walks the array, builds a vector, runs ChordAnalyzer off-
    // thread. Per-note atomic (not packed word) keeps the update path
    // CAS-free.
    static constexpr int kNumMidiNotes = 128;
    std::array<std::atomic<bool>, kNumMidiNotes> heldMidiNotes {};

    // Lock-free swap via AtomicSnapshot. Audio sets pendingTransportAction
    // when a binding hits a transport target; TransportBar's 20 Hz timer
    // drains it (engine.play/stop/record aren't RT-safe).
    AtomicSnapshot<std::vector<MidiBinding>> midiBindings;
    std::atomic<int>         pendingTransportAction { (int) PendingTransportAction::None };
    std::atomic<int>         midiLearnPending       { -1 };

    // UI writes; audio reads to resolve bank-relative MIDI bindings
    // (TrackFaderBank etc.) -> absoluteTrack = activeBank*kBankSize + pos.
    std::atomic<int>         activeBank { 0 };
    std::atomic<juce::int64> midiLearnCapture       { 0 };

    // Persisted as device identifier in JSON; engine resolves to index
    // on load + on every hot-plug rebuild. externalBpm: smoothed BPM
    // by MidiSyncReceiver. v1 is tempo-only — engine reads BPM but
    // ignores the rolling flag unless externalSyncChasesTransport.
    juce::String              syncSourceInputIdentifier;
    std::atomic<int>          syncSourceInputIdx      { -1 };
    mutable std::atomic<float> externalBpm            { 0.0f };
    mutable std::atomic<bool>  externalSyncRolling    { false };
    std::atomic<bool>          externalSyncFollowsTempo { true };
    std::atomic<bool>          externalSyncChasesTransport { false };

    // MIDI Clock OUT (master). v1 = single port; widen identifier to
    // comma-sep list for multi-port later.
    juce::String              syncOutputIdentifier;
    std::atomic<int>          syncOutputIdx        { -1 };
    std::atomic<bool>         syncOutputEmitClock  { false };

    // MTC slave-decoded state. Shares syncSourceInputIdx port with
    // Clock (QF + F8 multiplex). externalTimeCodeFrames has the
    // 2-frame QF transmission-delay compensation applied — matches
    // what the master's display reads RIGHT NOW.
    mutable std::atomic<juce::int64> externalTimeCodeFrames    { 0 };
    mutable std::atomic<bool>        externalTimeCodeRolling   { false };
    mutable std::atomic<bool>        externalTimeCodeReversed  { false };
    mutable std::atomic<int>         externalTimeCodeFrameRate { 3 };  // 0=24, 1=25, 2=29.97DF, 3=30

    std::atomic<bool> externalTimeCodeChasesTransport { false };

    // -1 = nothing pending. Audio writes target sample; message-thread
    // timer drains, calls Transport::setPlayhead, stores -1.
    std::atomic<juce::int64> pendingTransportPlayhead { -1 };

    // MTC master-emit. Reuses syncOutputIdx port (Clock + MTC mux).
    std::atomic<bool> syncOutputEmitTimeCode     { false };
    std::atomic<int>  syncOutputTimeCodeFrameRate { 3 };

    // Bank/select/assign are session-runtime, NOT persisted — a fresh
    // launch always starts on bank 0 / channel 0 / PAN so controller
    // LEDs match on-screen mixer reset state.
    struct McuSessionState
    {
        juce::String inputIdentifier;        // empty = off
        juce::String outputIdentifier;       // empty = off
        std::atomic<int> resolvedInputIdx  { -1 };
        std::atomic<int> resolvedOutputIdx { -1 };

        // 0 = PAN, 1..4 = SEND1..4, 5 = EQ, 6 = COMP. EQ/COMP target
        // selectedChannel; PAN/SEND target the 8 banked channels.
        std::atomic<int> assignMode      { 0 };

        // Which 8 of the 24 tracks the controller shows.
        std::atomic<int> bank            { 0 };

        // 0..15 — drives EQ/COMP encoder target + plugin-editor focus.
        std::atomic<int> selectedChannel { 0 };
    };
    McuSessionState mcu;

    // -2 = follow track index, -1 = no input.
    int resolveInputForTrack (int trackIndex) const noexcept;
    // -1 in Mono / Midi mode (second channel meaningless).
    int resolveInputRForTrack (int trackIndex) const noexcept;

private:
    std::array<Track, kNumTracks> tracks;
    std::array<Bus, kNumBuses> buses;
    std::array<AuxLane, kNumAuxLanes> auxLanes;
    std::vector<Marker> markers;
    MasterBusParams masterParams;
    MasteringParams masteringParams;
    juce::File sessionDir;

    // Single relaxed load per callback instead of scanning all atoms.
    std::atomic<int> soloTrackCount { 0 };
    std::atomic<int> soloBusCount   { 0 };
    std::atomic<int> armedTrackCount { 0 };
};
} // namespace duskstudio
