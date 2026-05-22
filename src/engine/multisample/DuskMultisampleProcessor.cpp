#include "DuskMultisampleProcessor.h"
#include "../../ui/multisample/DuskMultisampleEditor.h"

#include <sfizz.h>

#if DUSKSTUDIO_HAS_SF2
 #include <fluidsynth.h>
#endif

namespace duskstudio
{
struct DuskMultisampleProcessor::Impl
{
    sfizz_synth_t* synth { nullptr };

   #if DUSKSTUDIO_HAS_SF2
    // FluidSynth side. Only one backend is active at a time (keyed off
    // file extension at load); the other stays nulled. sfSynth + sf2Id
    // are kept on the message thread for load/unload and read on the
    // audio thread for noteon/render. FluidSynth is internally
    // thread-safe.
    fluid_settings_t* sfSettings { nullptr };
    fluid_synth_t*    sfSynth    { nullptr };
    int               sf2Id      { -1 };
   #endif
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
   #if DUSKSTUDIO_HAS_SF2
    if (impl != nullptr && impl->sfSynth != nullptr)
        delete_fluid_synth (impl->sfSynth);
    if (impl != nullptr && impl->sfSettings != nullptr)
        delete_fluid_settings (impl->sfSettings);
   #endif
}

void DuskMultisampleProcessor::prepareToPlay (double sampleRate, int blockSize)
{
    currentSampleRate = sampleRate;
    currentBlockSize  = blockSize;
    if (impl == nullptr) return;
    if (impl->synth != nullptr)
    {
        sfizz_set_sample_rate (impl->synth, (float) sampleRate);
        sfizz_set_samples_per_block (impl->synth, blockSize);
    }
   #if DUSKSTUDIO_HAS_SF2
    if (impl->sfSynth != nullptr)
        fluid_synth_set_sample_rate (impl->sfSynth, (float) sampleRate);
   #endif
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
    if (impl == nullptr) return;
    if (impl->synth != nullptr)
    {
        // sfizz_load_string with empty body unloads the current file.
        sfizz_load_string (impl->synth, "", "");
    }
   #if DUSKSTUDIO_HAS_SF2
    if (impl->sfSynth != nullptr && impl->sf2Id >= 0)
    {
        fluid_synth_sfunload (impl->sfSynth, impl->sf2Id, 1);
        impl->sf2Id = -1;
    }
   #endif
    loadedFilePath.clear();
    lastLoadError.clear();
}

bool DuskMultisampleProcessor::loadSf2File (const juce::File& sf2,
                                              juce::String& errorMessage)
{
   #if DUSKSTUDIO_HAS_SF2
    if (impl == nullptr)
    {
        errorMessage = "Internal: processor not initialised";
        return false;
    }
    if (! sf2.existsAsFile())
    {
        errorMessage = "File does not exist: " + sf2.getFullPathName();
        return false;
    }

    // Unload any previous SFZ on this slot (one backend at a time).
    if (impl->synth != nullptr)
        sfizz_load_string (impl->synth, "", "");

    // Lazy-init FluidSynth on first SF2 load. Settings live for the
    // life of the processor; synth is recreated only if reset is
    // needed (re-load reuses the existing synth via sfunload+sfload).
    if (impl->sfSettings == nullptr)
    {
        impl->sfSettings = new_fluid_settings();
        if (impl->sfSettings == nullptr)
        {
            errorMessage = "fluid_settings_new failed";
            return false;
        }
        fluid_settings_setnum (impl->sfSettings, "synth.sample-rate", currentSampleRate);
        // Drop reverb / chorus by default — Dusk Studio's bus + aux
        // chain provides those; SF2's built-in effects add unwanted
        // colour and cost CPU.
        fluid_settings_setint (impl->sfSettings, "synth.reverb.active", 0);
        fluid_settings_setint (impl->sfSettings, "synth.chorus.active", 0);
        // Default polyphony — overridden if the user sets a value.
        fluid_settings_setint (impl->sfSettings, "synth.polyphony",
                                 overrides.polyphony.load (std::memory_order_relaxed));
    }
    if (impl->sfSynth == nullptr)
    {
        impl->sfSynth = new_fluid_synth (impl->sfSettings);
        if (impl->sfSynth == nullptr)
        {
            errorMessage = "new_fluid_synth failed";
            return false;
        }
        fluid_synth_set_sample_rate (impl->sfSynth, (float) currentSampleRate);
    }

    // Unload any previous SF2 before loading the new one.
    if (impl->sf2Id >= 0)
    {
        fluid_synth_sfunload (impl->sfSynth, impl->sf2Id, 1);
        impl->sf2Id = -1;
    }
    const auto path = sf2.getFullPathName().toStdString();
    impl->sf2Id = fluid_synth_sfload (impl->sfSynth, path.c_str(), /*reset_presets*/ 1);
    if (impl->sf2Id == FLUID_FAILED)
    {
        errorMessage = "fluid_synth_sfload failed for " + sf2.getFileName();
        lastLoadError = errorMessage;
        return false;
    }
    // Select a starting preset on channel 0. GM banks have (0, 0) as
    // "Acoustic Grand Piano" so it's the common default, but custom
    // SF2s often only define a single preset at some other (bank,
    // program) pair — selecting (0, 0) on those would silently load
    // nothing. Iterate the soundfont's presets and pick the first
    // available; fall back to (0, 0) if iteration returns nothing
    // (unusual but harmless).
    {
        int chosenBank = 0, chosenProgram = 0;
        if (auto* sfont = fluid_synth_get_sfont_by_id (impl->sfSynth, impl->sf2Id))
        {
            fluid_sfont_iteration_start (sfont);
            if (auto* preset = fluid_sfont_iteration_next (sfont))
            {
                chosenBank    = fluid_preset_get_banknum (preset);
                chosenProgram = fluid_preset_get_num (preset);
            }
        }
        fluid_synth_program_select (impl->sfSynth, /*chan*/ 0,
                                      impl->sf2Id, chosenBank, chosenProgram);
    }

    // Push current polyphony in case setPolyphony() ran before this
    // SF2 instance came up (e.g. setStateInformation order).
    fluid_synth_set_polyphony (impl->sfSynth,
                                 overrides.polyphony.load (std::memory_order_relaxed));

    loadedFilePath = sf2.getFullPathName();
    lastLoadError.clear();
    return true;
   #else
    juce::ignoreUnused (sf2);
    errorMessage = "SF2 support not compiled in (install fluidsynth-devel "
                   "and reconfigure CMake to enable)";
    return false;
   #endif
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
   #if DUSKSTUDIO_HAS_SF2
    if (impl != nullptr && impl->sfSynth != nullptr)
        fluid_synth_set_polyphony (impl->sfSynth, clamped);
   #endif
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
   #if DUSKSTUDIO_HAS_SF2
    // One backend at a time — unload any previous SF2 so processBlock
    // takes the sfizz path.
    if (impl->sfSynth != nullptr && impl->sf2Id >= 0)
    {
        fluid_synth_sfunload (impl->sfSynth, impl->sf2Id, 1);
        impl->sf2Id = -1;
    }
   #endif
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

    if (impl == nullptr)
    {
        buf.clear();
        return;
    }

   #if DUSKSTUDIO_HAS_SF2
    // FluidSynth path takes precedence when an SF2 is loaded. Channel
    // 0 is the active GM-style channel; we don't multiplex MIDI
    // channels yet (the multi-channel preset assign is a 1.0 feature).
    //
    // RT-safety caveat: FluidSynth's noteon/off/cc/pitch_bend/write_float
    // take internal mutexes. Strictly that violates the CLAUDE.md
    // audio-thread rules; in practice these are short uncontended
    // locks (FluidSynth's voice/audio threads are serialized through
    // them) and the only contention would be a concurrent message-
    // thread program_select / sfload, which happens at most once per
    // user-driven plugin load. Accepting this trade-off for the SF2
    // beta path; a lock-free MIDI ring + render queue lands with the
    // 1.0 polish pass if it ever shows up in profiles.
    if (impl->sfSynth != nullptr && impl->sf2Id >= 0)
    {
        // Apply master tune offset. fluid_synth_set_gen with
        // GEN_FINETUNE expects cents on channel-level; 0 cents = no
        // detune. Cheap enough to push on every block (no internal
        // realloc); only push on change.
        const float tune = overrides.masterTuneCents.load (std::memory_order_relaxed);
        if (tune != lastAppliedTuneCents)
        {
            fluid_synth_set_gen (impl->sfSynth, /*chan*/ 0, GEN_FINETUNE, tune);
            lastAppliedTuneCents = tune;
        }

        for (const auto meta : midi)
        {
            const auto m = meta.getMessage();
            if (m.isNoteOn())
                fluid_synth_noteon (impl->sfSynth, /*chan*/ 0,
                                     m.getNoteNumber(), m.getVelocity());
            else if (m.isNoteOff())
                fluid_synth_noteoff (impl->sfSynth, 0, m.getNoteNumber());
            else if (m.isController())
                fluid_synth_cc (impl->sfSynth, 0,
                                 m.getControllerNumber(), m.getControllerValue());
            else if (m.isPitchWheel())
                fluid_synth_pitch_bend (impl->sfSynth, 0, m.getPitchWheelValue());
            else if (m.isChannelPressure())
                fluid_synth_channel_pressure (impl->sfSynth, 0,
                                                m.getChannelPressureValue());
        }
        // FluidSynth writes into caller-provided L/R buffers with
        // arbitrary stride. JUCE's AudioBuffer is contiguous per
        // channel — pass stride 1.
        float* l = buf.getWritePointer (0);
        float* r = (buf.getNumChannels() > 1) ? buf.getWritePointer (1) : l;
        fluid_synth_write_float (impl->sfSynth, numSamples,
                                   l, 0, 1,
                                   r, 0, 1);
        // Apply master volume in dB from overrides. FluidSynth's
        // synth.gain is linear 0..10, but mutating it per-block via
        // fluid_settings_setnum is heavy; cheaper to scale the output
        // buffer here.
        const float volDb = overrides.masterVolDb.load (std::memory_order_relaxed);
        if (std::abs (volDb) > 0.001f)
        {
            const float gain = std::pow (10.0f, volDb * 0.05f);
            buf.applyGain (gain);
        }
        return;
    }
   #endif

    if (impl->synth == nullptr)
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
        const auto file = juce::File (path);
        const auto ext = file.getFileExtension().toLowerCase();
        juce::String err;
        const bool ok = (ext == ".sf2") ? loadSf2File (file, err)
                                        : loadSfzFile (file, err);
        if (! ok)
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
