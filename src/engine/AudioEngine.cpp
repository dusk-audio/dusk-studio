#include "AudioEngine.h"
#if defined(DUSKSTUDIO_HAS_FOCAL_ALSA)
  #include "alsa/AlsaAudioIODeviceType.h"
#endif
#include <cstring>
#include <thread>

namespace duskstudio
{
AudioEngine::AudioEngine (Session& sessionToBindTo) : session (sessionToBindTo)
{
    // Cap the undo stack. Each RegionEditAction's getSizeInUnits is 1
    // (Split = 2, Clone = 4) so 500 covers thousands of typical edits
    // while bounding memory under a multi-hour edit session — without
    // this, the stack grows unbounded and the destructor walks the
    // whole list on app exit. minimumTransactionsToKeep = 50 means
    // the most recent 50 are always retained even if their total
    // exceeds 500 units.
    undoManager.setMaxNumberOfStoredUnits (500, 50);

    // Build the playhead now that all the atom addresses (transport,
    // session.tempoBpm, currentSampleRate) are stable. Hosted plugins
    // get this via setPlayHead so tempo-synced features (LFOs, arps,
    // delays, transport-driven UIs) read the live session BPM and
    // playhead position.
    playHead = std::make_unique<DuskStudioPlayHead> (transport,
                                                  &session.tempoBpm,
                                                  &currentSampleRate);

    // Sentinel for "no previous input" - the first audio block will compare
    // the current midiInputIndex against this and (correctly) emit a flush
    // if a device is already selected, which is harmless when no notes are
    // held yet.
    lastMidiInputIndex.fill (-1);

    for (int i = 0; i < Session::kNumTracks; ++i)
    {
        strips[(size_t) i].bind (session.track (i).strip);
        // Each channel strip gets a reference to the shared PluginManager
        // so its PluginSlot can resolve plugin files. Call must happen
        // before any audio processing - the slot is bypassed until a
        // plugin is actually loaded, so binding here costs nothing.
        strips[(size_t) i].bindPluginManager (pluginManager);
        // Hardware-insert side of the channel insert slot. The slot is
        // pre-allocated; this just latches a pointer to the per-track
        // HardwareInsertParams so the audio thread can read routing /
        // gain / dry-wet lock-free.
        strips[(size_t) i].bindHardwareInsert (session.track (i).hardwareInsert);
    }
    for (int i = 0; i < Session::kNumBuses; ++i)
        busStrips[(size_t) i].bind (session.bus (i).strip);
    for (int i = 0; i < Session::kNumAuxLanes; ++i)
    {
        auxLaneStrips[(size_t) i].bind (session.auxLane (i).params);
        // Lanes host plugins (reverb / delay / etc.) so they need the
        // shared PluginManager to resolve files into AudioPluginInstances.
        auxLaneStrips[(size_t) i].bindPluginManager (pluginManager);
        // Hardware-insert side of each lane slot. One bind per slot
        // index so the audio thread can read each slot's routing /
        // gain / dry-wet without indirection.
        for (int s = 0; s < AuxLaneParams::kMaxLanePlugins; ++s)
            auxLaneStrips[(size_t) i].bindHardwareInsert (
                s, session.auxLane (i).hardwareInserts[(size_t) s]);
    }
    master.bind (session.master());
    masteringChain.bind (session.mastering());

    // Linux: pre-register Dusk Studio's ALSA backend + JACK BEFORE
    // initialiseWithDefaultDevices. JUCE's createDeviceTypesIfNeeded only
    // auto-registers its defaults (which would re-add the stock ALSA path
    // we don't want) when availableDeviceTypes is empty; pre-adding ours
    // makes that branch a no-op. We then explicitly call scanForDevices
    // on each pre-registered type so init's pickCurrentDeviceTypeWithDevices
    // pass can query device counts without tripping our hasScanned
    // assertions. The dropdown now shows two backends: "ALSA" (ours) and
    // "JACK" - no double-listing.
    //
    // On macOS / Windows we let JUCE auto-register its native backends
    // (CoreAudio / WASAPI / ASIO / JACK-if-installed) the standard way -
    // DUSKSTUDIO_HAS_FOCAL_ALSA isn't defined there so the pre-registration
    // path is skipped entirely.
   #if defined(DUSKSTUDIO_HAS_FOCAL_ALSA)
    if (auto* jackType = juce::AudioIODeviceType::createAudioIODeviceType_JACK())
        deviceManager.addAudioDeviceType (std::unique_ptr<juce::AudioIODeviceType> (jackType));
    deviceManager.addAudioDeviceType (std::make_unique<AlsaAudioIODeviceType>());

    for (auto* t : deviceManager.getAvailableDeviceTypes())
        if (t != nullptr) t->scanForDevices();
   #endif

    if (const auto err = deviceManager.initialiseWithDefaultDevices (16, 2);
        err.isNotEmpty())
    {
        std::fprintf (stderr,
                      "[Dusk Studio/AudioEngine] device-manager init reported: %s\n",
                      err.toRawUTF8());
    }

    deviceManager.addAudioCallback (this);

    // MIDI input wiring. Build the per-input collector list once at startup.
    // addMidiInputDeviceCallback with an empty deviceIdentifier means "every
    // enabled input fans out to this callback" — handleIncomingMidiMessage
    // routes by source pointer.
    rebuildMidiInputBank();
    rebuildMidiOutputBank();
    deviceManager.addMidiInputDeviceCallback ({}, this);
}

void AudioEngine::refreshMidiInputs()
{
    // Detach both callbacks so neither the audio thread nor the MIDI
    // thread can be inside the engine while we mutate the device /
    // collector / buffer vectors. JUCE's removeAudioCallback joins the
    // audio thread before returning, and removeMidiInputDeviceCallback
    // joins on the MIDI dispatch side. This is the cheapest correct
    // synchronisation for a rare user-triggered event - mutating the
    // vectors lock-free under callback would require a snapshot pattern
    // throughout the audio path that costs more than it's worth for a
    // hot-plug refresh.
    deviceManager.removeAudioCallback (this);
    deviceManager.removeMidiInputDeviceCallback ({}, this);

    // Disable currently-enabled inputs before rebuilding so the device
    // manager's own bookkeeping releases the OS handles. The new pass
    // re-enables whichever devices are still present.
    for (const auto& d : midiInputDevices)
        deviceManager.setMidiInputDeviceEnabled (d.identifier, false);

    rebuildMidiInputBank();
    rebuildMidiOutputBank();

    // The track-side index atoms may now point at moved or removed
    // devices. Re-resolve each track's saved identifier so a refresh
    // doesn't silently break existing routing. Tracks with no saved
    // identifier (very old sessions) keep their raw index, clamped.
    auto resolveByIdentifier = [] (const juce::Array<juce::MidiDeviceInfo>& devices,
                                    const juce::String& wantedId)
    {
        for (int i = 0; i < devices.size(); ++i)
            if (devices[i].identifier == wantedId)
                return i;
        return -1;
    };
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto& track = session.track (t);
        if (track.midiInputIdentifier.isNotEmpty())
        {
            track.midiInputIndex.store (
                resolveByIdentifier (midiInputDevices, track.midiInputIdentifier),
                std::memory_order_relaxed);
        }
        else
        {
            const int cur = track.midiInputIndex.load (std::memory_order_relaxed);
            if (cur >= midiInputDevices.size())
                track.midiInputIndex.store (-1, std::memory_order_relaxed);
        }
        if (track.midiOutputIdentifier.isNotEmpty())
        {
            track.midiOutputIndex.store (
                resolveByIdentifier (midiOutputDevices, track.midiOutputIdentifier),
                std::memory_order_relaxed);
        }
        else
        {
            const int cur = track.midiOutputIndex.load (std::memory_order_relaxed);
            if (cur >= midiOutputDevices.size())
                track.midiOutputIndex.store (-1, std::memory_order_relaxed);
        }
    }

    // The output bank was rebuilt with all-null entries (lazy open). Re-
    // open whichever ports tracks are currently routed to so playback
    // doesn't silently drop notes after a hot-plug refresh. Must run
    // BEFORE re-attaching the audio/MIDI callbacks - otherwise the audio
    // thread can iterate midiOutputs while ensureMidiOutputOpen() mutates
    // it.
    openConfiguredMidiOutputs();

    deviceManager.addMidiInputDeviceCallback ({}, this);
    deviceManager.addAudioCallback (this);

    // Wake any UI listeners (each ChannelStripComponent's MIDI dropdown)
    // so they rebuild their item list and re-select via identifier.
    sendChangeMessage();
}

void AudioEngine::rebuildMidiInputBank()
{
    // Snapshot the available MIDI inputs and prepare a parallel collector
    // list. Index alignment with `midiInputDevices` is what
    // `Track::midiInputIndex` references (-1 = none, 0..N = this list).
    //
    // Safe to mutate the three vectors here ONLY when both the audio and
    // MIDI callbacks are detached. The constructor's first call satisfies
    // that (callbacks aren't registered yet); refreshMidiInputs() does the
    // remove-then-call-then-add dance. Calling this with callbacks active
    // races the audio + MIDI threads and is undefined behaviour.
    midiInputDevices = juce::MidiInput::getAvailableDevices();
    midiInputCollectors.clear();
    midiInputCollectors.reserve ((size_t) midiInputDevices.size() + 1);
    const double sr = currentSampleRate.load (std::memory_order_relaxed);
    for (int i = 0; i < midiInputDevices.size(); ++i)
    {
        auto col = std::make_unique<juce::MidiMessageCollector>();
        if (sr > 0.0) col->reset (sr);
        // Enabling the device routes its messages to handleIncomingMidiMessage
        // via the empty-id callback registration in the ctor.
        // setMidiInputDeviceEnabled returns void, so we re-query the manager
        // to verify the enable took. Failure usually means the OS denied
        // access (e.g. another app exclusively owns the port); log so the
        // user can diagnose missing MIDI input rather than silently leaving
        // the dropdown showing a non-functioning device.
        const auto& devId = midiInputDevices[i].identifier;
        deviceManager.setMidiInputDeviceEnabled (devId, true);
        if (! deviceManager.isMidiInputDeviceEnabled (devId))
        {
            std::fprintf (stderr,
                          "[Dusk Studio/AudioEngine] WARNING: failed to enable MIDI input \"%s\" "
                          "(id %s). Another application may be holding it open.\n",
                          midiInputDevices[i].name.toRawUTF8(),
                          devId.toRawUTF8());
        }
        midiInputCollectors.push_back (std::move (col));
    }

    // Append the synthetic VKB device after real hardware so its index is
    // stable across hot-plug (real devices come and go; the VKB is always
    // last). Identifier is fixed so saved sessions resolve back to it on
    // load. The collector is not bound to any OS device — VKB UI calls
    // addMessageToQueue on it directly; the audio thread drains it as
    // perInputMidi[virtualKeyboardCollectorIndex].
    juce::MidiDeviceInfo virtualKb { "Virtual Keyboard (Dusk Studio)", "Dusk Studio:virtual-keyboard" };
    midiInputDevices.add (virtualKb);
    {
        auto vkbCol = std::make_unique<juce::MidiMessageCollector>();
        if (sr > 0.0) vkbCol->reset (sr);
        midiInputCollectors.push_back (std::move (vkbCol));
    }
    virtualKeyboardCollectorIndex = (int) midiInputCollectors.size() - 1;

    perInputMidi.assign ((size_t) midiInputDevices.size(), juce::MidiBuffer{});

    // Resolve session.syncSourceInputIdentifier -> syncSourceInputIdx.
    // Done on every rebuild so a hot-plug doesn't strand the index at
    // a stale slot. Empty identifier or no match -> -1 (no sync).
    int resolvedSyncIdx = -1;
    const auto& wantedId = session.syncSourceInputIdentifier;
    if (wantedId.isNotEmpty())
    {
        for (int i = 0; i < midiInputDevices.size(); ++i)
        {
            if (midiInputDevices[i].identifier == wantedId)
            {
                resolvedSyncIdx = i;
                break;
            }
        }
    }
    session.syncSourceInputIdx.store (resolvedSyncIdx, std::memory_order_release);
    midiSyncReceiver.reset();
    midiTimeCodeReceiver.reset();
}

juce::MidiMessageCollector* AudioEngine::getVirtualKeyboardCollector() noexcept
{
    const int idx = virtualKeyboardCollectorIndex;
    if (idx < 0 || idx >= (int) midiInputCollectors.size())
        return nullptr;
    return midiInputCollectors[(size_t) idx].get();
}

void AudioEngine::rebuildMidiOutputBank()
{
    // Tear down any existing outputs first (closes the OS handles and
    // joins each MidiOutput's background thread). Mutating midiOutputs
    // here is only safe when the audio callback is detached - the audio
    // thread iterates this vector while sending events.
    midiOutputs.clear();

    // Enumerate, but do NOT open. Eagerly opening every available output
    // at startup blocks the message thread on each ALSA snd_seq_connect_to
    // and spawns one delivery thread per port - on a system with USB MIDI
    // gear that can stall the MainWindow's setVisible(true) for seconds
    // (or indefinitely if a port misbehaves). Most sessions use zero or
    // one output; opening on demand via ensureMidiOutputOpen() pays only
    // for what's actually routed.
    midiOutputDevices = juce::MidiOutput::getAvailableDevices();
    midiOutputs.resize ((size_t) midiOutputDevices.size());  // all nullptr

    // Resolve session.syncOutputIdentifier -> syncOutputIdx + eagerly
    // open the chosen port so the first clock byte the audio thread
    // emits doesn't have to wait on a synchronous ALSA connect. Empty
    // identifier or no match -> -1 (no emission).
    int resolvedSyncOutIdx = -1;
    const auto& wantedOutId = session.syncOutputIdentifier;
    if (wantedOutId.isNotEmpty())
    {
        for (int i = 0; i < midiOutputDevices.size(); ++i)
        {
            if (midiOutputDevices[i].identifier == wantedOutId)
            {
                resolvedSyncOutIdx = i;
                ensureMidiOutputOpen (i);
                break;
            }
        }
    }
    session.syncOutputIdx.store (resolvedSyncOutIdx, std::memory_order_release);
    midiClockEmitter.reset();
}

