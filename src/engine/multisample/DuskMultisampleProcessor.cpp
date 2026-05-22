#include "DuskMultisampleProcessor.h"

#include <sfizz.h>

namespace duskstudio
{
struct DuskMultisampleProcessor::Impl
{
    sfizz_synth_t* synth { nullptr };
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
    impl->synth = sfizz_create_synth();
    // sfizz allocates per-voice state lazily on prepareToPlay, so the
    // ctor is cheap. Polyphony default (128 voices on v1.2) is fine
    // until step 5's editor exposes a slider override.
}

DuskMultisampleProcessor::~DuskMultisampleProcessor()
{
    if (impl != nullptr && impl->synth != nullptr)
        sfizz_free (impl->synth);
}

void DuskMultisampleProcessor::prepareToPlay (double sampleRate, int blockSize)
{
    currentSampleRate = sampleRate;
    currentBlockSize  = blockSize;
    if (impl == nullptr || impl->synth == nullptr) return;
    sfizz_set_sample_rate (impl->synth, (float) sampleRate);
    sfizz_set_samples_per_block (impl->synth, blockSize);
}

void DuskMultisampleProcessor::releaseResources()
{
    // sfizz keeps its voice state allocated across releaseResources -
    // a subsequent prepareToPlay reuses the buffers. No-op here.
}

bool DuskMultisampleProcessor::loadSfzFile (const juce::File& sfz,
                                              juce::String& errorMessage)
{
    if (impl == nullptr || impl->synth == nullptr)
    {
        errorMessage = "Internal: sfizz synth not initialised";
        return false;
    }
    if (! sfz.existsAsFile())
    {
        errorMessage = "File does not exist: " + sfz.getFullPathName();
        return false;
    }
    const auto path = sfz.getFullPathName().toStdString();
    const bool ok = sfizz_load_file (impl->synth, path.c_str());
    if (! ok)
    {
        errorMessage = "sfizz_load_file failed for " + sfz.getFileName();
        return false;
    }
    loadedFilePath = sfz.getFullPathName();
    return true;
}

void DuskMultisampleProcessor::processBlock (juce::AudioBuffer<float>& buf,
                                              juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buf.getNumSamples();
    if (numSamples == 0) return;

    if (impl == nullptr || impl->synth == nullptr)
    {
        buf.clear();
        return;
    }

    // Dispatch incoming MIDI events to sfizz. sfizz batches them
    // against the current block; delays are sample offsets within
    // the block.
    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        const int delay = juce::jlimit (0, numSamples - 1, meta.samplePosition);
        if (m.isNoteOn())
            sfizz_send_note_on (impl->synth, delay,
                                 m.getNoteNumber(), m.getVelocity());
        else if (m.isNoteOff())
            sfizz_send_note_off (impl->synth, delay,
                                  m.getNoteNumber(), m.getVelocity());
        else if (m.isController())
            sfizz_send_cc (impl->synth, delay,
                            m.getControllerNumber(), m.getControllerValue());
        else if (m.isPitchWheel())
            sfizz_send_pitch_wheel (impl->synth, delay, m.getPitchWheelValue());
        else if (m.isChannelPressure())
            sfizz_send_channel_aftertouch (impl->synth, delay,
                                            m.getChannelPressureValue());
    }

    // Render. sfizz wants float** with 2 channels for the default
    // stereo output layout. JUCE's AudioBuffer already gives us
    // contiguous per-channel pointers.
    float* chans[2] = { buf.getWritePointer (0), buf.getWritePointer (1) };
    sfizz_render_block (impl->synth, chans, 2, numSamples);
}

void DuskMultisampleProcessor::getStateInformation (juce::MemoryBlock& block)
{
    // Step 4 carries (a) the loaded file path + (b) the override
    // params. For now, persist just the file path so a session
    // reload re-loads the same .sfz.
    juce::ValueTree state ("DuskMultisample");
    state.setProperty ("file", loadedFilePath, nullptr);
    juce::MemoryOutputStream stream (block, false);
    state.writeToStream (stream);
}

void DuskMultisampleProcessor::setStateInformation (const void* data, int size)
{
    if (data == nullptr || size <= 0) return;
    juce::MemoryInputStream stream (data, (size_t) size, false);
    const auto state = juce::ValueTree::readFromStream (stream);
    if (! state.isValid()) return;
    const auto path = state.getProperty ("file").toString();
    if (path.isNotEmpty())
    {
        juce::String err;
        loadSfzFile (juce::File (path), err);
        // Loading errors are logged but non-fatal - the processor
        // stays alive in a no-file state so the user can pick a
        // replacement file via the editor.
        if (err.isNotEmpty())
            DBG ("DuskMultisample setState: " << err);
    }
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
    desc.fileOrIdentifier    = loadedFilePath;   // empty until a file is loaded
    desc.isInstrument        = true;
    desc.numInputChannels    = 0;
    desc.numOutputChannels   = 2;
    desc.hasSharedContainer  = false;
}
} // namespace duskstudio
