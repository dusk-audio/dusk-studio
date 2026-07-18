#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <vector>

namespace duskstudio
{
// Native Multisample instrument: a juce::AudioPluginInstance that
// loads .sfz (and, in 1.0, .sf2) files and renders them through the
// vendored sfizz engine. Lives in src/engine/multisample/ alongside
// the SFZ/SF2 -> sfizz adapters.
//
// Lifetime: created on the message thread by PluginManager via
// DuskMultisamplePluginFormat::createPluginInstance.
// Owned by PluginSlot via std::unique_ptr<juce::AudioPluginInstance>.
// Atomic-swap of the slot's currentInstance pointer follows the same
// rules every other hosted plugin uses (see PluginSlot.h:34).
class DuskMultisampleProcessor : public juce::AudioPluginInstance
{
public:
    DuskMultisampleProcessor();
    ~DuskMultisampleProcessor() override;

    // juce::AudioProcessor surface.
    const juce::String getName() const override                { return "Dusk Multisample"; }
    void prepareToPlay (double sampleRate, int blockSize) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>& buf,
                       juce::MidiBuffer& midi) override;
    bool acceptsMidi() const override                          { return true; }
    bool producesMidi() const override                         { return false; }
    bool isMidiEffect() const override                         { return false; }
    double getTailLengthSeconds() const override               { return 0.0; }
    bool hasEditor() const override                            { return true; }
    juce::AudioProcessorEditor* createEditor() override;
    int getNumPrograms() override                              { return 1; }
    int getCurrentProgram() override                           { return 0; }
    void setCurrentProgram (int) override                      {}
    const juce::String getProgramName (int) override           { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // juce::AudioPluginInstance surface. JUCE relies on these for
    // plugin-host bookkeeping; the picker reads getPluginDescription
    // to populate the menu entry.
    void fillInPluginDescription (juce::PluginDescription& desc) const override;
    void refreshParameterList() override {}

    // Load an .sfz file synchronously on the message thread. Returns
    // true + clears errorMessage on success. PluginSlot's atomic-swap
    // pattern moves the loaded processor into place once this returns.
    bool loadSfzFile (const juce::File& sfz, juce::String& errorMessage);

    // Load an .sf2 (SoundFont 2) file. Converts the first preset to SFZ
    // + extracted WAVs (Sf2Reader + Sf2ToSfz) and plays it through the
    // vendored sfizz engine - no fluidsynth dependency. Caches preset
    // display metadata for the editor's program switcher.
    bool loadSf2File (const juce::File& sf2, juce::String& errorMessage);

    // Switch the loaded SF2 to another preset (re-converts + reloads).
    // No-op error when the current file isn't an SF2.
    bool loadSf2Preset (int presetIndex, juce::String& errorMessage);

    struct Sf2PresetInfo
    {
        juce::String name;
        int sourceIndex { 0 };
        int bank        { 0 };
        int program     { 0 };
    };

    // Sorted for display by bank/program. sourceIndex remains the preset's
    // original PHDR position, which is the stable ID used for loading and
    // session persistence. Hands back a snapshot, not a reference: a background
    // load clears + rebuilds sf2Presets on the loader thread, so a caller
    // holding a reference could see it realloc mid-iteration. Empty while a load
    // is pending (matches getNumRegions / getControlCcLabels /
    // getControlImagePath).
    std::vector<Sf2PresetInfo> getSf2Presets() const
    {
        if (isLoadPending()) return {};
        const juce::ScopedLock sl (sf2PresetsLock);
        return sf2Presets;
    }
    int getSf2PresetIndex() const noexcept { return sf2PresetIndex; }

    // True if a soundfont has been loaded. Used by editor UI to show
    // "(no file)" vs the loaded file name.
    bool hasLoadedFile() const noexcept { return loadedFilePath.isNotEmpty(); }
    const juce::String& getLoadedFilePath() const noexcept { return loadedFilePath; }

    // Number of regions sfizz parsed from the loaded .sfz. 0 when
    // no file is loaded. Editor polls this on its refresh timer.
    int getNumRegions() const noexcept;

    // Reload the currently-loaded .sfz from disk. No-op when no
    // file is loaded. Returns true on success.
    bool reloadCurrentFile (juce::String& errorMessage);

    // Background variants: the load (SF2 sample extraction + sfizz parse can
    // take seconds on a GM bank) runs on the processor's own worker thread
    // and onDone(ok, error) fires on the message thread. The worker joins in
    // the destructor, so a slot unload blocks until an in-flight load
    // finishes instead of destroying the synth under it. One load at a time -
    // callers gate UI on isLoadPending().
    void loadFileAsync (const juce::File& file,
                        std::function<void (bool, juce::String)> onDone);
    void loadSf2PresetAsync (int presetIndex,
                             std::function<void (bool, juce::String)> onDone);
    bool isLoadPending() const noexcept
    {
        return loadPending.load (std::memory_order_relaxed);
    }

    // Drop the loaded soundfont. After this call the processor
    // renders silence; subsequent loadSfzFile / setStateInformation
    // can replace it.
    void clearLoadedFile();

    // Polyphony change. Per sfizz.h, sfizz_set_num_voices is marked
    // OFF - "cannot be invoked while a thread is calling RT
    // functions". setPolyphony() runs on the message thread (editor
    // / state-load callers), pauses sfizz briefly, applies, then
    // resumes. The Overrides atom is updated so getStateInformation
    // sees the latest value.
    void setPolyphony (int newPolyphony);

    // Surfaces the most-recent error from setStateInformation /
    // loadSfzFile when called from the deserialiser. Editor polls it
    // so a missing-file restore shows "(file not found)" instead of
    // silent emptiness. Cleared when a load succeeds.
    juce::String getLastLoadError() const noexcept { return lastLoadError; }

    // Override parameters. Phase 1 v1: master volume, master tune,
    // polyphony cap. Phase 2 widens to ADSR + filter + LFO overrides
    // wired through sfizz's CC automation surface. UI mutates via
    // relaxed atomic stores; processBlock loads each value once at
    // block top + applies via the sfizz API.
    struct Overrides
    {
        std::atomic<float> masterVolDb     { 0.0f };   // -60..+12 dB
        std::atomic<float> masterTuneCents { 0.0f };   // -100..+100
        std::atomic<int>   polyphony       { 64 };     // 1..256
    };
    Overrides& getOverrides() noexcept { return overrides; }
    const Overrides& getOverrides() const noexcept { return overrides; }

    // High-definition CC control (drives ARIA custom-UI widgets)
    // setHDCC is called from the message thread (editor widget drag);
    // it caches the value + queues it lock-free for the audio thread,
    // which drains the queue at block top and calls sfizz_send_hdcc.
    // cc is 0..kNumHdcc-1 (sfizz's extended CC space). normValue 0..1.
    static constexpr int kNumHdcc = 512;
    void  setHDCC (int cc, float normValue);
    // Last value set for this CC, or -1 if never set (widget then uses
    // its own default). Read on the message thread by the editor.
    float getHDCC (int cc) const noexcept;

    // Control-block metadata for the stock auto-skin (non-ARIA SFZ that
    // declares `image=` + `label_cc&`). Queried via sfizz's messaging
    // API. Message-thread only.
    //   getControlImagePath -> resolved absolute path of <control> image=,
    //                          or empty File when none / not loaded.
    //   getControlCcLabels  -> (cc number, label) pairs from label_cc&.
    juce::File getControlImagePath() const;
    std::vector<std::pair<int, juce::String>> getControlCcLabels() const;

private:
    // Convert + load one preset of an SF2 through sfizz. Shared by
    // loadSf2File (index 0 + name caching) and loadSf2Preset (switch).
    bool applySf2Preset (const juce::File& sf2, int presetIndex,
                          juce::String& errorMessage);

    double currentSampleRate { 48000.0 };
    int    currentBlockSize  { 512 };
    juce::String loadedFilePath;     // empty when no file loaded
    juce::String lastLoadError;      // most recent setState / load failure
    Overrides overrides;

    // SF2 program switcher state: display metadata + active source index.
    // sf2PresetsLock guards every replacement of the vector against the
    // snapshot getSf2Presets() copies out on the message thread.
    std::vector<Sf2PresetInfo> sf2Presets;
    juce::CriticalSection      sf2PresetsLock;
    int                        sf2PresetIndex { 0 };

    // CC control plumbing. ccCache holds the last value the UI set per
    // CC (-1 = unset) for read-back + state save. ccFifo carries
    // (cc,value) changes from the message thread to the audio thread,
    // drained at the top of processBlock. SPSC: editor pushes, audio
    // drains.
    struct CcChange { int cc; float value; };
    std::array<std::atomic<float>, kNumHdcc> ccCache;
    juce::AbstractFifo            ccFifo { 2048 };
    std::array<CcChange, 2048>    ccQueue;

    // Cached "last applied" override values so processBlock only
    // hits sfizz's setter when the user has actually moved a knob.
    // sfizz internally takes a sample-rate lock on volume change;
    // skipping the no-op path avoids that on every block.
    float lastAppliedVolDb     { 0.0f };
    float lastAppliedTuneCents { 0.0f };
    int   lastAppliedPolyphony { 64 };

    // Serialises sfizz mutations (load/unload/voice-count) against
    // processBlock: the audio thread TRY-locks and passes one silent block
    // when a mutator holds it (PluginSlot's prepare<->process pattern).
    juce::SpinLock sfizzLock;

    // sfizz handle owned via pimpl so the public header doesn't drag
    // in sfizz.h (keeps compile time + ABI surface clean). The .cpp
    // owns the sfizz_synth_t*.
    struct Impl;
    std::unique_ptr<Impl> impl;

    // The destructor joins loadPool before freeing impl->synth (a running load
    // still dereferences it). Declared after impl so member teardown can't
    // invert that order either.
    std::atomic<bool> loadPending { false };
    juce::ThreadPool  loadPool { 1 };

    // A background load's completion is posted via MessageManager::callAsync,
    // which outlives the pool job the destructor joins. The queued callback
    // guards on this token so it no-ops if the processor was destroyed before
    // it ran (destruction + the callback are both message-thread, so the weak
    // ref's clear/get never race).
    JUCE_DECLARE_WEAK_REFERENCEABLE (DuskMultisampleProcessor)
};
} // namespace duskstudio
