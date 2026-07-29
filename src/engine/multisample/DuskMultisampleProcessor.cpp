#include "DuskMultisampleProcessor.h"
#include "Sf2ToSfz.h"
#include "Sf2PresetSort.h"
#include "../../foundation/MessageThread.h"
#include "../../foundation/ScopedNoDenormals.h"

#include <juce_data_structures/juce_data_structures.h>
#include <sfizz.h>

#include <algorithm>

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
    : impl (std::make_unique<Impl>())
{
    using hosting::BusInfo;
    // Instrument port shape: no audio input, stereo output, one MIDI-in event
    // bus. isInstrument is what routes the strip's MIDI here instead of audio.
    layout.outputs.push_back ({ BusInfo::Kind::Audio, BusInfo::Direction::Output,
                                BusInfo::Role::Main, 2, true, false, "Output" });
    layout.inputs.push_back ({ BusInfo::Kind::Event, BusInfo::Direction::Input,
                               BusInfo::Role::Main, 0, true, true, "MIDI In" });
    layout.mainOutIndex = 0;
    layout.eventInIndex = 0;
    layout.isInstrument = true;

    impl->synth = sfizz_create_synth();
    // sfizz allocates per-voice state lazily on activate(), so the ctor is
    // cheap.

    // -1 = "CC never set by the UI" so widgets fall back to their own
    // default instead of snapping to 0.
    for (auto& c : ccCache)
        c.store (-1.0f, std::memory_order_relaxed);
}

