#include "DuskMultisampleProcessor.h"

namespace duskstudio
{
// Forward-decl Impl so the unique_ptr in the header is happy. Step 2
// fills this with the sfizz_synth_t* + the loaded file path + the
// override-param atomics. Step 1 is intentionally empty so the
// processor builds against sfizz without invoking any of its API.
struct DuskMultisampleProcessor::Impl
{
    // sfizz handle goes here in step 2.
};

DuskMultisampleProcessor::DuskMultisampleProcessor()
    : juce::AudioPluginInstance (BusesProperties()
        // Instrument bus layout: no audio input, stereo output.
        // PluginSlot's instrument-vs-effect routing reads
        // getTotalNumInputChannels() == 0 to pick the instrument
        // path (MIDI -> stereo audio); matches VST3 instrument
        // bus conventions.
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      impl (std::make_unique<Impl>())
{
}

DuskMultisampleProcessor::~DuskMultisampleProcessor() = default;

void DuskMultisampleProcessor::prepareToPlay (double sampleRate, int blockSize)
{
    currentSampleRate = sampleRate;
    currentBlockSize  = blockSize;
    // sfizz_set_sample_rate / sfizz_set_samples_per_block land here in
    // step 2. For step 1, the stub renders silence regardless of
    // prepare params.
}

void DuskMultisampleProcessor::releaseResources()
{
    // sfizz_synth_t teardown lands in step 2. Stub: nothing to release.
}

void DuskMultisampleProcessor::processBlock (juce::AudioBuffer<float>& buf,
                                              juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    juce::ignoreUnused (midi);
    // Silence: clear every output channel for the block. Step 2
    // replaces this with sfizz_render_block().
    buf.clear();
}

void DuskMultisampleProcessor::getStateInformation (juce::MemoryBlock& block)
{
    juce::ignoreUnused (block);
    // Step 4 carries (a) the loaded file path + (b) the override
    // params. For now, empty state is correct - PluginSlot's
    // setStateInformation no-ops on a zero-length block.
}

void DuskMultisampleProcessor::setStateInformation (const void* data, int size)
{
    juce::ignoreUnused (data, size);
}

void DuskMultisampleProcessor::fillInPluginDescription (juce::PluginDescription& desc) const
{
    // Identifies this processor to the picker + session save. Format
    // matches the AudioPluginFormat we'll register in step 3
    // (DuskMultisamplePluginFormat::getName).
    desc.name                = "Dusk Multisample";
    desc.descriptiveName     = "Dusk Studio native multisample instrument (.sfz / .sf2)";
    desc.pluginFormatName    = "DuskMultisample";
    desc.category            = "Instrument";
    desc.manufacturerName    = "Dusk Audio";
    desc.version             = "0.9.0";
    desc.fileOrIdentifier    = juce::String();  // file path lands at load time
    desc.isInstrument        = true;
    desc.numInputChannels    = 0;
    desc.numOutputChannels   = 2;
    desc.hasSharedContainer  = false;
}
} // namespace duskstudio
