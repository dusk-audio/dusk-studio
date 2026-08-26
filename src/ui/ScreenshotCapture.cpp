// Screenshot-capture harness for the manual. Activated by
// DUSKSTUDIO_CAPTURE_DIR (see MainComponent ctor). Synthesises a small demo
// session, drives each documented stage / strip / modal, writes one PNG per
// figure into the output directory, then quits the app.
//
// This is a developer/docs tool, not part of the shipping signal path. It runs
// once, on the message thread, with the transport stopped - so directly writing
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
#include "TapePanel.h"
#include "MidiBindingsPanel.h"
#include "HardwareInsertEditor.h"
#include "PluginPickerPanel.h"
#include "BounceDialog.h"
#include "../engine/BounceEngine.h"
#include "../engine/audiofile/FileWriter.h"
#include "../foundation/PlanarBuffer.h"

#include "../session/Session.h"
#include "../engine/AudioEngine.h"

#include <algorithm>

namespace duskstudio
{
namespace
{
void writePng (const juce::Image& img, const juce::File& f)
{
    if (! img.isValid()) return;

    // Flatten onto an opaque dark backing before writing. Components captured
    // in isolation - the settings panel, dialogs, rounded-corner strips - don't
    // paint a fully opaque background: at runtime they sit over the dimmed dark
    // window, so their snapshot has transparent pixels. Written straight to PNG
    // those become WHITE wherever the manual / viewers composite on white, which
    // reads as an inverted/negative image. Compositing onto the app's window
    // colour makes those areas dark, matching the live UI. Opaque snapshots
    // (strips that fill their whole bounds) are unchanged.
    juce::Image flat (juce::Image::RGB, img.getWidth(), img.getHeight(), true);
    {
        juce::Graphics g (flat);
        g.fillAll (juce::Colour (0xff121214));
        g.drawImageAt (img, 0, 0);
    }

    f.deleteFile();
    if (auto os = std::unique_ptr<juce::FileOutputStream> (f.createOutputStream()))
    {
        juce::PNGImageFormat png;
        png.writeImageToStream (flat, *os);
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
    dusk::audio::PlanarBuffer buf;
    buf.setSize (numCh, numFrames);
    for (int n = 0; n < numFrames; ++n)
    {
        const double t   = (double) n / sampleRate;
        const double env = std::sin (juce::MathConstants<double>::pi * (double) n / numFrames);
        buf.channel (0)[n] = (float) (env * 0.6 * std::sin (2.0 * juce::MathConstants<double>::pi * 196.0 * t));
        buf.channel (1)[n] = (float) (env * 0.6 * std::sin (2.0 * juce::MathConstants<double>::pi * 198.0 * t));
    }

    file.deleteFile();
    dusk::audio::WriteSpec spec;
    spec.sampleRate = sampleRate;
    spec.numChannels = numCh;
    spec.bitsPerSample = 24;
    spec.format = dusk::audio::WriteSpec::Format::Wav;
    auto writer = dusk::audio::FileWriter::create (
        std::filesystem::u8path (file.getFullPathName().toStdString()), spec);
    if (writer == nullptr
        || ! writer->write (buf.data(), numCh, numFrames))
        return {};
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

    // Synthesise demo content
    const char* names[8] = { "Kick", "Snare", "Bass", "Gtr L", "Gtr R", "Keys", "Vox", "Room" };
    for (int t = 0; t < 8; ++t)
    {
        auto& tr = session.track (t);
        tr.name = names[t];
        tr.recordArmed.store (true, std::memory_order_relaxed);
    }

    // Audio regions on tracks 0 and 1 (bank 0, visible on the tape strip).
    const auto wav = writeDemoWav (outDir.getChildFile ("_demo"), sr);
    if (wav == juce::File())
    {
        std::fprintf (stderr, "[Dusk Studio/capture] demo WAV write failed - aborting capture\n");
        juce::JUCEApplication::getInstance()->quit();
        return;
    }
    const std::int64_t lenSamples = (std::int64_t) (sr * 1.8);
    auto makeRegion = [&] (std::int64_t start) {
        AudioRegion r;
        r.file            = wav;
        r.timelineStart   = start;
        r.lengthInSamples = lenSamples;
        r.numChannels     = 2;
        r.fadeInSamples   = (std::int64_t) (sr * 0.05);
        r.fadeOutSamples  = (std::int64_t) (sr * 0.15);
        r.fadeInShape     = FadeShape::EqualPower;
        r.fadeOutShape    = FadeShape::Exp;
        return r;
    };
    session.track (0).regions = { makeRegion (0), makeRegion ((std::int64_t) (sr * 2.4)) };
    session.track (1).regions = { makeRegion ((std::int64_t) (sr * 0.6)) };

    // MIDI region with a short riff on track 8 (for the piano roll).
    session.track (8).name = "Synth";
    session.track (8).mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
    {
        MidiRegion m;
        m.timelineStart = 0;
        m.lengthInTicks = 4 * 480;
        m.lengthInSamples = (std::int64_t) (sr * 2.0);
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

    session.addMarker ((std::int64_t) (sr * 1.2), "Verse");
    session.addMarker ((std::int64_t) (sr * 3.0), "Chorus");
    session.track (0).strip.busAssign[0].store (true, std::memory_order_relaxed);

    if (consoleView != nullptr)
        consoleView->setBank (0);

    // RECORDING stage
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

        auto& tr = session.track (0);
        tr.meterInputDb.store (-7.0f, std::memory_order_relaxed);
        tr.meterOutLDb .store (-100.0f, std::memory_order_relaxed);
        tr.meterOutRDb .store (-100.0f, std::memory_order_relaxed);
        tr.inputMonitor.store (false, std::memory_order_relaxed);
        auto* strip = consoleView->getStripComponent (0);
        strip->refreshMetersForCapture();
        snapshotComponent (strip, outDir, "qg-03-arm-track.png");
    }

    // Lit input meters -> "record rolling" / "overdub".
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

    // MIXING stage
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

        // Compact-mode strips: EQ / COMP / AUX collapse into split buttons;
        // TAPE retains its existing section-pill grammar. Capture channel, bus,
        // and master compacted,
        // then restore full mode so later shots aren't collapsed.
        consoleView->setStripsCompactMode (true);
        resized();
        settle (300);
        snapshotComponent (consoleView->getStripComponent (0),     outDir, "cs-01-channel-compact.png");
        snapshotComponent (consoleView->getBusComponent (0),       outDir, "cs-02-bus-compact.png");
        snapshotComponent (consoleView->getMasterStripComponent(), outDir, "cs-03-master-compact.png");
        consoleView->setStripsCompactMode (false);
        resized();
        settle (200);

        // Automation-mode label in WRITE.
        session.track (0).automationMode.store ((int) AutomationMode::Write, std::memory_order_relaxed);
        resized();
        snapshotComponent (consoleView->getStripComponent (0), outDir, "mm-01-automation-modes.png", 120);
        session.track (0).automationMode.store ((int) AutomationMode::Off, std::memory_order_relaxed);

        // Offline-plugin slot: force track 2's insert into the offline display
        // state, snapshot the strip, then clear it so later full-window shots
        // stay clean.
        engine.getStrip (2).getPluginSlot().setOfflineForCapture ("Vintage Reverb");
        if (auto* s2 = consoleView->getStripComponent (2))
        {
            s2->refreshInsertButtonForCapture();
            settle (60);
            snapshotComponent (s2, outDir, "ts-02-plugin-offline.png");
        }
        engine.getStrip (2).getPluginSlot().setOfflineForCapture ({});
        if (auto* s2 = consoleView->getStripComponent (2))
            s2->refreshInsertButtonForCapture();
    }

    // AUX stage
    switchToStage (AudioEngine::Stage::Aux);
    resized();
    settle (300);
    snapshotComponent (auxView.get(), outDir, "np-07-aux-view.png");

    // np-08 / mm-02 are the mastering stage, whose EQ and limiter panels are framework
    // children: that figure is taken in captureNativePanels below, from the running
    // message loop, because a child cannot draw while settle() holds the loop.

    // Back to a normal stage before modal shots.
    switchToStage (AudioEngine::Stage::Mixing);
    resized();
    settle (200);

    // I/O config popup (three mode variants). The popup borrows a strip's
    // live combos, so drive it through a real strip and snapshot the modal
    // body. Restored to mono afterwards.
    if (consoleView != nullptr)
    {
        if (auto* s0 = consoleView->getStripComponent (0))
        {
            const char* ioNames[3] = { "io-01-input-config-mono.png",
                                       "io-02-input-config-stereo.png",
                                       "io-03-input-config-midi.png" };
            for (int m = 0; m < 3; ++m)
            {
                if (auto* body = s0->openIoConfigPopupForCapture (m))
                    snapshotComponent (body, outDir, ioNames[m], 200);
                s0->closeIoConfigPopupForCapture();
            }
            s0->openIoConfigPopupForCapture (0);
            s0->closeIoConfigPopupForCapture();
        }
    }

    // Modal panels (standalone, snapshot directly)
    auto modalShot = [&] (juce::Component& m, int w, int h, const juce::String& name, int settleMs)
    {
        addAndMakeVisible (m);
        m.setBounds ((std::max (w, getWidth())  - w) / 2,
                     (std::max (h, getHeight()) - h) / 2, w, h);
        snapshotComponent (&m, outDir, name, settleMs);
        removeChildComponent (&m);
    };

    // np-10/ed-04 and np-11/ed-05 are the same figure under two names: render
    // once, copy to the alias.
    auto alias = [&] (const juce::String& from, const juce::String& to)
    {
        outDir.getChildFile (from).copyFileTo (outDir.getChildFile (to));
    };
    {
        AudioRegionEditor ed (session, engine, 0, 0);
        modalShot (ed, 1000, 640, "np-10-region-editor.png", 500);
    }
    alias ("np-10-region-editor.png", "ed-04-region-editor-modal.png");
    {
        PianoRollComponent pr (session, engine, 8, 0);
        modalShot (pr, 1100, 680, "np-11-piano-roll.png", 500);
    }
    alias ("np-11-piano-roll.png", "ed-05-piano-roll-full.png");
    {
        MidiBindingsPanel p (session, engine, [] {});
        modalShot (p, MidiBindingsPanel::kPanelW, MidiBindingsPanel::kPanelH, "sync-01-mcu-bindings.png", 300);
    }
    {
        HardwareInsertEditor p (session.track (0).hardwareInsert, engine.getDeviceManager(), [] {}, true);
        modalShot (p, 480, 520, "pl-04-hw-insert.png", 300);
    }
    {
        // Channel EQ editor - the 4-band EQ the strip's EQ button opens. Give
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
        auto& m = session.master();
        const bool wasTapeEnabled = m.tapeEnabled.load (std::memory_order_relaxed);
        m.tapeEnabled.store (true, std::memory_order_relaxed);
        TapePanel tp (m, engine);
        const int w = tp.getWidth()  > 0 ? tp.getWidth()  : 720;
        const int h = tp.getHeight() > 0 ? tp.getHeight() : 420;
        modalShot (tp, w, h, "fx-03-tape.png", 300);
        m.tapeEnabled.store (wasTapeEnabled, std::memory_order_relaxed);
    }
    // qg-01-startup and qg-02-audio-settings are native panels now, captured with
    // the other framework children in captureNativePanels below.
    {
        // Plugin picker with a synthetic effect list (no real scan in capture mode).
        auto mk = [] (const char* n, const char* mfr, const char* cat)
        {
            PluginDescriptor d;
            d.name = n; d.manufacturer = mfr; d.category = cat;
            d.formatName = "VST3"; d.version = "1.0";
            return d;
        };
        std::vector<PluginDescriptor> descs {
            mk ("Ambience",        "Smartelectronix", "Reverb"),
            mk ("TAL-Chorus-LX",   "TAL",             "Modulation"),
            mk ("Dragonfly Hall",  "Michael Willis",  "Reverb"),
            mk ("Calf Vintage Delay", "Calf",         "Delay"),
            mk ("ZamComp",         "Zam Audio",       "Dynamics"),
            mk ("x42 Convolver",   "Robin Gareus",    "Reverb")
        };
        PluginPickerPanel::Callbacks cb;   // all null - display only
        PluginPickerPanel pp (descs, PluginPickerPanel::Kind::Effects, cb);
        modalShot (pp, 480, 560, "pl-01-plugin-picker.png", 300);
    }
    {
        // Bounce dialog (progress UI). Its ctor kicks an offline render to the
        // temp file; snapshot the progress panel immediately, then it cancels
        // on destruction. Done last so the offline-render device detach can't
        // disturb earlier snapshots.
        auto target = outDir.getChildFile ("_demo").getChildFile ("bounce.wav");
        BounceDialog bd (engine, session, target,
                         BounceEngine::Mode::MasterMix);
        modalShot (bd, 520, 200, "qg-07-bounce-dialog.png", 200);
    }
    alias ("qg-07-bounce-dialog.png", "bnc-01-bounce-dialog.png");

    // The native panels come last and from the running message loop: their frames
    // are drawn by a message-thread timer, so a phase that blocks the loop the way
    // settle() does would capture nothing. captureNativePanels quits when it is done.
    captureNativePanels (outDir.getFullPathName().toStdString());
}

namespace
{
// One PPM a framework child read its own frame back into, pasted into a JUCE snapshot
// at the rectangle the child covers. Under the harness the window is unscaled, so the
// two are the same pixels; a scaled run is letterboxed rather than skewed.
void pastePpm (juce::Image& into, const juce::File& ppm, juce::Rectangle<int> at)
{
    juce::FileInputStream stream (ppm);
    if (! stream.openedOk())
        return;

    if (stream.readNextLine().trim() != "P6")
        return;
    const auto dimensions = juce::StringArray::fromTokens (stream.readNextLine().trim(),
                                                           true);
    if (dimensions.size() < 2 || stream.readNextLine().trim() != "255")
        return;

    const int width = dimensions[0].getIntValue();
    const int height = dimensions[1].getIntValue();
    if (width < 1 || height < 1)
        return;

    std::vector<unsigned char> rgb (static_cast<std::size_t> (width)
                                    * static_cast<std::size_t> (height) * 3u);
    if (stream.read (rgb.data(), static_cast<int> (rgb.size()))
        != static_cast<int> (rgb.size()))
        return;

    juce::Image frame (juce::Image::RGB, width, height, false);
    {
        const juce::Image::BitmapData pixels (frame, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < height; ++y)
        {
            const unsigned char* row = rgb.data()
                                     + static_cast<std::size_t> (y)
                                           * static_cast<std::size_t> (width) * 3u;
            for (int x = 0; x < width; ++x, row += 3)
                pixels.setPixelColour (x, y, juce::Colour (row[0], row[1], row[2]));
        }
    }

    juce::Graphics g (into);
    g.drawImage (frame, at.toFloat(), juce::RectanglePlacement::centred);
    ppm.deleteFile();
}
} // namespace

void MainComponent::captureNativePanels (std::string outDir)
{
    // Written as a list rather than nested callbacks because every step needs the
    // message loop to actually run between them: a framework child draws from a
    // message-thread timer, so the blocking settle() the snapshot phase uses would
    // leave it with nothing on screen.
    struct Step
    {
        int delayMs;   // how long the loop runs before this step
        std::function<void (MainComponent&)> run;
    };

    auto masteringCaptures =
        std::make_shared<std::vector<MasteringView::NativePanelCapture>>();
    const juce::File dir { juce::String (outDir) };

    auto steps = std::make_shared<std::vector<Step>>();

    // The mastering stage's EQ and limiter are framework children inside a JUCE view, so
    // its figure is the JUCE snapshot with each child's own frame pasted back in.
    steps->push_back ({ 0, [] (MainComponent& self)
    {
        self.switchToStage (AudioEngine::Stage::Mastering);
        self.resized();
    } });
    steps->push_back ({ 900, [masteringCaptures, dir] (MainComponent& self)
    {
        if (self.masteringView != nullptr)
            *masteringCaptures = self.masteringView->captureNativePanels (dir);
    } });
    steps->push_back ({ 1500, [masteringCaptures, dir] (MainComponent& self)
    {
        auto* const view = self.masteringView.get();
        if (view != nullptr && view->getWidth() > 0 && view->getHeight() > 0)
        {
            auto image = view->createComponentSnapshot (view->getLocalBounds(), true);
            for (const auto& capture : *masteringCaptures)
                pastePpm (image, capture.file, capture.bounds);
            writePng (image, dir.getChildFile ("np-08-mastering-view.png"));
            writePng (image, dir.getChildFile ("mm-02-mastering-chain.png"));
        }
        self.switchToStage (AudioEngine::Stage::Mixing);
        self.resized();
    } });

    steps->push_back ({ 600, [outDir] (MainComponent& self)
    {
        auto* const strip0 = self.consoleView != nullptr
                           ? self.consoleView->getStripComponent (0) : nullptr;
        if (strip0 == nullptr)
            return;
        auto& strip = self.session.track (0).strip;
        strip.compEnabled.store (true, std::memory_order_relaxed);
        strip.compMode.store (2, std::memory_order_relaxed);   // VCA
        // The bounce phase above leaves the input meter reading, and this figure is of
        // the panel rather than of a signal. Park the meters at rest.
        self.session.track (0).meterInputDb.store (-100.0f, std::memory_order_relaxed);
        self.session.track (0).meterGrDb.store (0.0f, std::memory_order_relaxed);
        strip0->openCompEditorForCapture (outDir + "/fx-02-comp.ppm");
    } });
    // One panel at a time: each is a modal surface, and two open at once would put one
    // child over the other.
    steps->push_back ({ 1500, [] (MainComponent& self)
    {
        if (self.consoleView != nullptr)
            if (auto* strip0 = self.consoleView->getStripComponent (0))
                strip0->closeCompEditorForCapture();
    } });
    steps->push_back ({ 400, [outDir] (MainComponent& self)
    {
        self.openVirtualKeyboardForCapture (outDir + "/vkb-01-virtual-keyboard.ppm");
    } });
    steps->push_back ({ 1500, [] (MainComponent& self) { self.closeVirtualKeyboard(); } });
    steps->push_back ({ 400, [outDir] (MainComponent& self)
    {
        self.openAudioSettingsForCapture (outDir + "/qg-02-audio-settings.ppm");
    } });
    steps->push_back ({ 1500, [] (MainComponent& self) { self.closeAudioSettings(); } });
    // Quitting with the startup panel still up is the point: dismissing it would run the
    // launch follow-ups the harness has already been past for a whole session.
    steps->push_back ({ 400, [outDir] (MainComponent& self)
    {
        self.openStartupForCapture (outDir + "/qg-01-startup.ppm");
    } });
    steps->push_back ({ 1500, [] (MainComponent&) {} });

    juce::Component::SafePointer<MainComponent> safeThis (this);
    auto runFrom = std::make_shared<std::function<void (std::size_t)>>();
    // The runner holds itself weakly and the pending timer holds it strongly, so it
    // lives exactly as long as it has a step left. Capturing it strongly inside itself
    // would be a cycle that never frees.
    std::weak_ptr<std::function<void (std::size_t)>> weakRunner = runFrom;
    *runFrom = [safeThis, steps, weakRunner] (std::size_t index)
    {
        auto* const self = safeThis.getComponent();
        if (self == nullptr || index >= steps->size())
        {
            std::fprintf (stderr, "[Dusk Studio/capture] done\n");
            juce::JUCEApplication::getInstance()->quit();
            return;
        }
        (*steps)[index].run (*self);
        const auto next = index + 1;
        const int delay = next < steps->size() ? (*steps)[next].delayMs : 0;
        if (auto runner = weakRunner.lock())
            dusk::Timer::callAfterDelay (delay, [runner, next] { (*runner) (next); });
    };
    (*runFrom) (0);
}
} // namespace duskstudio
