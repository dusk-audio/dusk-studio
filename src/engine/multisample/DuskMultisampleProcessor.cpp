#include "DuskMultisampleProcessor.h"
#include "../../ui/multisample/DuskMultisampleEditor.h"

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

int DuskMultisampleProcessor::getNumRegions() const noexcept
{
    if (impl == nullptr || impl->synth == nullptr) return 0;
    return sfizz_get_num_regions (impl->synth);
}

bool DuskMultisampleProcessor::reloadCurrentFile (juce::String& errorMessage)
{
    if (loadedFilePath.isEmpty())
    {
        errorMessage = "No file loaded";
        return false;
    }
    return loadSfzFile (juce::File (loadedFilePath), errorMessage);
}

void DuskMultisampleProcessor::clearLoadedFile()
{
    if (impl == nullptr || impl->synth == nullptr) return;
    // sfizz_load_string with empty body unloads the current file.
    sfizz_load_string (impl->synth, "", "");
    loadedFilePath.clear();
    lastLoadError.clear();
}

void DuskMultisampleProcessor::setPolyphony (int newPolyphony)
{
    // Message-thread entry point. sfizz_set_num_voices is marked OFF
    // in sfizz.h (cannot be called while RT functions run). JUCE's
    // AudioProcessorEditor + state-load both run on the message
    // thread, so we're already off the audio thread here. The
    // processor's audio thread may be mid-block when this fires;
    // sfizz handles its own internal synchronisation - we don't add
    // a lock from this side.
    const int clamped = juce::jlimit (1, 256, newPolyphony);
    overrides.polyphony.store (clamped, std::memory_order_relaxed);
    if (impl != nullptr && impl->synth != nullptr)
    {
        sfizz_set_num_voices (impl->synth, clamped);
        lastAppliedPolyphony = clamped;
    }
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
        lastLoadError = errorMessage;
        return false;
    }
    loadedFilePath = sfz.getFullPathName();
    lastLoadError.clear();
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

    // Apply RT-safe override drift before MIDI dispatch. Each
    // setter is a no-op when the cached "last applied" equals the
    // current atom. sfizz documents sfizz_set_volume + sfizz_set_
    // tuning_frequency as RT functions (safe from processBlock);
    // sfizz_set_num_voices is marked OFF and CANNOT be called here -
    // it's invoked from the message thread via setPolyphony().
    {
        const float vol = overrides.masterVolDb.load (std::memory_order_relaxed);
        if (vol != lastAppliedVolDb)
        {
            sfizz_set_volume (impl->synth, vol);
            lastAppliedVolDb = vol;
        }
        const float tune = overrides.masterTuneCents.load (std::memory_order_relaxed);
        if (tune != lastAppliedTuneCents)
        {
            // sfizz tunes via absolute Hz of A4. Convert cents offset
            // from 440 Hz to Hz: f = 440 * 2^(cents/1200).
            const float a4 = 440.0f * std::pow (2.0f, tune / 1200.0f);
            sfizz_set_tuning_frequency (impl->synth, a4);
            lastAppliedTuneCents = tune;
        }
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
    juce::ValueTree state ("DuskMultisample");
    state.setProperty ("file", loadedFilePath, nullptr);
    state.setProperty ("masterVolDb",
                        overrides.masterVolDb.load (std::memory_order_relaxed),
                        nullptr);
    state.setProperty ("masterTuneCents",
                        overrides.masterTuneCents.load (std::memory_order_relaxed),
                        nullptr);
    state.setProperty ("polyphony",
                        overrides.polyphony.load (std::memory_order_relaxed),
                        nullptr);
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
        if (! loadSfzFile (juce::File (path), err))
        {
            // Non-fatal: processor stays in no-file state so the user
            // can pick a replacement via the editor. lastLoadError
            // surfaces the reason - editor polls + displays it.
            lastLoadError = err.isNotEmpty()
                              ? err
                              : ("File not found: " + path);
            DBG ("DuskMultisample setState: " << lastLoadError);
        }
    }

    if (state.hasProperty ("masterVolDb"))
        overrides.masterVolDb.store (
            juce::jlimit (-60.0f, 12.0f, (float) state.getProperty ("masterVolDb")),
            std::memory_order_relaxed);
    if (state.hasProperty ("masterTuneCents"))
        overrides.masterTuneCents.store (
            juce::jlimit (-100.0f, 100.0f, (float) state.getProperty ("masterTuneCents")),
            std::memory_order_relaxed);
    if (state.hasProperty ("polyphony"))
        setPolyphony (juce::jlimit (1, 256, (int) state.getProperty ("polyphony")));
}

juce::AudioProcessorEditor* DuskMultisampleProcessor::createEditor()
{
    return new DuskMultisampleEditor (*this);
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