void DuskMultisampleProcessor::setHDCC (int cc, float normValue)
{
    if (cc < 0 || cc >= kNumHdcc) return;
    const float v = std::clamp (normValue, 0.0f, 1.0f);
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

void DuskMultisampleProcessor::publishLoadedPath (const juce::String& path)
{
    const std::lock_guard<std::mutex> lock (loadedPathLock);
    loadedPathShared = path.toStdString();
}

std::string DuskMultisampleProcessor::getLoadedPathSnapshot() const
{
    const std::lock_guard<std::mutex> lock (loadedPathLock);
    return loadedPathShared;
}

void DuskMultisampleProcessor::cancelPendingLoads()
{
    loadPool.removeAllJobs (true, -1);
    // A job discarded before it ran never posts its completion callback, so the
    // flag would stay set and every later load would bail as already-in-progress.
    // The bump disowns the completion already posted by a job that WAS running.
    loadGeneration.fetch_add (1, std::memory_order_acq_rel);
    loadPending.store (false, std::memory_order_release);
}

bool DuskMultisampleProcessor::create (const MultisampleBundle& bundle,
                                        const std::string&,
                                        std::string& errorOut)
{
    const juce::File file (juce::String::fromUTF8 (bundle.getFile().u8string().c_str()));
    juce::String err;
    const bool ok = file.getFileExtension().toLowerCase() == ".sf2"
                        ? loadSf2File (file, err)
                        : loadSfzFile (file, err);
    if (! ok) errorOut = err.toStdString();
    return ok;
}

bool DuskMultisampleProcessor::activate (double sampleRate, int maxBlockFrames,
                                          std::string& errorOut)
{
    if (impl == nullptr || impl->synth == nullptr)
    {
        errorOut = "sfizz synth not initialised";
        return false;
    }
    if (sampleRate <= 0.0 || maxBlockFrames <= 0)
    {
        errorOut = "invalid activation spec";
        return false;
    }
    {
        // Both setters reallocate sfizz's voice buffers - hold the render lock
        // so processBlock dry-passes instead of racing them. The block-size
        // publish lives in here too: the audio thread reads it under the same
        // lock, so it can never see a size sfizz has not been resized to yet.
        const juce::SpinLock::ScopedLockType lock (sfizzLock);
        sfizz_set_sample_rate (impl->synth, (float) sampleRate);
        sfizz_set_samples_per_block (impl->synth, maxBlockFrames);
        currentBlockSize.store (maxBlockFrames, std::memory_order_release);
    }
    active.store (true, std::memory_order_release);
    return true;
}

void DuskMultisampleProcessor::deactivate()
{
    // sfizz keeps its voice state allocated across a deactivate - a subsequent
    // activate reuses the buffers, so only the gate flips.
    active.store (false, std::memory_order_release);
}

bool DuskMultisampleProcessor::reactivate (double sampleRate, int maxBlockFrames,
                                            std::string& errorOut)
{
    return activate (sampleRate, maxBlockFrames, errorOut);
}

int DuskMultisampleProcessor::getNumRegions() const noexcept
{
    // A background load is mutating the synth off-thread - don't read it.
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
    {
        const juce::ScopedLock sl (sf2PresetsLock);
        sf2Presets.clear();
    }
    sf2PresetIndex = -1;
    loadedFilePath.clear();
    publishLoadedPath ({});
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

    // Cache display metadata without disturbing source indices. SF2 does not
    // require PHDR records to be ordered, while the source index is persisted
    // in sessions and passed to the converter. Build it in temporary storage
    // and commit only after the preset-0 load below succeeds, so a failed load
    // leaves the previously loaded SF2's metadata intact.
    std::vector<Sf2PresetInfo> candidates;
    if (auto parsed = readSf2 (std::filesystem::u8path (sf2.getFullPathName().toStdString())); parsed.ok)
    {
        candidates.reserve (parsed.presets.size());
        for (size_t i = 0; i < parsed.presets.size(); ++i)
        {
            const auto& p = parsed.presets[i];
            candidates.push_back ({ juce::String (p.name), (int) i, (int) p.bank, (int) p.preset });
        }
        sortSf2PresetsForDisplay (candidates);
    }

    if (! applySf2Preset (sf2, 0, errorMessage))
        return false;

    {
        const juce::ScopedLock sl (sf2PresetsLock);
        sf2Presets = std::move (candidates);
    }
    return true;
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
    const std::uint64_t gen = loadGeneration.load (std::memory_order_acquire);
    loadPool.addJob ([this, file, gen, onDone = std::move (onDone)]
    {
        juce::String err;
        const bool ok = file.getFileExtension().toLowerCase() == ".sf2"
                            ? loadSf2File (file, err)
                            : loadSfzFile (file, err);
        juce::WeakReference<DuskMultisampleProcessor> weak (this);
        dusk::callAsync ([weak, onDone, ok, err, gen]
        {
            // Skip entirely if the processor was destroyed after posting this:
            // removeAllJobs joins the pool job, not this queued callback.
            auto* self = weak.get();
            if (self == nullptr) return;
            // Same for a cancel: this load was disowned, and the pending flag
            // it would clear now belongs to whatever load came after it.
            if (self->loadGeneration.load (std::memory_order_acquire) != gen) return;
            // Clear pending FIRST so the UI refresh in onDone observes the
            // finished load: getSf2Presets / getNumRegions / control-image
            // queries all return empty while a load is pending.
            self->loadPending.store (false, std::memory_order_release);
            if (onDone) onDone (ok, err);
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
    const std::uint64_t gen = loadGeneration.load (std::memory_order_acquire);
    loadPool.addJob ([this, presetIndex, gen, onDone = std::move (onDone)]
    {
        juce::String err;
        const bool ok = loadSf2Preset (presetIndex, err);
        juce::WeakReference<DuskMultisampleProcessor> weak (this);
        dusk::callAsync ([weak, onDone, ok, err, gen]
        {
            auto* self = weak.get();
            if (self == nullptr) return;
            if (self->loadGeneration.load (std::memory_order_acquire) != gen) return;
            // Clear pending FIRST, mirroring loadFileAsync - onDone re-runs the
            // editor's timerCallback, whose pending-guarded getters would
            // otherwise observe an empty snapshot.
            self->loadPending.store (false, std::memory_order_release);
            if (onDone) onDone (ok, err);
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
    const auto body       = conv.sfzText;
    const auto pathStr    = virtualSfz.getFullPathName().toStdString();
    // Only the sfizz call itself is serialised against processBlock - the
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

    int presetCount;
    {
        const juce::ScopedLock sl (sf2PresetsLock);
        presetCount = (int) sf2Presets.size();
    }
    sf2PresetIndex = juce::jlimit (0, juce::jmax (0, presetCount - 1), presetIndex);
    loadedFilePath = sf2.getFullPathName();
    publishLoadedPath (loadedFilePath);
    lastLoadError.clear();
    return true;
}

void DuskMultisampleProcessor::setPolyphony (int newPolyphony)
{
    // Message/loader-thread entry point - never the audio thread.
    const int clamped = std::clamp (newPolyphony, 1, 256);
    overrides.polyphony.store (clamped, std::memory_order_relaxed);
    if (impl != nullptr && impl->synth != nullptr)
    {
        // sfizz_set_num_voices is marked OFF (not callable while RT
        // functions run) - hold the render lock so processBlock dry-passes
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
    // Clear SF2 preset state - an SFZ load has no presets, and leaving
    // the previous SF2's list around would show a stale program switcher.
    {
        const juce::ScopedLock sl (sf2PresetsLock);
        sf2Presets.clear();
    }
    sf2PresetIndex = -1;
    loadedFilePath = sfz.getFullPathName();
    publishLoadedPath (loadedFilePath);
    lastLoadError.clear();
    return true;
}

void DuskMultisampleProcessor::processBlock (const hosting::PortBuffers& io) noexcept
{
    dusk::audio::ScopedNoDenormals noDenormals;
    const int numSamples = io.numFrames;
    // The adapter pre-clears the output scratch, so every bail here is silence.
    if (numSamples <= 0) return;
    if (io.mainOut == nullptr || io.mainOutChannels < 2) return;
    // Acquire-load the gate FIRST: it is what publishes everything activate()
    // wrote, currentBlockSize included.
    if (! active.load (std::memory_order_acquire)) return;
    if (impl == nullptr || impl->synth == nullptr) return;

    // Loads mutate the sfizz synth from the loader thread; TRY-lock and pass
    // one silent block instead of racing them (PluginSlot's prepare<->process
    // pattern). The message-thread mutators take this lock blocking.
    const juce::SpinLock::ScopedTryLockType renderLock (sfizzLock);
    if (! renderLock.isLocked()) return;

    // Under the render lock: sfizz is sized for the activate() block size and a
    // longer block would run past its scratch.
    if (numSamples > currentBlockSize.load (std::memory_order_acquire)) return;

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

    // Dispatch incoming MIDI events to sfizz. sfizz batches them against the
    // current block; delays are sample offsets within the block. dusk::MidiBuffer
    // is a byte-level container, so the status nibble is decoded inline - no
    // message objects, no scratch buffer, no allocation.
    if (io.midiIn != nullptr)
    {
        for (const auto meta : *io.midiIn)
        {
            const auto* d = meta.data;
            if (d == nullptr || meta.numBytes < 2) continue;
            const int delay = std::clamp (meta.samplePosition, 0, numSamples - 1);
            const int d1 = d[1] & 0x7F;
            const int d2 = meta.numBytes > 2 ? (d[2] & 0x7F) : 0;
            switch (d[0] & 0xF0)
            {
                case 0x90:
                    if (d2 > 0)
                    {
                        sfizz_send_note_on (impl->synth, delay, d1, d2);
                        break;
                    }
                    // Note-on at velocity 0 is a note-off.
                    [[fallthrough]];
                case 0x80:
                    sfizz_send_note_off (impl->synth, delay, d1, d2);
                    break;
                case 0xB0:
                    sfizz_send_cc (impl->synth, delay, d1, d2);
                    break;
                case 0xE0:
                    // sfizz normalises against +/-8191, so the wheel must arrive
                    // 0-centred - a raw 0..16383 value pins it full sharp.
                    sfizz_send_pitch_wheel (impl->synth, delay, ((d2 << 7) | d1) - 8192);
                    break;
                case 0xD0:
                    sfizz_send_channel_aftertouch (impl->synth, delay, d1);
                    break;
                default:
                    break;
            }
        }
    }

    // Render. sfizz wants float** with 2 channels for the default stereo
    // output layout; the adapter hands us exactly that.
    float* chans[2] = { io.mainOut[0], io.mainOut[1] };
    sfizz_render_block (impl->synth, chans, 2, numSamples);
}

bool DuskMultisampleProcessor::saveState (std::vector<uint8_t>& out) const
{
    juce::ValueTree state ("DuskMultisample");
    state.setProperty ("file", juce::String::fromUTF8 (getLoadedPathSnapshot().c_str()),
                        nullptr);
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

    juce::MemoryOutputStream stream;
    state.writeToStream (stream);
    const auto* bytes = static_cast<const uint8_t*> (stream.getData());
    out.assign (bytes, bytes + stream.getDataSize());
    return true;
}

bool DuskMultisampleProcessor::loadState (const std::vector<uint8_t>& in)
{
    if (in.empty()) return false;
    juce::MemoryInputStream stream (in.data(), in.size(), false);
    const auto state = juce::ValueTree::readFromStream (stream);
    if (! state.isValid()) return false;

    const auto path = state.getProperty ("file").toString();
    // Skip the re-load when create() already loaded this exact file from the
    // slot's bundle path (the common session-restore path). Loading a
    // soundfont is the single most expensive thing this plugin does (~1.5s of
    // sample decode); doing it twice per restored instance is pure waste.
    bool fileLoadOk = true;
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
            fileLoadOk = false;
        }
    }

    if (state.hasProperty ("masterVolDb"))
        overrides.masterVolDb.store (
            std::clamp ((float) state.getProperty ("masterVolDb"), -60.0f, 12.0f),
            std::memory_order_relaxed);
    if (state.hasProperty ("masterTuneCents"))
        overrides.masterTuneCents.store (
            std::clamp ((float) state.getProperty ("masterTuneCents"), -100.0f, 100.0f),
            std::memory_order_relaxed);
    if (state.hasProperty ("polyphony"))
        setPolyphony (std::clamp ((int) state.getProperty ("polyphony"), 1, 256));

    // Restore the SF2 preset selection (no-op for SFZ). Must run after
    // the file load above so the SF2 metadata + sfizz state exist.
    // Skipped when that load failed: sf2Presets + loadedFilePath then still
    // point at the previously loaded SF2, so restoring the saved index would
    // switch the OLD file to a stale preset and clobber lastLoadError.
    // idx == 0 is deliberately skipped: loadSf2File already loaded
    // preset 0, so re-loading it would be redundant work. Only a
    // non-default saved preset needs an explicit switch.
    if (fileLoadOk && state.hasProperty ("sf2Preset"))
    {
        const int idx = (int) state.getProperty ("sf2Preset");
        int presetCount;
        {
            const juce::ScopedLock sl (sf2PresetsLock);
            presetCount = (int) sf2Presets.size();
        }
        if (idx > 0 && idx < presetCount)
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
    return true;
}
} // namespace duskstudio
