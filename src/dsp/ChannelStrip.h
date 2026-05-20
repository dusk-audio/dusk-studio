#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <memory>
#include <vector>
#include "../session/Session.h"
#include "../engine/PluginSlot.h"
#include "HardwareInsertSlot.h"

#if DUSKSTUDIO_HAS_DUSK_DSP
  #include "BritishEQProcessor.h"     // multi-q
  // Use UniversalCompressor directly (NOT the ChannelComp facade) so each
  // channel strip behaves identically to a DAW hosting the standalone
  // multi-comp plugin: full feature set (sidechain HP, true-peak detector,
  // transient shaper, lookahead, stereo linking, auto-makeup, bypass-fade
  // crossfader). Donor's internal oversampling is DISABLED at prepare time
  // because Dusk Studio wraps the strip in its own 4× oversampler — running both
  // would double-OS.
  #include "UniversalCompressor.h"    // multi-comp
#endif

namespace duskstudio
{
// Phase 1a channel strip: 4K-style HPF + 4-band EQ + per-aux sends + pan +
// fader + SIP gating. FET/Opto/VCA compressor inserts after the EQ stage in
// the next chunk; the API stays the same.
class ChannelStrip
{
public:
    static constexpr int kNumBuses    = ChannelStripParams::kNumBuses;
    static constexpr int kNumAuxSends = ChannelStripParams::kNumAuxSends;

    void prepare (double sampleRate, int blockSize, int oversamplingFactor = 1);
    void bind (const ChannelStripParams& params) noexcept { paramsRef = &params; }

    // Bind the per-app PluginManager so this strip's PluginSlot can resolve
    // file paths to plugin instances. Called once at engine construction
    // after AudioEngine builds its PluginManager. The slot is dormant until
    // a plugin is loaded - strips with no plugin pay zero RT cost.
    void bindPluginManager (PluginManager& mgr) noexcept { pluginSlot.setManager (mgr); }
    PluginSlot&       getPluginSlot()       noexcept { return pluginSlot; }
    const PluginSlot& getPluginSlot() const noexcept { return pluginSlot; }

    // Hardware-insert side of the channel insert slot. The strip's
    // insert is "either a plugin OR a hardware insert" - the live mode
    // is chosen by insertMode (0 = empty, 1 = plugin, 2 = hardware).
    // Audio thread reads insertMode with acquire; mode flips cross-
    // fade through activeInsertGain over ~20 ms to mask the routing
    // discontinuity.
    void bindHardwareInsert (const HardwareInsertParams& params) noexcept;
    HardwareInsertSlot&       getHardwareInsertSlot()       noexcept { return hardwareSlot; }
    const HardwareInsertSlot& getHardwareInsertSlot() const noexcept { return hardwareSlot; }

    enum InsertMode : int { kInsertEmpty = 0, kInsertPlugin = 1, kInsertHardware = 2 };

    // Message thread sets this; audio thread reads with acquire and
    // initiates a 20 ms crossfade to the new mode. Default is plugin so
    // existing channel-strip behaviour (load-a-plugin-via-picker) is
    // preserved with no UI changes.
    std::atomic<int> insertMode { kInsertPlugin };

    // Engine sets this to true when the recorder is going to read
    // getLastProcessedMono() this block (i.e. armed && recording && printEffects).
    // When false, the strip is allowed to skip the DSP if it isn't passing
    // to master (passByGate=false), saving the cost of a full
    // UniversalCompressor + EQ pass on every silent channel.
    void setNeedsProcessedMono (bool needed) noexcept { needsProcessedMono = needed; }

    // Latest peak gain reduction in dB (negative = reducing). Read from any
    // thread; written from the audio thread. Used by the per-channel GR
    // meter in the UI.
    float getCurrentGrDb() const noexcept { return currentGrDb.load (std::memory_order_relaxed); }

    // Pointer to the post-EQ/post-comp L (or mono) buffer from the most-
    // recent processAndAccumulate() call. Valid for `lastProcessedSamples`
    // samples. The recorder reads this when "print effects" is engaged on
    // a track. Returns nullptr if no DSP ran this block.
    const float* getLastProcessedMono() const noexcept { return lastProcessedPtr; }

    // Pointer to the post-EQ/post-comp R buffer when the strip ran in
    // stereo mode this block. nullptr in mono mode (or no input) — callers
    // that want stereo print-effects must check this before reading R.
    const float* getLastProcessedR() const noexcept { return lastProcessedR; }
    int getLastProcessedSamples() const noexcept { return lastProcessedSamples; }