bool AudioEngine::ensureMidiOutputOpen (int index)
{
    if (index < 0 || index >= (int) midiOutputs.size())
        return false;
    if (midiOutputs[(size_t) index] != nullptr)
        return true;  // already open

    auto out = juce::MidiOutput::openDevice (midiOutputDevices[index].identifier);
    if (out == nullptr)
    {
        std::fprintf (stderr,
                      "[Dusk Studio/AudioEngine] WARNING: failed to open MIDI output \"%s\" "
                      "(id %s). Another application may be holding it open.\n",
                      midiOutputDevices[index].name.toRawUTF8(),
                      midiOutputDevices[index].identifier.toRawUTF8());
        return false;
    }
    // Start the background-delivery thread so audio-thread sends via
    // sendBlockOfMessages enqueue without blocking on the OS port.
    out->startBackgroundThread();
    midiOutputs[(size_t) index] = std::move (out);
    return true;
}

void AudioEngine::openConfiguredMidiOutputs()
{
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        const int idx = session.track (t).midiOutputIndex.load (std::memory_order_relaxed);
        if (idx >= 0)
            ensureMidiOutputOpen (idx);
    }
}

void AudioEngine::handleIncomingMidiMessage (juce::MidiInput* source,
                                                const juce::MidiMessage& message)
{
    if (source == nullptr) return;
    // Identify the source's index in our parallel list. JUCE guarantees
    // identifier stability for a given device across the session.
    const auto& sourceId = source->getIdentifier();
    for (int i = 0; i < midiInputDevices.size(); ++i)
    {
        if (midiInputDevices[(size_t) i].identifier == sourceId)
        {
            if (i < (int) midiInputCollectors.size()
                && midiInputCollectors[(size_t) i] != nullptr)
                midiInputCollectors[(size_t) i]->addMessageToQueue (message);
            return;
        }
    }
}

int AudioEngine::getBackendXRunCount() const noexcept
{
    // const_cast: AudioDeviceManager::getCurrentAudioDevice is non-const for
    // historical reasons, but the read here is benign - getXRunCount itself
    // is noexcept and just returns an internal counter on the patched JUCE
    // ALSA backend (and on JACK).
    if (auto* dev = const_cast<juce::AudioDeviceManager&> (deviceManager).getCurrentAudioDevice())
        return dev->getXRunCount();
    return 0;
}

void AudioEngine::setStage (Stage s) noexcept
{
    const auto current = stage.load (std::memory_order_relaxed);
    if (current == s) return;

    // Recording / Mixing / Aux share the live track-to-master signal flow -
    // only the visible UI changes between them, so playback / recording
    // continue uninterrupted across those stage swaps. Mastering swaps to a
    // wholly separate path (stereo file → MasteringChain → output), so
    // transport must be force-stopped when crossing into or out of it.
    const bool crossesMastering = (current == Stage::Mastering)
                                 || (s == Stage::Mastering);
    if (crossesMastering)
    {
        if (transport.isRecording())
            recordManager.stopRecording (transport.getPlayhead());
        transport.setState (Transport::State::Stopped);
        masteringPlayer.stop();
        playbackEngine.stopPlayback();
    }

    stage.store (s, std::memory_order_relaxed);
}

AudioEngine::~AudioEngine()
{
    if (transport.isRecording())
        recordManager.stopRecording (transport.getPlayhead());
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();
}

void AudioEngine::play()
{
    if (transport.isPlaying() || transport.isRecording()) return;

    // When loop is enabled and the playhead sits outside the loop range,
    // snap to loopStart so playback always begins inside the loop. The
    // audio-callback wraparound (in audioDeviceIOCallbackWithContext)
    // already handles `playhead >= loopEnd` by snapping back, which is
    // why pressing Play with the playhead AFTER the loop "just works".
    // The before-loop case has no symmetric guard there - linear playback
    // would audibly run through the pre-loop area until reaching loopEnd.
    // Snapping here at Play-press matches the intent of "loop is engaged".
    if (transport.isLoopEnabled())
    {
        const auto lStart = transport.getLoopStart();
        const auto lEnd   = transport.getLoopEnd();
        if (lEnd > lStart)
        {
            const auto ph = transport.getPlayhead();
            if (ph < lStart || ph >= lEnd)
                transport.setPlayhead (lStart);
        }
    }

    playbackEngine.preparePlayback();
    transport.setState (Transport::State::Playing);
}

void AudioEngine::stop()
{
    if (transport.isStopped()) return;

    const bool wasRecording = transport.isRecording();
    transport.setState (Transport::State::Stopped);

    if (wasRecording)
    {
        recordManager.stopRecording (transport.getPlayhead());
        activeRecordStart.store (std::numeric_limits<juce::int64>::min(),
                                  std::memory_order_relaxed);
    }

    playbackEngine.stopPlayback();
    transport.setPlayhead (0);
}

void AudioEngine::record()
{
    if (transport.isRecording())
    {
        std::fprintf (stderr, "[Dusk Studio/AudioEngine] record(): already recording, ignored.\n");
        return;
    }

    // Defensive resync of the armedTrackCount fast-path counter. The
    // counter is normally maintained by Session::setTrackArmed, but a
    // couple of code paths (notably RegionEditActions clone/restore)
    // wrote recordArmed directly and left the counter stale - which
    // would then make anyTrackArmed() incorrectly return false and
    // silently bail out of recording. Cheap to recompute - one pass
    // over 16 tracks. Also helps surface user-visible "I armed the
    // track but record won't engage" symptoms.
    session.recomputeRtCounters();
    if (! session.anyTrackArmed())
    {
        std::fprintf (stderr, "[Dusk Studio/AudioEngine] record(): no track is armed; "
                              "click ARM on the strip you want to record into.\n");
        return;
    }

    const double sr = currentSampleRate.load (std::memory_order_relaxed);
    if (sr <= 0.0)
    {
        std::fprintf (stderr, "[Dusk Studio/AudioEngine] record(): no audio device open "
                              "(sample rate is 0); recording cannot start.\n");
        return;
    }

    // With punch enabled, the captured WAV's first audible sample is at
    // punchIn - so the recorded region's timelineStart must be punchIn,
    // not the playhead at Record-press. The audio thread skips writes
    // until the playhead reaches punchIn, so the WAV time-zero lines up.
    // If the playhead is already inside the punch window (or past it) we
    // fall back to the current playhead so we don't truncate the take.
    juce::int64 startSample = transport.getPlayhead();
    if (transport.isPunchEnabled()
        && transport.getPunchOut() > transport.getPunchIn()
        && startSample < transport.getPunchIn())
    {
        startSample = transport.getPunchIn();
    }

    if (! recordManager.startRecording (sr, startSample))
        return;

    activeRecordStart.store (startSample, std::memory_order_relaxed);

    // Stamp the take's start sample as the session's "last record point"
    // so STOP+FFWD can return the user to where they last started a take.
    // Persisted via the serializer; survives reload.
    session.lastRecordPointSamples.store (startSample, std::memory_order_relaxed);

    // Count-in pre-roll: roll the playhead back one bar so the metronome
    // ticks a full bar before the take begins. Audio between the pre-roll
    // start and `startSample` is gated out of the recorder by the audio
    // callback, so the WAV's first sample still maps to `startSample`.
    if (session.countInEnabled.load (std::memory_order_relaxed))
    {
        const float bpm     = session.tempoBpm.load    (std::memory_order_relaxed);
        const int   beatsBar = session.beatsPerBar.load (std::memory_order_relaxed);
        if (bpm > 0.0f && beatsBar > 0)
        {
            const auto countInSamples =
                (juce::int64) (sr * 60.0 / (double) bpm * (double) beatsBar);
            transport.setPlayhead (startSample - countInSamples);
        }
    }

    // Punch pre-roll: when punch is enabled and preRollSeconds > 0, roll
    // the playhead back so the user hears existing material BEFORE the
    // punch-in window. Same gate logic as count-in - the audio callback's
    // punch window already prevents committing audio before punchIn so
    // the WAV time-zero still maps to startSample. Stacks with count-in
    // (whichever rolls back further wins) since they're independent
    // pre-rolls solving different problems.
    if (transport.isPunchEnabled())
    {
        const float pre = session.preRollSeconds.load (std::memory_order_relaxed);
        if (pre > 0.0f)
        {
            const auto preSamples = (juce::int64) ((double) pre * sr);
            const auto candidate = juce::jmax ((juce::int64) 0, startSample - preSamples);
            if (candidate < transport.getPlayhead())
                transport.setPlayhead (candidate);
        }
    }

    playbackEngine.preparePlayback();  // un-armed tracks still play through
    transport.setState (Transport::State::Recording);
}

void AudioEngine::jumpToPrevMarker()
{
    const auto& markers = session.getMarkers();
    if (markers.empty()) { transport.setPlayhead (0); return; }
    const auto cur = transport.getPlayhead();
    // Walk in reverse for the largest marker strictly before the playhead.
    // "Strictly before" so a press while sitting ON a marker steps to the
    // previous one rather than restating the current position.
    juce::int64 target = 0;
    bool found = false;
    for (auto it = markers.rbegin(); it != markers.rend(); ++it)
    {
        if (it->timelineSamples < cur) { target = it->timelineSamples; found = true; break; }
    }
    transport.setPlayhead (found ? target : 0);
}

void AudioEngine::jumpToNextMarker()
{
    const auto& markers = session.getMarkers();
    if (markers.empty()) return;   // no overshoot beyond known points
    const auto cur = transport.getPlayhead();
    for (const auto& m : markers)
    {
        if (m.timelineSamples > cur) { transport.setPlayhead (m.timelineSamples); return; }
    }
    // Past the last marker: stay where we are. Tascam-style "stops at end".
}

void AudioEngine::jumpToZero()
{
    transport.setPlayhead (0);
}

void AudioEngine::jumpToLastRecordPoint()
{
    transport.setPlayhead (session.lastRecordPointSamples.load (std::memory_order_relaxed));
}

void AudioEngine::publishPluginStateForSave (bool audioCallbackDetached)
{
    // Snapshot each track's PluginSlot into its session-model strings so
    // SessionSerializer (which only sees Session) can serialise plugin
    // state alongside everything else. Empty strings = no plugin loaded.
    //
    // Plugin state I/O briefly suspends each plugin (releaseResources /
    // prepareToPlay around getStateInformation) - JUCE's contract says
    // state I/O must not race the renderer, and plugins like u-he Diva
    // crash hard when it does. The audio thread is parked across each
    // suspension, so the engine's master output goes silent for a few
    // milliseconds per loaded plugin during the save.
    //
    // For the autosave timer (audioCallbackDetached=false) that runs
    // every 30 s during normal playback, that dropout is unacceptable.
    // We skip plugin state entirely on that path: the existing values
    // in Session::pluginStateBase64 (set by the most recent manual save
    // or session load) are preserved, so a crash recovery still gets
    // valid plugin state - just possibly stale relative to in-flight
    // knob tweaks. Manual save (Ctrl+S, File>Save, quit-save) is the
    // path that captures fresh plugin state.
    if (! audioCallbackDetached)
    {
        // Plugin description/state strings on Session are kept as-is.
        // Future enhancement: a "Save (with plugin state)" UI option
        // that explicitly opts in to the audio dropout.
        return;
    }

    const int parkSleepMs = 0;  // audio thread already gone; no need to wait

    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto& track = session.track (t);
        auto& slot  = strips[(size_t) t].getPluginSlot();
        track.pluginDescriptionXml = slot.getDescriptionXmlForSave (parkSleepMs);
        track.pluginStateBase64    = slot.getStateBase64ForSave   (parkSleepMs);
    }
    for (int a = 0; a < Session::kNumAuxLanes; ++a)
    {
        auto& lane = session.auxLane (a);
        for (int s = 0; s < AuxLaneParams::kMaxLanePlugins; ++s)
        {
            auto& slot = auxLaneStrips[(size_t) a].getPluginSlot (s);
            lane.pluginDescriptionXml[(size_t) s] = slot.getDescriptionXmlForSave (parkSleepMs);
            lane.pluginStateBase64[(size_t) s]    = slot.getStateBase64ForSave   (parkSleepMs);
        }
    }

#if DUSKSTUDIO_HAS_DUSK_DSP
    // Master tape (TapeMachine) state. Mirrors the per-slot pattern: serialise
    // getStateInformation() into a base64 string on the session model so the
    // serializer (which only sees Session) can persist it.
    {
        juce::MemoryBlock mb;
        master.getTapeProcessor().getStateInformation (mb);
        session.master().tapeStateBase64 = mb.toBase64Encoding();
    }
#endif
}

