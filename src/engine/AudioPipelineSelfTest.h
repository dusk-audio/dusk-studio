#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <array>
#include <vector>
#include "AudioEngine.h"
#include "../session/Session.h"

namespace duskstudio
{
// Headless self-test for the audio pipeline. The synthetic tests detach the
// AudioEngine from deviceManager and call audioDeviceIOCallbackWithContext
// directly with known input buffers, then measure the output. This catches
// internal bugs (mute, fader, master DSP, channel routing) without requiring
// audio hardware. The backend tests cycle through available device types and
// report what each one negotiates - useful for verifying ALSA/JACK both open
// cleanly with our settings.
//
// IMPORTANT: this class mutates session state during the test (track 0 IN,
// fader values, mute, master tape, etc.). It saves/restores around each run
// so the user's configuration is preserved.
class AudioPipelineSelfTest
{
public:
    AudioPipelineSelfTest (AudioEngine& engine,
                            juce::AudioDeviceManager& dm,
                            Session& session) noexcept;

    // Runs the full suite, returns a multi-line formatted report suitable for
    // pasting into bug reports. Safe to call from the message thread; takes
    // a few hundred milliseconds to a few seconds depending on backend count.
    juce::String runAll();

    // Headless engine-CPU benchmark: configures the engine for a given load
    // (number of active tracks, EQ on/off, comp on/off, tape on/HQ), drives
    // numBlocks callbacks at the requested SR/BS, and reports per-callback
    // wall-clock timings vs. the buffer budget. Independent from any audio
    // device - measures the engine's pure DSP cost, exactly the figure you
    // compare against Reaper / Bitwig / Ardour at the same load.
    juce::String runPerfSuite();

private:
    struct Measurements
    {
        float peakL = 0.0f, peakR = 0.0f;
        float rmsL  = 0.0f, rmsR  = 0.0f;
        float thdL  = 0.0f;  // simple periodicity-based distortion estimate
    };

    struct SavedState
    {
        std::array<float, Session::kNumTracks> faderDb {};
        std::array<float, Session::kNumTracks> pan {};
        std::array<bool,  Session::kNumTracks> mute {};
        std::array<bool,  Session::kNumTracks> solo {};
        std::array<bool,  Session::kNumTracks> recordArmed {};
        std::array<bool,  Session::kNumTracks> inputMonitor {};
        std::array<bool,  Session::kNumTracks> compEnabled {};
        std::array<bool,  Session::kNumTracks> phaseInvert {};
        std::array<bool,  Session::kNumTracks> hpfEnabled {};
        std::array<float, Session::kNumTracks> lfGainDb {};
        std::array<float, Session::kNumTracks> lmGainDb {};
        std::array<float, Session::kNumTracks> hmGainDb {};
        std::array<float, Session::kNumTracks> hfGainDb {};
        // prepareCleanState clears track 0's busAssign and testMasterComp...
        // enables the master comp; capture both so the live session is fully
        // restored after a run.
        std::array<bool, ChannelStripParams::kNumBuses> track0BusAssign {};
        float masterFaderDb     = 0.0f;
        bool  masterTapeEnabled = false;
        bool  masterTapeHQ      = false;
        bool  masterCompEnabled = false;
    };

    SavedState saveState() const;
    void restoreState (const SavedState&);

    // Set up session for "track 0 only, unity, pan center, comp & EQ neutral,
    // master unity, tape off". All other tracks fully muted so they can't
    // contribute. Returns the index of the track under test.
    int prepareCleanState();

    // Drive numWarmup blocks (let smoothers settle) then numMeasure blocks
    // and return aggregate measurements over the measurement blocks.
    Measurements runSynthetic (double sampleRate,
                                int blockSize,
                                int numInChannels,
                                int numOutChannels,
                                float inputAmplitude,
                                float toneHz,
                                int numWarmupBlocks,
                                int numMeasureBlocks);

    // Test cases - each formats a "[PASS]/[FAIL] name: details" string.
    juce::String testPassThroughUnity();
    juce::String testMuteSilences();
    juce::String testMasterFaderMinusSix();
    juce::String testMasterCompNoNoiseFloor(); // comp ON + silent in stays silent (donor analog-noise off)
    juce::String testBusSoloMutesDirect();      // SIP bus solo: unassigned/direct track muted when a bus is soloed
    juce::String testChannelRoutingTwoOut();
    juce::String testChannelRoutingFourOut();
    juce::String testMasterTapeAddsGain();   // sanity: tape ON should not silently drop signal
    juce::String testCompEachMode();         // -18 dBFS sine through Opto/FET/VCA; THD + alias floor
    juce::String testCompHeavyGR();          // extreme settings: characterize harmonics at heavy GR
    juce::String testCompPerTrack();         // all 16 tracks under Opto produce identical output
    juce::String testBackendsOpenCleanly();
    juce::String probeUMC1820AlsaFormat();   // explicitly open UMC1820 ALSA & report format

    // Per-config benchmark cell. Drives `numBlocks` callbacks and returns one
    // formatted line "[OK]/[OVER]  config  → median X us / p95 Y us / max Z us
    // (budget B us, overruns N/total)". budget = blockSize * 1e6 / sampleRate.
    juce::String runPerfBenchmark (const juce::String& label,
                                    double sampleRate, int blockSize,
                                    int numActiveTracks,
                                    bool eqEnabled, bool compEnabled,
                                    bool tapeOn, int oversamplingFactor,
                                    int numWarmupBlocks, int numMeasureBlocks);

    AudioEngine& engine;
    juce::AudioDeviceManager& deviceManager;
    Session& session;
};
} // namespace duskstudio