    // Reads `numSamples` of audio from inL (and optionally inR for stereo
    // tracks). Pass inR == nullptr for mono tracks; the strip then runs its
    // EQ + Comp on a single channel and pans the mono signal to (L, R) on
    // accumulation. With a non-null inR the strip processes 2 channels end-
    // to-end and applies (panGainL, panGainR) as a per-channel balance.
    //
    // When `isMidi` is true the strip ignores inL/inR entirely and instead
    // sources its stereo audio from the loaded plugin (an instrument). The
    // `trackMidi` buffer holds the per-track-filtered MIDI events for this
    // block; the engine fills it before calling. EQ + Comp + sends + pan +
    // fader run on the synth's stereo output exactly like a stereo audio
    // track. `trackMidi` should be empty for non-MIDI calls.
    //
    // Internals: HPF + 4-band EQ (single source from dsp-cores) → comp →
    // accumulates:
    //   • post-fader stereo signal into masterL/masterR (when not bus-routed)
    //   • binary-routed signal into busL[N]/busR[N] for each assigned bus
    //   • aux-send signal into auxLaneL[N]/auxLaneR[N] at auxSendDb[N] gain,
    //     pre- or post-fader per auxSendPreFader[N]
    // deviceInputs / deviceOutputs are the raw audio-device I/O arrays
    // for the current callback (passed by AudioEngine). They're forwarded
    // to the hardware-insert slot's processStereoBlock so the SEND can
    // accumulate into device outputs and the RETURN can be read from
    // device inputs. Defaulted to null/0 so non-engine call sites (tests,
    // self-test) keep compiling without engine I/O context.
    void processAndAccumulate (const float* inL,
                               const float* inR,                   // nullptr = mono path
                               juce::MidiBuffer& trackMidi,         // filtered MIDI for this track
                               bool  isMidi,                        // true => synth source
                               float* masterL, float* masterR,
                               const std::array<float*, kNumBuses>& busL,
                               const std::array<float*, kNumBuses>& busR,
                               const std::array<float*, kNumAuxSends>& auxLaneL,
                               const std::array<float*, kNumAuxSends>& auxLaneR,
                               int numSamples,
                               bool passByGate,
                               const float* const* deviceInputs  = nullptr,
                               int   numDeviceInputs             = 0,
                               float* const*       deviceOutputs = nullptr,
                               int   numDeviceOutputs            = 0) noexcept;

private:
    const ChannelStripParams* paramsRef = nullptr;
    juce::SmoothedValue<float> faderGain  { 0.0f };
    juce::SmoothedValue<float> panGainL   { 0.7071f };
    juce::SmoothedValue<float> panGainR   { 0.7071f };
    // Binary bus routing - smoothed 0..1 to avoid clicks on toggle.
    std::array<juce::SmoothedValue<float>, kNumBuses> busGain;
    // Aux send levels - linear gain (mapped from auxSendDb each block).
    // Smoothed so knob turns and pre/post toggles don't click.
    std::array<juce::SmoothedValue<float>, kNumAuxSends> auxSendGain;
    // Cached per-block: which aux sends use the pre-fader tap. Sampled at
    // the top of processAndAccumulate so the inner loop avoids per-sample
    // atomic loads.
    std::array<bool, kNumAuxSends> auxSendPre {};

    std::vector<float> tempMono;  // pre-fader scratch buffer (post-EQ)
    juce::AudioBuffer<float> eqMonoBuffer;  // wraps tempMono for BritishEQProcessor::process

    // Stereo scratch — held in a juce::AudioBuffer so EQ + Comp can process
    // it directly without a per-block wrap. Channel 1 is unused on mono
    // tracks; preparing 2 channels for everything keeps internal DSP state
    // (filter histories, comp envelope) sized correctly so a mono→stereo
    // mode switch mid-session doesn't fault on a missing channel.
    juce::AudioBuffer<float> tempStereoBuffer;

    // Per-strip oversamplers wrapping the (EQ + Comp) chain. Built once
    // per prepare based on Session::oversamplingFactor: nullptr at 1×, 1
    // stage at 2×, 2 stages at 4×. Mono tracks use the 1-channel instance;
    // stereo tracks use the 2-channel one. The donor's BritishEQ console
    // saturation and ChannelComp/UC saturation alias without this; the
    // user's "Effect Oversampling" pick is the single source of truth.
    std::unique_ptr<juce::dsp::Oversampling<float>> oversamplerMono;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversamplerStereo;
    int oversamplerStages = 0;  // 0 = no oversampling; 1 = 2×; 2 = 4×.

    // Per-channel insert plugin. Sits between phase invert and the EQ stage
    // so the user can drop a guitar amp / synth / character processor in
    // the signal path before the channel's EQ + comp shape it.
    PluginSlot pluginSlot;

    // Hardware-insert side. Either pluginSlot OR hardwareSlot runs in any
    // given block, chosen by insertMode + the crossfade gate below.
    HardwareInsertSlot hardwareSlot;

    // Audio-thread crossfade state for the plugin <-> hardware mode flip.
    // activeInsertMode is the slot we're CURRENTLY running; insertMode is
    // what the UI wants us to run. Mismatch triggers a ramp-out / mode-
    // swap / ramp-in cycle via activeInsertGain (20 ms equal-power
    // smoother). resetTailsAndDelayLine() is called on the new slot at
    // the swap point so the freshly-activated path starts clean.
    int activeInsertMode = kInsertPlugin;
    juce::SmoothedValue<float> activeInsertGain;