void AudioEngine::releaseAllPluginResources()
{
    // Walk every plugin slot and call releaseResources on its loaded
    // instance. PluginSlot::releaseResources also clears currentInstance,
    // so any audio callback that does happen to fire after this (despite
    // the caller's contract) will see null and bypass.
    for (auto& strip : strips)
        strip.getPluginSlot().releaseResources();
    for (auto& laneStrip : auxLaneStrips)
        for (int s = 0; s < AuxLaneParams::kMaxLanePlugins; ++s)
            laneStrip.getPluginSlot (s).releaseResources();
}

void AudioEngine::leakAllPluginInstancesForShutdown()
{
    for (auto& strip : strips)
        strip.getPluginSlot().leakInstanceForShutdown();
    for (auto& laneStrip : auxLaneStrips)
        for (int s = 0; s < AuxLaneParams::kMaxLanePlugins; ++s)
            laneStrip.getPluginSlot (s).leakInstanceForShutdown();
}

void AudioEngine::publishTransportStateForSave()
{
    session.savedLoopStart    = transport.getLoopStart();
    session.savedLoopEnd      = transport.getLoopEnd();
    session.savedLoopEnabled  = transport.isLoopEnabled();
    session.savedPunchIn      = transport.getPunchIn();
    session.savedPunchOut     = transport.getPunchOut();
    session.savedPunchEnabled = transport.isPunchEnabled();
}

void AudioEngine::consumeTransportStateAfterLoad()
{
    transport.setLoopRange (session.savedLoopStart, session.savedLoopEnd);
    transport.setLoopEnabled (session.savedLoopEnabled);
    transport.setPunchRange (session.savedPunchIn, session.savedPunchOut);
    transport.setPunchEnabled (session.savedPunchEnabled);
}

void AudioEngine::consumePluginStateAfterLoad()
{
    // Mirror of publish: read the (just-deserialised) strings on each track
    // and re-instantiate the plugin in the live PluginSlot. Failures log
    // and continue rather than aborting the whole session load - a missing
    // plugin shouldn't lose the rest of the user's saved state. Failures
    // are captured in lastPluginLoadFailures so the UI can surface a
    // single summary AlertWindow listing every slot that fell back to
    // empty - silent fallback is the worst case (user thinks the saved
    // mix is intact but the plugin chain it depended on is gone).
    lastPluginLoadFailures.clear();

    auto parsePluginName = [] (const juce::String& descXml) -> juce::String
    {
        if (descXml.isEmpty()) return {};
        if (auto xml = juce::XmlDocument::parse (descXml))
        {
            juce::PluginDescription desc;
            if (desc.loadFromXml (*xml)) return desc.name;
        }
        return juce::String ("(unknown)");
    };

    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto& track = session.track (t);
        auto& slot  = strips[(size_t) t].getPluginSlot();
        if (track.pluginDescriptionXml.isEmpty()) continue;   // no plugin to restore
        juce::String error;
        if (! slot.restoreFromSavedState (track.pluginDescriptionXml,
                                            track.pluginStateBase64, error))
        {
            DBG ("AudioEngine: failed to restore plugin on track " << (t + 1)
                  << ": " << error);
            lastPluginLoadFailures.push_back ({
                "Track " + juce::String (t + 1),
                parsePluginName (track.pluginDescriptionXml) });
        }
    }
    for (int a = 0; a < Session::kNumAuxLanes; ++a)
    {
        auto& lane = session.auxLane (a);
        for (int s = 0; s < AuxLaneParams::kMaxLanePlugins; ++s)
        {
            auto& slot = auxLaneStrips[(size_t) a].getPluginSlot (s);
            const auto& descXml = lane.pluginDescriptionXml[(size_t) s];
            if (descXml.isEmpty()) continue;
            juce::String error;
            if (! slot.restoreFromSavedState (descXml,
                                                lane.pluginStateBase64[(size_t) s], error))
            {
                DBG ("AudioEngine: failed to restore plugin on aux lane " << (a + 1)
                      << " slot " << (s + 1) << ": " << error);
                lastPluginLoadFailures.push_back ({
                    "Aux " + juce::String (a + 1) + " / slot " + juce::String (s + 1),
                    parsePluginName (descXml) });
            }
        }
    }

#if DUSKSTUDIO_HAS_DUSK_DSP
    // Master tape state: push the deserialised base64 blob back into the
    // hosted TapeMachineAudioProcessor. fromBase64Encoding fails-soft (no
    // exception); empty / malformed data leaves the processor at its
    // donor defaults rather than blowing up the load.
    {
        const auto& s64 = session.master().tapeStateBase64;
        if (s64.isNotEmpty())
        {
            juce::MemoryBlock mb;
            if (mb.fromBase64Encoding (s64) && mb.getSize() > 0)
                master.getTapeProcessor().setStateInformation (mb.getData(),
                                                                  (int) mb.getSize());
        }
    }
#endif
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    // Detect the silent-failure mode where a per-device ALSA name resolves
    // to 0 active output channels (PipeWire's ALSA shim does this for the
    // surround/front entries - JUCE accepts the asymmetric setup, the
    // engine writes the master mix to a non-existent destination, and the
    // user gets silence with no error). Loud stderr line + a flag the UI
    // surfaces so the failure is visible instead of mysterious.
    const int activeIn  = device->getActiveInputChannels().countNumberOfSetBits();
    const int activeOut = device->getActiveOutputChannels().countNumberOfSetBits();
    if (activeOut <= 0)
    {
        std::fprintf (stderr,
                      "[Dusk Studio/AudioEngine] WARNING: device \"%s\" (type %s) opened with "
                      "0 output channels (in=%d). Engine output will be silent. Pick a "
                      "different device - \"Default ALSA Output\" or another backend - or "
                      "stop PipeWire if you want raw ALSA on this interface.\n",
                      device->getName().toRawUTF8(),
                      deviceManager.getCurrentAudioDeviceType().toRawUTF8(),
                      activeIn);
        usableOutputs.store (false, std::memory_order_relaxed);
    }
    else
    {
        usableOutputs.store (true, std::memory_order_relaxed);
    }

    // Reset every MIDI collector with the current sample rate so it can
    // convert the MIDI thread's millisecond timestamps into per-block sample
    // positions. Without this, removeNextBlockOfMessages would emit
    // garbage timestamps the first block. Safe to call here — the audio
    // callback hasn't fired yet for this open.
    {
        const double sr = device->getCurrentSampleRate();
        for (auto& c : midiInputCollectors)
            if (c != nullptr)
                c->reset (sr);
    }

    prepareForSelfTest (device->getCurrentSampleRate(),
                         device->getCurrentBufferSizeSamples());
}

void AudioEngine::stageTestMidiInjection (int inputIdx, juce::MidiBuffer events)
{
    // SPSC: wait for any previously staged buffer to be consumed before we
    // touch testInjectMidi. The synchronous self-test caller drives the
    // audio callback after every stage, so this normally observes false on
    // the first load - the spin is just defensive against future callers.
    while (testInjectReady.load (std::memory_order_acquire))
        std::this_thread::yield();

    testInjectMidi.swapWith (events);
    testInjectInputIdx.store (inputIdx, std::memory_order_relaxed);
    testInjectReady.store (true, std::memory_order_release);
}

void AudioEngine::prepareForSelfTest (double sr, int bs)
{
    // Must run on the message thread. The function mutates shared DSP
    // state (every strip's prepare, every smoother reset, every scratch
    // buffer resize) without locks. If a future caller invokes this
    // from another thread while the audio callback is live, the audio
    // thread reads half-prepared state and corrupts buffers silently.
    // The existing call paths (audioDeviceAboutToStart, BounceEngine,
    // DuskStudioApp startup, AudioPipelineSelfTest) all run here; assert
    // catches a future caller that gets this wrong.
    jassert (juce::MessageManager::existsAndIsCurrentThread());

    currentSampleRate.store (sr, std::memory_order_relaxed);
    currentBlockSize.store  (bs, std::memory_order_relaxed);

    // Cache the tick->seconds reciprocal once - the xrun watchdog hits this
    // twice per callback otherwise, and highResolutionTicksToSeconds does a
    // 64-bit divide internally.
    {
        const auto tps = juce::Time::getHighResolutionTicksPerSecond();
        secondsPerTick = (tps > 0) ? 1.0 / (double) tps : 0.0;
    }

    // Read the global oversampling factor once per prepare. Per-channel
    // strips don't take an oversampling param (ChannelComp is fast-path /
    // native-rate only); aux + master propagate it to their bus comps and
    // (for master) to the tape oversampler.
    const int oxFactor = session.oversamplingFactor.load (std::memory_order_relaxed);

    for (auto& s : strips)        s.prepare (sr, bs, oxFactor);
    for (auto& a : busStrips)     a.prepare (sr, bs, oxFactor);
    for (auto& a : auxLaneStrips) a.prepare (sr, bs);
    master.prepare (sr, bs, oxFactor);

    // Sync receiver uses the same sample-clock the engine derives its
    // per-block playhead from. Reset on every prepare so a sample-rate
    // change drops stale BPM history.
    midiSyncReceiver.prepare (sr);
    midiTimeCodeReceiver.prepare (sr);
    midiClockEmitter.prepare (sr);
    midiTimeCodeEmitter.prepare (sr);
#if DUSKSTUDIO_HAS_DUSK_DSP
    // TapeMachine animates its reels + level-integration timing from
    // getPlayHead()->getPosition(). Without a playhead the donor reads
    // null and the reels stay still even while audio passes through.
    master.getTapeProcessor().setPlayHead (playHead.get());
#endif

    // Push the playhead onto every per-channel plugin slot so hosted
    // synths/effects see the session's BPM, transport state, and sample
    // position. Without this, tempo-synced LFOs / arps / delays in
    // plugins like Diva default to 120 BPM regardless of session tempo.
    for (auto& s : strips)
        s.getPluginSlot().setHostPlayHead (playHead.get());
    for (auto& a : auxLaneStrips)
        for (int p = 0; p < AuxLaneParams::kMaxLanePlugins; ++p)
            a.getPluginSlot (p).setHostPlayHead (playHead.get());
    masteringChain.prepare (sr, bs, oxFactor);
    masteringPlayer.prepare (bs);
    metronome.prepare (sr);
    playbackEngine.prepare (bs);  // size the playback read scratch - audio thread mustn't allocate
    pitchDetector.prepare (sr);   // ~46 ms history at the device rate

    mixL.assign ((size_t) bs, 0.0f);
    mixR.assign ((size_t) bs, 0.0f);
    for (auto& v : busL)      v.assign ((size_t) bs, 0.0f);
    for (auto& v : busR)      v.assign ((size_t) bs, 0.0f);
    for (auto& v : auxLaneL)  v.assign ((size_t) bs, 0.0f);
    for (auto& v : auxLaneR)  v.assign ((size_t) bs, 0.0f);
    playbackScratch .assign ((size_t) bs, 0.0f);
    playbackScratchR.assign ((size_t) bs, 0.0f);
    silentInputScratch.assign ((size_t) bs, 0.0f);
}

void AudioEngine::audioDeviceError (const juce::String& errorMessage)
{
    // Fires from the audio thread (ALSA run() at fatal-recover exit;
    // macOS / Windows backends similar). stderr fprintf is RT-safe
    // enough for a dying-device path; everything else (FileLogger
    // write, RecordManager::stopRecording, Transport state-flip) is
    // message-thread-only and gets dispatched via callAsync. Without
    // the dispatch we'd join the disk thread + flush a ThreadedWriter
    // on the audio thread — RecordManager's stopRecording contract
    // forbids it. JUCE doesn't guarantee a follow-up
    // audioDeviceStopped after this callback, so we force it.
    std::fprintf (stderr, "[Dusk Studio/AudioEngine] audioDeviceError: %s\n",
                  errorMessage.toRawUTF8());

    juce::MessageManager::callAsync (
        [this, errorMessage]
        {
            juce::Logger::writeToLog ("[Dusk Studio/AudioEngine] audioDeviceError: "
                                          + errorMessage);
            audioDeviceStopped();
        });
}

