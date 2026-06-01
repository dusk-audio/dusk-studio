// Screenshot-capture harness for the manual. Activated by
// DUSKSTUDIO_CAPTURE_DIR (see MainComponent ctor). Synthesises a small demo
// session, drives each documented stage / strip / modal, writes one PNG per
// figure into the output directory, then quits the app.
//
// This is a developer/docs tool, not part of the shipping signal path. It runs
// once, on the message thread, with the transport stopped — so directly writing
// Session region vectors (normally only touched at load) is safe here because
// the audio thread never reads them without a play/prepare cycle.

#include "MainComponent.h"

#include "ConsoleView.h"
#include "TransportBar.h"
#include "TapeStrip.h"
#include "AuxView.h"
#include "MasteringView.h"
#include "AudioRegionEditor.h"
#include "PianoRollComponent.h"
#include "ChannelEqEditor.h"
#include "ChannelCompEditor.h"
#include "AudioSettingsPanel.h"
#include "MidiBindingsPanel.h"
#include "HardwareInsertEditor.h"
#include "StartupDialog.h"
#include "PluginPickerPanel.h"
#include "BounceDialog.h"
#include "../engine/BounceEngine.h"

#include "../session/Session.h"
#include "../engine/AudioEngine.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace duskstudio
{
namespace
{
void writePng (const juce::Image& img, const juce::File& f)
{
    if (! img.isValid()) return;
    f.deleteFile();
    if (auto os = std::unique_ptr<juce::FileOutputStream> (f.createOutputStream()))
    {
        juce::PNGImageFormat png;
        png.writeImageToStream (img, *os);
        std::fprintf (stderr, "[Dusk Studio/capture] wrote %s\n",
                      f.getFileName().toRawUTF8());
    }
}

// Give async work (audio-thumbnail loads on their own thread, layout) time to
// settle before the snapshot. We're already inside a message-thread callback,
// so we can't re-enter the dispatch loop; sleeping lets background threads
// finish and createComponentSnapshot then paints the current state.
void settle (int ms)
{
    juce::Thread::sleep (ms);
}

void snapshotComponent (juce::Component* c, const juce::File& outDir,
                        const juce::String& name, int settleMs = 200)
{
    if (c == nullptr || c->getWidth() <= 0 || c->getHeight() <= 0) return;
    settle (settleMs);
    auto img = c->createComponentSnapshot (c->getLocalBounds(), true);
    writePng (img, outDir.getChildFile (name));
}

// Generate a short stereo WAV so audio regions have a real file for the
// thumbnail to load. Two detuned sines with an amplitude envelope read as a
// plausible recorded take.
juce::File writeDemoWav (const juce::File& dir, double sampleRate)
{
    dir.createDirectory();
    auto file = dir.getChildFile ("demo-take.wav");

    const int numCh     = 2;
    const int numFrames = (int) (sampleRate * 1.8);
    juce::AudioBuffer<float> buf (numCh, numFrames);
    for (int n = 0; n < numFrames; ++n)
    {
        const double t   = (double) n / sampleRate;
        const double env = std::sin (juce::MathConstants<double>::pi * (double) n / numFrames);
        const float  l   = (float) (env * 0.6 * std::sin (2.0 * juce::MathConstants<double>::pi * 196.0 * t));
        const float  r   = (float) (env * 0.6 * std::sin (2.0 * juce::MathConstants<double>::pi * 198.0 * t));
        buf.setSample (0, n, l);
        buf.setSample (1, n, r);
    }

    juce::WavAudioFormat wav;
    file.deleteFile();
    if (auto os = std::unique_ptr<juce::FileOutputStream> (file.createOutputStream()))
    {
        if (auto* writer = wav.createWriterFor (os.get(), sampleRate, (unsigned int) numCh,
                                                24, {}, 0))
        {
            os.release();   // writer owns the stream now
            writer->writeFromAudioSampleBuffer (buf, 0, numFrames);
            delete writer;
        }
    }
    return file;
}
} // namespace

void MainComponent::captureScreenshots (const juce::File& outDir)
{
    static bool ran = false;
    if (ran) return;
    ran = true;

    outDir.createDirectory();
    std::fprintf (stderr, "[Dusk Studio/capture] -> %s\n", outDir.getFullPathName().toRawUTF8());

    const double sr = engine.getCurrentSampleRate() > 0 ? engine.getCurrentSampleRate() : 48000.0;

    // ── Synthesise demo content ─────────────────────────────────────────
    const char* names[8] = { "Kick", "Snare", "Bass", "Gtr L", "Gtr R", "Keys", "Vox", "Room" };
    for (int t = 0; t < 8; ++t)
    {
        auto& tr = session.track (t);
        tr.name = names[t];
        tr.recordArmed.store (true, std::memory_order_relaxed);
    }

    // Audio regions on tracks 0 and 1 (bank 0, visible on the tape strip).
    const auto wav = writeDemoWav (outDir.getChildFile ("_demo"), sr);
    const juce::int64 lenSamples = (juce::int64) (sr * 1.8);
    auto makeRegion = [&] (juce::int64 start) {
        AudioRegion r;
        r.file            = wav;
        r.timelineStart   = start;
        r.lengthInSamples = lenSamples;
        r.numChannels     = 2;
        r.fadeInSamples   = (juce::int64) (sr * 0.05);
        r.fadeOutSamples  = (juce::int64) (sr * 0.15);
        r.fadeInShape     = FadeShape::EqualPower;
        r.fadeOutShape    = FadeShape::Exp;
        return r;
    };
    session.track (0).regions = { makeRegion (0), makeRegion ((juce::int64) (sr * 2.4)) };
    session.track (1).regions = { makeRegion ((juce::int64) (sr * 0.6)) };

    // MIDI region with a short riff on track 8 (for the piano roll).
    session.track (8).name = "Synth";
    session.track (8).mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
    {
        MidiRegion m;
        m.timelineStart = 0;
        m.lengthInTicks = 4 * 480;
        m.lengthInSamples = (juce::int64) (sr * 2.0);
        const int pitches[8] = { 60, 64, 67, 72, 71, 67, 64, 60 };
        for (int i = 0; i < 8; ++i)
        {
            MidiNote n;
            n.noteNumber    = pitches[i];
            n.velocity      = 88 + (i % 3) * 12;
            n.startTick     = i * 240;
            n.lengthInTicks = 220;
            m.notes.push_back (n);
        }
        session.track (8).midiRegions.mutate ([&] (std::vector<MidiRegion>& v)
        {
            v.clear();
            v.push_back (m);
        });
    }

    session.addMarker ((juce::int64) (sr * 1.2), "Verse");
    session.addMarker ((juce::int64) (sr * 3.0), "Chorus");
    session.track (0).strip.busAssign[0].store (true, std::memory_order_relaxed);

    if (consoleView != nullptr)
        consoleView->setBank (0);

    // ── RECORDING stage ─────────────────────────────────────────────────
    switchToStage (AudioEngine::Stage::Recording);

    // Tape-strip figure: expand the timeline. Keep strips FULL (non-compact)
    // so EQ / COMP sections aren't collapsed to buttons.
    tapeStripExpanded = true;
    if (tapeStrip != nullptr) tapeStrip->setVisible (true);
    if (consoleView != nullptr) consoleView->setStripsCompactMode (false);
    resized();
    settle (400);
    snapshotComponent (tapeStrip.get(), outDir, "np-09-tape-strip.png");

    // Console figures: collapse the timeline so the full-height strips (with
    // inline EQ + COMP visible) own the window.
    tapeStripExpanded = false;
    if (tapeStrip != nullptr) tapeStrip->setVisible (false);
    if (consoleView != nullptr) consoleView->setStripsCompactMode (false);
    resized();
    settle (400);

    snapshotComponent (this, outDir, "np-01-main-window.png");
    snapshotComponent (this, outDir, "rec-01-arm-multiple.png");
    snapshotComponent (transportBar.get(), outDir, "np-02-transport-bar.png");
    if (consoleView != nullptr)
    {
        snapshotComponent (consoleView->getStripComponent (0), outDir, "np-04-channel-strip-recording.png");
        snapshotComponent (consoleView->getStripComponent (0), outDir, "qg-03-arm-track.png");
    }

    // Lit input meters → "record rolling" / "overdub".
    for (int t = 0; t < 8; ++t)
    {
        auto& tr = session.track (t);
        tr.meterInputDb .store (-7.0f - (float) (t % 4) * 2.0f, std::memory_order_relaxed);
        tr.meterInputRDb.store (-8.0f - (float) (t % 4) * 2.0f, std::memory_order_relaxed);
        tr.inputMonitor.store (true, std::memory_order_relaxed);
    }
    resized();
    snapshotComponent (this, outDir, "qg-04-record-rolling.png", 120);
    snapshotComponent (this, outDir, "qg-05-overdub.png", 60);

    // ── MIXING stage ────────────────────────────────────────────────────
    switchToStage (AudioEngine::Stage::Mixing);
    if (consoleView != nullptr) consoleView->setStripsMixingMode (true);
    resized();
    settle (300);
    snapshotComponent (this, outDir, "qg-06-mixing-stage.png");
    if (consoleView != nullptr)
    {
        snapshotComponent (consoleView->getStripComponent (0), outDir, "np-03-channel-strip-mixing.png");
        snapshotComponent (consoleView->getBusComponent (0),   outDir, "np-05-bus-strip.png");
        snapshotComponent (consoleView->getMasterStripComponent(), outDir, "np-06-master-strip.png");

        // Automation-mode label in WRITE.
        session.track (0).automationMode.store ((int) AutomationMode::Write, std::memory_order_relaxed);
        resized();
        snapshotComponent (consoleView->getStripComponent (0), outDir, "mm-01-automation-modes.png", 120);
        session.track (0).automationMode.store ((int) AutomationMode::Off, std::memory_order_relaxed);
    }

    // ── AUX stage ───────────────────────────────────────────────────────
    switchToStage (AudioEngine::Stage::Aux);
    resized();
    settle (300);
    snapshotComponent (auxView.get(), outDir, "np-07-aux-view.png");

    // ── MASTERING stage ─────────────────────────────────────────────────
    switchToStage (AudioEngine::Stage::Mastering);
    resized();
    settle (300);
    snapshotComponent (masteringView.get(), outDir, "np-08-mastering-view.png");
    snapshotComponent (masteringView.get(), outDir, "mm-02-mastering-chain.png");

    // Back to a normal stage before modal shots.
    switchToStage (AudioEngine::Stage::Mixing);
    resized();
    settle (200);

    // ── Modal panels (standalone, snapshot directly) ────────────────────
    auto modalShot = [&] (juce::Component& m, int w, int h, const juce::String& name, int settleMs)
    {
        addAndMakeVisible (m);
        m.setBounds ((juce::jmax (w, getWidth())  - w) / 2,
                     (juce::jmax (h, getHeight()) - h) / 2, w, h);
        snapshotComponent (&m, outDir, name, settleMs);
        removeChildComponent (&m);
    };

    {
        AudioRegionEditor ed (session, engine, 0, 0);
        modalShot (ed, 1000, 640, "np-10-region-editor.png", 500);
        // Re-snapshot under the second documented name (same figure).
        addAndMakeVisible (ed);
        ed.setBounds ((juce::jmax (1000, getWidth()) - 1000) / 2,
                      (juce::jmax (640, getHeight()) - 640) / 2, 1000, 640);
        snapshotComponent (&ed, outDir, "ed-04-region-editor-modal.png", 300);
        removeChildComponent (&ed);
    }
    {
        PianoRollComponent pr (session, engine, 8, 0);
        modalShot (pr, 1100, 680, "np-11-piano-roll.png", 500);
        addAndMakeVisible (pr);
        pr.setBounds ((juce::jmax (1100, getWidth()) - 1100) / 2,
                      (juce::jmax (680, getHeight()) - 680) / 2, 1100, 680);
        snapshotComponent (&pr, outDir, "ed-05-piano-roll-full.png", 300);
        removeChildComponent (&pr);
    }
    {
        AudioSettingsPanel p (engine.getDeviceManager(), engine, session);
        modalShot (p, 720, 560, "qg-02-audio-settings.png", 400);
    }
    {
        MidiBindingsPanel p (session, engine, [] {});
        modalShot (p, MidiBindingsPanel::kPanelW, MidiBindingsPanel::kPanelH, "sync-01-mcu-bindings.png", 300);
    }
    {
        HardwareInsertEditor p (session.track (0).hardwareInsert, engine.getDeviceManager(), [] {}, true);
        modalShot (p, 480, 520, "pl-04-hw-insert.png", 300);
    }
    {
        // Channel EQ editor — the 4-band EQ the strip's EQ button opens. Give
        // it a few non-flat bands so the curve reads.
        auto& s = session.track (0).strip;
        s.eqEnabled.store (true, std::memory_order_relaxed);
        s.lfGainDb.store (3.0f, std::memory_order_relaxed);
        s.lmGainDb.store (-4.0f, std::memory_order_relaxed);
        s.hmGainDb.store (2.5f, std::memory_order_relaxed);
        s.hfGainDb.store (4.0f, std::memory_order_relaxed);
        ChannelEqEditor eq (session.track (0));
        const int w = eq.getWidth()  > 0 ? eq.getWidth()  : 560;
        const int h = eq.getHeight() > 0 ? eq.getHeight() : 360;
        modalShot (eq, w, h, "fx-01-eq.png", 300);
    }
    {
        // Channel compressor editor (VCA mode, mid gain-reduction).
        auto& s = session.track (0).strip;
        s.compEnabled.store (true, std::memory_order_relaxed);
        s.compMode.store (2, std::memory_order_relaxed);   // VCA
        ChannelCompEditor cp (session.track (0));
        const int w = cp.getWidth()  > 0 ? cp.getWidth()  : 380;
        const int h = cp.getHeight() > 0 ? cp.getHeight() : 360;
        modalShot (cp, w, h, "fx-02-comp.png", 300);
    }
    {
        // Startup dialog — full-bleed, so size it to the window.
        juce::Array<juce::File> recents {
            juce::File ("~/Music/Dusk Studio/Album Demo").getFullPathName(),
            juce::File ("~/Music/Dusk Studio/Live Take 3").getFullPathName(),
            juce::File ("~/Music/Dusk Studio/Vocal Comp").getFullPathName()
        };
        StartupDialog sd (recents);
        modalShot (sd, getWidth(), getHeight(), "qg-01-startup.png", 300);
    }
    {
        // Plugin picker with a synthetic effect list (no real scan in capture mode).
        auto mk = [] (const char* n, const char* mfr, const char* cat)
        {
            juce::PluginDescription d;
            d.name = n; d.manufacturerName = mfr; d.category = cat;
            d.pluginFormatName = "VST3"; d.version = "1.0";
            return d;
        };
        juce::Array<juce::PluginDescription> descs {
            mk ("Ambience",        "Smartelectronix", "Reverb"),
            mk ("TAL-Chorus-LX",   "TAL",             "Modulation"),
            mk ("Dragonfly Hall",  "Michael Willis",  "Reverb"),
            mk ("Calf Vintage Delay", "Calf",         "Delay"),
            mk ("ZamComp",         "Zam Audio",       "Dynamics"),
            mk ("x42 Convolver",   "Robin Gareus",    "Reverb")
        };
        PluginPickerPanel::Callbacks cb;   // all null — display only
        PluginPickerPanel pp (descs, PluginPickerPanel::Kind::Effects, cb);
        modalShot (pp, 480, 560, "pl-01-plugin-picker.png", 300);
    }
    {
        // Bounce dialog (progress UI). Its ctor kicks an offline render to the
        // temp file; snapshot the progress panel immediately, then it cancels
        // on destruction. Done last so the offline-render device detach can't
        // disturb earlier snapshots.
        auto target = outDir.getChildFile ("_demo").getChildFile ("bounce.wav");
        BounceDialog bd (engine, session, engine.getDeviceManager(), target,
                         BounceEngine::Mode::MasterMix);
        modalShot (bd, 520, 200, "qg-07-bounce-dialog.png", 200);
        addAndMakeVisible (bd);
        bd.setBounds ((juce::jmax (520, getWidth()) - 520) / 2,
                      (juce::jmax (200, getHeight()) - 200) / 2, 520, 200);
        snapshotComponent (&bd, outDir, "bnc-01-bounce-dialog.png", 100);
        removeChildComponent (&bd);
    }

    std::fprintf (stderr, "[Dusk Studio/capture] done\n");
    juce::JUCEApplication::getInstance()->quit();
}
} // namespace duskstudio
