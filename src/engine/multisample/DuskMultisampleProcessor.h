#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>

namespace duskstudio
{
// Native Multisample instrument: a juce::AudioPluginInstance that
// loads .sfz (and, in 1.0, .sf2) files and renders them through the
// vendored sfizz engine. Lives in src/engine/multisample/ alongside
// the SFZ/SF2 -> sfizz adapters.
//
// Step 1 (this commit): zero-feature stub. Reports the correct bus
// layout + plugin description + clears the output buffer. Step 2
// wires the sfizz_synth_t lifecycle + processBlock; step 3 wires the
// PluginFormat that produces instances of this class via the plugin
// picker.
//
// Lifetime: created on the message thread by McuController... wait,
// by PluginManager via DuskMultisamplePluginFormat::createPluginInstance.
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
    bool hasEditor() const override                            { return false; }
    juce::AudioProcessorEditor* createEditor() override         { return nullptr; }
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

    // True if a soundfont has been loaded. Used by editor UI to show
    // "(no file)" vs the loaded file name.
    bool hasLoadedFile() const noexcept { return loadedFilePath.isNotEmpty(); }
    const juce::String& getLoadedFilePath() const noexcept { return loadedFilePath; }

private:
    double currentSampleRate { 48000.0 };
    int    currentBlockSize  { 512 };
    juce::String loadedFilePath;     // empty when no file loaded

    // sfizz handle owned via pimpl so the public header doesn't drag
    // in sfizz.h (keeps compile time + ABI surface clean). The .cpp
    // owns the sfizz_synth_t*.
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace duskstudio
