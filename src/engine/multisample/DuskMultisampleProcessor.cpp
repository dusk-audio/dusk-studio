#include "DuskMultisampleProcessor.h"
#include "Sf2ToSfz.h"
#include "../../ui/multisample/DuskMultisampleEditor.h"

#include <sfizz.h>

namespace duskstudio
{
struct DuskMultisampleProcessor::Impl
{
    sfizz_synth_t* synth { nullptr };

    // SF2 playback: convert the SoundFont to SFZ + extracted WAVs and
    // run it through the sfizz engine (no fluidsynth dependency). This
    // dir holds the extracted samples for the currently loaded SF2;
    // deleted on reload / clear / destruction.
    juce::File sf2TempDir;
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

    // -1 = "CC never set by the UI" so widgets fall back to their own
    // default instead of snapping to 0.
    for (auto& c : ccCache)
        c.store (-1.0f, std::memory_order_relaxed);
}

void DuskMultisampleProcessor::setHDCC (int cc, float normValue)
{
    if (cc < 0 || cc >= kNumHdcc) return;
    const float v = juce::jlimit (0.0f, 1.0f, normValue);
    ccCache[(size_t) cc].store (v, std::memory_order_relaxed);

    // Queue for the audio thread. If the FIFO is momentarily full
    // (user spamming a control faster than the audio block rate), drop
    // the oldest-unread by simply not writing - the cache still holds
    // the latest value and the next change re-queues it.
    const auto scope = ccFifo.write (1);
    if (scope.blockSize1 > 0)
        ccQueue[(size_t) scope.startIndex1] = { cc, v };
    else if (scope.blockSize2 > 0)
        ccQueue[(size_t) scope.startIndex2] = { cc, v };
}

float DuskMultisampleProcessor::getHDCC (int cc) const noexcept
{
    if (cc < 0 || cc >= kNumHdcc) return -1.0f;
    return ccCache[(size_t) cc].load (std::memory_order_relaxed);
}

juce::File DuskMultisampleProcessor::getControlImagePath() const
{
    if (impl == nullptr || impl->synth == nullptr || loadedFilePath.isEmpty()
        || isLoadPending())
        return {};

    // Round-trip the '/image' OSC query through a transient sfizz client.
    // The reply arrives synchronously in the receive callback.
    juce::String rel;
    auto* client = sfizz_create_client (&rel);
    sfizz_set_receive_callback (client,
        [] (void* data, int, const char* /*path*/, const char* sig, const sfizz_arg_t* args)
        {
            if (data != nullptr && sig != nullptr && sig[0] == 's'
                && args != nullptr && args[0].s != nullptr)
                *static_cast<juce::String*> (data) = juce::String::fromUTF8 (args[0].s);
        });
    sfizz_send_message (impl->synth, client, 0, "/image", "", nullptr);
    sfizz_delete_client (client);

    if (rel.isEmpty()) return {};
    if (juce::File::isAbsolutePath (rel))
        return juce::File (rel);
    // Relative to the loaded .sfz's directory.
    return juce::File (loadedFilePath).getParentDirectory().getChildFile (rel);
}

std::vector<std::pair<int, juce::String>> DuskMultisampleProcessor::getControlCcLabels() const
{
    std::vector<std::pair<int, juce::String>> out;
    if (impl == nullptr || impl->synth == nullptr || isLoadPending()) return out;
    const unsigned n = sfizz_get_num_cc_labels (impl->synth);
    out.reserve (n);
    for (unsigned i = 0; i < n; ++i)
    {
        const int cc = sfizz_get_cc_label_number (impl->synth, (int) i);
        const char* t = sfizz_get_cc_label_text (impl->synth, (int) i);
        if (cc >= 0 && t != nullptr)
            out.emplace_back (cc, juce::String::fromUTF8 (t));
    }
    return out;
}

DuskMultisampleProcessor::~DuskMultisampleProcessor()
{
    // Wait for any in-flight background load before freeing the synth it uses;
    // the load jobs dereference impl->synth.
    loadPool.removeAllJobs (false, -1);
    if (impl != nullptr && impl->synth != nullptr)
        sfizz_free (impl->synth);
    if (impl != nullptr && impl->sf2TempDir != juce::File())
        impl->sf2TempDir.deleteRecursively();
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
}

void DuskMultisampleProcessor::releaseResources()
{
    // sfizz keeps its voice state allocated across releaseResources -
    // a subsequent prepareToPlay reuses the buffers. No-op here.
}

int DuskMultisampleProcessor::getNumRegions() const noexcept
{
    // A background load is mutating the synth off-thread — don't read it.
    if (impl == nullptr || impl->synth == nullptr || isLoadPending()) return 0;
    return sfizz_get_num_regions (impl->synth);
}

bool DuskMultisampleProcessor::reloadCurrentFile (juce::String& errorMessage)
{
    if (loadedFilePath.isEmpty())
    {
        errorMessage = "No file loaded";
        return false;
    }
    const juce::File f (loadedFilePath);
    return f.getFileExtension().toLowerCase() == ".sf2"
             ? loadSf2File (f, errorMessage)
             : loadSfzFile (f, errorMessage);
}

void DuskMultisampleProcessor::clearLoadedFile()
{
    if (impl == nullptr) return;
    if (impl->synth != nullptr)
    {
        const juce::SpinLock::ScopedLockType lock (sfizzLock);
        // sfizz_load_string with empty body unloads the current file.
        sfizz_load_string (impl->synth, "", "");
    }
    if (impl->sf2TempDir != juce::File())
    {
        impl->sf2TempDir.deleteRecursively();
        impl->sf2TempDir = juce::File();
    }
    // Drop SF2 preset state too so the editor's program switcher doesn't
    // show stale presets from the just-unloaded SoundFont.
    sf2PresetNames.clear();
    sf2PresetIndex = -1;
    loadedFilePath.clear();
    lastLoadError.clear();
}

bool DuskMultisampleProcessor::loadSf2File (const juce::File& sf2,
                                              juce::String& errorMessage)
{
    if (impl == nullptr || impl->synth == nullptr)
    {
        errorMessage = "Internal: processor not initialised";
        return false;
    }
    if (! sf2.existsAsFile())
    {
        errorMessage = "File does not exist: " + sf2.getFullPathName();
        return false;
    }

    // Cache the preset name list (cheap metadata parse, no sample
    // extraction) so the editor can offer a program switcher.
    sf2PresetNames.clear();
    if (auto parsed = readSf2 (sf2); parsed.ok)
        for (const auto& p : parsed.presets)
            sf2PresetNames.add (p.name);

    return applySf2Preset (sf2, 0, errorMessage);
}

bool DuskMultisampleProcessor::loadSf2Preset (int presetIndex,
                                                juce::String& errorMessage)
{
    if (loadedFilePath.isEmpty()
        || juce::File (loadedFilePath).getFileExtension().toLowerCase() != ".sf2")
    {
        errorMessage = "No SF2 loaded";
        return false;
    }
    return applySf2Preset (juce::File (loadedFilePath), presetIndex, errorMessage);
}

void DuskMultisampleProcessor::loadFileAsync (
    const juce::File& file, std::function<void (bool, juce::String)> onDone)
{
    if (loadPending.exchange (true, std::memory_order_acq_rel))
    {
        if (onDone) onDone (false, "A load is already in progress");
        return;
    }
    loadPool.addJob ([this, file, onDone = std::move (onDone)]
    {
        juce::String err;
        const bool ok = file.getFileExtension().toLowerCase() == ".sf2"
                            ? loadSf2File (file, err)
                            : loadSfzFile (file, err);
        juce::MessageManager::callAsync ([this, onDone, ok, err]
        {
            if (onDone) onDone (ok, err);
            // Stay authoritative until the UI completion has run.
            loadPending.store (false, std::memory_order_release);
        });
    });
}

void DuskMultisampleProcessor::loadSf2PresetAsync (
    int presetIndex, std::function<void (bool, juce::String)> onDone)
{
    if (loadPending.exchange (true, std::memory_order_acq_rel))
    {
        if (onDone) onDone (false, "A load is already in progress");
        return;
    }
    loadPool.addJob ([this, presetIndex, onDone = std::move (onDone)]
    {
        juce::String err;
        const bool ok = loadSf2Preset (presetIndex, err);
        juce::MessageManager::callAsync ([this, onDone, ok, err]
        {
            if (onDone) onDone (ok, err);
            // Stay authoritative until the UI completion has run.
            loadPending.store (false, std::memory_order_release);
        });
    });
}

bool DuskMultisampleProcessor::applySf2Preset (const juce::File& sf2,
                                                 int presetIndex,
                                                 juce::String& errorMessage)
{
    // Native SF2 -> SFZ: convert one preset into an SFZ body plus a dir
    // of extracted WAVs, then load it through sfizz. No fluidsynth.
    auto newDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                      .getChildFile ("DuskStudio")
                      .getChildFile ("sf2_" + juce::String::toHexString (
                          juce::Random::getSystemRandom().nextInt64()));

    auto conv = convertSf2Preset (sf2, presetIndex, newDir);
    if (! conv.ok)
    {
        errorMessage = conv.error;
        lastLoadError = errorMessage;
        newDir.deleteRecursively();
        return false;
    }

    // sfizz roots relative sample= names at the SFZ path's parent dir,
    // so point the virtual SFZ inside the temp dir holding the WAVs.
    const auto virtualSfz = newDir.getChildFile ("preset.sfz");
    const auto body       = conv.sfzText.toStdString();
    const auto pathStr    = virtualSfz.getFullPathName().toStdString();
    // Only the sfizz call itself is serialised against processBlock — the
    // expensive conversion above runs unlocked so the audio thread keeps
    // rendering during it.
    const juce::SpinLock::ScopedLockType lock (sfizzLock);
    if (! sfizz_load_string (impl->synth, pathStr.c_str(), body.c_str()))
    {
        errorMessage = "sfizz rejected the converted SF2 preset";
        lastLoadError = errorMessage;
        newDir.deleteRecursively();
        return false;
    }

    // New sample set is live - drop the previous load's temp dir.
    if (impl->sf2TempDir != juce::File() && impl->sf2TempDir != newDir)
        impl->sf2TempDir.deleteRecursively();
    impl->sf2TempDir = newDir;

    sf2PresetIndex = juce::jlimit (0, juce::jmax (0, sf2PresetNames.size() - 1),
                                    presetIndex);
    loadedFilePath = sf2.getFullPathName();
    lastLoadError.clear();
    return true;
}

void DuskMultisampleProcessor::setPolyphony (int newPolyphony)
{
    // Message/loader-thread entry point — never the audio thread.
    const int clamped = juce::jlimit (1, 256, newPolyphony);
    overrides.polyphony.store (clamped, std::memory_order_relaxed);
    if (impl != nullptr && impl->synth != nullptr)
    {
        // sfizz_set_num_voices is marked OFF (not callable while RT
        // functions run) — hold the render lock so processBlock dry-passes
        // for the duration instead of racing it.
        const juce::SpinLock::ScopedLockType lock (sfizzLock);
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
    bool ok = false;
    {
        const juce::SpinLock::ScopedLockType lock (sfizzLock);
        ok = sfizz_load_file (impl->synth, path.c_str());
    }
    if (! ok)
    {
        errorMessage = "sfizz_load_file failed for " + sfz.getFileName();
        lastLoadError = errorMessage;
        return false;
    }
    // Drop any SF2-extracted temp samples - this slot is now a plain
    // SFZ load and the previous SF2's WAVs are no longer referenced.
    if (impl->sf2TempDir != juce::File())
    {
        impl->sf2TempDir.deleteRecursively();
        impl->sf2TempDir = juce::File();
    }
    // Clear SF2 preset state — an SFZ load has no presets, and leaving
    // the previous SF2's list around would show a stale program switcher.
    sf2PresetNames.clear();
    sf2PresetIndex = -1;
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

    if (impl->synth == nullptr)
    {
        buf.clear();
        return;
    }

    // Loads mutate the sfizz synth from the loader thread; TRY-lock and pass
    // one silent block instead of racing them (PluginSlot's prepare↔process
    // pattern). The message-thread mutators take this lock blocking.
    const juce::SpinLock::ScopedTryLockType renderLock (sfizzLock);
    if (! renderLock.isLocked())
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

    // Drain UI-driven CC changes (ARIA custom-UI knobs/faders) queued
    // by setHDCC on the message thread. sfizz_send_hdcc is the RT-side
    // entry point; delay 0 applies at block start.
    {
        const auto ready = ccFifo.getNumReady();
        if (ready > 0)
        {
            const auto scope = ccFifo.read (ready);
            for (int i = 0; i < scope.blockSize1; ++i)
            {
                const auto& c = ccQueue[(size_t) (scope.startIndex1 + i)];
                sfizz_send_hdcc (impl->synth, 0, c.cc, c.value);
            }
            for (int i = 0; i < scope.blockSize2; ++i)
            {
                const auto& c = ccQueue[(size_t) (scope.startIndex2 + i)];
                sfizz_send_hdcc (impl->synth, 0, c.cc, c.value);
            }
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
    state.setProperty ("sf2Preset", sf2PresetIndex, nullptr);

    // Persist any CC the UI has set (custom-UI knob/fader positions).
    // Only non-default (>= 0) entries are written so the blob stays
    // small. Each is a <cc n=".." v=".."/> child.
    juce::ValueTree ccTree ("cc");
    for (int i = 0; i < kNumHdcc; ++i)
    {
        const float v = ccCache[(size_t) i].load (std::memory_order_relaxed);
        if (v >= 0.0f)
        {
            juce::ValueTree e ("c");
            e.setProperty ("n", i, nullptr);
            e.setProperty ("v", v, nullptr);
            ccTree.appendChild (e, nullptr);
        }
    }
    if (ccTree.getNumChildren() > 0)
        state.appendChild (ccTree, nullptr);

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
    // Skip the re-load when createPluginInstance already loaded this exact file
    // from the description (the common session-restore path). Loading a
    // soundfont is the single most expensive thing this plugin does (~1.5s of
    // sample decode); doing it twice per restored instance is pure waste.
    if (path.isNotEmpty() && path != loadedFilePath)
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

    // Restore the SF2 preset selection (no-op for SFZ). Must run after
    // the file load above so the SF2 name list + sfizz state exist.
    // idx == 0 is deliberately skipped: loadSf2File already loaded
    // preset 0, so re-loading it would be redundant work. Only a
    // non-default saved preset needs an explicit switch.
    if (state.hasProperty ("sf2Preset"))
    {
        const int idx = (int) state.getProperty ("sf2Preset");
        if (idx > 0 && idx < sf2PresetNames.size())
        {
            juce::String err;
            loadSf2Preset (idx, err);
        }
    }

    // Restore custom-UI CC positions. Re-applies through setHDCC so the
    // values reach sfizz on the next audio block + the cache is correct
    // for the editor's widget read-back.
    if (auto ccTree = state.getChildWithName ("cc"); ccTree.isValid())
    {
        for (int i = 0; i < ccTree.getNumChildren(); ++i)
        {
            const auto e = ccTree.getChild (i);
            setHDCC ((int) e.getProperty ("n"), (float) e.getProperty ("v"));
        }
    }
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
