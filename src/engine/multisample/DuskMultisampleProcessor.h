#pragma once

#include <juce_core/juce_core.h>

#include "MultisampleBundle.h"
#include "../hosting/INativeInstance.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace duskstudio
{
// Native Multisample instrument: a hosting::INativeInstance that loads
// .sfz / .sf2 files and renders them through the vendored sfizz engine.
// Lives in src/engine/multisample/ alongside the SFZ/SF2 -> sfizz adapters.
//
// Lifetime: created on the message thread by NativeMultisampleSlot, which
// fences load / unload with the engine process gate exactly like the CLAP,
// LV2 and VST3 rungs.
class DuskMultisampleProcessor final : public hosting::INativeInstance
{
public:
    DuskMultisampleProcessor();
    ~DuskMultisampleProcessor() override;

    // NativeInsertSlot construction hook: load the bundle's soundfont. The
    // multisample rung has exactly one plugin per bundle, so pluginId is unused.
    bool create (const MultisampleBundle& bundle, const std::string& pluginId,
                 std::string& errorOut);

    // hosting::INativeInstance.
    const hosting::PortLayout& portLayout() const noexcept override { return layout; }
    bool activate (double sampleRate, int maxBlockFrames, std::string& errorOut) override;
    void deactivate() override;
    bool reactivate (double sampleRate, int maxBlockFrames, std::string& errorOut) override;
    bool isActive() const noexcept override { return active.load (std::memory_order_acquire); }
    void processBlock (const hosting::PortBuffers& io) noexcept override;
    bool saveState (std::vector<uint8_t>& out) const override;
    bool loadState (const std::vector<uint8_t>& in) override;
    int  getLatencySamples() const noexcept override { return 0; }

    // Load an .sfz file synchronously on the message thread. Returns
    // true + clears errorMessage on success.
    bool loadSfzFile (const juce::File& sfz, juce::String& errorMessage);

    // Load an .sf2 (SoundFont 2) file. Converts the first preset to SFZ
    // + extracted WAVs (Sf2Reader + Sf2ToSfz) and plays it through the
    // vendored sfizz engine - no fluidsynth dependency. Caches preset
    // display metadata for the editor's program switcher.
    bool loadSf2File (const juce::File& sf2, juce::String& errorMessage);

    // Switch the retained SF2 reference to another preset (re-converts + reloads).
    // No-op error when the retained reference isn't an SF2.
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
    int getSf2PresetIndex() const noexcept { return sf2PresetIndex.load (std::memory_order_relaxed); }

    // True if a soundfont path is retained. Used by editor UI to show
    // "(no file)" vs the file name and keep Reload available after failure.
    bool hasLoadedFile() const noexcept { return loadedFilePath.isNotEmpty(); }
    const juce::String& getLoadedFilePath() const noexcept { return loadedFilePath; }

    // Number of regions sfizz holds from the active .sfz. 0 when the runtime
    // state is empty, including after a failed reload.
    int getNumRegions() const noexcept;

    // Reload the retained soundfont path from disk. No-op when no path is
    // retained. Returns true on success.
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

    // Message thread. Join any in-flight background load. Call this BEFORE the
    // engine's process gate parks the audio thread on an unload: the slot's
    // teardown joins loadPool in the destructor, and a GM-bank load takes
    // seconds - parking the audio thread across it is an audible stall.
    void cancelPendingLoads();

    // Thread-safe copy of the persisted soundfont path, empty after Clear.
    // loadedFilePath itself is written by the loader thread, so JUCE's String
    // refcount must not be shared across the hand-off; this hands back an
    // independent std::string taken under a lock. Never reflects an in-flight
    // load - the shared value only advances once a load has succeeded.
    std::string getLoadedPathSnapshot() const;

    // Drop the runtime soundfont and its persisted reference. After this call
    // the processor renders silence; subsequent loadSfzFile / loadState can replace it.
    void clearLoadedFile();

    // Polyphony change. Per sfizz.h, sfizz_set_num_voices is marked
    // OFF - "cannot be invoked while a thread is calling RT
    // functions". setPolyphony() runs on the message thread (editor
    // / state-load callers), pauses sfizz briefly, applies, then
    // resumes. The Overrides atom is updated so saveState
    // sees the latest value.
    void setPolyphony (int newPolyphony);

    // Surfaces the most-recent error from loadState / loadSfzFile when
    // called from the deserialiser. Editor polls it so a missing-file
    // restore shows "(file not found)" instead of silent emptiness.
    // Cleared when a load succeeds.
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
    // it caches the value and flags the CC lock-free, and the audio thread
    // pushes the cached value to sfizz_send_hdcc at block top. Repeated sets
    // between two blocks collapse - sfizz sees only the newest value.
    // cc is 0..kNumHdcc-1 (sfizz's extended CC space). normValue 0..1.
    static constexpr int kNumHdcc = 512;
    void  setHDCC (int cc, float normValue);
    // Last value set for this CC, or -1 if never set (widget then uses
    // its own default). Read on the message thread by the editor.
    float getHDCC (int cc) const noexcept;

   #if defined(DUSKSTUDIO_TESTS)
    // Observes successful sfizz_send_hdcc dispatches at block top. Kept out
    // of production builds; tests use it to verify the coalescer without
    // reaching through the sfizz pimpl.
    using HdccDispatchObserverForTest = void (*) (void*, int, float);
    void setHdccDispatchObserverForTest (void* context,
                                         HdccDispatchObserverForTest observer) noexcept
    {
        hdccDispatchObserverContext = context;
        hdccDispatchObserver = observer;
    }
   #endif

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
    void clearRuntimeState();

    hosting::PortLayout layout;
    std::atomic<bool>   active { false };

    // The activate() block size, so processBlock can reject a block sfizz was
    // never sized for. Published INSIDE the sfizz render lock alongside
    // sfizz_set_samples_per_block and read by the audio thread under the same
    // lock, so the guard can never see a size sfizz has not applied yet.
    std::atomic<int> currentBlockSize { 512 };
    juce::String loadedFilePath;     // retained across failed loads

    // Cross-thread copy of loadedFilePath (see getLoadedPathSnapshot). Written
    // wherever loadedFilePath is, read by the message thread. Never the audio
    // thread, so a plain mutex is fine.
    mutable std::mutex loadedPathLock;
    std::string        loadedPathShared;
    void publishLoadedPath (const juce::String& path);
    juce::String lastLoadError;      // most recent loadState / load failure
    Overrides overrides;

    // SF2 program switcher state: display metadata + active source index.
    // sf2PresetsLock guards every replacement of the vector against the
    // snapshot getSf2Presets() copies out on the message thread.
    std::vector<Sf2PresetInfo> sf2Presets;
    juce::CriticalSection      sf2PresetsLock;
    // Written by the loader thread, read by the editor timer - atomic so the
    // int itself can't tear (the presets vector has its own lock).
    std::atomic<int>           sf2PresetIndex { 0 };

    // CC control plumbing. ccCache holds the last value the UI set per
    // CC (-1 = unset) for read-back + state save. ccDirty flags which of
    // those the audio thread has still to hand to sfizz, which reads the
    // value back out of the cache at drain time: a knob moved faster than
    // the block rate collapses onto its newest value instead of settling
    // sfizz on a stale one the cache and the UI disagree with.
    static constexpr int kCcDirtyWords = (kNumHdcc + 63) / 64;
    std::array<std::atomic<float>, kNumHdcc>              ccCache;
    std::array<std::atomic<std::uint64_t>, kCcDirtyWords> ccDirty {};

   #if defined(DUSKSTUDIO_TESTS)
    void* hdccDispatchObserverContext { nullptr };
    HdccDispatchObserverForTest hdccDispatchObserver { nullptr };
   #endif

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
    // Bumped by cancelPendingLoads. A job that was already running when
    // removeAllJobs returned still posts its completion; the captured
    // generation lets that completion recognise it has been disowned instead
    // of clearing a LATER load's pending flag and firing a stale onDone.
    std::atomic<std::uint64_t> loadGeneration { 0 };
    juce::ThreadPool  loadPool { 1 };

    // A background load's completion is posted via dusk::callAsync,
    // which outlives the pool job the destructor joins. The queued callback
    // guards on this token so it no-ops if the processor was destroyed before
    // it ran (destruction + the callback are both message-thread, so the weak
    // ref's clear/get never race).
    JUCE_DECLARE_WEAK_REFERENCEABLE (DuskMultisampleProcessor)
};
} // namespace duskstudio