void AudioEngine::audioDeviceStopped()
{
    // Stop any in-flight recording BEFORE clearing the sample rate: the
    // writer drain in stopRecording depends on recordSampleRate being
    // non-zero to map sample positions to seconds. Without this, a
    // device disconnect mid-take leaves recordManager.active = true,
    // and any audio block that slips in while the device is being torn
    // down would call writeInputBlock at sr=0 with stale writers - data
    // corruption shape. Also flips Transport state so the UI's
    // record button immediately reflects "stopped" rather than the
    // pre-disconnect "Recording".
    if (recordManager.isActive())
        recordManager.stopRecording (transport.getPlayhead());
    if (transport.getState() == Transport::State::Recording)
        transport.setState (Transport::State::Stopped);

    currentSampleRate.store (0.0, std::memory_order_relaxed);
    currentBlockSize.store  (0,   std::memory_order_relaxed);
    // Reset to the optimistic default so a transient "no device open" state
    // doesn't stick a NO OUTPUTS warning on the UI.
    usableOutputs.store (true, std::memory_order_relaxed);
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                                    int numInputChannels,
                                                    float* const* outputChannelData,
                                                    int numOutputChannels,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    juce::ScopedNoDenormals noDenormals;

    const auto callbackStart = juce::Time::getHighResolutionTicks();

    // Cache the device-I/O pointers + channel counts for the strips'
    // hardware-insert slots. Strips read/write through these for the
    // duration of THIS callback only - never cached across blocks.
    currentDeviceInputs     = inputChannelData;
    numCurrentDeviceInputs  = numInputChannels;
    currentDeviceOutputs    = outputChannelData;
    numCurrentDeviceOutputs = numOutputChannels;

    // Zero every device output up front. The master bus + click
    // overwrite their assigned channels at the end of the callback via
    // memcpy so pre-zeroing them is harmless. Hardware-insert sends
    // accumulate into their assigned channels via `+=`, which requires
    // a clean zero baseline. Pre-zeroing everything avoids baking in a
    // "master is always 0/1" assumption.
    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData != nullptr && outputChannelData[ch] != nullptr)
            std::memset (outputChannelData[ch], 0,
                          sizeof (float) * (size_t) numSamples);

    // Drain each MIDI input's collector into its per-input buffer for this
    // block. Each removeNextBlockOfMessages is lock-free with respect to
    // the MIDI thread's addMessageToQueue, so the audio thread never
    // contends. Empty buffers cost ~nothing.
    for (size_t i = 0; i < midiInputCollectors.size() && i < perInputMidi.size(); ++i)
    {
        perInputMidi[i].clear();
        if (midiInputCollectors[i] != nullptr)
            midiInputCollectors[i]->removeNextBlockOfMessages (perInputMidi[i], numSamples);
    }

    // Test-hook: if the message thread staged a buffer via
    // stageTestMidiInjection, merge it into the requested input's
    // per-block buffer, clear the staging slot, and release the SPSC
    // ready flag so the next stage call may proceed. Empty in production -
    // the relaxed-equivalent acquire load costs a single load + branch.
    if (testInjectReady.load (std::memory_order_acquire))
    {
        const int idx = testInjectInputIdx.load (std::memory_order_relaxed);
        if (idx >= 0 && (size_t) idx < perInputMidi.size())
            perInputMidi[(size_t) idx].addEvents (testInjectMidi, 0, numSamples, 0);
        testInjectMidi.clear();
        testInjectReady.store (false, std::memory_order_release);
    }

    // External MIDI Clock sync. Feed the chosen input's per-block
    // buffer to the receiver; if it derived a fresh BPM and the
    // follow-tempo flag is on, push the value into session.tempoBpm
    // so PlaybackEngine + the metronome track the master. Status
    // atoms feed the UI's "EXT" badge.
    //
    // Timestamping uses a private monotonic sample clock that ticks by
    // numSamples each block. transport.getPlayhead() jumps on loop
    // wrap / Cmd-jump / scrub - using it for clock intervals would
    // produce wild BPM swings whenever the user moves the playhead.
    const int syncIdx = session.syncSourceInputIdx.load (std::memory_order_relaxed);
    if (syncIdx != lastSyncSourceIdx)
    {
        // Source changed (or first run). Reset both the receiver's
        // history and the local sample clock so the new source starts
        // from a clean baseline. Also clear lastExtRolling — without
        // this, if the old source was rolling and the new source is
        // stopped, the first block fires a spurious Stop edge; if the
        // new source is rolling first-block, an old "rolling" latch can
        // suppress the Start edge that should fire.
        midiSyncReceiver.reset();
        midiTimeCodeReceiver.reset();
        midiSyncSampleClock = 0;
        lastSyncSourceIdx = syncIdx;
        lastExtRolling = false;
        lastMtcRolling = false;
        mtcDriftWindowFrames = 0;
        lastSeenMtcFrames = -1;
    }
    if (syncIdx >= 0 && (size_t) syncIdx < perInputMidi.size())
    {
        midiSyncReceiver.process (perInputMidi[(size_t) syncIdx], midiSyncSampleClock);
        midiTimeCodeReceiver.process (perInputMidi[(size_t) syncIdx],
                                         midiSyncSampleClock, numSamples);
        // Publish MTC decoded state. Same input port — Clock + MTC
        // multiplex on it; both decoders ignore bytes they don't own.
        const auto mtcFrames  = midiTimeCodeReceiver.getFrames();
        const bool mtcRolling = midiTimeCodeReceiver.isRolling();
        const auto mtcRate    = midiTimeCodeReceiver.getFrameRate();
        session.externalTimeCodeFrames   .store (mtcFrames, std::memory_order_relaxed);
        session.externalTimeCodeRolling  .store (mtcRolling, std::memory_order_relaxed);
        session.externalTimeCodeReversed .store (midiTimeCodeReceiver.isReversed(),
                                                   std::memory_order_relaxed);
        session.externalTimeCodeFrameRate.store ((int) mtcRate,
                                                   std::memory_order_relaxed);

        // MTC transport chase (freewheeling model).
        //   Initial lock: rolling false→true → set playhead + Play.
        //   Freewheel:    |transport - mtc| ≤ kFreewheelToleranceFrames
        //                 → trust internal clock, no relocate.
        //   Soft re-locate: drift > tolerance for kFreewheelReSyncWindow
        //                   consecutive MTC frames → set playhead.
        //   Stop:         rolling true→false → Stop (UNLESS the false
        //                 came from a reverse-scrub park; reverse keeps
        //                 transport rolling forward per plan).
        const bool chaseEnabled =
            session.externalTimeCodeChasesTransport.load (std::memory_order_relaxed);
        const bool reversed = midiTimeCodeReceiver.isReversed();

        // Force re-lock when user just enabled chase mid-roll. Without
        // this, the rising-edge happened while chase was off and the
        // edge detector missed it; transport would never snap.
        if (chaseEnabled && ! lastChaseEnabled && mtcRolling)
            lastMtcRolling = false;
        lastChaseEnabled = chaseEnabled;

        if (chaseEnabled)
        {
            constexpr int kFreewheelToleranceFrames = 2;
            constexpr int kFreewheelReSyncWindow    = 4;

            // Samples-per-frame at the current SMPTE rate. Reads sr
            // off the engine atom — well-defined because this block
            // can't execute until audioDeviceAboutToStart published a
            // non-zero rate.
            const double sr = currentSampleRate.load (std::memory_order_relaxed);
            const double sPerFrame =
                  mtcRate == MidiTimeCodeReceiver::Fps24      ? sr / 24.0
                : mtcRate == MidiTimeCodeReceiver::Fps25      ? sr / 25.0
                : mtcRate == MidiTimeCodeReceiver::Fps29_97DF ? sr * 1001.0 / 30000.0
                                                              : sr / 30.0;

            if (mtcRolling && ! lastMtcRolling)
            {
                // Initial lock on rising edge.
                session.pendingTransportPlayhead.store (
                    (juce::int64) ((double) mtcFrames * sPerFrame),
                    std::memory_order_relaxed);
                session.pendingTransportAction.store (
                    (int) PendingTransportAction::Play,
                    std::memory_order_relaxed);
                mtcDriftWindowFrames = 0;
                lastSeenMtcFrames    = mtcFrames;
            }
            else if (! mtcRolling && lastMtcRolling && ! reversed)
            {
                // Falling edge → stop. Gated on !reversed so a reverse-
                // scrub (which the receiver reports as rolling=false +
                // reversed=true) doesn't drag the transport into Stop;
                // master scrubbing back leaves Dusk Studio rolling forward.
                session.pendingTransportAction.store (
                    (int) PendingTransportAction::Stop,
                    std::memory_order_relaxed);
                mtcDriftWindowFrames = 0;
            }
            else if (mtcRolling && ! reversed && sPerFrame > 0.0)
            {
                // Freewheel + soft re-locate. Drift counter ticks only
                // when MTC actually advances a frame (not every audio
                // block) so kFreewheelReSyncWindow is in MTC-frame
                // units as the comment promises. Without the gate the
                // counter ticks at audio-block rate (~21 ms @ 48 k /
                // 1024 samples) and the window collapses to ~80 ms.
                if (mtcFrames != lastSeenMtcFrames)
                {
                    const auto transportSamples = transport.getPlayhead();
                    const auto mtcSamples =
                        (juce::int64) ((double) mtcFrames * sPerFrame);
                    const auto deltaSamples = transportSamples - mtcSamples;
                    const auto deltaFrames = std::llabs (
                        (juce::int64) ((double) deltaSamples / sPerFrame));
                    if (deltaFrames > kFreewheelToleranceFrames)
                    {
                        if (++mtcDriftWindowFrames >= kFreewheelReSyncWindow)
                        {
                            session.pendingTransportPlayhead.store (
                                mtcSamples, std::memory_order_relaxed);
                            mtcDriftWindowFrames = 0;
                        }
                    }
                    else
                    {
                        mtcDriftWindowFrames = 0;
                    }
                    lastSeenMtcFrames = mtcFrames;
                }
            }
        }

        // Edge detector advances every block regardless of chaseEnabled
        // so re-enabling chase mid-roll observes the correct current
        // state (combined with the force-relock above this gives the
        // user "snap to MTC now" UX without missing edges that
        // happened while disabled).
        lastMtcRolling = mtcRolling;

        const float ext = midiSyncReceiver.getBpm();
        const bool extRolling = midiSyncReceiver.isRolling();
        session.externalBpm.store (ext, std::memory_order_relaxed);
        session.externalSyncRolling.store (extRolling, std::memory_order_relaxed);
        if (ext > 0.0f
            && session.externalSyncFollowsTempo.load (std::memory_order_relaxed))
        {
            session.tempoBpm.store (ext, std::memory_order_relaxed);
        }

        // Transport chase. Edge-triggered on the rolling flag so a
        // long-running Start signal doesn't restart the transport
        // every block. The actual play()/stop() calls are message-
        // thread - we signal via pendingTransportAction and let the
        // engine's existing timer drain it (same path MIDI Learn
        // transport bindings use).
        if (session.externalSyncChasesTransport.load (std::memory_order_relaxed))
        {
            if (extRolling && ! lastExtRolling)
            {
                session.pendingTransportAction.store (
                    (int) PendingTransportAction::Play,
                    std::memory_order_relaxed);
            }
            else if (! extRolling && lastExtRolling)
            {
                session.pendingTransportAction.store (
                    (int) PendingTransportAction::Stop,
                    std::memory_order_relaxed);
            }
        }
        lastExtRolling = extRolling;
    }
    else
    {
        // Clear the readouts when no source is selected so the UI badge
        // doesn't stay frozen at the last value after the user turns
        // sync off.
        session.externalBpm.store (0.0f, std::memory_order_relaxed);
        session.externalSyncRolling.store (false, std::memory_order_relaxed);
        session.externalTimeCodeFrames  .store (0,     std::memory_order_relaxed);
        session.externalTimeCodeRolling .store (false, std::memory_order_relaxed);
        session.externalTimeCodeReversed.store (false, std::memory_order_relaxed);
        lastExtRolling = false;
    }
    midiSyncSampleClock += numSamples;

    // MIDI Clock OUTPUT (master mode). Generates F8 + FA/FC bytes into
    // a scratch MidiBuffer and hands it to the chosen MidiOutput's
    // background delivery thread. Uses the same monotonic sync clock
    // so receiver + emitter share a sample-time origin (matters when
    // a user has Dusk Studio as both slave AND master - rare but possible
    // via a MIDI thru).
    const int syncOutIdx = session.syncOutputIdx.load (std::memory_order_relaxed);
    if (syncOutIdx != lastSyncOutputIdx)
    {
        midiClockEmitter.reset();
        midiTimeCodeEmitter.reset();
        lastSyncOutputIdx = syncOutIdx;
    }
    const bool portReady = syncOutIdx >= 0
                         && (size_t) syncOutIdx < midiOutputs.size()
                         && midiOutputs[(size_t) syncOutIdx] != nullptr;
    const bool emitClock = portReady
                         && session.syncOutputEmitClock.load (std::memory_order_relaxed);
    const bool emitTimeCode = portReady
                            && session.syncOutputEmitTimeCode.load (std::memory_order_relaxed);
    if (emitClock || emitTimeCode)
    {
        // Rolling state from the engine's transport - the bytes drive
        // SLAVES, so they need to mirror Dusk Studio's local state, not the
        // external master's.
        const bool rolling = transport.isPlaying() || transport.isRecording();
        midiClockOutScratch.clear();

        if (emitClock)
        {
            const float emitBpm = session.tempoBpm.load (std::memory_order_relaxed);
            midiClockEmitter.generateBlock (midiSyncSampleClock - numSamples,
                                              numSamples, emitBpm, rolling,
                                              midiClockOutScratch);
        }
        if (emitTimeCode)
        {
            // MTC + Clock multiplex onto the same scratch buffer; the
            // chosen MidiOutput delivers both message types together.
            const auto rate = (MidiTimeCodeEmitter::FrameRate)
                session.syncOutputTimeCodeFrameRate.load (std::memory_order_relaxed);
            midiTimeCodeEmitter.generateBlock (midiSyncSampleClock - numSamples,
                                                  numSamples,
                                                  transport.getPlayhead(),
                                                  rolling, rate,
                                                  midiClockOutScratch);
        }

        if (! midiClockOutScratch.isEmpty())
        {
            // sendBlockOfMessages places the buffer into JUCE's background
            // delivery queue. The timestamp arg is the "now" wall-clock
            // ms used to scale the per-event sample offsets. sr > 0
            // guaranteed by audioDeviceAboutToStart so the divide is
            // safe here.
            const double srSend = currentSampleRate.load (std::memory_order_relaxed);
            midiOutputs[(size_t) syncOutIdx]->sendBlockOfMessages (
                midiClockOutScratch,
                juce::Time::getMillisecondCounterHiRes(),
                srSend > 0.0 ? srSend : 48000.0);
        }
    }

    // Held-MIDI-notes tracking for the chord display. Walk every drained
    // MIDI buffer and flip the per-note atomic on Note On / Note Off.
    // Treat NoteOn vel=0 as NoteOff (running-status convention). Cheap -
    // ~100 ns per event - and the SystemStatusBar's chord poll reads a
    // snapshot of the array off-thread.
    for (const auto& buf : perInputMidi)
    {
        if (buf.isEmpty()) continue;
        for (const auto meta : buf)
        {
            const auto m = meta.getMessage();
            if (m.isNoteOn() && m.getVelocity() > 0)
            {
                const int n = m.getNoteNumber();
                if (n >= 0 && n < Session::kNumMidiNotes)
                    session.heldMidiNotes[(size_t) n].store (true,
                                                                std::memory_order_relaxed);
            }
            else if (m.isNoteOff() || (m.isNoteOn() && m.getVelocity() == 0))
            {
                const int n = m.getNoteNumber();
                if (n >= 0 && n < Session::kNumMidiNotes)
                    session.heldMidiNotes[(size_t) n].store (false,
                                                                std::memory_order_relaxed);
            }
        }
    }

    // MIDI controller bindings - apply BEFORE the per-track filter so a
    // bound CC drives its target regardless of which track the message
    // happens to be addressed to. Lock-free acquire-load of the binding
    // snapshot (mutated only on the message thread via AtomicSnapshot's
    // copy-and-swap). Continuous targets write atoms directly; transport
    // actions queue into pendingTransportAction (engine.play/stop/record
    // aren't RT-safe). Note triggers fire on press only (NoteOn vel > 0);
    // NoteOff and CC release are ignored per the v1 spec.
    const auto* bindings = session.midiBindings.read();
    if (bindings != nullptr && ! bindings->empty())
    {
        for (const auto& buf : perInputMidi)
        {
            if (buf.isEmpty()) continue;
            for (const auto meta : buf)
            {
                const auto m = meta.getMessage();
                MidiBindingTrigger tg;
                int dn = 0, val = 0;
                if      (m.isController())             { tg = MidiBindingTrigger::CC;   dn = m.getControllerNumber(); val = m.getControllerValue(); }
                else if (m.isNoteOn() && m.getVelocity() > 0) { tg = MidiBindingTrigger::Note; dn = m.getNoteNumber();       val = m.getVelocity(); }
                else continue;
                const int ch = m.getChannel();

                // Learn capture - take the first matching event when a
                // learn target is pending. Audio-thread CAS-store; the
                // message thread drains and appends a binding, then
                // clears midiLearnPending. We still apply normal binding
                // matching below so a freshly-captured CC also drives
                // its target on the same block (cheap, harmless).
                if (session.midiLearnPending.load (std::memory_order_relaxed) >= 0
                    && ! learnCaptureIsValid (session.midiLearnCapture.load (std::memory_order_relaxed)))
                {
                    session.midiLearnCapture.store (
                        packLearnCapture (tg, ch, dn), std::memory_order_relaxed);
                }

                for (const auto& b : *bindings)
                {
                    if (! b.sourceMatches (ch, dn, tg)) continue;
                    if (! b.isValid()) continue;

                    switch (b.target)
                    {
                        case MidiBindingTarget::TransportPlay:
                            session.pendingTransportAction.store (
                                (int) PendingTransportAction::Play, std::memory_order_relaxed);
                            break;
                        case MidiBindingTarget::TransportStop:
                            session.pendingTransportAction.store (
                                (int) PendingTransportAction::Stop, std::memory_order_relaxed);
                            break;
                        case MidiBindingTarget::TransportRecord:
                            session.pendingTransportAction.store (
                                (int) PendingTransportAction::Record, std::memory_order_relaxed);
                            break;
                        case MidiBindingTarget::TransportToggle:
                            session.pendingTransportAction.store (
                                (int) PendingTransportAction::Toggle, std::memory_order_relaxed);
                            break;

                        case MidiBindingTarget::TrackFader:
                            if (b.targetIndex >= 0 && b.targetIndex < Session::kNumTracks)
                            {
                                // CC 0..127 -> -90..+12 dB linearly. -90 maps below
                                // the kFaderInfThreshDb hard-mute floor so a value
                                // of 0 cleanly silences the strip.
                                const float frac = (float) val / 127.0f;
                                const float db = -90.0f + frac * (12.0f + 90.0f);
                                session.track (b.targetIndex).strip.faderDb.store (
                                    db, std::memory_order_relaxed);
                            }
                            break;
                        case MidiBindingTarget::TrackPan:
                            if (b.targetIndex >= 0 && b.targetIndex < Session::kNumTracks)
                            {
                                const float p = ((float) val / 127.0f) * 2.0f - 1.0f;
                                session.track (b.targetIndex).strip.pan.store (
                                    p, std::memory_order_relaxed);
                            }
                            break;
                        case MidiBindingTarget::TrackMute:
                            if (val >= 64 && b.targetIndex >= 0 && b.targetIndex < Session::kNumTracks)
                            {
                                auto& a = session.track (b.targetIndex).strip.mute;
                                a.store (! a.load (std::memory_order_relaxed),
                                          std::memory_order_relaxed);
                            }
                            break;
                        case MidiBindingTarget::TrackSolo:
                            if (val >= 64 && b.targetIndex >= 0 && b.targetIndex < Session::kNumTracks)
                            {
                                auto& a = session.track (b.targetIndex).strip.solo;
                                a.store (! a.load (std::memory_order_relaxed),
                                          std::memory_order_relaxed);
                            }
                            break;
                        case MidiBindingTarget::TrackArm:
                            if (val >= 64 && b.targetIndex >= 0 && b.targetIndex < Session::kNumTracks)
                            {
                                auto& a = session.track (b.targetIndex).recordArmed;
                                a.store (! a.load (std::memory_order_relaxed),
                                          std::memory_order_relaxed);
                            }
                            break;
                        case MidiBindingTarget::TrackAuxSend:
                        {
                            // Decode the packed (track, aux) index and map
                            // CC 0..127 onto the aux-send dB range. CC 0
                            // lands on the kAuxSendOffDb sentinel so a
                            // fully-down controller hard-mutes the send
                            // (matches the UI knob's CCW behaviour).
                            const int trk = unpackTrackAuxTrack (b.targetIndex);
                            const int aux = unpackTrackAuxLane  (b.targetIndex);
                            if (trk >= 0 && trk < Session::kNumTracks
                                && aux >= 0 && aux < ChannelStripParams::kNumAuxSends)
                            {
                                const float db = (val == 0)
                                    ? ChannelStripParams::kAuxSendOffDb
                                    : ChannelStripParams::kAuxSendMinDb
                                       + ((float) val / 127.0f)
                                          * (ChannelStripParams::kAuxSendMaxDb
                                             - ChannelStripParams::kAuxSendMinDb);
                                session.track (trk).strip.auxSendDb[(size_t) aux]
                                    .store (db, std::memory_order_relaxed);
                            }
                            break;
                        }
                        case MidiBindingTarget::TrackHpfFreq:
                        {
                            // CC 0 maps to kHpfOffHz (bypass sentinel). 1..127
                            // is log-mapped over the rest of the range so the
                            // perceived sweep stays linear across the band.
                            if (b.targetIndex >= 0 && b.targetIndex < Session::kNumTracks)
                            {
                                float freq;
                                if (val == 0)
                                {
                                    freq = ChannelStripParams::kHpfOffHz;
                                }
                                else
                                {
                                    const float lo = ChannelStripParams::kHpfMinHz;
                                    const float hi = ChannelStripParams::kHpfMaxHz;
                                    const float frac = (float) val / 127.0f;
                                    freq = lo * std::exp (std::log (hi / lo) * frac);
                                }
                                session.track (b.targetIndex).strip.hpfFreq
                                    .store (freq, std::memory_order_relaxed);
                                session.track (b.targetIndex).strip.hpfEnabled
                                    .store (freq > ChannelStripParams::kHpfOffHz + 0.5f,
                                             std::memory_order_relaxed);
                            }
                            break;
                        }
                        case MidiBindingTarget::TrackEqGain:
                        {
                            // CC 0..127 -> -15..+15 dB linearly. Band index
                            // 0=LF, 1=LM, 2=HM, 3=HF; the apply path writes
                            // the matching atom on the strip.
                            const int trk  = unpackTrackEqTrack (b.targetIndex);
                            const int band = unpackTrackEqBand  (b.targetIndex);
                            if (trk >= 0 && trk < Session::kNumTracks
                                && band >= 0 && band < kPackedEqBands)
                            {
                                const float frac = (float) val / 127.0f;
                                const float db = -15.0f + frac * 30.0f;
                                auto& strip = session.track (trk).strip;
                                switch (band)
                                {
                                    case 0: strip.lfGainDb.store (db, std::memory_order_relaxed); break;
                                    case 1: strip.lmGainDb.store (db, std::memory_order_relaxed); break;
                                    case 2: strip.hmGainDb.store (db, std::memory_order_relaxed); break;
                                    case 3: strip.hfGainDb.store (db, std::memory_order_relaxed); break;
                                }
                            }
                            break;
                        }
                        case MidiBindingTarget::TrackCompThresh:
                        case MidiBindingTarget::TrackCompMakeup:
                        {
                            // Mode-aware: the strip exposes Opto / FET / VCA
                            // each with its own "threshold-ish" and "makeup-
                            // ish" knob, on different ranges. Pick the atom
                            // matching the CURRENT mode and remap CC 0..127
                            // onto that range. A single binding therefore
                            // tracks whatever mode the user has selected.
                            if (b.targetIndex < 0 || b.targetIndex >= Session::kNumTracks)
                                break;
                            auto& strip = session.track (b.targetIndex).strip;
                            const int mode = juce::jlimit (0, 2,
                                strip.compMode.load (std::memory_order_relaxed));
                            const float frac = (float) val / 127.0f;
                            const bool isMakeup = (b.target == MidiBindingTarget::TrackCompMakeup);
                            auto remap = [frac] (float lo, float hi)
                            {
                                return lo + frac * (hi - lo);
                            };
                            if (isMakeup)
                            {
                                switch (mode)
                                {
                                    case 0: strip.compOptoGain.store (remap (0.0f, 100.0f), std::memory_order_relaxed); break;
                                    case 1: strip.compFetOutput.store (remap (-20.0f, 20.0f), std::memory_order_relaxed); break;
                                    case 2: strip.compVcaOutput.store (remap (-20.0f, 20.0f), std::memory_order_relaxed); break;
                                }
                                // Mirror to the unified makeup atom so the
                                // FET threshold math stays composed (see
                                // ChannelStripParams::compMakeupDb comment).
                                strip.compMakeupDb.store (remap (-20.0f, 20.0f), std::memory_order_relaxed);
                            }
                            else // threshold
                            {
                                switch (mode)
                                {
                                    case 0: strip.compOptoPeakRed.store (remap (0.0f, 100.0f), std::memory_order_relaxed); break;
                                    case 1: strip.compFetInput.store    (remap (-20.0f, 40.0f), std::memory_order_relaxed); break;
                                    case 2: strip.compVcaThreshDb.store (remap (-38.0f, 12.0f), std::memory_order_relaxed); break;
                                }
                            }
                            break;
                        }
                        case MidiBindingTarget::BusFader:
                            if (b.targetIndex >= 0 && b.targetIndex < Session::kNumBuses)
                            {
                                const float frac = (float) val / 127.0f;
                                const float db = -90.0f + frac * (12.0f + 90.0f);
                                session.bus (b.targetIndex).strip.faderDb.store (
                                    db, std::memory_order_relaxed);
                            }
                            break;
                        case MidiBindingTarget::BusPan:
                            if (b.targetIndex >= 0 && b.targetIndex < Session::kNumBuses)
                            {
                                const float p = ((float) val / 127.0f) * 2.0f - 1.0f;
                                session.bus (b.targetIndex).strip.pan.store (
                                    p, std::memory_order_relaxed);
                            }
                            break;
                        case MidiBindingTarget::BusMute:
                            if (val >= 64 && b.targetIndex >= 0 && b.targetIndex < Session::kNumBuses)
                            {
                                auto& a = session.bus (b.targetIndex).strip.mute;
                                a.store (! a.load (std::memory_order_relaxed),
                                          std::memory_order_relaxed);
                            }
                            break;
                        case MidiBindingTarget::BusSolo:
                            // Routes through Session::setBusSoloed so the
                            // anyBusSoloed counter stays in sync (the audio
                            // thread relies on it for O(1) "is any bus
                            // soloed?" checks).
                            if (val >= 64 && b.targetIndex >= 0 && b.targetIndex < Session::kNumBuses)
                            {
                                const bool was = session.bus (b.targetIndex)
                                                       .strip.solo.load (std::memory_order_relaxed);
                                session.setBusSoloed (b.targetIndex, ! was);
                            }
                            break;
                        case MidiBindingTarget::TrackPluginParam:
                        {
                            // CC 0..127 -> 0..1 normalised. Targets the
                            // strip's plugin slot at the param index
                            // stored in the binding (filled at learn-
                            // resolve time from the slot's last-touched
                            // tracker). paramIndex >= 0 is enforced
                            // here so the apply site matches the
                            // inline-validation pattern other targets
                            // use; setParamNormalised also no-ops on
                            // out-of-range as a second line of defence.
                            if (b.targetIndex >= 0
                                && b.targetIndex < Session::kNumTracks
                                && b.paramIndex >= 0)
                            {
                                const float v = (float) val / 127.0f;
                                getChannelStrip (b.targetIndex)
                                    .getPluginSlot()
                                    .setParamNormalised (b.paramIndex, v);
                            }
                            break;
                        }
                        case MidiBindingTarget::AuxLaneFader:
                            if (b.targetIndex >= 0 && b.targetIndex < Session::kNumAuxLanes)
                            {
                                const float frac = (float) val / 127.0f;
                                const float db = -90.0f + frac * (12.0f + 90.0f);
                                session.auxLane (b.targetIndex).params.returnLevelDb
                                    .store (db, std::memory_order_relaxed);
                            }
                            break;
                        case MidiBindingTarget::AuxLaneMute:
                            if (val >= 64 && b.targetIndex >= 0 && b.targetIndex < Session::kNumAuxLanes)
                            {
                                auto& a = session.auxLane (b.targetIndex).params.mute;
                                a.store (! a.load (std::memory_order_relaxed),
                                          std::memory_order_relaxed);
                            }
                            break;

                        case MidiBindingTarget::MasterFader:
                        {
                            const float frac = (float) val / 127.0f;
                            const float db = -90.0f + frac * (12.0f + 90.0f);
                            session.master().faderDb.store (db, std::memory_order_relaxed);
                            break;
                        }
                        case MidiBindingTarget::None:
                            break;
                    }
                }
            }
        }
    }

    if ((int) mixL.size() < numSamples)
    {
        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (auto* out = outputChannelData[ch])
                std::memset (out, 0, sizeof (float) * (size_t) numSamples);
        return;
    }

    // Mastering stage runs an entirely different signal flow: stereo file →
    // MasteringChain → output. No track processing, no aux/master, no
    // recorder. We share `mixL/mixR` as scratch so we don't allocate on
    // the audio thread.
    if (stage.load (std::memory_order_relaxed) == Stage::Mastering)
    {
        masteringPlayer.process (mixL.data(), mixR.data(), numSamples);
        masteringChain.processInPlace (mixL.data(), mixR.data(), numSamples);

        if (numOutputChannels >= 1 && outputChannelData[0] != nullptr)
            std::memcpy (outputChannelData[0], mixL.data(), sizeof (float) * (size_t) numSamples);
        if (numOutputChannels >= 2 && outputChannelData[1] != nullptr)
            std::memcpy (outputChannelData[1], mixR.data(), sizeof (float) * (size_t) numSamples);

        const auto sr = currentSampleRate.load (std::memory_order_relaxed);
        if (sr > 0.0)
        {
            const double bufferMs = 1000.0 * (double) numSamples / sr;
            // Multiply by cached reciprocal instead of calling
            // highResolutionTicksToSeconds (which does an internal divide
            // every call). secondsPerTick is set in prepareForSelfTest.
            const double elapsedMs = (double) (juce::Time::getHighResolutionTicks() - callbackStart)
                                         * secondsPerTick * 1000.0;
            if (elapsedMs > bufferMs)
                xrunCount.fetch_add (1, std::memory_order_relaxed);
        }
        return;
    }

    // SIMD-friendly clear via JUCE's FloatVectorOperations - it dispatches to
    // the platform's vector zero-store on x86 (SSE) / aarch64 (NEON) where
    // memset would still call into libc and miss the alignment-aware path.
    juce::FloatVectorOperations::clear (mixL.data(), numSamples);
    juce::FloatVectorOperations::clear (mixR.data(), numSamples);
    for (auto& v : busL) juce::FloatVectorOperations::clear (v.data(), numSamples);
    for (auto& v : busR) juce::FloatVectorOperations::clear (v.data(), numSamples);
    for (auto& v : auxLaneL) juce::FloatVectorOperations::clear (v.data(), numSamples);
    for (auto& v : auxLaneR) juce::FloatVectorOperations::clear (v.data(), numSamples);

    const bool anyChannelSolo = session.anyTrackSoloed();
    const bool anyBusSolo     = session.anyBusSoloed();

    std::array<float*, ChannelStrip::kNumBuses> busLPtrs {};
    std::array<float*, ChannelStrip::kNumBuses> busRPtrs {};
    for (int a = 0; a < Session::kNumBuses; ++a)
    {
        busLPtrs[(size_t) a] = busL[(size_t) a].data();
        busRPtrs[(size_t) a] = busR[(size_t) a].data();
    }
    std::array<float*, ChannelStripParams::kNumAuxSends> auxLanePtrsL {};
    std::array<float*, ChannelStripParams::kNumAuxSends> auxLanePtrsR {};
    for (int a = 0; a < Session::kNumAuxLanes; ++a)
    {
        auxLanePtrsL[(size_t) a] = auxLaneL[(size_t) a].data();
        auxLanePtrsR[(size_t) a] = auxLaneR[(size_t) a].data();
    }

    const auto state = transport.getState();
    const bool isPlaying   = (state == Transport::State::Playing);
    const bool isRecording = (state == Transport::State::Recording);
    const juce::int64 blockStartSamples = transport.getPlayhead();

    // Hanging-note protection. Detect two events that warrant a per-MIDI-
    // track "All Notes Off" flush this block:
    //   • Transport rolling -> stopped (held notes won't get their Note
    //     Off from the region or input stream).
    //   • Playhead discontinuity while still rolling (loop wrap, scrub).
    // Both cases produce stuck synth voices without an explicit flush.
    const bool isRolling = isPlaying || isRecording;
    const bool transportJustStopped = wasRolling && ! isRolling;
    const bool playheadJumped = isRolling
                              && lastBlockEndSample != 0
                              && blockStartSamples != lastBlockEndSample;
    const bool flushHangingMidi = transportJustStopped || playheadJumped;
    wasRolling         = isRolling;
    lastBlockEndSample = blockStartSamples + numSamples;

    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        const auto& trackParams = session.track (t).strip;

        // Automation routing runs FIRST so the per-strip routing decisions
        // below (passes / monitorPasses / etc.) see the automated values
        // when in Read or Touch mode. Writes liveFaderDb / livePan /
        // liveAuxSendDb / liveMute. ChannelStrip's processAndAccumulate
        // also reads these live atoms when computing per-sample gains.
        //
        // The mode atom is loaded with `acquire` so the audio thread sees
        // every prior write to the lane's points vector (made on the
        // message thread during a Write pass) before reading it - the UI
        // release-stores the new mode after appending, so the load-acquire
        // here pairs with the store-release there.
        {
            const int amode = session.track (t).automationMode.load (std::memory_order_acquire);

            // Per-param continuous routing: Read pulls from the lane
            // unconditionally; Touch pulls from the lane only while the
            // user is NOT grabbing that specific control; Off / Write
            // always pass the manual setpoint through. Per-control
            // `touched` flags so a grab on the fader doesn't release pan.
            auto routeContinuous = [&] (AutomationParam param,
                                          const std::atomic<float>& manual,
                                          const std::atomic<bool>* touched,
                                          std::atomic<float>& live)
            {
                const auto& lane = session.track (t).automationLanes[(size_t) param];
                const bool readsLane =
                       amode == (int) AutomationMode::Read
                    || (amode == (int) AutomationMode::Touch
                        && touched != nullptr
                        && ! touched->load (std::memory_order_acquire));
                const float v = (readsLane && ! lane.points.empty())
                    ? evaluateLane (lane, blockStartSamples, param)
                    : manual.load (std::memory_order_relaxed);
                live.store (v, std::memory_order_relaxed);
            };

            routeContinuous (AutomationParam::FaderDb,
                              trackParams.faderDb, &trackParams.faderTouched,
                              trackParams.liveFaderDb);
            routeContinuous (AutomationParam::Pan,
                              trackParams.pan, &trackParams.panTouched,
                              trackParams.livePan);
            for (int i = 0; i < ChannelStripParams::kNumAuxSends; ++i)
            {
                const auto param = (AutomationParam) ((int) AutomationParam::AuxSend1 + i);
                routeContinuous (param,
                                  trackParams.auxSendDb[(size_t) i],
                                  &trackParams.auxSendTouched[(size_t) i],
                                  trackParams.liveAuxSendDb[(size_t) i]);
            }

            // Mute / Solo - discrete, no Touch flag. Read or Touch reads
            // lane; Off or Write reads manual. Discrete params return
            // 0.0 or 1.0 from evaluateLane (after denormalize); we
            // threshold at 0.5 to a bool. Empty lane falls through to
            // manual.
            auto routeDiscrete = [&] (AutomationParam param,
                                       const std::atomic<bool>& manual,
                                       std::atomic<bool>& live)
            {
                const auto& lane = session.track (t).automationLanes[(size_t) param];
                const bool readsLane =
                       amode == (int) AutomationMode::Read
                    || amode == (int) AutomationMode::Touch;
                const bool effective = (readsLane && ! lane.points.empty())
                    ? (evaluateLane (lane, blockStartSamples, param) >= 0.5f)
                    : manual.load (std::memory_order_relaxed);
                live.store (effective, std::memory_order_relaxed);
            };
            routeDiscrete (AutomationParam::Mute, trackParams.mute, trackParams.liveMute);
            routeDiscrete (AutomationParam::Solo, trackParams.solo, trackParams.liveSolo);
        }

        // Reads liveMute / liveSolo - just-routed by the block above, so
        // the strip's passes / monitorPasses calculation sees automated
        // mute and solo state.
        const bool muted   = trackParams.liveMute.load (std::memory_order_relaxed);
        const bool soloed  = trackParams.liveSolo.load (std::memory_order_relaxed);
        const bool armed   = session.track (t).recordArmed.load (std::memory_order_relaxed);
        const bool monitorEnabled = session.track (t).inputMonitor.load (std::memory_order_relaxed);
        const bool midiTrack = session.track (t).mode.load (std::memory_order_relaxed)
                                   == (int) Track::Mode::Midi;

        // The track will read from disk during Play, or during Record on
        // un-armed tracks (so other tracks keep playing while we record into
        // an armed one). Otherwise the strip's source is live device input.
        const bool willReadFromDisk = isPlaying || (isRecording && ! armed);

        // The IN button (inputMonitor) gates LIVE input from passing through
        // the strip to master - engineer turns IN off when monitoring direct
        // from the hardware interface. It must NOT gate disk playback: when
        // we're playing back a previous take (regardless of arm state), the
        // disk audio should always reach master. So monitorPasses is forced
        // true when reading from disk; only when we're routing live input
        // does the IN toggle actually silence the master path.
        const bool monitorPasses = willReadFromDisk
            ? true
            : (armed ? monitorEnabled : true);
        const bool passes = ! muted && (anyChannelSolo ? soloed : true) && monitorPasses;

        // Resolve the input source for this track.
        const int inputIdx = session.resolveInputForTrack (t);
        const float* deviceInput = (inputIdx >= 0 && inputIdx < numInputChannels)
                                    ? inputChannelData[(size_t) inputIdx] : nullptr;

        // Tuner: when this track is the selected target, feed the device
        // input to the YIN-style PitchDetector. Allocation-free; the
        // detector publishes the latest Hz / level into Session atoms
        // that the TunerOverlay polls at 30 Hz on the message thread.
        // Inactive when tuneTrackIndex < 0.
        if (deviceInput != nullptr
            && session.tuneTrackIndex.load (std::memory_order_relaxed) == t)
        {
            pitchDetector.pushBlock (deviceInput, numSamples);
            session.tuneLatestHz   .store (pitchDetector.getLatestHz(),    std::memory_order_relaxed);
            session.tuneLatestLevel.store (pitchDetector.getLatestLevel(), std::memory_order_relaxed);
        }

        // Choose the source the channel strip will process.
        //   - Playing & not armed: read previous take from disk (un-armed
        //     playback tracks).
        //   - Playing & armed: ALSO read previous take from disk. Standard
        //     DAW behavior - stopping a record pass leaves the track armed
        //     for the next take, but Play should still reproduce what was
        //     recorded. Without this, hitting Play after a stop produced
        //     silence on the just-recorded track (the live input branch
        //     fired instead of disk read).
        //   - Recording & armed: live input feeds the strip; the recorder
        //     captures via writeInputBlock below. The IN toggle gates
        //     master accumulation (passByGate / monitorPasses), not whether
        //     the strip processes.
        //   - Stopped & armed (or stopped + IN on, un-armed): live input
        //     for monitoring.
        //   - Stopped & un-armed & IN off: monoIn null → strip is silent.
        const float* monoIn = nullptr;
        const bool stereoTrackInput = session.track (t).mode.load (std::memory_order_relaxed)
                                          == (int) Track::Mode::Stereo;

        if (willReadFromDisk)
        {
            // Stereo tracks read both channels from disk; mono tracks pass
            // nullptr for outR which makes readForTrack skip the second
            // channel entirely.
            float* outR = stereoTrackInput ? playbackScratchR.data() : nullptr;
            playbackEngine.readForTrack (t, blockStartSamples,
                                          playbackScratch.data(), outR,
                                          numSamples);
            monoIn = playbackScratch.data();
        }
        else if (deviceInput != nullptr && (armed || monitorEnabled))
        {
            monoIn = deviceInput;
        }
        else if (strips[(size_t) t].getPluginSlot().isLoaded())
        {
            // Generator-style insert with no input source: feed a
            // pre-zeroed buffer so the chain runs and the plugin can
            // emit its output. Costs one extra strip per loaded-but-
            // unfed channel; effects feeding zero just produce zero.
            monoIn = silentInputScratch.data();
        }

        // Input meter - armed tracks always show live input (so the user can
        // set gain for tracking even with monitor muted). Un-armed tracks
        // show their playback signal during transport, or live input when IN
        // is engaged. With IN off and no transport, the meter reads silence
        // even though the strip's DSP is still running on deviceInput (so a
        // PRINT-mode recorder can capture post-effects audio).
        const float* meterSrc = nullptr;
        if (armed && deviceInput != nullptr)
            meterSrc = deviceInput;
        else if ((isPlaying || isRecording) && ! armed)
            meterSrc = monoIn;
        else if (monitorEnabled && deviceInput != nullptr)
            meterSrc = deviceInput;

        // SIMD'd absolute-peak via findMinAndMax then |min| vs max - one
        // vector pass over the buffer instead of a scalar abs-and-compare
        // loop per sample. Same numeric result, much faster on long blocks.
        float inputPeak = 0.0f;
        if (meterSrc != nullptr && numSamples > 0)
        {
            const auto rng = juce::FloatVectorOperations::findMinAndMax (meterSrc, numSamples);
            inputPeak = juce::jmax (std::abs (rng.getStart()), std::abs (rng.getEnd()));
        }
        const float inputDb = (inputPeak > 1e-5f)
                              ? juce::Decibels::gainToDecibels (inputPeak, -100.0f)
                              : -100.0f;
        session.track (t).meterInputDb.store (inputDb, std::memory_order_relaxed);

        // R-channel input meter for stereo tracks. Phase 1 only renders the
        // peak; the strip's audio path is still mono until Phase 2 wires in
        // stereo recording. Mono / Midi tracks get -100 so the UI can fall
        // back to a single LED bar without gating on mode.
        float inputRDb = -100.0f;
        const int rIdx = session.resolveInputRForTrack (t);
        if (rIdx >= 0 && rIdx < numInputChannels)
        {
            const float* deviceInputR = inputChannelData[(size_t) rIdx];
            if (deviceInputR != nullptr && (armed || monitorEnabled) && numSamples > 0)
            {
                const auto rng = juce::FloatVectorOperations::findMinAndMax (deviceInputR, numSamples);
                const float peak = juce::jmax (std::abs (rng.getStart()), std::abs (rng.getEnd()));
                if (peak > 1e-5f)
                    inputRDb = juce::Decibels::gainToDecibels (peak, -100.0f);
            }
        }
        session.track (t).meterInputRDb.store (inputRDb, std::memory_order_relaxed);

        // MIDI input filter — pull events from the track's selected input
        // Hanging-note flush. Emitted FIRST (sample 0) so the synth
        // releases any held voices before we add this block's new events
        // on top. CC 64 = sustain pedal off, CC 123 = all notes off.
        // We hit all 16 channels because we don't track which channels
        // had active notes - cheap brute-force is fine, the synth ignores
        // the redundant channels in the same processBlock pass.
        // Skipped on non-MIDI tracks; perTrackMidiScratch on those tracks
        // stays an empty buffer for effect inserts.
        //
        // Three triggers warrant a flush, all OR'd together:
        //   • engine-wide flushHangingMidi (transport stop / playhead jump)
        //   • this track's midiInputIndex changed since last block (the
        //     user swapped MIDI controllers - held notes from the old
        //     device would otherwise hang on the synth forever).
        const int currentMidiIdx = session.track (t).midiInputIndex.load (
                                       std::memory_order_relaxed);
        const bool midiInputSwapped = (currentMidiIdx != lastMidiInputIndex[(size_t) t]);
        lastMidiInputIndex[(size_t) t] = currentMidiIdx;
        const bool perTrackFlush = flushHangingMidi || midiInputSwapped;

        perTrackMidiScratch.clear();
        if (midiTrack && perTrackFlush)
        {
            for (int ch = 1; ch <= 16; ++ch)
            {
                perTrackMidiScratch.addEvent (
                    juce::MidiMessage::controllerEvent (ch, 64,  0), 0);
                perTrackMidiScratch.addEvent (
                    juce::MidiMessage::controllerEvent (ch, 123, 0), 0);
            }
        }

        // Build this block's per-track MIDI buffer. Two source paths,
        // mutually exclusive (matches the audio source decision above):
        //   • Disk playback (willReadFromDisk): walk the track's
        //     midiRegions and emit any note/CC events whose absolute
        //     sample-position falls inside this block. This is the
        //     scheduled-playback path - the synth hears notes that were
        //     previously recorded onto the timeline.
        //   • Live monitoring (else): pull from the input MIDI collector
        //     and apply the per-track channel filter, just like before.
        // The strip's instrument plugin then sees one unified buffer.
        // Note: scratch already cleared above by the flush block, plus any
        // emitted All Notes Off events. Both source paths add to those.
        if (willReadFromDisk)
        {
            const float bpm = session.tempoBpm.load (std::memory_order_relaxed);
            const double sr = currentSampleRate.load (std::memory_order_relaxed);
            // Plugin latency comp for instrument tracks: shift the scheduling
            // window forward by the plugin's reported latency so the
            // delayed audio output aligns to the correct timeline sample.
            // 0 latency = no shift (the common case for synths). Audio
            // tracks have no instrument plugin so latency is 0 even when
            // the slot is loaded with an effect.
            const juce::int64 pluginLatency = midiTrack
                ? (juce::int64) strips[(size_t) t].getPluginSlot().getLatencySamples()
                : 0;
            const auto schedStart = blockStartSamples + pluginLatency;
            const auto blockEnd   = schedStart + numSamples;

            // Acquire-load the track's MIDI region snapshot once for the
            // block. Mutated on the message thread by RecordManager (when
            // a take finishes) and SessionSerializer (load); the snapshot
            // pointer is stable for the rest of this callback.
            // AtomicSnapshot's default ctor publishes an empty vector at
            // construction time so this pointer is non-null.
            const auto& midiRegionsForBlock = *session.track (t).midiRegions.read();

            // Chase pass: when the playhead jumps INTO the middle of a
            // sustained note (transport start after seek, loop wrap, etc.)
            // the synth would otherwise sit silent until the Note Off fires
            // since the Note On is in the past. Emit Note On at sample 1
            // (one sample after the All Notes Off the flush block already
            // emitted at sample 0, so the synth doesn't immediately silence
            // the chase) for every note whose on-time is before blockStart
            // and off-time is after blockStart.
            if (flushHangingMidi)
            {
                for (const auto& region : midiRegionsForBlock)
                {
                    if (region.muted) continue;
                    const auto regStart = region.timelineStart;
                    for (const auto& n : region.notes)
                    {
                        const auto onAbs  = regStart + ticksToSamples (n.startTick, sr, bpm);
                        const auto offAbs = onAbs + ticksToSamples (n.lengthInTicks, sr, bpm);
                        if (onAbs < blockStartSamples && offAbs > blockStartSamples)
                        {
                            perTrackMidiScratch.addEvent (
                                juce::MidiMessage::noteOn (n.channel, n.noteNumber,
                                                            (juce::uint8) n.velocity),
                                1);
                        }
                    }
                }
            }

            for (const auto& region : midiRegionsForBlock)
            {
                if (region.muted) continue;
                const auto regStart = region.timelineStart;
                const auto regEnd   = regStart + region.lengthInSamples;
                // Skip regions that don't overlap the SHIFTED block at all.
                // schedStart/blockEnd already include the plugin-latency
                // offset for MIDI tracks; on audio tracks pluginLatency=0
                // so this collapses to the original [blockStart, blockEnd)
                // window.
                if (regEnd <= schedStart || regStart >= blockEnd) continue;

                for (const auto& n : region.notes)
                {
                    const auto onAbs  = regStart + ticksToSamples (n.startTick, sr, bpm);
                    const auto offAbs = onAbs + ticksToSamples (n.lengthInTicks, sr, bpm);
                    if (onAbs >= schedStart && onAbs < blockEnd)
                    {
                        perTrackMidiScratch.addEvent (
                            juce::MidiMessage::noteOn (n.channel, n.noteNumber,
                                                        (juce::uint8) n.velocity),
                            (int) (onAbs - schedStart));
                    }
                    if (offAbs >= schedStart && offAbs < blockEnd)
                    {
                        perTrackMidiScratch.addEvent (
                            juce::MidiMessage::noteOff (n.channel, n.noteNumber),
                            (int) (offAbs - schedStart));
                    }
                }
                for (const auto& c : region.ccs)
                {
                    const auto sAbs = regStart + ticksToSamples (c.atTick, sr, bpm);
                    if (sAbs >= schedStart && sAbs < blockEnd)
                    {
                        perTrackMidiScratch.addEvent (
                            juce::MidiMessage::controllerEvent (c.channel,
                                                                  c.controller,
                                                                  c.value),
                            (int) (sAbs - schedStart));
                    }
                }
            }
            if (! perTrackMidiScratch.isEmpty())
                session.track (t).midiActivity.store (true, std::memory_order_relaxed);
        }
        else if (midiTrack)
        {
            // Live monitoring path - only meaningful on MIDI tracks; effect
            // inserts on Mono / Stereo strips don't consume per-track MIDI
            // (their pluginMidiScratch is built fresh inside the strip).
            // Gating here skips a per-block allocation+copy on every audio
            // track that has a midiInputIndex set.
            const int midiIdx = session.track (t).midiInputIndex.load (std::memory_order_relaxed);
            if (midiIdx >= 0 && midiIdx < (int) perInputMidi.size()
                && ! perInputMidi[(size_t) midiIdx].isEmpty())
            {
                const int chFilter = session.track (t)
                                        .midiChannel.load (std::memory_order_relaxed);
                for (const auto meta : perInputMidi[(size_t) midiIdx])
                {
                    const auto m = meta.getMessage();
                    if (chFilter == 0 || m.getChannel() == chFilter)
                        perTrackMidiScratch.addEvent (m, meta.samplePosition);
                }
                if (! perTrackMidiScratch.isEmpty())
                    session.track (t).midiActivity.store (true, std::memory_order_relaxed);
            }
        }

        // Recording capture has to happen BEFORE the strip's instrument
        // plugin sees the buffer. AudioPluginInstance::processBlock is
        // allowed (and most VST3 instruments do) to consume / replace the
        // MIDI buffer in place, so by the time processAndAccumulate
        // returns, perTrackMidiScratch is typically empty. Writing here
        // captures the events the user actually played.
        if (isRecording && armed && midiTrack && ! perTrackMidiScratch.isEmpty())
        {
            const auto recStart = activeRecordStart.load (std::memory_order_relaxed);
            const auto blockOffsetFromRecord = blockStartSamples - recStart;
            recordManager.writeMidiBlock (t, perTrackMidiScratch, blockOffsetFromRecord);
        }

        // External MIDI output. When this MIDI track has a hardware port
        // selected, mirror the just-built per-track buffer to that port
        // so an external synth/drum machine receives the same notes the
        // loaded instrument plugin (if any) does. JUCE's MidiOutput
        // delivers via its own background thread (started in
        // rebuildMidiOutputBank), so the audio thread just enqueues.
        // Empty buffer skipped to avoid pointless wakeups on the
        // delivery thread.
        if (midiTrack && ! perTrackMidiScratch.isEmpty())
        {
            const int outIdx = session.track (t).midiOutputIndex.load (
                                  std::memory_order_relaxed);
            if (outIdx >= 0 && outIdx < (int) midiOutputs.size())
            {
                if (auto* out = midiOutputs[(size_t) outIdx].get())
                {
                    // sendBlockOfMessages takes an absolute "ms-since-epoch"
                    // start time; pass juce::Time::getMillisecondCounterHiRes()
                    // so events fire as close to "now + sampleOffset" as
                    // the OS scheduler allows. The samples-per-second
                    // arg lets the delivery thread map sample offsets to
                    // wall-clock deltas.
                    const double sendRate = currentSampleRate.load (std::memory_order_relaxed);
                    out->sendBlockOfMessages (perTrackMidiScratch,
                                                juce::Time::getMillisecondCounterHiRes(),
                                                sendRate > 0.0 ? sendRate : 48000.0);
                }
            }
        }

        // Tell the strip whether the recorder is going to ask for the
        // post-effects buffer this block - if so, the strip MUST run its
        // DSP even when it's not passing to master. Otherwise we let it
        // skip the heavy pass to save CPU on silent tracks.
        const bool needPrintBuffer = isRecording && armed && deviceInput != nullptr
                                  && session.track (t).printEffects.load (std::memory_order_relaxed);
        strips[(size_t) t].setNeedsProcessedMono (needPrintBuffer);

        // Stereo input source for stereo tracks. Two paths:
        //   • Disk playback: PlaybackEngine wrote the R channel into
        //     playbackScratchR above (when stereoTrackInput is true), so
        //     point monoInR at that buffer.
        //   • Live input: source from the user's R input mapping.
        const float* monoInR = nullptr;
        if (stereoTrackInput && monoIn != nullptr)
        {
            if (willReadFromDisk)
            {
                monoInR = playbackScratchR.data();
            }
            else if (monoIn == silentInputScratch.data())
            {
                // Generator-style insert on a stereo track: feed a silent
                // R buffer so ChannelStrip enters the stereo path and the
                // donor plugin's stereo output is preserved end-to-end.
                monoInR = silentInputScratch.data();
            }
            else
            {
                const int rIdxStrip = session.resolveInputRForTrack (t);
                if (rIdxStrip >= 0 && rIdxStrip < numInputChannels)
                    monoInR = inputChannelData[(size_t) rIdxStrip];
            }
        }

        // MIDI tracks have no audio input - their gate is just mute/solo.
        // Audio tracks still require a non-null source to pass.
        const bool stripPasses = midiTrack ? passes : (passes && monoIn != nullptr);
        strips[(size_t) t].processAndAccumulate (monoIn, monoInR,
                                                 perTrackMidiScratch, midiTrack,
                                                 mixL.data(), mixR.data(),
                                                 busLPtrs, busRPtrs,
                                                 auxLanePtrsL, auxLanePtrsR,
                                                 numSamples,
                                                 stripPasses,
                                                 currentDeviceInputs,
                                                 numCurrentDeviceInputs,
                                                 currentDeviceOutputs,
                                                 numCurrentDeviceOutputs);
        session.track (t).meterGrDb.store (strips[(size_t) t].getCurrentGrDb(),
                                            std::memory_order_relaxed);

        // Output peak for MIDI tracks: the strip's "input" meter only
        // measures the audio source feeding the strip, which is null on
        // a MIDI track since the audio is generated INSIDE the strip by
        // the instrument plugin. Without this branch the strip meter
        // stays flat even while the synth is audible at master. Write
        // the post-DSP peak (same buffer the recorder uses for printed-
        // FX captures) into the input-meter atom so the UI's existing
        // poll renders a real level for MIDI tracks.
        if (midiTrack)
        {
            const int n = strips[(size_t) t].getLastProcessedSamples();
            if (auto* lp = strips[(size_t) t].getLastProcessedMono(); lp != nullptr && n > 0)
            {
                const auto rng = juce::FloatVectorOperations::findMinAndMax (lp, n);
                const float pk = juce::jmax (std::abs (rng.getStart()),
                                              std::abs (rng.getEnd()));
                session.track (t).meterInputDb.store (
                    pk > 1e-5f ? juce::Decibels::gainToDecibels (pk, -100.0f) : -100.0f,
                    std::memory_order_relaxed);
            }
            if (auto* rp = strips[(size_t) t].getLastProcessedR(); rp != nullptr && n > 0)
            {
                const auto rng = juce::FloatVectorOperations::findMinAndMax (rp, n);
                const float pk = juce::jmax (std::abs (rng.getStart()),
                                              std::abs (rng.getEnd()));
                session.track (t).meterInputRDb.store (
                    pk > 1e-5f ? juce::Decibels::gainToDecibels (pk, -100.0f) : -100.0f,
                    std::memory_order_relaxed);
            }
        }

        // Recording capture - armed tracks always commit their input to
        // disk while recording. By default the raw deviceInput is written;
        // when `printEffects` is engaged, the post-EQ/post-comp buffer that
        // the strip just produced is written instead, "printing" the
        // channel-strip processing to the WAV.
        //
        // Punch: when transport.isPunchEnabled(), only samples whose timeline
        // position lies inside [punchIn, punchOut) are committed. The block
        // is sliced to the intersection of [blockStart, blockEnd) and the
        // punch window; outside samples are silently discarded (NOT written
        // as zeros, so the WAV's frame count equals the punch window length).
        if (isRecording && armed && deviceInput != nullptr)
        {
            const bool printEfx = session.track (t).printEffects.load (std::memory_order_relaxed);

            // Default sources: deviceInput on L; deviceInputR on R for stereo
            // tracks (resolved earlier as monoInR). printEffects swaps both
            // L and R to the strip's post-effect buffers when available.
            const float* recL = deviceInput;
            const float* recR = stereoTrackInput ? monoInR : nullptr;
            if (printEfx)
            {
                auto& strip = strips[(size_t) t];
                if (auto* processed = strip.getLastProcessedMono();
                    processed != nullptr
                    && strip.getLastProcessedSamples() >= numSamples)
                {
                    recL = processed;
                    if (stereoTrackInput)
                        recR = strip.getLastProcessedR();  // may be nullptr → recorder duplicates L
                }
            }

            // Unified write-gate - honours BOTH:
            //   • activeRecordStart  (count-in pre-roll: skip writes until
            //     the playhead reaches the take's intended start)
            //   • punch-in / punch-out  (only commit samples in the window
            //     when punch is on)
            // Both reduce to a single [from, to) intersection with the
            // current block.
            const auto recStart  = activeRecordStart.load (std::memory_order_relaxed);
            juce::int64 effIn    = recStart;  // floor; never write before this
            juce::int64 effOut   = std::numeric_limits<juce::int64>::max();
            if (transport.isPunchEnabled())
            {
                const auto pIn  = transport.getPunchIn();
                const auto pOut = transport.getPunchOut();
                if (pOut > pIn)
                {
                    effIn  = juce::jmax (effIn, pIn);
                    effOut = pOut;
                }
                else
                {
                    effIn = effOut;  // empty/inverted punch window → no capture
                }
            }

            const auto blockEnd = blockStartSamples + numSamples;
            const auto sliceStart = juce::jmax (blockStartSamples, effIn);
            const auto sliceEnd   = juce::jmin (blockEnd,        effOut);
            if (sliceEnd > sliceStart)
            {
                const int writeOffset = (int) (sliceStart - blockStartSamples);
                const int writeLength = (int) (sliceEnd - sliceStart);
                const float* writeL = recL + writeOffset;
                const float* writeR = (recR != nullptr) ? recR + writeOffset : nullptr;
                recordManager.writeInputBlock (t, writeL, writeR, writeLength);
            }
        }

    }

    for (int a = 0; a < Session::kNumBuses; ++a)
    {
        const auto& params = session.bus (a).strip;
        const bool muted   = params.mute.load (std::memory_order_relaxed);
        const bool soloed  = params.solo.load (std::memory_order_relaxed);
        const bool passes  = ! muted && (anyBusSolo ? soloed : true);

        if (! passes) continue;

        // Skip aux DSP entirely when the bus buffer is silent - no channel
        // routed to it AND no smoothing tail from a recently-unassigned
        // channel. UniversalCompressor (Bus mode) is one of the heaviest
        // per-block ops in the engine; skipping the whole EQ + comp pass on
        // an idle aux is a big saving when only 1-2 of the 4 buses are in
        // use. Aux's own fader/pan smoothers don't tick during the skip,
        // but they pick up correctly from their last-known current value
        // when audio resumes, so no click on re-engage.
        {
            const auto rngL = juce::FloatVectorOperations::findMinAndMax (
                                  busL[(size_t) a].data(), numSamples);
            const auto rngR = juce::FloatVectorOperations::findMinAndMax (
                                  busR[(size_t) a].data(), numSamples);
            const float peak = juce::jmax (
                juce::jmax (std::abs (rngL.getStart()), std::abs (rngL.getEnd())),
                juce::jmax (std::abs (rngR.getStart()), std::abs (rngR.getEnd())));
            if (peak <= 1e-6f)
            {
                // Bus pass is being skipped (no audio routed). Reset the
                // post-bus meter to silence so the UI doesn't keep showing
                // the last-played level — otherwise the LED freezes at
                // whatever value the meter held right before the skip
                // engaged.
                params.meterPostBusLDb.store (-100.0f, std::memory_order_relaxed);
                params.meterPostBusRDb.store (-100.0f, std::memory_order_relaxed);
                params.meterPostBusRmsL.store (0.0f, std::memory_order_relaxed);
                params.meterPostBusRmsR.store (0.0f, std::memory_order_relaxed);
                continue;
            }
        }

        busStrips[(size_t) a].processInPlace (busL[(size_t) a].data(),
                                              busR[(size_t) a].data(),
                                              numSamples);
        // SIMD'd mix accumulate - hot inner loop, runs once per active aux
        // per callback. JUCE picks the right SSE/NEON path based on the
        // platform; cheaper than scalar [i]+= even with -O3.
        juce::FloatVectorOperations::add (mixL.data(),
                                            busL[(size_t) a].data(),
                                            numSamples);
        juce::FloatVectorOperations::add (mixR.data(),
                                            busR[(size_t) a].data(),
                                            numSamples);
    }

    // AUX return lanes - process each lane's accumulated send buffer
    // through its plugin chain, then sum the wet output into master. Same
    // silence-skip optimisation as the bus pass above so idle lanes (no
    // channel sending to them) don't run their plugins.
    for (int a = 0; a < Session::kNumAuxLanes; ++a)
    {
        {
            const auto rngL = juce::FloatVectorOperations::findMinAndMax (
                                  auxLaneL[(size_t) a].data(), numSamples);
            const auto rngR = juce::FloatVectorOperations::findMinAndMax (
                                  auxLaneR[(size_t) a].data(), numSamples);
            const float peak = juce::jmax (
                juce::jmax (std::abs (rngL.getStart()), std::abs (rngL.getEnd())),
                juce::jmax (std::abs (rngR.getStart()), std::abs (rngR.getEnd())));
            if (peak <= 1e-6f)
            {
                // Reset the aux-lane meter on skip — same rationale as the
                // bus-pass skip above. Without this the lane LED freezes
                // at the last-played level.
                auto& auxLaneRef = session.auxLane (a);
                auxLaneRef.params.meterPostL.store (-100.0f, std::memory_order_relaxed);
                auxLaneRef.params.meterPostR.store (-100.0f, std::memory_order_relaxed);
                continue;
            }
        }

        auxLaneStrips[(size_t) a].processStereoBlock (auxLaneL[(size_t) a].data(),
                                                        auxLaneR[(size_t) a].data(),
                                                        numSamples,
                                                        currentDeviceInputs,
                                                        numCurrentDeviceInputs,
                                                        currentDeviceOutputs,
                                                        numCurrentDeviceOutputs);
        juce::FloatVectorOperations::add (mixL.data(),
                                            auxLaneL[(size_t) a].data(),
                                            numSamples);
        juce::FloatVectorOperations::add (mixR.data(),
                                            auxLaneR[(size_t) a].data(),
                                            numSamples);
    }

    master.processInPlace (mixL.data(), mixR.data(), numSamples);

    // Metronome - push session BPM + enable into the click generator each
    // block (cheap atomic loads), then mix the click into the post-master
    // bus. Click is post-master so it never gets EQ'd or compressed by
    // the master strip - it's purely a monitoring aid.
    metronome.setEnabled    (session.metronomeEnabled.load (std::memory_order_relaxed));
    metronome.setBpm        (session.tempoBpm.load (std::memory_order_relaxed));
    metronome.setBeatsPerBar(session.beatsPerBar.load (std::memory_order_relaxed));
    metronome.setVolumeDb   (session.metronomeVolDb.load (std::memory_order_relaxed));
    {
        // Force the click on during count-in pre-roll so the user always
        // gets the count-in even if they haven't engaged CLICK manually.
        const auto recStart   = activeRecordStart.load (std::memory_order_relaxed);
        const bool inCountIn  = isRecording && blockStartSamples < recStart;
        metronome.process (blockStartSamples, isPlaying || isRecording,
                            mixL.data(), mixR.data(), numSamples,
                            /*forceEnable*/ inCountIn);
    }

    if (numOutputChannels >= 1 && outputChannelData[0] != nullptr)
        std::memcpy (outputChannelData[0], mixL.data(), sizeof (float) * (size_t) numSamples);
    if (numOutputChannels >= 2 && outputChannelData[1] != nullptr)
        std::memcpy (outputChannelData[1], mixR.data(), sizeof (float) * (size_t) numSamples);

    if (isPlaying || isRecording)
    {
        transport.advancePlayhead (numSamples);

        // Loop wrap-around. Only honoured during plain playback - during
        // Recording we keep the playhead linear so the captured WAV maps
        // cleanly onto the timeline (loop-take-stacking is a future
        // feature). Wrap is whole-block accurate: we do not split the
        // current block, so the playhead may briefly read up to one block
        // past loopEnd before snapping back. That overshoot is silent
        // because PlaybackEngine returns silence outside region bounds.
        if (isPlaying && ! isRecording && transport.isLoopEnabled())
        {
            const auto lStart = transport.getLoopStart();
            const auto lEnd   = transport.getLoopEnd();
            if (lEnd > lStart)
            {
                const auto curr = transport.getPlayhead();
                if (curr >= lEnd)
                {
                    const auto loopLen   = lEnd - lStart;
                    const auto overshoot = (curr - lEnd) % loopLen;
                    transport.setPlayhead (lStart + overshoot);
                }
            }
        }
    }

    // Detect xrun: callback work shouldn't exceed the buffer's wall-clock
    // budget. If it does, we'd glitch on the next callback. Track the count
    // for the status bar. Same pass also updates the smoothed CPU usage
    // (callback wall-time / buffer audio-time, one-pole LPF) which the
    // status bar polls.
    const auto sr = currentSampleRate.load (std::memory_order_relaxed);
    if (sr > 0.0)
    {
        const double bufferMs = 1000.0 * (double) numSamples / sr;
        const double elapsedMs = (double) (juce::Time::getHighResolutionTicks() - callbackStart)
                                     * secondsPerTick * 1000.0;
        if (elapsedMs > bufferMs)
            xrunCount.fetch_add (1, std::memory_order_relaxed);

        if (bufferMs > 0.0)
        {
            const float instant = (float) juce::jlimit (0.0, 2.0, elapsedMs / bufferMs);
            // 0.2 coefficient -> ~5-block smoothing; fast enough that the
            // user sees real spikes, slow enough to mask single-block jitter.
            const float prev = cpuUsage.load (std::memory_order_relaxed);
            cpuUsage.store (prev + 0.2f * (instant - prev), std::memory_order_relaxed);
        }
    }
}
} // namespace duskstudio