    // Scratch buffers used to hold the pre-insert signal so the
    // crossfade can blend pre vs post per-sample. Sized in prepare().
    std::vector<float> insertScratchL;
    std::vector<float> insertScratchR;

    // Empty MIDI buffer passed to the channel insert plugin. PluginSlot's
    // processBlock now requires a MidiBuffer& for instrument hosting in
    // 4a-5; insert effects ignore it. Held as a member so the audio thread
    // never default-constructs one. Stays empty in the effect-insert path.
    juce::MidiBuffer pluginMidiScratch;

#if DUSKSTUDIO_HAS_DUSK_DSP
    BritishEQProcessor eq;
    // Cache of the most recent Parameters we pushed to `eq`. updateEqParameters
    // memcmp's the freshly-built Parameters against this and only calls
    // setParameters() when the bytes differ - this skips the full coefficient
    // recompute (8-14 biquads) on every block when the user isn't currently
    // turning a knob. Value-init {} so padding bytes are zeroed and the
    // memcmp doesn't false-positive on garbage padding.
    BritishEQProcessor::Parameters lastEqParams {};

    // Per-channel comp - direct UniversalCompressor (full multi-comp feature
    // set; matches standalone plugin behavior). One instance per channel.
    // Per-mode parameter pointers cached in bindCompParams() to avoid string
    // lookups on the audio thread. Donor's internal oversampling is disabled
    // in prepare() because Dusk Studio's strip-level wrapper already oversamples.
    UniversalCompressor compressor;
    juce::MidiBuffer compMidiScratch;          // unused but required by processBlock signature

    // Cached APVTS raw atomic value pointers - set once in bindCompParams().
    // These point at the SAME std::atomic<float> that UniversalCompressor's
    // processBlock() reads via getRawParameterValue(); writing here is the
    // direct, lock-free path. Stores hold DENORMALISED (SI-unit / index /
    // 0-or-1) values - no normalisation step needed.
    std::atomic<float>* compModeAtom        = nullptr;
    std::atomic<float>* compBypassAtom      = nullptr;
    std::atomic<float>* compMixAtom         = nullptr;
    std::atomic<float>* compAutoMakeupAtom  = nullptr;
    std::atomic<float>* compOptoPeakRedAtom = nullptr;
    std::atomic<float>* compOptoGainAtom    = nullptr;
    std::atomic<float>* compOptoLimitAtom   = nullptr;
    std::atomic<float>* compFetInputAtom    = nullptr;
    std::atomic<float>* compFetOutputAtom   = nullptr;
    std::atomic<float>* compFetAttackAtom   = nullptr;
    std::atomic<float>* compFetReleaseAtom  = nullptr;
    std::atomic<float>* compFetRatioAtom    = nullptr;
    std::atomic<float>* compVcaThreshAtom   = nullptr;
    std::atomic<float>* compVcaRatioAtom    = nullptr;
    std::atomic<float>* compVcaAttackAtom   = nullptr;
    std::atomic<float>* compVcaReleaseAtom  = nullptr;
    std::atomic<float>* compVcaOutputAtom   = nullptr;

    // SmoothedValue wrappers for the continuous comp params. The session
    // atoms hold the user's setpoint; these ramp toward it at 20 ms so
    // knob drags don't zipper the donor's per-sample DSP. Discrete params
    // (mode, bypass, LIMIT toggle, FET ratio index) bypass the smoother.
    juce::SmoothedValue<float> smoothedOptoPeakRed;
    juce::SmoothedValue<float> smoothedOptoGain;
    juce::SmoothedValue<float> smoothedFetInput;
    juce::SmoothedValue<float> smoothedFetOutput;
    juce::SmoothedValue<float> smoothedFetAttack;
    juce::SmoothedValue<float> smoothedFetRelease;
    juce::SmoothedValue<float> smoothedVcaThresh;
    juce::SmoothedValue<float> smoothedVcaRatio;
    juce::SmoothedValue<float> smoothedVcaAttack;
    juce::SmoothedValue<float> smoothedVcaRelease;
    juce::SmoothedValue<float> smoothedVcaOutput;

    void publishSmoothedCompParams (int numSamples) noexcept;

    void bindCompParams();
    static inline void storeAtom (std::atomic<float>* a, float v) noexcept
    {
        if (a != nullptr) a->store (v, std::memory_order_relaxed);
    }
#endif

    std::atomic<float> currentGrDb { 0.0f };
    const float* lastProcessedPtr = nullptr;  // L (or mono) — updated each processAndAccumulate()
    const float* lastProcessedR   = nullptr;  // R (stereo path only); nullptr in mono path
    int          lastProcessedSamples = 0;
    bool         needsProcessedMono = false;  // set per-block by the engine

    void updateGainTargets() noexcept;
    void updateEqParameters() noexcept;
    void updateCompParameters() noexcept;
};
} // namespace duskstudio
