#include "MainComponent.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>  // std::getenv (DUSKSTUDIO_USE_OOP_PLUGINS)
#include "AppConfig.h"
#if __has_include("BinaryData.h")
 #include "BinaryData.h"
 #define DUSKSTUDIO_HAS_BINARY_ICON 1
#else
 #define DUSKSTUDIO_HAS_BINARY_ICON 0
#endif
#include "DuskFileBrowser.h"
#include "AuxView.h"
#include "BounceDialog.h"
#include "PluginScanModal.h"
#include "ShortcutsPanel.h"
#include "SupportersPanel.h"
#include "MidiBindingsPanel.h"
#include "SelfTestPanel.h"
#if DUSKSTUDIO_HAS_NATIVE_UI
 #include "imgui/AudioSettingsView.h"
 #include "imgui/StartupView.h"
 #include "imgui/VirtualKeyboardView.h"
 #include "NativeNotepadWindow.h"
 #include "NativeEditorEmbedScale.h"
 #include "imgui/DuskPanelWindow.h"
#endif
#include "DuskContextMenu.h"
#include "../session/MidiBindings.h"
#include "ConsoleView.h"
#include "EditModeToolbar.h"   // EditModeToolbar::labelFor for the snap-resolution label
#include "CursorOverlay.h"
#include "DimOverlay.h"
#include "DuskAlerts.h"
#include "EmbeddedModal.h"
#include "PianoRollComponent.h"
#include "PlatformWindowing.h"
#include "AudioRegionEditor.h"
#include "TunerOverlay.h"
#include "../session/SessionTemplates.h"
#include "MasteringView.h"
#include "UpdateChecker.h"
#include "SystemStatusBar.h"
#include "TapeStrip.h"
#include "MiniTimelineStrip.h"
#include "TransportBar.h"
#include "../session/MarkerEditActions.h"
#include "../session/RecentSessions.h"
#include "../session/SessionSerializer.h"
#include "../engine/FileImporter.h"
#include "../engine/PlaybackEngine.h"
#include "../engine/PluginStateDiagnostics.h"
#include "ImportTargetPicker.h"
#include "DpImportDialog.h"
#include "../engine/DpImporter.h"
#include "../engine/DpAligner.h"
#include "../engine/audiofile/FileReader.h"
#include "../engine/audiofile/FileWriter.h"
#include "../engine/midi/MidiFileReader.h"
#include "../foundation/PlanarBuffer.h"
#include "../foundation/Text.h"
#include <algorithm>

namespace duskstudio
{
namespace
{
template <typename T>
inline T jlimit (T lo, T hi, T value) noexcept { return std::clamp (value, lo, std::max (lo, hi)); }
// UTF-8-safe bridges across the RecentSessions (std::filesystem) boundary -
// native narrow conversions lose non-ASCII paths on Windows.
juce::File toFile (const std::filesystem::path& p)
{
    return juce::File (juce::String::fromUTF8 (p.u8string().c_str()));
}

std::filesystem::path toPath (const juce::File& f)
{
    return std::filesystem::u8path (f.getFullPathName().toStdString());
}

juce::Array<juce::File> toFileArray (const std::vector<std::filesystem::path>& paths)
{
    juce::Array<juce::File> out;
    for (auto& p : paths) out.add (toFile (p));
    return out;
}

auto pluginRestoreFailureBody (const std::vector<AudioEngine::PluginLoadFailure>& failures)
{
    juce::String body =
        "These plug-ins could not be loaded, restored, or reactivated and are "
        "offline. Saved references and state were preserved where available:\n\n";
    for (const auto& failure : failures)
    {
        const auto line = pluginstate::restoreFailureLine (
            failure.location.toStdString(), failure.pluginName.toStdString(),
            failure.format, failure.reason);
        body += "    " + juce::String::fromUTF8 (line.c_str()) + "\n";
    }
    body += "\nResolve the reported error. For a saved-state failure, correct the "
            "plug-in installation or compatibility issue and reload the session. "
            "For a device or render-rate failure, restore a supported audio setting "
            "or remove and reload the plug-in.";
    return body;
}

// Note count and rendered length for the import picker's file card. False when
// the file does not parse, which each caller reports its own way.
bool peekMidiSummary (const juce::File& file, ImportTargetPicker::FileSummary& summary)
{
    midi::MidiFileReader reader;
    if (! reader.readFile (toPath (file)))
        return false;

    int noteCount = 0;
    std::int64_t maxTick = 0;
    for (const auto& track : reader.tracks())
        for (const auto& ev : track)
        {
            if (ev.isNoteOn()) ++noteCount;
            maxTick = std::max (maxTick, ev.tick);
        }

    const int ppq = reader.timeFormat();
    summary.numMidiNotes = noteCount;
    if (ppq <= 0)
        summary.lengthTicks = 0;   // SMPTE division: maxTick is frame-based, not beats
    else if (ppq == kMidiTicksPerQuarter)
        summary.lengthTicks = maxTick;
    else
        summary.lengthTicks = (std::int64_t) std::llround (
            (double) maxTick * (double) kMidiTicksPerQuarter / (double) ppq);
    return true;
}

#if DUSKSTUDIO_HAS_NATIVE_UI
// Where the embedded notepad child sits inside the top-level window, in
// logical coordinates. The native child's geometry and the dim overlay's
// click-outside test are the same rectangle in two coordinate spaces.
juce::Rectangle<int> notepadChildBounds (const juce::Component& topLevel)
{
    // A window narrower than the margin would otherwise centre a negative
    // extent, which offsets the origin instead of shrinking the child.
    return topLevel.getLocalBounds().withSizeKeepingCentre (
        std::clamp (topLevel.getWidth() - 32, 0, (int) NativeNotepadWindow::kPreferredWidth),
        std::clamp (topLevel.getHeight() - 32, 0, (int) NativeNotepadWindow::kPreferredHeight));
}

NativeNotepadWindow::EmbeddedGeometry notepadGeometryFor (const juce::Component& topLevel)
{
    auto* const peer = topLevel.getPeer();
    const double peerScale = peer != nullptr ? peer->getPlatformScaleFactor() : 1.0;
    const double backingScale = peer != nullptr
                              ? platform::nativeViewBackingScale (peer->getNativeHandle())
                              : 1.0;
   #if JUCE_MAC
    constexpr bool useNativeViewScale = true;
   #else
    constexpr bool useNativeViewScale = false;
   #endif
    const double scale = embedscale::factorFromSources (
        embedscale::globalScale(), peerScale, backingScale, useNativeViewScale);
    const auto logical = notepadChildBounds (topLevel);
    const auto bounds = embedscale::toPhysicalBounds (
        logical.getX(), logical.getY(), logical.getWidth(), logical.getHeight(), scale);
    return {
        bounds.x, bounds.y,
        static_cast<std::uint32_t> (std::max (2, bounds.width)),
        static_cast<std::uint32_t> (std::max (2, bounds.height)),
        scale
    };
}
#endif

// Shared helpers for the file-import flow (both menu-driven and
// drag-and-drop). Lives at file scope so the TapeStrip drop callback
// wired up in the MainComponent ctor can reference them.

// Background worker for the DP-song bulk import: mixdown alignment plus every
// fragment decode/convert runs here so a large song doesn't wedge the message
// thread for minutes. NO session access - the outcomes are applied on the
// message thread in MainComponent::finishDpImport once `done` flips. Cancel
// deletes the WAVs already produced (nothing references them until apply).
class DpImportJob final : public juce::Thread
{
public:
    struct TrackOutcome
    {
        int index = -1;
        bool imported = false;
        juce::String skipReason;                       // set when !imported
        duskstudio::fileimport::AudioImportResult res; // valid when imported
    };

    // stereoCombineFn = makeStereoTempWav, injected because that helper is
    // defined at namespace scope below this anonymous namespace.
    DpImportJob (dp::SongScan s, double sessionSr, juce::File audioDirIn,
                 std::array<bool, Session::kNumTracks> frozenTracks,
                 bool wantTimeline,
                 std::function<juce::File (const juce::File&, const juce::File&)> stereoCombineFn)
        : juce::Thread ("Dusk Studio DP import"),
          scan (std::move (s)), sr (sessionSr), audioDir (std::move (audioDirIn)),
          frozen (frozenTracks), importTimeline (wantTimeline),
          stereoCombine (std::move (stereoCombineFn)) {}

    // Never force-kill: importAudio streams a fragment in bounded chunks, so
    // the wait is seconds at worst, and a killed thread would leak a
    // half-written WAV with the writer still open.
    ~DpImportJob() override
    {
        signalThreadShouldExit();
        waitForThreadToExit (-1);
    }

    void run() override
    {
        const int n = std::min ((int) scan.tracks.size(), Session::kNumTracks);
        placeAt.assign ((size_t) n, 0);

        // Timeline placement: song.sys clip offsets first, mixdown onset
        // alignment for anything song.sys didn't place.
        if (importTimeline)
        {
            const double songSr = scan.sampleRate > 0.0 ? scan.sampleRate : sr;
            for (int i = 0; i < n; ++i)
            {
                const auto ss = scan.tracks[(size_t) i].timelineStart;
                if (ss > 0)
                {
                    placeAt[(size_t) i] = (std::int64_t) std::llround ((double) ss * sr / songSr);
                    ++placedCount;
                }
            }
            if (scan.hasMixdown && ! threadShouldExit())
            {
                std::vector<juce::File> sources;
                sources.reserve ((size_t) n);
                for (int i = 0; i < n; ++i)
                    sources.push_back (scan.tracks[(size_t) i].fragment.mono1);
                const auto al = dp::alignToMixdown (scan.mixdownFile, sources);
                for (int i = 0; i < n && i < (int) al.size(); ++i)
                    if (placeAt[(size_t) i] == 0 && al[(size_t) i].placed
                        && ! al[(size_t) i].fullLength && al[(size_t) i].positionSeconds > 0.0)
                    {
                        placeAt[(size_t) i] = (std::int64_t) std::llround (
                            al[(size_t) i].positionSeconds * sr);
                        ++placedCount;
                    }
            }
        }

        for (int i = 0; i < n && ! threadShouldExit(); ++i)
        {
            const auto& it   = scan.tracks[(size_t) i];
            const auto& frag = it.fragment;
            TrackOutcome out;
            out.index = i;

            if (frozen[(size_t) i])
            {
                out.skipReason = it.name + ": target track is frozen (unfreeze to import)";
                outcomes.push_back (std::move (out));
                continue;
            }

            juce::File src, tmp;
            const int channels = frag.stereo ? 2 : 1;
            if (frag.stereo)
            {
                tmp = stereoCombine (frag.mono1, frag.mono2);
                if (tmp == juce::File())
                {
                    out.skipReason = it.name + ": could not combine stereo halves";
                    outcomes.push_back (std::move (out));
                    continue;
                }
                src = tmp;
            }
            else
            {
                src = frag.mono1;
            }

            duskstudio::fileimport::AudioImportRequest req;
            req.source            = src;
            req.audioDir          = audioDir;
            req.trackIndex        = i;
            req.sessionSampleRate = sr;
            req.targetChannels    = channels;
            req.timelineStart     = placeAt[(size_t) i];

            out.res = duskstudio::fileimport::importAudio (req);
            if (tmp != juce::File()) tmp.deleteFile();
            if (out.res.ok) out.imported = true;
            else            out.skipReason = it.name + ": " + out.res.errorMessage;
            outcomes.push_back (std::move (out));

            progress.store ((float) (i + 1) / (float) std::max (1, n),
                             std::memory_order_relaxed);
        }

        if (threadShouldExit())
        {
            for (auto& o : outcomes)
                if (o.imported) o.res.region.file.deleteFile();
            outcomes.clear();
            cancelled.store (true, std::memory_order_release);
        }
        done.store (true, std::memory_order_release);
    }

    dp::SongScan scan;
    double       sr;
    juce::File   audioDir;
    std::array<bool, Session::kNumTracks> frozen;
    bool importTimeline;
    std::function<juce::File (const juce::File&, const juce::File&)> stereoCombine;

    // Written by run(), read on the message thread strictly after `done`.
    std::vector<std::int64_t> placeAt;
    int placedCount = 0;
    std::vector<TrackOutcome> outcomes;

    std::atomic<float> progress  { 0.0f };
    std::atomic<bool>  done      { false };
    std::atomic<bool>  cancelled { false };
};

// Progress body for the DP-import modal: bar + cancel, 20 Hz poll of the
// job's atomics. Fires onFinished exactly once on the message thread when
// the worker flips `done`.
class DpImportProgressPanel final : public juce::Component, private dusk::Timer
{
public:
    explicit DpImportProgressPanel (DpImportJob& j) : job (j), bar (value)
    {
        title.setText ("Importing DP song...", juce::dontSendNotification);
        title.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        title.setColour (juce::Label::textColourId, juce::Colour (0xffe8e8e8));
        addAndMakeVisible (title);
        bar.setColour (juce::ProgressBar::backgroundColourId, juce::Colour (0xff101012));
        bar.setColour (juce::ProgressBar::foregroundColourId, juce::Colour (0xff5fa8ff));
        addAndMakeVisible (bar);
        cancelButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff202024));
        cancelButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffd0d0d0));
        cancelButton.onClick = [this]
        {
            job.signalThreadShouldExit();
            cancelButton.setEnabled (false);
        };
        addAndMakeVisible (cancelButton);
        startTimerHz (20);
    }

    // The base timer dtor runs after these members are gone; a 20 Hz tick landing
    // in that window would read the freed job reference.
    ~DpImportProgressPanel() override { stopTimer(); }

    std::function<void()> onFinished;

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xff202024)); }
    void resized() override
    {
        auto area = getLocalBounds().reduced (16);
        title.setBounds (area.removeFromTop (24));
        area.removeFromTop (10);
        bar.setBounds (area.removeFromTop (22));
        area.removeFromTop (12);
        cancelButton.setBounds (area.removeFromTop (28).removeFromRight (110));
    }

private:
    void timerCallback() override
    {
        value = (double) job.progress.load (std::memory_order_relaxed);
        if (job.done.load (std::memory_order_acquire) && ! notified)
        {
            notified = true;
            stopTimer();
            if (onFinished) onFinished();
        }
    }

    DpImportJob& job;
    double value = 0.0;
    juce::Label title;
    juce::ProgressBar bar;
    juce::TextButton cancelButton { "Cancel" };
    bool notified = false;
};

// Route every import-error alert through the in-window Dusk alert
// modal. The free function pulls the active top-level so existing
// call sites don't need to thread a Component reference through.
void showImportError (const juce::String& title, const juce::String& message)
{
    if (auto* tlw = juce::TopLevelWindow::getActiveTopLevelWindow())
    {
        if (auto* dw = dynamic_cast<juce::DocumentWindow*> (tlw))
            if (auto* content = dw->getContentComponent())
            {
                showDuskAlert (*content, title, message);
                return;
            }
    }
    // No top-level to host the in-window alert (headless / shutdown paths
    // only). A native AlertWindow here would be the one separate-window
    // dialog left in the app - and on a headless path it can't be seen
    // anyway. Log instead; the assert makes a reachable-in-normal-use
    // regression loud in debug builds.
    std::fprintf (stderr, "[Dusk Studio] %s: %s\n",
                  title.toRawUTF8(), message.toRawUTF8());
    jassertfalse;
}
} // namespace

// Tracks the top-level window so AuxView's editor hosts (separate X11
// toplevels on Linux/Wayland) can follow the main window across the
// screen. Override targets ComponentMovementWatcher protected virtuals.
namespace
{
// Dusk Studio-styled "save changes before quitting?" panel hosted by
// MainComponent::quitModal via EmbeddedModal. Three actions: Save (writes
// session.json or pops the Save As chooser, then quits on success),
// Don't Save (quits immediately, autosave keeps the changes for next
// launch's recovery prompt), Cancel (just dismisses).
class QuitConfirmDialog final : public juce::Component
{
public:
    QuitConfirmDialog (juce::String title = "Save changes before quitting?",
                       juce::String body  =
                           "Your session has unsaved changes since the last manual save. "
                           "If you don't save, those changes are discarded.")
    {
        titleLabel.setText (title, juce::dontSendNotification);
        titleLabel.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
        titleLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe8e8e8));
        addAndMakeVisible (titleLabel);

        bodyLabel.setText (body, juce::dontSendNotification);
        bodyLabel.setFont (juce::Font (juce::FontOptions (13.0f)));
        bodyLabel.setColour (juce::Label::textColourId, juce::Colour (0xffc0c0c8));
        bodyLabel.setJustificationType (juce::Justification::topLeft);
        bodyLabel.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (bodyLabel);

        styleAccent (saveButton);
        styleNeutral (dontSaveButton);
        styleNeutral (cancelButton);

        saveButton    .onClick = [this] { if (onSave)     onSave(); };
        dontSaveButton.onClick = [this] { if (onDontSave) onDontSave(); };
        cancelButton  .onClick = [this] { if (onCancel)   onCancel(); };

        addAndMakeVisible (saveButton);
        addAndMakeVisible (dontSaveButton);
        addAndMakeVisible (cancelButton);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1a1a20));
        g.setColour (juce::Colour (0xff2a2a32));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 6.0f, 1.0f);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (24);
        titleLabel.setBounds (area.removeFromTop (28));
        area.removeFromTop (10);
        bodyLabel.setBounds (area.removeFromTop (60));

        // Right-aligned button row, primary action on the right (Save).
        auto buttonRow = area.removeFromBottom (36);
        const int btnW = 110;
        saveButton    .setBounds (buttonRow.removeFromRight (btnW));
        buttonRow.removeFromRight (8);
        dontSaveButton.setBounds (buttonRow.removeFromRight (btnW));
        buttonRow.removeFromRight (8);
        cancelButton  .setBounds (buttonRow.removeFromRight (90));
    }

    std::function<void()> onSave;
    std::function<void()> onDontSave;
    std::function<void()> onCancel;

private:
    static void styleAccent (juce::TextButton& b)
    {
        b.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff4a7c9e));
        b.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    }
    static void styleNeutral (juce::TextButton& b)
    {
        b.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a2a30));
        b.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffe0e0e0));
    }

    juce::Label      titleLabel;
    juce::Label      bodyLabel;
    juce::TextButton saveButton    { "Save" };
    juce::TextButton dontSaveButton { "Don't Save" };
    juce::TextButton cancelButton  { "Cancel" };
};

// Autosave recovery prompt. Replaces juce::AlertWindow::showAsync which
// rendered the native question-mark dialog and didn't match the rest
// of the app's modal styling. Same three actions: Recover (load the
// newer autosave), Load (load the saved session, autosave is discarded
// by the next manual save), Cancel (bail out of the load entirely).
class AutosaveRecoveryDialog final : public juce::Component
{
public:
    AutosaveRecoveryDialog (const juce::File& sessionDir,
                              const juce::Time& autosaveTime,
                              const juce::Time& savedTime)
    {
        titleLabel.setText ("Recover from autosave?", juce::dontSendNotification);
        titleLabel.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
        titleLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe8e8e8));
        addAndMakeVisible (titleLabel);

        bodyLabel.setText (
            "An autosave file is newer than the saved session at\n"
            + sessionDir.getFullPathName()
            + "\n\nAutosave:  " + autosaveTime.toString (true, true)
            + "\nSaved:        " + savedTime    .toString (true, true)
            + "\n\nDusk Studio probably exited unexpectedly. "
              "Recover the newer autosave, or load the saved session and "
              "discard it?",
            juce::dontSendNotification);
        bodyLabel.setFont (juce::Font (juce::FontOptions (13.0f)));
        bodyLabel.setColour (juce::Label::textColourId, juce::Colour (0xffc0c0c8));
        bodyLabel.setJustificationType (juce::Justification::topLeft);
        bodyLabel.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (bodyLabel);

        recoverButton .setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff4a7c9e));
        recoverButton .setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        loadButton    .setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff2a2a30));
        loadButton    .setColour (juce::TextButton::textColourOffId, juce::Colour (0xffe0e0e0));
        cancelButton  .setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff2a2a30));
        cancelButton  .setColour (juce::TextButton::textColourOffId, juce::Colour (0xffe0e0e0));

        recoverButton.onClick = [this] { if (onRecover) onRecover(); };
        loadButton   .onClick = [this] { if (onLoad)    onLoad();    };
        cancelButton .onClick = [this] { if (onCancel)  onCancel();  };

        addAndMakeVisible (recoverButton);
        addAndMakeVisible (loadButton);
        addAndMakeVisible (cancelButton);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1a1a20));
        g.setColour (juce::Colour (0xff2a2a32));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 6.0f, 1.0f);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (24);
        titleLabel.setBounds (area.removeFromTop (28));
        area.removeFromTop (10);
        bodyLabel.setBounds (area.removeFromTop (140));

        auto buttonRow = area.removeFromBottom (36);
        recoverButton.setBounds (buttonRow.removeFromRight (160));
        buttonRow.removeFromRight (8);
        loadButton   .setBounds (buttonRow.removeFromRight (170));
        buttonRow.removeFromRight (8);
        cancelButton .setBounds (buttonRow.removeFromRight (90));
    }

    std::function<void()> onRecover;
    std::function<void()> onLoad;
    std::function<void()> onCancel;

private:
    juce::Label      titleLabel;
    juce::Label      bodyLabel;
    juce::TextButton recoverButton { "Recover autosave" };
    juce::TextButton loadButton    { "Load saved session" };
    juce::TextButton cancelButton  { "Cancel" };
};
} // namespace

MainComponent::MainComponent()
{
    // Accessibility floor - screen readers announce the root view as
    // "Dusk Studio mixer" instead of "Component". Tab navigation across
    // strips is enabled per-strip via setWantsKeyboardFocus on each
    // ChannelStripComponent in resized().
    setTitle ("Dusk Studio mixer");
    setDescription ("16-channel portastudio-style mixer");

    // JUCE's TooltipWindow never disables click interception, and ours is
    // parented in-window and anchored into the menu row - every visible tip
    // was an always-on-top click shield over the stage tabs / bank buttons /
    // transport ("I have to click twice"). Tips are display-only; let every
    // click pass through to what's underneath.
    tooltipWindow.setInterceptsMouseClicks (false, false);

    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    // Plugins run IN-PROCESS by default. On Linux/XWayland the out-of-process
    // editor path (cross-process XEmbed) is structurally unreliable - the
    // compositor fights X11 reparenting - and in-process hosting gives instant,
    // correct plugin editors with the lowest CPU/latency. The trade-off is
    // crash isolation: a misbehaving plugin can take down the app instead of
    // just a child. Opt back into the OOP sandbox with
    // DUSKSTUDIO_USE_OOP_PLUGINS=1. Read once at startup - flipping mid-session
    // would require reloading every plugin to pick up the new mode.
    {
        const char* env = std::getenv ("DUSKSTUDIO_USE_OOP_PLUGINS");
        const bool enableOop = (env != nullptr && env[0] == '1' && env[1] == '\0');
        engine.getPluginManager().setOopEnabled (enableOop);
    }
   #endif

    // Optional plugin scan on launch. Per-machine setting (AppConfig);
    // default off. Synchronous - blocks the message thread for a few
    // Push the user's persisted Stop-behavior preference into the session
    // atom so AudioEngine::stop reads the right policy on the first Stop
    // after launch. The audio settings panel's combo updates this same atom
    // when the user changes the dropdown.
    session.stopBehavior.store ((int) appconfig::getStopBehavior(),
                                  std::memory_order_relaxed);
    engine.setMidiSoftTakeover (appconfig::getMidiSoftTakeover());
    engine.setRecordingLatencyOffsetSamples (appconfig::getRecordingLatencyOffsetSamples());

    // Plugin scan-on-startup is deferred (see resized() ->
    // maybeStartStartupPluginScan): it runs on a background thread behind a
    // progress modal once the window is on screen, so a full plugin folder
    // doesn't make the app look frozen on launch.

    // Default to a session under ~/Music/Dusk Studio/Untitled. The user can change
    // this later via a session-management UI; for the recorder MVP this is
    // enough to get WAVs on disk.
    auto musicDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    if (! musicDir.exists()) musicDir = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    session.setSessionDirectory (musicDir.getChildFile ("Dusk Studio").getChildFile ("Untitled"));

    // Top-of-window menu bar drives File / View / Settings actions. Replaces the
    // old row of TextButtons (Audio settings... / Save / Save As... / etc).
    menuBar.setModel (this);
    addAndMakeVisible (menuBar);

    // Stage selector - segmented look. The active stage gets a brighter
    // fill so the user always knows where they are.
    auto styleStageButton = [] (juce::TextButton& b, juce::Colour onColour)
    {
        b.setClickingTogglesState (true);
        b.setRadioGroupId (1001);
        b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
        b.setColour (juce::TextButton::buttonOnColourId, onColour);
        b.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff909094));
        b.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
    };
    // Stage tab accent colours. Saturations chosen so each active tab
    // has comparable visual weight - the prior aux 0xff9080c0 read as
    // washed-out next to the saturated red / blue / purple of the other
    // three, making the AUX active state look like an outline rather
    // than a filled tab.
    // TransportBar must be added BEFORE the stage tabs + bank buttons so
    // those overlay controls naturally render on top in JUCE's child
    // z-order. Constructing + addAndMakeVisible'ing it here means we
    // don't have to call toFront() per-resize for every overlay child.
    transportBar = std::make_unique<TransportBar> (engine);
    transportBar->onTunerToggle = [this] { toggleTuner(); };
    transportBar->onVirtualKeyboardToggle = [this] { toggleVirtualKeyboard(); };
    transportBar->onNotepadToggle = [this] { toggleNotepad(); };
    transportBar->onTapeStripToggle = [this] (bool expanded)
    {
        // Collapse each track strip's EQ + COMP into popup buttons while the
        // TIMELINE view is up - without this the fader and bus assigns get
        // pushed off the bottom of the strip and become unusable.
        setTimelineVisible (expanded);
    };
    addAndMakeVisible (transportBar.get());

    styleStageButton (recordingStageBtn, juce::Colour (0xffd03030));   // red, like REC
    styleStageButton (mixingStageBtn,    juce::Colour (0xff5a8ad0));   // mix-desk blue
    styleStageButton (auxStageBtn,       juce::Colour (0xff6e5ad0));   // aux indigo-violet
    styleStageButton (masteringStageBtn, juce::Colour (0xff8a5ad0));   // mastering purple
    // isCommandDown() is Cmd on macOS, Ctrl elsewhere - keep the hint in sync.
   #if JUCE_MAC
    const juce::String modKey = "Cmd";
   #else
    const juce::String modKey = "Ctrl";
   #endif
    recordingStageBtn.setTooltip ("Recording stage - press " + modKey + "+1");
    mixingStageBtn   .setTooltip ("Mixing stage - press " + modKey + "+2");
    masteringStageBtn.setTooltip ("Mastering stage - press " + modKey + "+3");
    auxStageBtn      .setTooltip ("Aux send / return stage - press " + modKey + "+4");
    recordingStageBtn.setConnectedEdges (juce::Button::ConnectedOnRight);
    mixingStageBtn   .setConnectedEdges (juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
    masteringStageBtn.setConnectedEdges (juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
    auxStageBtn      .setConnectedEdges (juce::Button::ConnectedOnLeft);
    recordingStageBtn.onClick = [this] { switchToStage (AudioEngine::Stage::Recording); };
    mixingStageBtn   .onClick = [this] { switchToStage (AudioEngine::Stage::Mixing); };
    auxStageBtn      .onClick = [this] { switchToStage (AudioEngine::Stage::Aux); };
    masteringStageBtn.onClick = [this] { switchToStage (AudioEngine::Stage::Mastering); };
    addAndMakeVisible (recordingStageBtn);
    addAndMakeVisible (mixingStageBtn);
    addAndMakeVisible (auxStageBtn);
    addAndMakeVisible (masteringStageBtn);
    recordingStageBtn.setToggleState (true, juce::dontSendNotification);  // Recording is the workflow default

    // SNAP / zoom-out / zoom-in / fit cluster. Pulled out of TapeStrip
    // to free the ruler band; placed in the header row between the
    // rightmost bank button and the tuner button (transport bar).
    auto styleHdrPill = [] (juce::TextButton& b)
    {
        b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff262630));
        b.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff3f5870));
        b.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffd0d0d4));
        b.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
        b.setMouseClickGrabsKeyboardFocus (false);
    };
    styleHdrPill (hdrZoomOutBtn);
    styleHdrPill (hdrZoomInBtn);
    styleHdrPill (hdrZoomFitBtn);
    hdrZoomOutBtn.setTooltip ("Zoom out (-)");
    hdrZoomInBtn .setTooltip ("Zoom in (=)");
    hdrZoomFitBtn.setTooltip ("Zoom to fit (Cmd+0)");
    hdrZoomOutBtn.onClick = [this]
    {
        if (tapeStrip != nullptr) tapeStrip->zoomByFactor (1.0f / 1.25f);
    };
    hdrZoomInBtn.onClick = [this]
    {
        if (tapeStrip != nullptr) tapeStrip->zoomByFactor (1.25f);
    };
    hdrZoomFitBtn.onClick = [this]
    {
        if (tapeStrip != nullptr) tapeStrip->zoomFit();
    };
    addAndMakeVisible (hdrZoomOutBtn);
    addAndMakeVisible (hdrZoomInBtn);
    addAndMakeVisible (hdrZoomFitBtn);

    // Grid snap toggle + resolution. Gates every tape-strip region / marker /
    // loop / tempo drag (SnapHelpers no-op when session.snapToGrid is false).
    styleHdrPill (hdrSnapBtn);
    styleHdrPill (hdrSnapResBtn);
    hdrSnapBtn.setClickingTogglesState (true);
    hdrSnapBtn.setTooltip ("Snap region edits, markers, loop / punch and tempo points to the grid");
    hdrSnapBtn.onClick = [this]
    {
        session.snapToGrid = hdrSnapBtn.getToggleState();
        refreshSnapUi();
        if (tapeStrip != nullptr) tapeStrip->repaint();
    };
    hdrSnapResBtn.setTooltip ("Grid resolution used when Snap is on");
    hdrSnapResBtn.onClick = [this] { showSnapResolutionMenu(); };
    addAndMakeVisible (hdrSnapBtn);
    addAndMakeVisible (hdrSnapResBtn);

    // Chase toggle - green when on, matching the audio / MIDI editors.
    hdrChaseBtn.setClickingTogglesState (true);
    hdrChaseBtn.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff262630));
    hdrChaseBtn.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff406030));
    hdrChaseBtn.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffd0d0d4));
    hdrChaseBtn.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
    hdrChaseBtn.setMouseClickGrabsKeyboardFocus (false);
    hdrChaseBtn.setToggleState (appconfig::getFollowPlayheadDefault(), juce::dontSendNotification);
    hdrChaseBtn.setTooltip ("Scroll the timeline to follow the playhead during playback");
    hdrChaseBtn.onClick = [this]
    {
        if (tapeStrip != nullptr) tapeStrip->setChaseEnabled (hdrChaseBtn.getToggleState());
    };
    addAndMakeVisible (hdrChaseBtn);
    refreshSnapUi();

    // Bank-button row is rebuilt by syncBankButtons() each layout pass
    // (visible only when the window can't fit all 24 strips at min
    // width). Sits on a row directly below the stage selector so the
    // channel strips inside ConsoleView get the full body height.

    // Save / Save As / Open / Mixdown / Bounce / Audio settings now live in
    // the menu bar - see getMenuForIndex() and menuItemSelected() below.

    statusLabel.setJustificationType (juce::Justification::centredLeft);
    // Dim grey + smaller font so the top bar's session-name readout
    // doesn't compete with the menu bar / system meters for visual
    // priority. Full path attached as tooltip via setStatusForPath.
    statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff909094));
    statusLabel.setFont (juce::Font (juce::FontOptions (11.5f)));
    setStatusForPath ("Session", session.getSessionDirectory());
    addAndMakeVisible (statusLabel);

    systemStatusBar = std::make_unique<SystemStatusBar> (engine);
    addAndMakeVisible (systemStatusBar.get());

    // Read the user's persisted "tape strip expanded by default"
    // preference from AppConfig (per-machine setting in the General
    // settings panel). Default off so the strips get full vertical
    // room unless the user explicitly opted in.
    tapeStripExpanded = appconfig::getTapeStripExpandedDefault();
    tapeStrip = std::make_unique<TapeStrip> (session, engine);
    tapeStrip->setVisible (tapeStripExpanded);
    tapeStrip->setChaseEnabled (appconfig::getFollowPlayheadDefault());
    tapeStrip->onMidiRegionDoubleClicked  = [this] (int t, int r) { openPianoRoll   (t, r); };
    tapeStrip->onAudioRegionDoubleClicked = [this] (int t, int r) { openAudioEditor (t, r); };
    tapeStrip->onFilesDropped = [this] (juce::Array<juce::File> files,
                                          std::int64_t timelineStart,
                                          int trackHint)
    {
        if (! engine.getTransport().isStopped())
        {
            showImportError ("Import",
                              "Stop playback before importing files.");
            return;
        }
        if (files.size() > 1)
            openMultiImportPicker (std::move (files), timelineStart);
        else
            enqueueImports (std::move (files), timelineStart, trackHint);
    };
    addAndMakeVisible (tapeStrip.get());

    // Song-map ribbon shown only when the tape strip is collapsed (resized()
    // owns its bounds + visibility from tapeStripExpanded / fullscreen state).
    miniTimeline = std::make_unique<MiniTimelineStrip> (session, engine);
    addChildComponent (miniTimeline.get());
    // Double-clicking a marker on the mini strip renames it (same flow as the
    // tape strip's marker rename, so undo + the modal are shared).
    miniTimeline->onMarkerEdit = [this] (int idx)
    {
        if (tapeStrip != nullptr) tapeStrip->promptRenameMarker (idx, "Rename marker");
    };

    // Sync the transport-bar TAPE toggle with the collapsed default.
    if (transportBar != nullptr)
        transportBar->setTapeStripExpanded (tapeStripExpanded);

    // Intentionally NO auto-load on startup. Standard DAW behavior is to
    // start with a fresh session and require the user to explicitly Load. The
    // previous best-effort auto-load of ~/Music/Dusk Studio/Untitled/session.json
    // was confusing - settings (master fader, mutes, etc.) silently persisted
    // across launches. The startup dialog (launchStartupDialog) offers
    // New / Open Recent / Browse instead.

    consoleView = std::make_unique<ConsoleView> (session, engine);
    addAndMakeVisible (consoleView.get());
    // Propagate the persisted timeline-expanded state: tapeStrip visibility +
    // the transport toggle were set above, but the console strips' compact
    // mode wasn't - without this they start full-height even when the timeline
    // is expanded on launch.
    consoleView->setStripsCompactMode (tapeStripExpanded);
    consoleView->setOnStripFocusRequested ([this] (int t)
    {
        if (tapeStrip != nullptr) tapeStrip->setSelectedTrack (t);
    });

    // Initial stage is Recording (engine default + recordingStageBtn
    // toggled on) - sync strip controls so the first paint shows
    // input / IN / ARM / PRINT instead of aux sends. switchToStage()
    // handles subsequent changes.
    consoleView->setStripsMixingMode (engine.getStage() == AudioEngine::Stage::Mixing);

    // Auto-size to screen: prefer the reference size (1440x1280) but shrink to
    // fit smaller displays. The ConsoleView itself reflows responsively.
    const int kPreferredW = 1440;
    const int kPreferredH = 1280;
    const int kTopBarH    = 32 + 8 + 16;  // settings button + status row + outer padding

    int w = kPreferredW;
    int h = kPreferredH;
    if (auto* primary = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
    {
        const auto userArea = primary->userArea;
        w = std::min (kPreferredW, userArea.getWidth()  - 24);
        h = std::min (kPreferredH, userArea.getHeight() - 24);
    }

    const int minContentW = consolelayout::responsiveMinimumMainComponentWidth();
    const int minContentH = 480 + kTopBarH;
    w = std::max (w, minContentW);
    h = std::max (h, minContentH);

    // Gate the startup plugin scan BEFORE setSize(): setSize drives a
    // synchronous resized() -> maybeStartStartupPluginScan(), and if the
    // dialog-pending flag isn't set yet that call latches startupScanTriggered
    // and shows the scan modal over the still-blank canvas, defeating the
    // startup-dialog gate. We're pending iff the picker will actually appear:
    // no DUSKSTUDIO_LOAD_SESSION and no DUSKSTUDIO_SKIP_STARTUP_DIALOG.
    {
        const char* loadPathEnv = std::getenv ("DUSKSTUDIO_LOAD_SESSION");
        const bool willLoadSession = (loadPathEnv != nullptr && *loadPathEnv);
        startupDialogPending = (! willLoadSession
                                && std::getenv ("DUSKSTUDIO_SKIP_STARTUP_DIALOG") == nullptr
                                && std::getenv ("DUSKSTUDIO_CAPTURE_DIR") == nullptr);
    }

    setSize (w, h);

    // Dev affordance for screenshot / xdotool flows: set DUSKSTUDIO_START_STAGE=mastering
    // to land in Mastering at startup. No-op in normal use.
    if (auto envStage = std::getenv ("DUSKSTUDIO_START_STAGE"))
    {
        const juce::String s (envStage);
        if (s.equalsIgnoreCase ("mastering"))
            switchToStage (AudioEngine::Stage::Mastering);
        else if (s.equalsIgnoreCase ("recording"))
            switchToStage (AudioEngine::Stage::Recording);
    }

    // Receive keyboard input - needed for Ctrl+Z / Ctrl+Y. Children
    // (text editors, sliders) still grab focus when interacted with;
    // this just ensures clicks on empty canvas land here.
    setWantsKeyboardFocus (true);

    // Register as the global focus-restore target for EmbeddedModal.
    // Every embedded popup / alert / combo / context menu calls back
    // here on close so transport-key hotkeys (spacebar, R) keep
    // working without the user needing to click the canvas first.
    EmbeddedModal::focusRestoreTarget() = this;

    // Route the MIDI-Learn right-click menu through our in-window renderer
    // instead of a native juce::PopupMenu (which flashes / mis-dismisses on
    // X11 + Wayland). Set once; lives for the app's lifetime.
    midilearn::setLearnMenuShowHook ([] (const juce::PopupMenu& menu, juce::Component& target)
    {
        duskstudio::showContextMenu (menu, target);
    });

    // Surface a "can't record" reason (no armed track / no device) as an
    // in-window alert instead of only logging it to stderr. Covers every
    // record trigger - on-screen Record, the R key, and the MCU Record
    // button - since they all funnel through engine.record().
    engine.setRecordBlockedSink ([this] (juce::String msg)
    {
        showDuskAlert (*this, "Cannot record", msg);
    });

    // Hot-unplug (H5): surface the runtime device-lost message in-window
    // instead of only logging it. Distinct title from the startup alert below.
    engine.setDeviceLostAlertSink ([this] (juce::String msg)
    {
        showDuskAlert (*this, "Audio device disconnected", msg);
    });

    // Surface deferred session restores and later device/render-rate
    // reactivation failures through the same detailed in-window path used by
    // immediate restores.
    engine.setPluginRestoreAlertSink ([this] (std::vector<AudioEngine::PluginLoadFailure> failures)
    {
        auto body = pluginRestoreFailureBody (failures);
        juce::Component::SafePointer<MainComponent> safeThis (this);
        dusk::callAsync (
            [body = std::move (body), safeThis]
            {
                if (auto* self = safeThis.getComponent())
                    showDuskAlert (*self, "Plug-in unavailable", body);
            });
    });

    // The notepad and an embedded modal must never share the window (see
    // beforeModalShown). The notepad saves whenever it closes, so the alerts
    // that can fire over it - sample-rate mismatch, device lost - take it back
    // rather than open where nobody can see or click them.
    EmbeddedModal::beforeModalShown() = [this]
    {
        yieldNotepadWindow();
       #if DUSKSTUDIO_HAS_NATIVE_UI
        // Every native view is an opaque child over the shell, so a modal that opens
        // behind one is invisible and unclickable - not just the notepad. The panels
        // that a user opened deliberately step aside the way it does. The startup
        // dialog is left alone: dismissing it would spend the session choice nobody
        // has made yet, and it is never up alongside these two.
        closeVirtualKeyboard();
        closeAudioSettings();
        if (consoleView != nullptr)
            for (int t = 0; t < Session::kNumTracks; ++t)
                if (auto* strip = consoleView->getStripComponent (t))
                    strip->closeCompEditorPopup();
       #endif
    };

    // Startup: if the saved audio device was in use (held by PipeWire / JACK /
    // another DAW), the engine ctor already tried to fall back to a working one.
    // Tell the user what happened - switched to another device, or left with
    // none - with a route into Audio Settings. Deferred so it paints over the
    // main window, not a blank canvas. consume clears it (one-shot).
    if (auto deviceMsg = engine.consumeStartupDeviceMessage(); deviceMsg.isNotEmpty())
    {
        juce::Component::SafePointer<MainComponent> safeThis (this);
        dusk::callAsync ([safeThis, deviceMsg]
        {
            if (auto* self = safeThis.getComponent())
                showDuskConfirm (*self, "Audio device unavailable", deviceMsg,
                                 "Open Audio Settings", [self] { self->openAudioSettings(); },
                                 "Continue", [] {});
        });
    }

    // Defer the startup dialog to the next message-loop tick so the main
    // window paints first - otherwise the dialog can pop up over a blank
    // canvas. SafePointer guards the case where the user closes the app
    // before the message loop catches up to the queued lambda.
    // DUSKSTUDIO_SKIP_STARTUP_DIALOG=1 bypasses the picker for screenshot /
    // xdotool flows.
    // DUSKSTUDIO_LOAD_SESSION=/path/to/session.json bypasses the startup
    // dialog and loads the named session immediately. Useful for
    // benchmarking the load path (the [Dusk Studio/Load] timing line ends
    // up in the parent terminal) and for scripted reproductions of
    // user-reported regressions.
    if (const char* loadPath = std::getenv ("DUSKSTUDIO_LOAD_SESSION");
        loadPath != nullptr && *loadPath)
    {
        juce::Component::SafePointer<MainComponent> safeThis (this);
        juce::String pathStr (loadPath);
        dusk::callAsync ([safeThis, pathStr]
        {
            if (safeThis != nullptr)
                safeThis->loadSessionFromJson (juce::File (pathStr));
        });
    }
    else if (std::getenv ("DUSKSTUDIO_SKIP_STARTUP_DIALOG") == nullptr
             && std::getenv ("DUSKSTUDIO_CAPTURE_DIR") == nullptr)
    {
        // startupDialogPending was already set before setSize() above (so the
        // scan-kicking resized() saw the gate); just queue the dialog here.
        // Capture mode suppresses the picker too - its modal would overlay
        // the snapshots (mirrors the CAPTURE_DIR guard in the scan path).
        juce::Component::SafePointer<MainComponent> safeThis (this);
        dusk::callAsync ([safeThis]
        {
            if (safeThis != nullptr) safeThis->launchStartupDialog();
        });
    }

    // DUSKSTUDIO_CAPTURE_DIR=/path drives the screenshot-capture harness:
    // synthesise a demo session, snapshot each documented view / strip /
    // modal into that directory, then quit. Suppress the startup plugin
    // scan so its modal doesn't overlay the shots; delay so the window is
    // shown + laid out before the first snapshot.
    if (const char* capDir = std::getenv ("DUSKSTUDIO_CAPTURE_DIR");
        capDir != nullptr && *capDir)
    {
        // The startup scan is suppressed in maybeStartStartupPluginScan() via
        // the same env var (resized() can fire mid-ctor, so a member flag set
        // here would be too late).
        juce::String dirStr (capDir);
        juce::Component::SafePointer<MainComponent> safeThis (this);
        dusk::Timer::callAfterDelay (1500, [safeThis, dirStr]
        {
            if (safeThis != nullptr)
                safeThis->captureScreenshots (juce::File (dirStr));
        });
    }

    // Cross-OS cursor overlay. Mounted last so it sits above every
    // other MainComponent child + paints the Grab / Cut / Draw glyph
    // at the mouse position. The editors push their local mouse
    // position to it via callbacks (component-to-component conversion
    // via the JUCE tree - Wayland's screen coords are broken for
    // Desktop::getMousePosition / Component::getScreenPosition, so
    // we cannot rely on screen-space polling).
    cursorOverlay = std::make_unique<CursorOverlay>();
    addAndMakeVisible (cursorOverlay.get());
    // Size it now - setSize() above already ran resized() before this overlay
    // existed, so without this it sits at 0x0 (invisible glyph) until the
    // first window resize.
    cursorOverlay->setBounds (getLocalBounds());

    // Feed the TapeStrip's Grab/Cut pointer into the overlay - same sink the
    // audio/MIDI editors use. Without this the strip hides the native cursor
    // but nothing paints the glyph, so the pointer vanishes over the lanes.
    if (tapeStrip != nullptr)
    {
        tapeStrip->onMouseMovedForCursor =
            [this] (juce::Component& src, juce::Point<int> localInSrc, EditMode m,
                    juce::Range<int> cutLine)
            {
                if (cursorOverlay != nullptr)
                    cursorOverlay->setMousePosition (src, localInSrc, m, cutLine);
            };
        tapeStrip->onMouseExitedForCursor =
            [this] { if (cursorOverlay != nullptr) cursorOverlay->clearMousePosition(); };
    }

    // Autosave heartbeat. Writes session.json.autosave next to the canonical
    // session.json on the configured cadence (default 30 s) so a crash loses at most
    // ~30 s of work. The write is atomic (temp + rename), so even a kill
    // during the timer fire never leaves a half-written autosave.
    startTimer (appconfig::getAutosaveIntervalSeconds() * 1000);

    // Seed the dirty-compare baseline for the bootstrap "Untitled" session so
    // edits to a brand-new, never-saved session are caught at quit instead of
    // only after the first 30 s autosave (the quit check skips when this is
    // empty). saveSessionTo / finishLoadingSessionFrom reseed it on Save As /
    // load; an async startup-dialog load just overwrites this blank baseline.
    if (lastSavedSessionJson.isEmpty())
        lastSavedSessionJson = SessionSerializer::serialize (session);
}

MainComponent::~MainComponent()
{
    tearingDown = true;
    stopTimer();   // halt autosave before tearing down engine / session
    engine.setPluginRestoreAlertSink ({});

    // Drop the modal hook before anything else: its closure holds a raw this,
    // and the teardown below can still raise an alert.
    EmbeddedModal::beforeModalShown() = nullptr;

    // The notepad owns its own DGL application and embedded native child. Drop it
    // while the main peer and message loop are still alive, and suppress its
    // normal close callback because the component is already being destroyed.
    dismissNotepad (false);

    // Close every plugin editor window (real top-level juce::DocumentWindows
    // with their own native X11 peers) BEFORE the rest of the UI cascade
    // tears down. Without this, ChannelStripComponent destructors run
    // their dropPluginEditor() inside Mutter's own teardown of our main
    // window, which on Linux/Wayland race-crashes the compositor. Belt-
    // and-suspenders for any quit path that doesn't go through
    // requestQuit's onSave / onDontSave handlers above (e.g. SIGTERM,
    // window-manager-initiated kill).
    if (consoleView != nullptr)
        consoleView->dropAllPluginEditors (NativeEditorTeardown::LeakForExit);

    // Aux native editors (CLAP, LV2, VST3) need the same early teardown while the
    // main peer + message loop are still alive. beginSafeShutdown() does this, but
    // a quit path that skips it (SIGTERM / WM kill) must still tear them down here
    // rather than leave them for the late ~AuxView cascade.
    if (auxView != nullptr)
        auxView->dropAllNativeEditors (NativeEditorTeardown::LeakForExit);

    // Sidecar flush for abnormal quit paths that bypass requestQuit's combined
    // session/notepad dirty prompt. Normal Save and Don't Save paths have
    // already cleared notepadDirty before reaching destruction.
    (void) saveNotepadNow();

    // Force-delete any modal body we launched, synchronously. close()
    // would defer body destruction to the next message-loop tick - but
    // the dispatch loop has already exited on this path, so the deferred
    // lambda would run AFTER our AudioEngine + AudioDeviceManager are
    // gone. SelfTestPanel's destructor removes listeners from the engine and the
    // device manager; PluginScanModal's stops a worker that calls into the
    // engine-owned PluginManager; BounceDialog's cancels + joins a BounceEngine
    // rendering through the engine; DpImportProgressPanel holds a
    // DpImportJob& and a 20 Hz timer, so a leaked body outlives the job
    // it polls. All of those must run while the engine is still alive.
    // No body callback can be on the stack here (the dtor runs from app
    // shutdown), so in-place destruction is safe.
    midiBindingsModal    .closeAndDeleteBodyNow();
    selfTestModal        .closeAndDeleteBodyNow();
    mixdownModal         .closeAndDeleteBodyNow();
    bounceModal          .closeAndDeleteBodyNow();
    scanModal            .closeAndDeleteBodyNow();
    quitModal            .closeAndDeleteBodyNow();
    recoveryModal        .closeAndDeleteBodyNow();
   #if DUSKSTUDIO_HAS_NATIVE_UI
    // The settings view listens to the engine and the device manager, so its window
    // goes down here rather than waiting for a deferred teardown that would run after
    // both are gone.
    audioSettingsHandOff = nullptr;
    audioSettingsWindow.reset();
    audioSettingsDim.reset();
    virtualKeyboardWindow.reset();
    virtualKeyboardDim.reset();
   #endif
    importTargetModal    .closeAndDeleteBodyNow();
    shortcutsModal       .closeAndDeleteBodyNow();
    supportersModal      .closeAndDeleteBodyNow();
    dpImportProgressModal.closeAndDeleteBodyNow();

    // Intentionally NO auto-save here. Standard DAW behavior is to require
    // an explicit Save before exit. The previous auto-save on destruct
    // paired with auto-load on construct caused settings (master fader
    // position, mutes, etc.) to silently persist across launches. Use the
    // Save button when you want to keep state.

    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0d0d0f));
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    auto& um = engine.getUndoManager();
    const auto mods    = key.getModifiers();
    int        code    = key.getKeyCode();
    // On this Linux/X11 (JUCE-wayland) build getKeyCode() returns the actual
    // XLookupString glyph, so unmodified letters arrive lowercase ('m', not
    // 'M') and every uppercase comparison below would silently miss. Normalise
    // ASCII letters to uppercase - a no-op on macOS/Windows where getKeyCode()
    // is already uppercase. Brackets / digits / symbols are untouched.
    if (code >= 'a' && code <= 'z') code -= ('a' - 'A');
    const bool cmd     = mods.isCommandDown();   // Ctrl on Linux/Windows, Cmd on macOS
    const bool shift   = mods.isShiftDown();
    const bool noMods  = ! cmd && ! shift && ! mods.isAltDown();

    // Edit-mode shortcuts (Ardour-style). 'G' picks Grab Mode so the
    // user can flip back to move/select after a Range or Cut detour. No
    // modifiers so it never collides with the Cmd+letter clipboard ops.
    if (code == 'G' && noMods)
    {
        session.editMode = EditMode::Grab;
        if (audioEditor != nullptr) audioEditor->syncEditModeToolbar();
        if (pianoRoll   != nullptr) pianoRoll->syncEditModeToolbar();
        if (tapeStrip   != nullptr) { tapeStrip->refreshModeCursor(); tapeStrip->repaint(); }
        return true;
    }

    // Bank switching: plain 1 through 8 select the visible channel page. Maps to
    // ConsoleView::setBank which also publishes the active bank to the audio
    // thread so bank-relative MIDI bindings retarget. Out-of-range digits (more
    // banks than the window currently shows) fall through unhandled.
    if (noMods && consoleView != nullptr)
    {
        const int bankIndex = screenPageForDigitKey (code, consoleView->numBanks());
        if (bankIndex >= 0)
        {
            consoleView->setBank (bankIndex);
            return true;
        }
    }

    // Stage switching: Cmd/Ctrl + 1/2/3/4 select Recording / Mixing /
    // Mastering / Aux. Plain digits are bank switching (above).
    if (cmd && ! shift)
    {
        switch (code)
        {
            case '1': switchToStage (AudioEngine::Stage::Recording); return true;
            case '2': switchToStage (AudioEngine::Stage::Mixing);    return true;
            case '3': switchToStage (AudioEngine::Stage::Mastering); return true;
            case '4': switchToStage (AudioEngine::Stage::Aux);       return true;
            default:  break;
        }
    }

    // Shift+Left / Shift+Right mirror the transport Rewind / Forward taps:
    // previous / next marker, with the buttons' stopped-and-empty fallbacks
    // (start of session / last record point). Cmd+arrows are region nudge,
    // plain arrows are strip focus - this claims the remaining pair.
    if (shift && ! cmd && ! mods.isAltDown()
        && (key == juce::KeyPress::leftKey || key == juce::KeyPress::rightKey))
    {
        const bool back = key == juce::KeyPress::leftKey;
        if (engine.getTransport().isStopped() && session.getMarkers().empty())
        {
            if (back) engine.jumpToZero();
            else      engine.jumpToLastRecordPoint();
        }
        else if (back) engine.jumpToPrevMarker();
        else           engine.jumpToNextMarker();
        return true;
    }

    // Plain Left/Right move the channel-strip focus ring across the 24
    // strips (Recording / Mixing stages), auto-flipping the visible bank at a
    // boundary. The focused strip becomes the A / S / X target. Cmd+arrows are
    // region nudge (handled above), so the unmodified form is free here.
    if (noMods && consoleView != nullptr
        && (key == juce::KeyPress::leftKey || key == juce::KeyPress::rightKey)
        && (engine.getStage() == AudioEngine::Stage::Recording
            || engine.getStage() == AudioEngine::Stage::Mixing))
    {
        consoleView->moveFocus (key == juce::KeyPress::leftKey ? -1 : 1);
        return true;
    }

    // '?' (Shift+/) opens the keyboard-shortcut reference.
    if (key.getTextCharacter() == '?')
    {
        openShortcuts();
        return true;
    }

    // TIMELINE toggle: T (or Cmd/Ctrl + \) shows / hides the tape strip.
    // Mirrors the TransportBar's TIMELINE button so the user can flip the
    // arrangement view without mousing. Plain T is the mnemonic ("Timeline");
    // \\ is the Reaper / Pro Tools-style alias. (Alt+T is take cycling below.)
    if ((code == 'T' && noMods)
        || (cmd && ! shift && key.getTextCharacter() == '\\'))
    {
        setTimelineVisible (! tapeStripExpanded);
        return true;
    }

    // Take cycling. Alt+T = forward (next take), Alt+Shift+T = backward.
    // Routes through TapeStrip's selection state; no-op when no region
    // is selected or the selection has no take history. T (plain) is
    // already claimed by split-at-playhead, hence the Alt modifier.
    if (code == 'T' && mods.isAltDown() && ! cmd)
    {
        if (tapeStrip != nullptr)
        {
            const bool ok = shift ? tapeStrip->cycleSelectedTakeBackward()
                                   : tapeStrip->cycleSelectedTakeForward();
            if (ok) return true;
        }
    }

    // TapeStrip zoom: '=' / '+' zoom in, '-' zoom out, '0' fit.
    // Skipped when a modal editor (audio / piano roll) has focus -
    // those have their own zoom keypress paths and grab focus first.
    if (tapeStrip != nullptr && audioEditor == nullptr && pianoRoll == nullptr)
    {
        const auto ch = key.getTextCharacter();
        if (noMods && (ch == '=' || ch == '+'))
        {
            tapeStrip->zoomByFactor (1.15f);
            return true;
        }
        if (noMods && ch == '-')
        {
            tapeStrip->zoomByFactor (1.0f / 1.15f);
            return true;
        }
        if (noMods && ch == '0')
        {
            tapeStrip->zoomFit();
            return true;
        }
    }

    // Edit: Ctrl/Cmd+Z, Ctrl/Cmd+Shift+Z, Ctrl/Cmd+Y
    if (code == 'Z' && cmd && ! shift) { um.undo(); return true; }
    if ((code == 'Z' && cmd && shift) || (code == 'Y' && cmd))
    {
        um.redo();
        return true;
    }

    // Region clipboard: Ctrl/Cmd+C, Ctrl/Cmd+X, Ctrl/Cmd+V; Delete
    // Each routes through TapeStrip, which owns the selection state. They
    // no-op when nothing's selected (or for paste, when the clipboard's
    // empty), letting the keypress fall through to default handling.
    if (tapeStrip != nullptr)
    {
        if (code == 'C' && cmd && ! shift)
        {
            if (tapeStrip->copySelectedRegion()) return true;
        }
        if (code == 'X' && cmd && ! shift)
        {
            if (tapeStrip->cutSelectedRegion()) return true;
        }
        if (code == 'V' && cmd && ! shift)
        {
            if (tapeStrip->pasteAtPlayhead()) return true;
        }
        if ((key == juce::KeyPress::deleteKey
             || key == juce::KeyPress::backspaceKey)
            && noMods)
        {
            if (tapeStrip->deleteSelectedRegion()) return true;
        }
        // Cmd/Ctrl+E splits the selected region at the playhead - razor
        // without a tool mode. Pro Tools / Ableton convention, uniform with
        // the audio + MIDI editors. No-op when nothing is selected or the
        // playhead is outside the region. (Plain T is the timeline toggle.)
        if (code == 'E' && cmd && ! shift)
        {
            if (tapeStrip->splitSelectedAtPlayhead()) return true;
        }
        // Cmd/Ctrl+D duplicates the selected region. The piano roll's
        // own Cmd+D handler runs first when the modal is open (its
        // keyPressed has priority via JUCE focus), so this only fires
        // for tape-strip selection. Falls through harmlessly when
        // nothing is selected.
        if (code == 'D' && cmd && ! shift)
        {
            if (tapeStrip->duplicateSelectedRegion()) return true;
        }
        // Cmd/Ctrl + Left/Right nudges the selected region by 1 beat;
        // Shift adds for 1 bar. Computed from the live tempo so the
        // nudge matches the user's musical grid. Cmd was already used
        // for clipboard / save / open so it composes cleanly with
        // arrow keys (no existing binding).
        if ((key == juce::KeyPress::leftKey || key == juce::KeyPress::rightKey)
            && cmd)
        {
            const double sr   = engine.getCurrentSampleRate();
            const float  bpm  = session.tempoBpm.load (std::memory_order_relaxed);
            if (sr > 0.0 && bpm > 0.0f)
            {
                const int beatsPerBar = std::max (1,
                    session.beatsPerBar.load (std::memory_order_relaxed));
                const double beatSamples = sr * 60.0 / (double) bpm;
                const double stepSamples = shift ? beatSamples * (double) beatsPerBar
                                                  : beatSamples;
                const std::int64_t delta = (std::int64_t) std::round (stepSamples);
                const std::int64_t signedDelta = key == juce::KeyPress::leftKey ? -delta : delta;
                if (tapeStrip->nudgeSelectedRegion (signedDelta)) return true;
            }
        }
    }

    // File: Ctrl/Cmd+S (save), Ctrl/Cmd+Shift+S (save as), Ctrl/Cmd+O (open)
    // Buttons are gone (replaced by the menu bar) - dispatch to menu IDs
    // directly so the keyboard path keeps working.
    if (code == 'S' && cmd && ! shift) { menuItemSelected (1003, 0); return true; }   // Save
    if (code == 'S' && cmd &&   shift) { menuItemSelected (1004, 0); return true; }   // Save as
    if (code == 'O' && cmd)            { menuItemSelected (1002, 0); return true; }   // Open
    if (code == 'N' && cmd && ! shift) { menuItemSelected (1001, 0); return true; }   // New session
    if (code == 'I' && cmd && ! shift) { menuItemSelected (1006, 0); return true; }   // Import
    if (code == 'Q' && cmd)            { menuItemSelected (1099, 0); return true; }   // Quit

    // Bounce: Ctrl/Cmd+B (Logic-style; intuitive "B for Bounce")
    if (code == 'B' && cmd) { menuItemSelected (1011, 0); return true; }              // Bounce

    // Transport: Space toggles play/stop, R toggles record
    // Pro Tools / Reaper / Logic / Bitwig all use Space as the universal
    // play-stop toggle. R for record matches Pro Tools / Cubase. Both
    // require no modifiers so a focused button or text editor still owns
    // the key.
    if (key == juce::KeyPress::spaceKey && noMods)
    {
        // In the Mastering stage the audible source is the standalone mixdown
        // player, not the multitrack transport - so space drives that instead.
        if (engine.getStage() == AudioEngine::Stage::Mastering)
        {
            auto& mp = engine.getMasteringPlayer();
            if      (mp.isPlaying()) mp.stop();
            else if (mp.isLoaded())  mp.play();
            return true;
        }
        auto& transport = engine.getTransport();
        if (transport.isStopped()) engine.play();
        else                       { engine.stop(); if (transportBar != nullptr) transportBar->notifyRecordStopped(); }
        return true;
    }
    if (code == 'R' && noMods)
    {
        auto& transport = engine.getTransport();
        if (transport.isRecording()) { engine.stop(); if (transportBar != nullptr) transportBar->notifyRecordStopped(); }
        else                         engine.record();
        return true;
    }

    // Virtual MIDI Keyboard: K toggles the embedded VKB modal so the
    // user's typing keyboard becomes a MIDI input source. The modal pushes
    // events into the synthetic "Virtual Keyboard (Dusk Studio)" device - to
    // hear notes, a track must select that device on its MIDI input
    // dropdown and have an instrument plugin loaded.
    if (code == 'K' && noMods)
    {
        toggleVirtualKeyboard();
        return true;
    }

    // Navigation: Home -> playhead to 0 (universal). End is intentionally
    // skipped because "end of timeline" isn't a fixed sample on a portastudio
    // - the timeline grows with the longest region.
    if (key == juce::KeyPress::homeKey)
    {
        engine.getTransport().setPlayhead (0);
        return true;
    }

    // '.' (period) -> stop transport and rewind to 0. Pro Tools / Cubase
    // convention. Mirrors the Stop button on the transport bar with the
    // added rewind that the bare Stop doesn't provide.
    if (key.getTextCharacter() == '.' && noMods)
    {
        auto& tr = engine.getTransport();
        if (! tr.isStopped())
        {
            engine.stop();
            if (transportBar != nullptr) transportBar->notifyRecordStopped();
        }
        tr.setPlayhead (0);
        return true;
    }

    // Markers: 'M' (no modifiers) drops a marker at the current playhead.
    // Common DAW shortcut. Skips when typing - the noMods guard means this
    // only fires when no text editor has focus.
    if (code == 'M' && noMods)
    {
        const auto playhead = engine.getTransport().getPlayhead();
        um.beginNewTransaction ("Add marker");
        auto* add = new AddMarkerAction (session, playhead);
        // The UndoManager owns the action - and DELETES it if perform()
        // fails - so only dereference `add` after a successful perform.
        const bool added = um.perform (add);
        if (tapeStrip != nullptr)
        {
            tapeStrip->repaint();
            // Name-on-create: the input opens with the auto-generated name
            // pre-selected, so typing renames the fresh marker in one shot.
            // Escape keeps the default.
            if (added)
                tapeStrip->promptRenameMarker (add->insertedIndex(), "Name marker");
        }
        return true;
    }

    // Loop / punch: bracket keys set the current playhead as in/out;
    // L and P toggle the corresponding mode on/off. Shift+bracket switches
    // to punch boundaries; the unshifted form sets loop boundaries.
    auto& transport = engine.getTransport();
    // On Linux/X11 getKeyCode() returns the SHIFTED glyph (XLookupString
    // applies modifiers), so Shift+[ arrives as '{' and Shift+] as '}'.
    // Accept both forms or the punch (Shift+bracket) shortcuts never fire.
    if ((code == '[' || code == '{') && ! cmd && ! mods.isAltDown())
    {
        const auto playhead = transport.getPlayhead();
        if (shift)
        {
            const auto end = transport.getPunchOut();
            transport.setPunchRange (playhead,
                                       end > playhead ? end : playhead);
        }
        else
        {
            const auto end = transport.getLoopEnd();
            transport.setLoopRange (playhead,
                                      end > playhead ? end : playhead);
        }
        if (tapeStrip != nullptr) tapeStrip->repaint();
        return true;
    }
    if ((code == ']' || code == '}') && ! cmd && ! mods.isAltDown())
    {
        const auto playhead = transport.getPlayhead();
        if (shift)
        {
            const auto start = transport.getPunchIn();
            transport.setPunchRange (start < playhead ? start : playhead,
                                       playhead);
        }
        else
        {
            const auto start = transport.getLoopStart();
            transport.setLoopRange (start < playhead ? start : playhead,
                                      playhead);
        }
        if (tapeStrip != nullptr) tapeStrip->repaint();
        return true;
    }
    if (code == 'L' && noMods)
    {
        transport.setLoopEnabled (! transport.isLoopEnabled());
        if (tapeStrip != nullptr) tapeStrip->repaint();
        return true;
    }
    if (code == 'P' && noMods)
    {
        transport.setPunchEnabled (! transport.isPunchEnabled());
        if (tapeStrip != nullptr) tapeStrip->repaint();
        return true;
    }

    // Metronome click toggle: 'C' (no modifiers). Matches the C/I
    // (count-in) abbreviation already used on the transport bar's button.
    // The TransportBar's clickToggle button polls the same atom on its
    // 30 Hz timer and re-syncs its visual state.
    if (code == 'C' && noMods)
    {
        auto& enabled = session.metronomeEnabled;
        enabled.store (! enabled.load (std::memory_order_relaxed),
                        std::memory_order_relaxed);
        return true;
    }

    // Count-in toggle: Shift+C (pairs with plain C's metronome). The
    // transport bar's C/I button re-syncs from the atom on its timer.
    if (code == 'C' && shift && ! cmd && ! mods.isAltDown())
    {
        auto& ci = session.countInEnabled;
        ci.store (! ci.load (std::memory_order_relaxed), std::memory_order_relaxed);
        return true;
    }

    // Remaining transport-bar controls, one key each so every transport
    // control is reachable without the mouse: B = tap tempo ("BPM"),
    // Shift+M = time-signature menu (plain M drops a marker), U = tuner,
    // F = time/bars display format.
    if (transportBar != nullptr)
    {
        if (code == 'B' && noMods)                            { transportBar->tapTempo();         return true; }
        if (code == 'M' && shift && ! cmd && ! mods.isAltDown()) { transportBar->openTimeSigMenu();  return true; }
        if (code == 'F' && noMods)                            { transportBar->toggleTimeFormat(); return true; }
    }
    if (code == 'U' && noMods)
    {
        toggleTuner();
        return true;
    }

    // Track arm / solo / mute on the selected track. Selection state
    // lives on TapeStrip (the most-recently-clicked region's track); when
    // nothing's selected, the shortcuts no-op rather than guessing. The
    // ChannelStrip's existing 30 Hz timer picks up the atom changes and
    // refreshes its toggles.
    if (tapeStrip != nullptr)
    {
        const int sel = tapeStrip->getSelectedTrack();
        if (sel >= 0 && sel < Session::kNumTracks)
        {
            if (code == 'A' && noMods)
            {
                // Route through setTrackArmed so armedTrackCount stays
                // in sync. Bypassing it (atom.store directly) left the
                // counter stale and silently broke the Rec button's
                // anyTrackArmed() fast-path.
                const bool now = session.track (sel).recordArmed
                                       .load (std::memory_order_relaxed);
                session.setTrackArmed (sel, ! now);
                return true;
            }
            if (code == 'S' && noMods)
            {
                // Route through setTrackSoloed so soloTrackCount stays
                // in sync (same reason as ARM above).
                const bool now = session.track (sel).strip.solo
                                       .load (std::memory_order_relaxed);
                session.setTrackSoloed (sel, ! now);
                return true;
            }
            // 'X' = mute toggle. M is already taken by drop-marker; X is
            // mnemonic for "kill / cross out" and matches Reaper's mute
            // keybinding.
            if (code == 'X' && noMods)
            {
                auto& m = session.track (sel).strip.mute;
                m.store (! m.load (std::memory_order_relaxed),
                          std::memory_order_relaxed);
                return true;
            }
        }
    }

    // Window: F11 and View > Full Screen share the same path.
    if (key == juce::KeyPress::F11Key)
        return toggleFullScreen();

    return false;
}

bool MainComponent::toggleFullScreen()
{
    // MainComponent owns the menu and shortcuts, while the parent
    // ResizableWindow owns the native OS state. Keeping the transition here
    // gives View and F11 identical peer-recreation behaviour.
    if (auto* window = findParentComponentOfClass<juce::ResizableWindow>())
    {
        window->setFullScreen (! window->isFullScreen());
        return true;
    }
    return false;
}

void MainComponent::setStatusForPath (const juce::String& prefix,
                                          const juce::File& path,
                                          bool isAutosave)
{
    // Prefer the session-dir name (which the user picked) over the
    // session.json filename (always literal "session.json"). Falls
    // back to the file's own name when path == a non-session file
    // (e.g. error states pointing at a missing dir).
    auto display = path.getParentDirectory().getFileName();
    if (display.isEmpty()) display = path.getFileName();
    juce::String text = prefix + ": " + display;
    if (isAutosave) text += "  (autosave)";
    statusLabel.setText (text, juce::dontSendNotification);
    statusLabel.setTooltip (path.getFullPathName());
}

void MainComponent::setStatusText (const char* text)
{
    statusLabel.setText (text, juce::dontSendNotification);
    statusLabel.setTooltip ({});
}

void MainComponent::syncBankButtons (int desiredCount)
{
    // Grow / shrink the bank-button vector to match. Shrinking destroys
    // the trailing buttons (removes them from the parent automatically
    // via ~Component). Each fresh button is wired with the Dusk bank
    // style + click handler that hops the console to that bank index.
    while ((int) bankButtons.size() > desiredCount)
        bankButtons.pop_back();

    while ((int) bankButtons.size() < desiredCount)
    {
        const int idx = (int) bankButtons.size();
        auto btn = std::make_unique<juce::TextButton>();
        btn->setClickingTogglesState (true);
        btn->setRadioGroupId (1002);
        btn->setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
        btn->setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd0a060));
        btn->setColour (juce::TextButton::textColourOffId,  juce::Colours::lightgrey);
        btn->setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
        btn->onClick = [this, idx]
        {
            if (consoleView != nullptr) consoleView->setBank (idx);
        };
        // Every possible screen page is reachable through the plain digits 1-8.
        btn->setTooltip (dusk::text::format ("Channel page %d (press %d)",
                                             idx + 1, idx + 1));
        addAndMakeVisible (btn.get());
        bankButtons.push_back (std::move (btn));
    }
}

void MainComponent::maybeStartStartupPluginScan()
{
    // Screenshot-capture mode never scans - its progress modal would overlay
    // the full-window snapshots. Guard on the env var directly because
    // resized() (which calls us) can fire mid-ctor, before the capture hook
    // gets a chance to latch any member flag.
    if (std::getenv ("DUSKSTUDIO_CAPTURE_DIR") != nullptr) return;

    if (startupScanTriggered) return;
    // Defer while the startup dialog is up - dismissStartupDialog() re-invokes
    // us once the user has picked a session. Don't latch startupScanTriggered
    // yet, or that later call would no-op. (resized() may call us repeatedly
    // meanwhile; each just returns here until the dialog closes.)
    if (startupDialogPending) return;
    // Defer past an open crash-recovery or unsaved-changes prompt so the scan's
    // progress modal doesn't stack over the user's decision. Both prompts'
    // exit paths re-invoke us once that choice is made.
    if (recoveryModal.isOpen() || quitModal.isOpen()) return;
    startupScanTriggered = true;   // fire exactly once, whatever the toggle says

    const bool enabled = appconfig::getScanPluginsOnStartup();
    std::fprintf (stderr, "[Dusk Studio] startup plugin scan: toggle=%s\n",
                  enabled ? "ON - deferring progress modal" : "off - skipped");
    std::fflush (stderr);
    if (! enabled)
        return;

    // Defer one tick: don't build/show a modal from inside resized() (it would
    // re-enter layout). By the next message-loop tick the window is shown and
    // sized, so the EmbeddedModal centres over real bounds.
    juce::Component::SafePointer<MainComponent> safe (this);
    dusk::callAsync ([safe]
    {
        auto* self = safe.getComponent();
        if (self == nullptr) return;

        std::fprintf (stderr, "[Dusk Studio] startup plugin scan: showing progress modal\n");
        std::fflush (stderr);

        auto& mgr = self->engine.getPluginManager();
        auto body = std::make_unique<PluginScanModal> (mgr, [safe] (int added, bool cancelled)
        {
            auto* s = safe.getComponent();
            if (s == nullptr) return;
            s->scanModal.close();
            auto& m = s->engine.getPluginManager();
            const int total = m.getEffectDescriptions().size()
                            + m.getInstrumentDescriptions().size();
            std::fprintf (stderr,
                          "[Dusk Studio] Scan-on-startup: %s, added %d new plugins (%d total).\n",
                          cancelled ? "cancelled" : "finished", added, total);
            if (const int skipped = m.getLastScanSandboxSkips(); skipped > 0)
                std::fprintf (stderr,
                              "[Dusk Studio] Scan-on-startup: sandbox unavailable - %d plugin(s) left unscanned.\n",
                              skipped);
            std::fflush (stderr);
        });

        // No click-outside dismiss; the modal closes itself when the scan
        // completes. Esc cancels rather than dismissing: tearing the modal down
        // mid-scan would join the worker on the message thread.
        auto* scan = body.get();
        self->scanModal.show (*self, std::move (body),
                              /*onDismiss*/ [scan] { scan->requestCancel(); },
                              /*dismissOnClickOutside*/ false,
                              /*dismissOnEscape*/ true);
    });
}

void MainComponent::resized()
{
    // Kick the deferred scan-on-startup once the window has real bounds.
    maybeStartStartupPluginScan();

    if (cursorOverlay != nullptr)
        cursorOverlay->setBounds (getLocalBounds());

    auto area = getLocalBounds().reduced (8);

    // Top menu row. Grown to host the centered stage selector (RECORDING /
    // MIXING / MASTERING / AUX). File / View / Settings menus on the left, system meters
    // on the right; the stage selector and the session status text are placed
    // below, once the stage-tab width is known.
    constexpr int kMenuRowH = 32;
    const auto menuRow = area.removeFromTop (kMenuRowH);
    const int kMenuBarW = menuBar.getRequiredWidth();
    menuBar.setBounds (menuRow.withWidth (kMenuBarW));
    if (systemStatusBar != nullptr)
        systemStatusBar->setBounds (menuRow.withTrimmedLeft (menuRow.getWidth() - 300));
    area.removeFromTop (4);

    const auto curStage = engine.getStage();
    const bool inMastering = (curStage == AudioEngine::Stage::Mastering);
    const bool inAux       = (curStage == AudioEngine::Stage::Aux);
    const bool inFullscreenView = inMastering || inAux;

    // Combined transport / stage / bank row
    // Everything that used to live on three separate rows (stage selector,
    // bank buttons, transport bar) collapses into ONE row. The transport
    // bar paints the row's chrome + transport buttons + clock + right-edge
    // toggles; we overlay the stage selector centred on top of its hint
    // area, and bank buttons just to the left of the stage block. The
    // hint label is hidden so it doesn't render under the overlays.
    constexpr int kRowH = 44;
    juce::Rectangle<int> rowBounds;
    if (! inFullscreenView && transportBar != nullptr)
    {
        rowBounds = area.removeFromTop (kRowH);
        transportBar->setBounds (rowBounds);
        transportBar->setHintVisible (false);
    }
    else
    {
        // In mastering / aux the transport bar is hidden; the stage selector
        // still needs a row to live on. Reserve the same height so the stage
        // row doesn't jump positions when the user switches stages.
        rowBounds = area.removeFromTop (kRowH);
    }

    constexpr int kStageBtnH = 28;

    // Banking decision: ConsoleView reports how many channel strips fit
    // alongside the always-anchored bus + master column at min width.
    // When all 24 fit we render NO bank buttons (numBanks == 1). When
    // they don't fit, we render one button per bank with the actual
    // 1-based range as the label ("1-13", "14-16", etc.) - bank size
    // tracks the window width, not a fixed stride of 8.
    //
    // CRITICAL: use the WIDTH the console is ABOUT to receive (area's
    // current width), not consoleView->getWidth() - the latter reflects
    // the PREVIOUS resize pass, so a fast snap-to-half-screen would
    // render labels one frame stale (showed "1-6" with only 3 strips
    // visible).
    const int consoleTargetWidth = area.getWidth();
    if (consoleView != nullptr && ! inFullscreenView)
        consoleView->synchroniseBankStateForWidth (consoleTargetWidth);
    const int numBanks = (consoleView != nullptr && ! inFullscreenView)
                            ? ConsoleView::numBanksForWidth (consoleTargetWidth) : 1;
    const bool needsBanking = numBanks > 1;
    const bool transportCompact = rowBounds.getWidth() < TransportBar::kCompactTransportWidth;

    // Stage tabs shrink in compact mode so they don't crowd against the
    // (now centered) transport cluster at the OS minimum window width.
    const int stageW = transportCompact ? 92 : 110;
    const int stageBlockW = stageW * 4;
    const int preferredBankBtnW = transportCompact ? 70 : 100;
    constexpr int preferredBankBtnGap = 6;
    constexpr int kBankBtnH  = 26;

    // Stage selector lives in the TOP menu row, centered between the File /
    // View / Settings menus and the system meters. The session status text fills the
    // gap to its left. (stageW / stageBlockW were computed above with the
    // transport metrics so the tabs follow the same compact breakpoint.)
    // stageCentreX is reused below to centre the bank / toolbar group directly
    // under the stage selector.
    int stageCentreX = rowBounds.getCentreX();
    {
        const int menuStageY   = menuRow.getY() + (menuRow.getHeight() - kStageBtnH) / 2;
        const int menuLeftPad  = menuRow.getX() + kMenuBarW + 12;      // past File / View / Settings
        const int menuRightPad = menuRow.getRight()
                                   - (systemStatusBar != nullptr ? 300 + 12 : 0);
        int stageX = menuRow.getX() + (menuRow.getWidth() - stageBlockW) / 2;
        stageX = jlimit (menuLeftPad,
                                 std::max (menuLeftPad, menuRightPad - stageBlockW),
                                 stageX);
        stageCentreX = stageX + stageBlockW / 2;
        recordingStageBtn.setBounds (stageX,              menuStageY, stageW, kStageBtnH);
        mixingStageBtn   .setBounds (stageX + stageW,     menuStageY, stageW, kStageBtnH);
        masteringStageBtn.setBounds (stageX + 2 * stageW, menuStageY, stageW, kStageBtnH);
        auxStageBtn      .setBounds (stageX + 3 * stageW, menuStageY, stageW, kStageBtnH);

        // Help text in the gap between the menu bar and the tabs.
        const int statusW = std::max (0, stageX - 12 - menuLeftPad);
        statusLabel.setBounds (menuLeftPad, menuRow.getY(), statusW, menuRow.getHeight());

        // Parameter tooltips live in this same band; keep them off the tabs.
        lookAndFeel.setTooltipPlacement (
            juce::Rectangle<int> (menuLeftPad, menuRow.getY(),
                                    std::max (0, menuRightPad - menuLeftPad),
                                    menuRow.getHeight()),
            juce::Rectangle<int> (stageX, menuRow.getY(), stageBlockW, menuRow.getHeight()));
    }

    // Transport-row overlays: the bank buttons and, when the tape strip is
    // open, the timeline toolbar to their right - centred together as one group
    // directly under the stage selector above. The stage selector vacated this
    // row (it lives in the menu row now).
    constexpr int kHdrZoomBtnW = 30;
    constexpr int kHdrFitW     = 36;
    constexpr int kHdrSnapW    = 48;
    constexpr int kHdrSnapResW = 78;
    constexpr int kHdrChaseW   = 56;
    constexpr int kHdrBtnGap   = 4;
    constexpr int kHdrBtnH     = 24;
    const int kHdrClusterW = kHdrSnapW + kHdrSnapResW + kHdrZoomBtnW * 2
                           + kHdrFitW + kHdrChaseW + kHdrBtnGap * 5;
    constexpr int kBankToolbarGap = 16;

    syncBankButtons (needsBanking ? numBanks : 0);
    const int bankY = rowBounds.getY() + (rowBounds.getHeight() - kBankBtnH) / 2;
    const int clockRight = rowBounds.getX()
                         + (transportBar != nullptr ? transportBar->getClockRightX() : 0);
    const int utilityLeft = rowBounds.getX()
                         + (transportBar != nullptr ? transportBar->getUtilityClusterLeftX()
                                                      : rowBounds.getWidth());
    const int bankBandLeft  = clockRight + 12;
    const int bankBandRight = utilityLeft - 12;
    const int bankBandW = std::max (0, bankBandRight - bankBandLeft);

    // Up to eight three-strip pages can exist at the minimum window width.
    // Fit their buttons inside the actual clock-to-tuner band: reduce the gap
    // first, then the button width. Labels below follow the resulting width.
    const auto pageButtons = needsBanking
                           ? consolelayout::fitPageButtons (bankBandW, numBanks,
                                                            preferredBankBtnW,
                                                            preferredBankBtnGap)
                           : consolelayout::PageButtonMetrics {};
    const int bankBtnGap = pageButtons.gap;
    const int bankBtnW = pageButtons.width;
    const int bankClusterW = pageButtons.clusterWidth;

    // The toolbar joins the group only with the tape strip open AND when the
    // combined group still fits between the clock and the BPM / tuner cluster.
    const bool wantToolbar = ! inFullscreenView && tapeStrip != nullptr && tapeStripExpanded;
    int  groupW = bankClusterW;
    bool toolbarVisible = false;
    if (wantToolbar)
    {
        const int combinedW = bankClusterW + (needsBanking ? kBankToolbarGap : 0) + kHdrClusterW;
        const int gX = std::max (clockRight + 12, stageCentreX - combinedW / 2);
        if (gX + combinedW <= utilityLeft - 12)
        {
            toolbarVisible = true;
            groupW = combinedW;
        }
    }

    // Centre the group under the stage selector, clamped past the clock.
    const int groupX = std::clamp (stageCentreX - groupW / 2,
                                   bankBandLeft,
                                   std::max (bankBandLeft, bankBandRight - groupW));

    if (needsBanking && consoleView != nullptr)
    {
        int bankX = groupX;
        const int activeBank = consoleView->getBank();
        for (int i = 0; i < (int) bankButtons.size(); ++i)
        {
            auto* btn = bankButtons[(size_t) i].get();
            const auto range = ConsoleView::rangeForBankAtWidth (i, consoleTargetWidth);
            std::string label;
            if (bankBtnW >= 90 && ! transportCompact)
                label = dusk::text::format ("BANK %d  (%d-%d)", i + 1, range.first, range.second);
            else if (bankBtnW >= 44)
                label = dusk::text::format ("%d-%d", range.first, range.second);
            else
                label = dusk::text::format ("%d", i + 1);
            btn->setButtonText (label);
            btn->setTooltip (dusk::text::format (
                "Channel page %d: channels %d-%d (press %d)",
                i + 1, range.first, range.second, i + 1));
            btn->setBounds (bankX, bankY, bankBtnW, kBankBtnH);
            btn->setToggleState (i == activeBank, juce::dontSendNotification);
            bankX += bankBtnW + bankBtnGap;
        }
    }

    hdrZoomOutBtn.setVisible (toolbarVisible);
    hdrZoomInBtn .setVisible (toolbarVisible);
    hdrZoomFitBtn.setVisible (toolbarVisible);
    hdrSnapBtn   .setVisible (toolbarVisible);
    hdrSnapResBtn.setVisible (toolbarVisible);
    hdrChaseBtn  .setVisible (toolbarVisible);
    if (toolbarVisible)
    {
        int x = groupX + bankClusterW + (needsBanking ? kBankToolbarGap : 0);
        const int cy = rowBounds.getY() + (rowBounds.getHeight() - kHdrBtnH) / 2;
        hdrSnapBtn   .setBounds (x, cy, kHdrSnapW,    kHdrBtnH); x += kHdrSnapW    + kHdrBtnGap;
        hdrSnapResBtn.setBounds (x, cy, kHdrSnapResW, kHdrBtnH); x += kHdrSnapResW + kHdrBtnGap;
        hdrZoomOutBtn.setBounds (x, cy, kHdrZoomBtnW, kHdrBtnH); x += kHdrZoomBtnW + kHdrBtnGap;
        hdrZoomInBtn .setBounds (x, cy, kHdrZoomBtnW, kHdrBtnH); x += kHdrZoomBtnW + kHdrBtnGap;
        hdrZoomFitBtn.setBounds (x, cy, kHdrFitW,     kHdrBtnH); x += kHdrFitW     + kHdrBtnGap;
        hdrChaseBtn  .setBounds (x, cy, kHdrChaseW,   kHdrBtnH);
    }

    area.removeFromTop (4);

    if (miniTimeline != nullptr && inFullscreenView)
        miniTimeline->setVisible (false);

    if (! inFullscreenView)
    {
        if (tapeStrip != nullptr && tapeStripExpanded)
        {
            // Mirror the timeline's rows to the console's ACTIVE BANK
            // range for this window width, so the timeline tracks the
            // visible mixer strips (switch to bank 7-12 -> timeline shows
            // 7-12) and an empty session fills that same track count
            // instead of one row + over-tall faders. Set BEFORE
            // naturalHeight().
            if (consoleView != nullptr)
            {
                const int bank = consoleView->getBank();
                const auto [first1, last1] =
                    ConsoleView::rangeForBankAtWidth (bank, area.getWidth());
                tapeStrip->setConsoleVisibleRange (first1 - 1, last1 - first1 + 1);
            }
            // Cap the strip at ~half the available height so vertical
            // zoom (taller rows) can't swallow the console - past the cap
            // the rows scroll inside the strip. Console keeps a usable
            // minimum.
            const int wanted = tapeStrip->naturalHeight();
            const int cap    = std::max (120, area.getHeight() / 2);
            tapeStrip->setBounds (area.removeFromTop (std::min (wanted, cap)));

            area.removeFromTop (4);
            if (miniTimeline != nullptr) miniTimeline->setVisible (false);
        }
        else if (miniTimeline != nullptr)
        {
            // Collapsed timeline: show the thin song-map ribbon in the band
            // the tape strip would have occupied.
            miniTimeline->setVisible (true);
            miniTimeline->setBounds (area.removeFromTop (20));
            area.removeFromTop (4);
        }

        if (consoleView != nullptr) consoleView->setBounds (area);
    }
    else if (inMastering)
    {
        if (masteringView != nullptr) masteringView->setBounds (area);
    }
    else if (inAux)
    {
        if (auxView != nullptr) auxView->setBounds (area);
    }

    // Re-centre the startup modal + its dim backdrop when the main window
    // resizes while the dialog is up. DimOverlay::parentSizeChanged handles
    // its own resize, but we still drive the centred dialog bounds.
    if (startupDim != nullptr)
        startupDim->setBounds (getLocalBounds());

   #if DUSKSTUDIO_HAS_NATIVE_UI
    if (notepadWindow != nullptr && notepadWindow->isOpen())
    {
        if (auto* const topLevel = getTopLevelComponent())
        {
            notepadWindow->setEmbeddedGeometry (notepadGeometryFor (*topLevel));
            if (notepadDim != nullptr)
                notepadDim->setNativeChildArea (
                    getLocalArea (topLevel, notepadChildBounds (*topLevel)).expanded (1));
        }
    }
   #endif
}

void MainComponent::refreshSnapUi()
{
    hdrSnapBtn.setToggleState (session.snapToGrid, juce::dontSendNotification);
    hdrSnapResBtn.setButtonText (EditModeToolbar::labelFor (session.snapResolution));
    hdrSnapResBtn.setEnabled (session.snapToGrid);   // resolution irrelevant when snap is off
}

void MainComponent::showSnapResolutionMenu()
{
    // The musically useful subset for region-level snapping (timecode / CD-frame
    // resolutions are time-mode niche; the modal editors expose the full set).
    static const SnapResolution kOpts[] = {
        SnapResolution::Bar, SnapResolution::Half, SnapResolution::Quarter,
        SnapResolution::Eighth, SnapResolution::Sixteenth, SnapResolution::ThirtySecond,
        SnapResolution::QuarterTriplet, SnapResolution::EighthTriplet,
        SnapResolution::QuarterDotted, SnapResolution::EighthDotted,
    };
    juce::PopupMenu m;
    for (int i = 0; i < (int) (sizeof (kOpts) / sizeof (kOpts[0])); ++i)
    {
        if (i == 6 || i == 8) m.addSeparator();   // straight | triplet | dotted
        m.addItem (i + 1, EditModeToolbar::labelFor (kOpts[i]), true,
                    session.snapResolution == kOpts[i]);
    }
    juce::Component::SafePointer<MainComponent> safe (this);
    // Route through the app's in-window popup path, not JUCE's showMenuAsync -
    // raw JUCE popups flicker / mis-dismiss under X11 / Wayland, which is why
    // every other menu in this file goes through showContextMenu.
    duskstudio::showContextMenu (m, hdrSnapResBtn,
        [safe] (int chosen)
        {
            if (safe == nullptr || chosen <= 0) return;
            safe->session.snapResolution = kOpts[(size_t) (chosen - 1)];
            safe->refreshSnapUi();
            if (safe->tapeStrip != nullptr) safe->tapeStrip->repaint();
        });
}

void MainComponent::setTimelineVisible (bool show)
{
    tapeStripExpanded = show;
    const auto stage = engine.getStage();
    const bool inFullscreenView = stage == AudioEngine::Stage::Mastering
                               || stage == AudioEngine::Stage::Aux;
    if (tapeStrip    != nullptr) tapeStrip->setVisible (show && ! inFullscreenView);
    if (consoleView  != nullptr) consoleView->setStripsCompactMode (show);
    if (transportBar != nullptr) transportBar->setTapeToggleVisualState (show);
    resized();
}

#if DUSKSTUDIO_HAS_NATIVE_UI
namespace
{
// JUCE refreshes its display model when the global scale changes, and the Cocoa and
// Windows peers resize their component tree from it. The X11 peer's screen-size
// callback deliberately does not, so there the new logical bounds have to be carried
// back into the peer by hand.
//
// That round trip is confined to X11 because it is only correct where the peer is
// inert. logicalToPhysical and physicalToLogical both fold in the global scale factor,
// so converting out and back across a scale change divides the window's logical size by
// the ratio between the two scales, and neither takes a display argument, so on a
// multi-display desktop it converts against the primary display rather than the one
// holding the window. Where the peer already resizes itself, that shrinks the window
// toward nothing and can throw it onto another screen.
void applyGlobalUiScale (juce::Component& source, float scale)
{
    auto& desktop = juce::Desktop::getInstance();
    if (std::abs (desktop.getGlobalScaleFactor() - scale) < 0.0001f)
        return;

   #if ! JUCE_LINUX
    (void) source;
    desktop.setGlobalScaleFactor (scale);
   #else
    auto* topLevel = source.getTopLevelComponent();
    auto* peer = topLevel != nullptr ? topLevel->getPeer() : nullptr;
    if (peer == nullptr)
    {
        desktop.setGlobalScaleFactor (scale);
        return;
    }

    juce::Component::SafePointer<juce::Component> safeTopLevel (topLevel);
    const auto physicalBounds = desktop.getDisplays().logicalToPhysical (peer->getBounds());
    const bool wasFullScreen = peer->isFullScreen();

    desktop.setGlobalScaleFactor (scale);

    if (auto* component = safeTopLevel.getComponent())
    {
        if (auto* refreshedPeer = component->getPeer())
        {
            const auto logicalBounds = desktop.getDisplays().physicalToLogical (physicalBounds);
            const auto componentBoundsBefore = component->getBounds();
            refreshedPeer->setBounds (logicalBounds, wasFullScreen);

            // The physical-to-logical round trip can produce unchanged JUCE bounds even
            // though the global scale changed. Force JUCE's base relayout callback in
            // that case; LinuxComponentPeer's override otherwise only refreshes frame
            // extents and leaves every child at the old scale.
            if (component->getBounds() == componentBoundsBefore)
                refreshedPeer->juce::ComponentPeer::handleScreenSizeChange();
        }
    }
   #endif
}
} // namespace
#endif

void MainComponent::openAudioSettings()
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    setStatusText ("Audio settings unavailable: built without the native UI");
   #else
    if (audioSettingsWindow != nullptr && audioSettingsWindow->isOpen())
        return;

    auto* const topLevel = getTopLevelComponent();
    const auto parentHandle = topLevel != nullptr
                            ? embedscale::nativeParentHandle (*topLevel) : 0;
    if (parentHandle == 0)
    {
        setStatusText ("Audio settings unavailable: main window is not ready");
        return;
    }

    if (auto hook = EmbeddedModal::beforeModalShown())
        hook();

    audioSettingsReferenceScale = embedscale::globalScale();

    audioSettingsWindow = std::make_unique<imgui::DuskPanelWindow> (
        "dusk-studio-audio-settings", "audio-settings", "Audio settings");

    imgui::DuskPanelWindow::Callbacks callbacks;
    callbacks.dismissed = [this] { closeAudioSettings(); };
    callbacks.closed = [this]
    {
        audioSettingsDim.reset();
        audioSettingsHider.restore();
        reclaimFocusFromNotepad();
        // Whatever the panel changed, the autosave cadence is the one setting that
        // lives on this side, so the timer restarts at the value it now holds.
        startTimer (appconfig::getAutosaveIntervalSeconds() * 1000);
        if (auto handOff = std::exchange (audioSettingsHandOff, {}))
            handOff();
    };
    callbacks.shortcut = [] (imgui::ShellShortcut shortcut)
    {
        return dispatchShellShortcut (shortcut);
    };
    callbacks.geometry = [this]
    {
        auto* const top = getTopLevelComponent();
        if (top == nullptr || audioSettingsWindow == nullptr)
            return imgui::DuskPanelWindow::Geometry {};
        const auto plate = audioSettingsWindow->plateSize();
        const auto pinned = embedscale::pinnedChildGeometry (*top, plate.width, plate.height,
                                                             audioSettingsReferenceScale);
        if (audioSettingsDim != nullptr)
        {
            audioSettingsDim->setBounds (top->getLocalBounds());
            audioSettingsDim->setNativeChildArea (pinned.logical.expanded (1));
        }
        return imgui::DuskPanelWindow::Geometry { pinned.geometry.x, pinned.geometry.y,
                                                  pinned.geometry.width,
                                                  pinned.geometry.height,
                                                  pinned.geometry.scale };
    };
    audioSettingsWindow->setCallbacks (std::move (callbacks));

    juce::Component::SafePointer<MainComponent> safeThis (this);
    imgui::AudioSettingsHost host;
    host.alert = [safeThis] (std::string title, std::string message)
    {
        auto* const self = safeThis.getComponent();
        if (self == nullptr)
            return;
        self->handOffAudioSettings ([safeThis, title = std::move (title),
                                     message = std::move (message)]
        {
            auto* const owner = safeThis.getComponent();
            if (owner == nullptr)
                return;
            showDuskAlert (*owner, title, message,
                           [safeThis]
                           {
                               if (auto* back = safeThis.getComponent())
                                   back->openAudioSettings();
                           });
        });
    };
    host.previewUiScale = [safeThis] (float scale)
    {
        if (auto* self = safeThis.getComponent())
            applyGlobalUiScale (*self, scale);
    };
    host.openMidiBindings = [safeThis]
    {
        auto* const self = safeThis.getComponent();
        if (self == nullptr)
            return;
        self->handOffAudioSettings ([safeThis]
        {
            auto* const owner = safeThis.getComponent();
            if (owner == nullptr)
                return;
            owner->midiBindingsModal.show (
                *owner,
                std::make_unique<MidiBindingsPanel> (
                    owner->session, owner->engine,
                    [safeThis]
                    {
                        auto* const back = safeThis.getComponent();
                        if (back == nullptr)
                            return;
                        // close() does not run the modal's own dismiss callback, so the
                        // way back to the settings panel has to be taken here too - or
                        // the panel's Done button leaves the user in the bare window
                        // while Esc returns them to where they came from.
                        back->midiBindingsModal.close();
                        back->openAudioSettings();
                    }),
                [safeThis]
                {
                    auto* const back = safeThis.getComponent();
                    if (back == nullptr)
                        return;
                    back->midiBindingsModal.close();
                    back->openAudioSettings();
                });
        });
    };
    host.openSelfTest = [safeThis]
    {
        auto* const self = safeThis.getComponent();
        if (self == nullptr)
            return;
        self->handOffAudioSettings ([safeThis]
        {
            auto* const owner = safeThis.getComponent();
            if (owner == nullptr)
                return;
            auto panel = std::make_unique<SelfTestPanel> (owner->engine,
                                                          owner->engine.getDeviceManager(),
                                                          owner->session);
            panel->setSize (760, 560);
            panel->onCloseRequested = [safeThis]
            {
                auto* const back = safeThis.getComponent();
                if (back == nullptr)
                    return;
                back->selfTestModal.close();
                back->openAudioSettings();
            };
            owner->selfTestModal.show (*owner, std::move (panel),
                                       [safeThis]
                                       {
                                           auto* const back = safeThis.getComponent();
                                           if (back == nullptr)
                                               return;
                                           back->selfTestModal.close();
                                           back->openAudioSettings();
                                       });
        });
    };

    audioSettingsWindow->setView (imgui::makeAudioSettingsView (engine.getDeviceManager(),
                                                                engine, session,
                                                                std::move (host)));

    const auto plate = audioSettingsWindow->plateSize();
    const auto pinned = embedscale::pinnedChildGeometry (*topLevel, plate.width, plate.height,
                                                         audioSettingsReferenceScale);

    audioSettingsDim = std::make_unique<DimOverlay> (audioSettingsWindow->dimAlpha());
    audioSettingsDim->setBounds (topLevel->getLocalBounds());
    audioSettingsDim->setNativeChildArea (pinned.logical.expanded (1));
    audioSettingsDim->onClick = [this] { closeAudioSettings(); };
    topLevel->addAndMakeVisible (audioSettingsDim.get());
    audioSettingsHider.hideUnder (*topLevel, { audioSettingsDim.get() });

    if (! audioSettingsWindow->open (parentHandle,
                                     { pinned.geometry.x, pinned.geometry.y,
                                       pinned.geometry.width, pinned.geometry.height,
                                       pinned.geometry.scale }))
    {
        audioSettingsDim.reset();
        audioSettingsHider.restore();
        const auto& why = audioSettingsWindow->lastOpenFailure();
        setStatusText (why.empty()
                           ? "Audio settings unavailable: this display cannot carry it"
                           : why.c_str());
    }
   #endif
}

void MainComponent::closeAudioSettings()
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    if (audioSettingsWindow != nullptr && audioSettingsWindow->isOpen())
        audioSettingsWindow->close();
   #endif
}

#if DUSKSTUDIO_HAS_NATIVE_UI
void MainComponent::handOffAudioSettings (std::function<void()> showModal)
{
    if (audioSettingsWindow == nullptr || ! audioSettingsWindow->isOpen())
    {
        if (showModal)
            showModal();
        return;
    }
    audioSettingsHandOff = std::move (showModal);
    closeAudioSettings();
}
#endif

void MainComponent::openAudioSettingsForCapture (const std::string& capturePath)
{
    openAudioSettings();
   #if DUSKSTUDIO_HAS_NATIVE_UI
    if (audioSettingsWindow != nullptr && audioSettingsWindow->isOpen())
        audioSettingsWindow->captureNextFrameTo (capturePath);
   #else
    (void) capturePath;
   #endif
}

void MainComponent::openShortcuts()
{
    if (shortcutsModal.isOpen()) return;
    shortcutsModal.show (*this, std::make_unique<ShortcutsPanel>());
}

void MainComponent::switchToStage (AudioEngine::Stage s)
{
    if (engine.getStage() == s) return;

    // A stage swap under the notepad re-adds a fullscreen view above the dim
    // and leaves the native child hovering over a screen the user can't reach.
    yieldNotepadWindow();

    std::fprintf (stderr, "[MainComponent] switchToStage(%d) enter\n", (int) s);
    std::fflush (stderr);
    engine.setStage (s);
    // Mirror into session so save/load round-trips the user's last
    // visited stage - new sessions still default to Recording via the
    // Session::uiStage default + AudioEngine::stage default.
    session.uiStage.store ((int) s, std::memory_order_relaxed);

    syncStageUi (s);
    resized();
}

AuxView* MainComponent::ensureAuxView()
{
    if (auxView == nullptr)
    {
        // addChildComponent (not addAndMakeVisible): stays hidden until a switch - or a
        // pre-warm - needs it. The AuxLaneComponent ctors run rebuildSlots here, which
        // is what builds the native-CLAP editors (gui->create). Bounds come from the
        // next resized() (it lays out auxView whenever Aux is the active stage), and the
        // X11 embed stays deferred to first-show via ClapPluginEditorComponent.
        auxView = std::make_unique<AuxView> (session, engine);
        addChildComponent (auxView.get());
    }
    return auxView.get();
}

void MainComponent::syncStageUi (AudioEngine::Stage s)
{
    // Mixing/Recording share the console + tape strip; Mastering swaps to
    // MasteringView; Aux swaps to AuxView. Both heavy views are constructed
    // lazily so users who never visit them pay no startup cost.
    const bool wantMastering = (s == AudioEngine::Stage::Mastering);
    const bool wantAux       = (s == AudioEngine::Stage::Aux);
    const bool wantMixing    = (s == AudioEngine::Stage::Mixing);
    const bool wantFullscreenView = wantMastering || wantAux;

    if (consoleView   != nullptr) consoleView  ->setVisible (! wantFullscreenView);
    if (transportBar  != nullptr) transportBar ->setVisible (! wantFullscreenView);
    if (tapeStrip     != nullptr) tapeStrip    ->setVisible (! wantFullscreenView && tapeStripExpanded);

    // Channel strips swap their tracking-only block (input/IN/ARM/PRINT) for
    // a row of 4 AUX send knobs while in Mixing.
    if (consoleView != nullptr) consoleView->setStripsMixingMode (wantMixing);

    if (wantMastering)
    {
        if (masteringView == nullptr)
        {
            masteringView = std::make_unique<MasteringView> (session, engine);
            addAndMakeVisible (masteringView.get());
        }
        masteringView->setVisible (true);
    }
    else if (masteringView != nullptr)
    {
        masteringView->setVisible (false);
    }

    if (wantAux)
    {
        ensureAuxView()->setVisible (true);
    }
    else if (auxView != nullptr)
    {
        auxView->setVisible (false);
    }

    // Sync the segmented buttons (radio group means only one is on, but
    // explicitly setting the right one keeps a programmatic switch - like
    // doMixdown's auto-handoff - visually consistent).
    recordingStageBtn.setToggleState (s == AudioEngine::Stage::Recording, juce::dontSendNotification);
    mixingStageBtn   .setToggleState (s == AudioEngine::Stage::Mixing,    juce::dontSendNotification);
    auxStageBtn      .setToggleState (s == AudioEngine::Stage::Aux,       juce::dontSendNotification);
    masteringStageBtn.setToggleState (s == AudioEngine::Stage::Mastering, juce::dontSendNotification);
}

void MainComponent::askBounceRealtime (std::function<void (bool realtime)> launch)
{
    if (! BounceEngine::anyHardwareInsertActive (session))
    {
        launch (false);
        return;
    }
    // The confirm's callbacks fire later from the shared alert modal; launch
    // captures raw `this` at every call site, so gate it on this component
    // still being alive.
    juce::Component::SafePointer<MainComponent> safe (this);
    showDuskConfirm (*this, "Hardware inserts in session",
                     "An offline bounce cannot run external gear, so hardware "
                     "inserts print dry. A realtime bounce plays the session "
                     "once through the interface and prints them.",
                     "Realtime", [safe, launch] { if (safe != nullptr) launch (true); },
                     "Offline",  [safe, launch] { if (safe != nullptr) launch (false); });
}

void MainComponent::doMixdown()
{
    // One-shot bounce of the master mix to <sessionDir>/mixdown.wav, then
    // auto-switch to Mastering with that file loaded. No file dialog -
    // overwrites mixdown.wav each time. The "Bounce..." button retains
    // the explicit dialog flow for ad-hoc renders.
    auto target = session.getSessionDirectory().getChildFile ("mixdown.wav");

    askBounceRealtime ([this, target] (bool realtime)
    {
        auto panel = std::make_unique<BounceDialog> (engine, session,
                                                       target,
                                                       BounceEngine::Mode::MasterMix,
                                                       BounceEngine::Format::Wav, 320, 0.0, 24,
                                                       realtime);
        panel->setSize (520, 200);

        // Close the modal, then hand off to Mastering once the bounce finishes
        // successfully. The handoff is deferred so the heavy stage swap does not
        // run re-entrantly from inside the dialog's close-button callback.
        juce::Component::SafePointer<MainComponent> safeThis (this);
        panel->onRequestClose = [safeThis] { if (safeThis != nullptr) safeThis->mixdownModal.close(); };
        panel->onSuccessfulFinish = [safeThis] (juce::File rendered)
        {
            dusk::callAsync ([safeThis, rendered]
            {
                if (safeThis == nullptr) return;
                safeThis->switchToStage (AudioEngine::Stage::Mastering);
                if (safeThis->masteringView != nullptr)
                    safeThis->masteringView->loadFile (rendered);
            });
        };

        // A render in progress must not be dismissed by a stray click / Escape -
        // only the dialog's own Cancel / Close buttons drive teardown.
        mixdownModal.show (*this, std::move (panel), {}, false, false);
    });
}

#if DUSKSTUDIO_HAS_NATIVE_UI
namespace
{
// The manual's startup figure, so it reads the same on any machine. The format
// columns stay empty because these directories do not exist, which is exactly what
// the figure showed before.
std::vector<imgui::RecentSession> demoStartupRecents()
{
    std::vector<imgui::RecentSession> rows;
    for (const char* const name : { "Album Demo", "Live Take 3", "Vocal Comp" })
    {
        imgui::RecentSession row;
        row.path = std::string ("~/Music/Dusk Studio/") + name;
        row.name = name;
        rows.push_back (std::move (row));
    }
    return rows;
}
} // namespace
#endif

void MainComponent::launchStartupDialog()
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    // Nothing to show and nothing to choose, so the bootstrap default session is
    // what the DAW opens with - the same outcome as skipping the dialog. Kick the
    // scan the way every other exit from this dialog does: the gate was raised
    // before the first resized(), so clearing it here leaves nobody to start one.
    // Skipping by environment variable never raises the gate at all, which is why
    // only this path needs the call.
    startupDialogPending = false;
    maybeStartStartupPluginScan();
   #else
    if (openStartupPanel (false))
        return;

    // A display that cannot carry the panel must not leave the app looking wedged
    // behind a dim overlay, so the launch continues as if the dialog was skipped.
    startupDialogPending = false;
    setStatusText ("Startup dialog unavailable on this display; opened the default session");
    maybeStartStartupPluginScan();
   #endif
}

void MainComponent::openStartupForCapture (const std::string& capturePath)
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    if (openStartupPanel (true))
        startupWindow->captureNextFrameTo (capturePath);
   #else
    (void) capturePath;
   #endif
}

bool MainComponent::openStartupPanel (bool demoRecents)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) demoRecents;
    return false;
   #else
    auto* const topLevel = getTopLevelComponent();
    const auto parentHandle = topLevel != nullptr
                            ? embedscale::nativeParentHandle (*topLevel) : 0;
    if (parentHandle == 0)
        return false;

    startupWindow = std::make_unique<imgui::DuskPanelWindow> (
        "dusk-studio-startup", "startup", "Startup dialog");

    imgui::DuskPanelWindow::Callbacks callbacks;
    // Only the dialog's own actions dismiss it. A stray click on the DAW behind
    // must not: losing the picker mid-choice cost a user their work once already.
    // The choice runs from dismissStartupDialog's follow-up, once the framework
    // child is down - a file chooser opened while it is still mapped lands behind
    // it, invisible and holding focus.
    callbacks.dismissed = [this] { runStartupChoice(); };
    callbacks.closed = [this]
    {
        startupView = nullptr;
        startupDim.reset();
    };
    callbacks.geometry = [this]
    {
        auto* const top = getTopLevelComponent();
        if (top == nullptr || startupWindow == nullptr)
            return imgui::DuskPanelWindow::Geometry {};
        const auto plate = startupWindow->plateSize();
        const auto bounds = embedscale::centredChildBounds (*top, plate.width, plate.height);
        if (startupDim != nullptr)
        {
            startupDim->setBounds (top->getLocalBounds());
            startupDim->setNativeChildArea (bounds.expanded (1));
        }
        const auto g = embedscale::childGeometryFor (*top, bounds);
        return imgui::DuskPanelWindow::Geometry { g.x, g.y, g.width, g.height, g.scale };
    };
    startupWindow->setCallbacks (std::move (callbacks));

    loadStartupBrandImage();
    auto recents = demoRecents ? demoStartupRecents()
                               : imgui::scanRecentSessions (RecentSessions::load());
    // The downloads page is Dusk-owned so the destination can change without an app
    // update, and the artifacts behind it are supporter-gated, so no direct URL
    // exists. It is the one banner action that leaves the dialog up.
    auto view = imgui::makeStartupView (
        std::move (recents),
        startupBrandRgba.empty() ? nullptr : startupBrandRgba.data(),
        startupBrandWidth, startupBrandHeight,
        [] { juce::URL ("https://builds.duskaudio.com/latest").launchInDefaultBrowser(); });
    startupView = view.get();
    startupWindow->setView (std::unique_ptr<imgui::DuskPanelView> (view.release()));

    const auto plate = startupWindow->plateSize();
    const auto logical = embedscale::centredChildBounds (*topLevel, plate.width, plate.height);

    startupDim = std::make_unique<DimOverlay>();
    startupDim->setBounds (topLevel->getLocalBounds());
    startupDim->setNativeChildArea (logical.expanded (1));
    startupDim->onClick = [] {};
    topLevel->addAndMakeVisible (startupDim.get());

    const auto geometry = embedscale::childGeometryFor (*topLevel, logical);
    if (! startupWindow->open (parentHandle,
                               { geometry.x, geometry.y, geometry.width, geometry.height,
                                 geometry.scale }))
    {
        startupView = nullptr;
        startupDim.reset();
        startupWindow.reset();
        return false;
    }

    // Background tag probe, then a flashing badge in the dialog when a newer
    // release exists. The response can arrive after the dialog is dismissed.
    juce::Component::SafePointer<MainComponent> safeThis (this);
    updatecheck::checkForNewerTagAsync (JUCE_APPLICATION_VERSION_STRING,
        [safeThis] (const juce::String& tag)
        {
            auto* const self = safeThis.getComponent();
            if (self == nullptr || self->startupView == nullptr)
                return;
            self->startupView->setUpdateAvailable (tag.toStdString());
        });
    return true;
   #endif
}

void MainComponent::runStartupChoice()
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    const auto action = startupView != nullptr ? startupView->chosenAction()
                                               : imgui::StartupAction::skip;
    const auto path = startupView != nullptr ? startupView->chosenPath() : std::string();

    if (action == imgui::StartupAction::quit)
    {
        startupQuitRequested = true;
        dismissStartupDialog();
        if (auto* app = juce::JUCEApplicationBase::getInstance())
            app->systemRequestedQuit();
        return;
    }

    juce::Component::SafePointer<MainComponent> safeThis (this);
    dismissStartupDialog ([safeThis, action, path]
    {
        auto* const self = safeThis.getComponent();
        if (self == nullptr)
            return;
        switch (action)
        {
            case imgui::StartupAction::openRecent:
                // Through toFile: the row carries a UTF-8 path out of RecentSessions,
                // and the narrow conversion a bare construction would use drops
                // non-ASCII on Windows.
                self->loadSessionFromJson (
                    toFile (std::filesystem::u8path (path)).getChildFile ("session.json"));
                break;
            case imgui::StartupAction::newSession: self->newSessionPrompt(); break;
            case imgui::StartupAction::openFile:   self->openFromFilePrompt(); break;
            case imgui::StartupAction::skip:
            case imgui::StartupAction::quit:
            case imgui::StartupAction::none:
                break;   // the bootstrap default dir stays
        }
    });
   #endif
}

void MainComponent::loadStartupBrandImage()
{
   #if DUSKSTUDIO_HAS_NATIVE_UI && DUSKSTUDIO_HAS_BINARY_ICON
    if (! startupBrandRgba.empty())
        return;

    // The framework side has no image reader, so the icon is decoded here and the
    // panel takes the pixels straight into its font atlas.
    const auto image = juce::ImageCache::getFromMemory (BinaryData::dsicon_png,
                                                        BinaryData::dsicon_pngSize);
    if (! image.isValid())
        return;

    startupBrandWidth = image.getWidth();
    startupBrandHeight = image.getHeight();
    startupBrandRgba.resize (static_cast<std::size_t> (startupBrandWidth)
                             * static_cast<std::size_t> (startupBrandHeight) * 4u);

    // Per-pixel rather than through a bitmap lock: this runs once, on an icon.
    auto* out = startupBrandRgba.data();
    for (int y = 0; y < startupBrandHeight; ++y)
        for (int x = 0; x < startupBrandWidth; ++x)
        {
            const auto colour = image.getPixelAt (x, y);
            *out++ = colour.getRed();
            *out++ = colour.getGreen();
            *out++ = colour.getBlue();
            *out++ = colour.getAlpha();
        }
   #endif
}

void MainComponent::dismissStartupDialog (std::function<void()> onDone)
{
    // Defer the actual delete by one message-loop tick - closeDialog is
    // typically called from inside one of the dialog's own button click
    // handlers, and tearing down the dialog from inside its own callback
    // chain is fragile. callAsync runs on the message thread after the
    // click handler returns, when nothing's still on the dialog's stack.
    juce::Component::SafePointer<MainComponent> safeThis (this);
    dusk::callAsync ([safeThis, onDone = std::move (onDone)]
    {
        if (safeThis == nullptr) return;
       #if DUSKSTUDIO_HAS_NATIVE_UI
        safeThis->startupView = nullptr;
        safeThis->startupWindow.reset();
       #endif
        safeThis->startupDim.reset();
        // The dialog held keyboard focus; with it gone, pull focus back to the
        // main canvas so transport / edit shortcuts work without a stray click
        // first (StartupDialog isn't an EmbeddedModal, so there's no
        // focusRestoreTarget hand-back to lean on).
        if (! safeThis->startupQuitRequested && safeThis->isShowing())
            safeThis->grabKeyboardFocus();
        // Run the caller's follow-up FIRST: onDone may open UI (a session-
        // recovery prompt, an open-with load) that must not be overlaid by the
        // scan-progress modal kicked off below.
        if (onDone) onDone();

        // Now safe to run the startup plugin scan we held off in
        // maybeStartStartupPluginScan(). Skip it when the dismissal came from
        // Quit: the app is shutting down, no point scanning.
        safeThis->startupDialogPending = false;
        if (! safeThis->startupQuitRequested)
            safeThis->maybeStartStartupPluginScan();
    });
}

void MainComponent::guardUnsavedThen (const juce::String& title,
                                       const juce::String& message,
                                       std::function<void()> proceed)
{
    // Clean session - nothing to lose, run straight through.
    if (! currentSessionDirty())
    {
        proceed();
        return;
    }

    // Unsaved work - Save / Don't Save / Cancel, deferring each action via
    // callAsync so the button-click stack unwinds before the modal closes
    // (closing an EmbeddedModal from inside its own button onClick is a UAF).
    // `proceed` runs only after a successful Save or an explicit Don't Save.
    if (quitModal.isOpen()) return;
    auto dialog = std::make_unique<QuitConfirmDialog> (title, message);
    dialog->setSize (440, 200);
    juce::Component::SafePointer<MainComponent> safe (this);
    auto go = std::make_shared<std::function<void()>> (std::move (proceed));
    // Every exit re-invokes the startup plugin scan, which holds off while this
    // prompt is up so its progress modal can't stack over the decision.
    dialog->onCancel = [safe]
    {
        dusk::callAsync ([safe]
        {
            if (auto* s = safe.getComponent())
            {
                s->quitModal.close();
                s->maybeStartStartupPluginScan();
            }
        });
    };
    dialog->onDontSave = [safe, go]
    {
        dusk::callAsync ([safe, go]
        {
            if (auto* s = safe.getComponent())
            {
                s->quitModal.close();
                // Discarding changes - delete the current session's autosave
                // (still the OLD dir here) so it doesn't later offer to
                // "recover" the work just thrown away. Mirrors requestQuit.
                s->deleteAutosaveFor (s->session.getSessionDirectory());
                (*go)();
            }
            if (auto* s = safe.getComponent()) s->maybeStartStartupPluginScan();
        });
    };
    dialog->onSave = [safe, go]
    {
        dusk::callAsync ([safe, go]
        {
            auto* s = safe.getComponent();
            if (s == nullptr) return;
            s->quitModal.close();
            s->saveSessionAndThen ([safe, go] (bool ok)
            {
                if (ok && safe.getComponent() != nullptr)
                    (*go)();
                if (auto* self = safe.getComponent())
                    self->maybeStartStartupPluginScan();
            });
        });
    };
    // Shortcuts stay off under this prompt: Space would restart the transport
    // the switch path just parked, putting a take back in flight behind a
    // dirty check that has already run.
    quitModal.show (*this, std::move (dialog), /*onDismiss*/ {},
                      /*dismissOnClickOutside*/ false, /*dismissOnEscape*/ false,
                      /*dimAlpha*/ 0.55f, /*hidePluginEditors*/ true,
                      /*useOverlay*/ true, /*forwardShortcuts*/ false);
}

void MainComponent::stopTransportForSessionSwitch()
{
    // In the Mastering stage the mixdown player is the audible source and the
    // multitrack transport is not, so a switch has to silence both. Nothing
    // else will: the loaded session often lands on the same stage, and the
    // stage setter returns early on that.
    auto& mastering = engine.getMasteringPlayer();
    if (mastering.isPlaying()) mastering.stop();

    auto& transport = engine.getTransport();
    if (! transport.isRecording() && ! transport.isPlaying()) return;

    engine.stop();
    if (transportBar != nullptr)
    {
        transportBar->notifyRecordStopped();
        // Consumed: the errors belong to the outgoing session, and leaving them
        // latched pops the same alert again over the incoming one.
        engine.getRecordManager().clearLastRecordErrors();
    }
}

void MainComponent::guardSessionSwitchThen (const char* title,
                                              const char* message,
                                              std::function<void()> proceed)
{
    // A bounce owns the transport and fails outright if anything stops it, and
    // its modal is the only thing standing between the user and these entries.
    // Mixdown drives the same dialog through mixdownModal.
    if (bounceModal.isOpen() || mixdownModal.isOpen())
    {
        setStatusText ("Session not switched: finish or cancel the bounce first");
        return;
    }
    // guardUnsavedThen would drop the request on a busy modal, and the recovery
    // prompt would be replaced mid-decision - either way silently, and after
    // the stop below had already taken the transport down for nothing.
    if (quitModal.isOpen() || recoveryModal.isOpen())
    {
        setStatusText ("Session not switched: close the open prompt first");
        return;
    }

    // Ahead of the guard, not after it: the dirty check serializes the session,
    // and a take still running is not in it yet. Stopping here commits the take
    // first, so the prompt offers to save the work rather than passing a
    // recording session off as clean and discarding it at the load.
    stopTransportForSessionSwitch();
    guardUnsavedThen (title, message, std::move (proceed));
}

void MainComponent::newSessionPrompt()
{
    // Starting a new session blanks the current one - guard unsaved work first.
    guardSessionSwitchThen (
        "Save changes before starting a new session?",
        "Your current session has unsaved changes. If you don't save, "
        "those changes are discarded when the new session opens.",
        [this] { promptNewSessionLocation(); });
}

void MainComponent::promptNewSessionLocation()
{
    // Single-dialog "Save As" UX: filename text field + folder browser in
    // one step. The typed name becomes the session folder; the navigated
    // directory becomes its parent.
    auto startDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                        .getChildFile ("Dusk Studio");
    if (! startDir.exists()) startDir.createDirectory();

    filebrowser::open (*this, {
        /*title*/                  "Name your new session",
        /*initialFileOrDirectory*/ startDir.getChildFile ("MySong"),
        /*filePatternsAllowed*/    juce::String(),
        /*mode*/                   filebrowser::Mode::Save,
        /*warnAboutOverwriting*/   true,
        /*selectDirectories*/      false,
    },
    [this] (juce::File chosen)
    {
        if (chosen == juce::File()) return;
        // The chosen path becomes the new session folder. Start from a clean
        // default state - NOT the current session saved under a new name.
        createNewSessionAt (chosen);
    });
}

void MainComponent::createNewSessionAt (const juce::File& dir)
{
    if (dir == juce::File()) return;
    dir.createDirectory();

    // "New Session" must be a clean slate. Write a fresh, default session.json
    // into the folder and open it through the normal load path: load() clears
    // every model collection (regions, takes, markers, automation, tempo map,
    // MIDI bindings), consumePluginStateAfterLoad unloads plugins absent from
    // the new session, and finishLoadingSessionFrom resets transport + undo,
    // re-resolves MIDI, and rebuilds the console / aux / mastering UI - so
    // nothing from the previous session carries over.
    const auto target = dir.getChildFile ("session.json");
    // Refuse to clobber an existing session - "New" must never blank someone
    // else's work. The user picks a fresh name (or opens the existing one).
    if (target.existsAsFile())
    {
        setStatusForPath ("A session already exists at", target);
        return;
    }

    auto fresh = std::make_unique<Session>();
    if (! SessionSerializer::writeAtomic (target, SessionSerializer::serialize (*fresh)))
    {
        setStatusForPath ("Could not create session at", target);
        return;
    }
    fresh.reset();   // release the scratch Session before the live load runs

    if (finishLoadingSessionFrom (target, dir))
        setStatusForPath ("New session", target);
}

bool MainComponent::saveSessionTo (const juce::File& dir)
{
    if (dir == juce::File()) return false;

    // Save As to a different folder must take the audio along: copy every
    // session-owned file into the new dir and repoint the model BEFORE the
    // directory swap, so serialize below emits relative paths. Plain Ctrl+S
    // (same dir) is a no-op here. Copying precedes the audio-callback detach
    // - it touches no plugin state, so a long copy adds no dropout.
    const auto oldDir   = session.getSessionDirectory();
    const bool isSaveAs = oldDir != juce::File() && dir != oldDir;
    if (isSaveAs)
    {
        const auto consolidated = SessionSerializer::consolidateInto (session, dir);
        if (! consolidated.ok)
        {
            setStatusForPath ("Save failed", dir);
            showDuskAlert (*this, "Save failed",
                              "Dusk Studio could not copy the session's audio "
                              "into the new folder:\n\n    "
                              + consolidated.errorMessage + "\n\n"
                              "Common causes: disk full or missing write "
                              "permission. The session is unchanged; it still "
                              "lives at:\n\n    " + oldDir.getFullPathName());
            return false;
        }
        if (! consolidated.missingSources.empty())
            setStatusForPath (juce::String (consolidated.missingSources.size())
                                + " missing audio file(s) were not copied to", dir);
    }

    // Sidecar before the JSON: the notepad and session.json are one user-visible
    // save, so a sidecar failure has to abort while the session still points at
    // its old directory, rather than leave a "Saved" session whose notes never
    // landed. writeAtomic creates the folder, so this works on a first save too.
    const auto notepadTarget = dir.getChildFile ("notepad.md");
    if (! SessionSerializer::saveNotepad (dir, notepadText))
    {
        notepadDirty = true;
       #if DUSKSTUDIO_HAS_NATIVE_UI
        if (notepadWindow != nullptr)
            notepadWindow->markSaveFailed();
       #endif
        setStatusForPath ("Notepad save failed", notepadTarget);
        showDuskAlert (*this, "Notepad save failed",
                       "Dusk Studio could not write:\n\n    "
                       + notepadTarget.getFullPathName() + "\n\n"
                       "The session was not saved. Your notes are still open in memory. "
                       "Check disk space and folder permissions, then save again.");
        return false;
    }

    // setSessionDirectory creates the dir + audio subdir if missing - safe
    // to call even when the user picked an existing session folder.
    session.setSessionDirectory (dir);

    // Plugin state I/O races the renderer on plugins that don't honour
    // JUCE's "must not overlap" contract (u-he Diva is the smoking-gun
    // example), corrupting the plugin's internal state and leading to
    // an abort inside ~VST3PluginInstance later. To capture fresh
    // plugin state safely we briefly remove the audio callback for the
    // duration of the save, then re-attach. The user hears a short
    // (~10-50 ms × N loaded plugins) dropout on Ctrl+S, which beats
    // crashing.
    //
    // engineDetached==true means a higher layer (the quit-save path)
    // has already detached + does NOT want re-attach. We honour that
    // and skip the re-attach below.
    const bool reattachAudioAfter = ! engineDetached;
    if (! engineDetached)
    {
        engine.detachAudioCallback();
        engineDetached = true;   // tell publishPluginStateForSave to skip park sleeps
    }

    engine.publishPluginStateForSave (/*audioCallbackDetached*/ true);
    engine.publishTransportStateForSave();

    const auto target = dir.getChildFile ("session.json");
    if (session.sessionSampleRate <= 0.0)
        session.sessionSampleRate = engine.getCurrentSampleRate();
    const juce::String json = SessionSerializer::serialize (session);
    const bool saveOk = SessionSerializer::writeAtomic (target, json);

    if (reattachAudioAfter)
    {
        // Re-attach so audio resumes after the save returns. PluginSlot
        // already called prepareToPlay on each plugin during state
        // capture (resume side of the suspend bracket), so each plugin
        // is ready for the next callback.
        engine.reattachAudioCallback();
        engineDetached = false;
    }

    if (saveOk)
    {
        notepadDirty = false;
       #if DUSKSTUDIO_HAS_NATIVE_UI
        if (notepadWindow != nullptr)
            notepadWindow->markSaved();
       #endif

        if (isSaveAs)
        {
            // Undo actions snapshot whole AudioRegions including their files -
            // undoing past the consolidation would resurrect old-dir paths.
            // Same policy as Clean Out Unreferenced Files.
            engine.getUndoManager().clearUndoHistory();
            // Reopen playback readers on the copied files while stopped;
            // rolling transport keeps streaming the (still present) originals
            // until the next stop/play rebuild.
            if (engine.getTransport().isStopped())
                engine.getPlaybackEngine().preparePlayback();
            // The old folder's autosave still holds pre-consolidation paths -
            // without this, re-opening the old session pops a stale recovery
            // prompt.
            deleteAutosaveFor (oldDir);
        }
        RecentSessions::add (toPath (dir));
        // A successful manual save makes the autosave stale - drop it so the
        // recovery prompt doesn't fire on the next clean load.
        deleteAutosaveFor (dir);
        // Remember the JSON so the next autosave tick can recognise an
        // idle (no further edits) state and skip the write.
        setLastSavedSessionJson    (json);
        setLastWrittenAutosaveJson (juce::String());
        setStatusForPath ("Saved", target);
        return true;
    }
    setStatusForPath ("Save failed", target);
    // Status-label-only feedback is too easy to miss on a critical
    // operation. Pop a modal so the user knows the session WASN'T
    // saved and can act before losing more work.
    showDuskAlert (*this, "Save failed",
                      "Dusk Studio could not write the session file:\n\n    "
                      + target.getFullPathName() + "\n\n"
                      "Common causes: disk full, missing write "
                      "permission, or the parent folder was moved "
                      "since the session was opened. The session "
                      "is unchanged in memory; try Save As to a "
                      "different location.");
    return false;
}

juce::File MainComponent::getAutosaveFileFor (const juce::File& sessionDir) const
{
    return sessionDir.getChildFile ("session.json.autosave");
}

// Hosted plugins (per-channel inserts, aux-lane slots) re-emit slightly
// different APVTS state on each getStateInformation call - internal counters /
// smoother phases that aren't user-meaningful. The autosave dirty-check must
// ignore that drift, otherwise it fires every 30 s on a freshly-saved session
// and resurfaces the recovery dialog on the next launch. Strip the base64
// state values to "" before any content compare.
static juce::String stripVolatileStateForDirtyCompare (juce::String s)
{
    const juce::String m ("\"plugin_state\":");
    int pos = 0;
    while ((pos = s.indexOf (pos, m)) != -1)
    {
        const int afterColon = pos + m.length();
        const int q1 = s.indexOfChar (afterColon, '"');
        if (q1 < 0) break;
        const int q2 = s.indexOfChar (q1 + 1, '"');
        if (q2 < 0) break;
        s = s.replaceSection (q1, q2 - q1 + 1, "\"\"");
        pos = q1 + 2;
    }
    return s;
}

bool MainComponent::autosaveIsNewerThan (const juce::File& sessionJson) const
{
    const auto autosave = getAutosaveFileFor (sessionJson.getParentDirectory());
    if (! autosave.existsAsFile()) return false;
    if (! sessionJson.existsAsFile()) return true;   // autosave is the only state we have
    // Content-based comparison only. mtime is unreliable on common
    // filesystems (ext4 typically has 1 s mtime resolution; FAT-class
    // filesystems are 2 s), so a manual save followed by an autosave
    // within the same second can look like "autosave not newer" via
    // mtime even when the in-memory state genuinely diverged. Manual
    // save deletes the autosave (see saveSession), so the autosave
    // file existing AND its content differing from session.json is
    // the authoritative signal that a recovery point is available.
    // Volatile hosted-plugin state drift is stripped so an idle session
    // doesn't see spurious diffs.
    const auto autosaveStripped    = stripVolatileStateForDirtyCompare (autosave.loadFileAsString());
    const auto sessionJsonStripped = stripVolatileStateForDirtyCompare (sessionJson.loadFileAsString());
    return autosaveStripped != sessionJsonStripped;
}

void MainComponent::deleteAutosaveFor (const juce::File& sessionDir) const
{
    const auto autosave = getAutosaveFileFor (sessionDir);
    if (autosave.existsAsFile()) autosave.deleteFile();
}

void MainComponent::writeAutosave()
{
    const auto dir = session.getSessionDirectory();
    if (dir == juce::File()) return;

    // Ensure the directory exists before serialising. setSessionDirectory's
    // ctor path normally handles this, but a session that was constructed
    // via the default ~/Music/Dusk Studio/Untitled fallback may never have been
    // touched on disk yet.
    dir.createDirectory();

    // Same publish bookend as the manual save - the serializer reads
    // only from Session, not from the live engine. Audio is running on
    // the autosave path (timer fires from the live message loop), so
    // the publish keeps its atomic-park sleeps to defend against the
    // audio-thread re-entry race.
    engine.publishPluginStateForSave (/*audioCallbackDetached*/ false);
    engine.publishTransportStateForSave();

    // Skip the write entirely when the snapshot matches what's already on
    // disk - either the last manual save (no real edits since Ctrl+S) or
    // the previous autosave tick (timer firing on idle state). Without
    // this skip the autosave file's mtime would creep past session.json
    // every 30 s, producing a false-positive recovery prompt on next load.
    const juce::String json = SessionSerializer::serialize (session);
    // Strip volatile plugin / tape APVTS state before comparing - donor
    // getStateInformation drifts between calls even with no user change,
    // which would otherwise fire an autosave write every tick on idle
    // sessions.
    //
    // M8: hash-based dedup. Hash the stripped JSON ONCE per tick + compare
    // against the 32-bit fingerprint cached at last-save / last-autosave
    // assignment. Avoids re-running a juce::String equality scan over the
    // full serialised tree (potentially hundreds of KB) on every tick.
    // juce::DefaultHashFunctions::generateHash is the JUCE-canonical fast
    // hash used by HashMap; collision risk on a real session string is
    // statistically negligible. Worst-case missed-write recovers next tick.
    const auto stripped = stripVolatileStateForDirtyCompare (json);
    const auto strippedHash = (std::uint32_t) (std::uint32_t)
        juce::DefaultHashFunctions::generateHash (stripped, 0x7fffffff);
    if (strippedHash == lastSavedSessionStrippedHash)    return;
    if (strippedHash == lastWrittenAutosaveStrippedHash) return;

    const auto target = getAutosaveFileFor (dir);
    if (! SessionSerializer::writeAtomic (target, json))
    {
        DBG ("MainComponent: autosave write failed at " << target.getFullPathName());
        return;
    }
    setLastWrittenAutosaveJson (json);
}

void MainComponent::setLastSavedSessionJson (const juce::String& json)
{
    lastSavedSessionJson = json;
    const auto stripped  = stripVolatileStateForDirtyCompare (json);
    lastSavedSessionStrippedHash = (std::uint32_t) (std::uint32_t)
        juce::DefaultHashFunctions::generateHash (stripped, 0x7fffffff);
}

void MainComponent::setLastWrittenAutosaveJson (const juce::String& json)
{
    lastWrittenAutosaveJson = json;
    const auto stripped     = stripVolatileStateForDirtyCompare (json);
    lastWrittenAutosaveStrippedHash = (std::uint32_t) (std::uint32_t)
        juce::DefaultHashFunctions::generateHash (stripped, 0x7fffffff);
}

void MainComponent::timerCallback()
{
    // The audio thread is independent of the message thread, so a slow JSON
    // serialise here doesn't glitch playback. Still, we want autosave cheap -
    // SessionSerializer::save on a 16-track session should land well under
    // 30 ms.
    writeAutosave();

    // A device rate changed in Settings after the session was opened silently
    // detunes every recorded WAV (playback is 1:1, no SRC). Nag once per
    // mismatch; the latch re-arms when the rates agree again.
    const double deviceSr = engine.getCurrentSampleRate();
    if (session.sessionSampleRate > 0.0 && deviceSr > 0.0)
    {
        const bool mismatch = std::abs (session.sessionSampleRate - deviceSr) > 0.5;
        if (! mismatch)
        {
            warnedSampleRateMismatch = false;
        }
        else if (! warnedSampleRateMismatch)
        {
            warnedSampleRateMismatch = true;
            showDuskAlert (*this, "Sample-rate mismatch",
                "The audio device now runs at " + juce::String ((int) deviceSr)
                + " Hz but this session's audio was made at "
                + juce::String ((int) session.sessionSampleRate) + " Hz.\n\n"
                "Recorded tracks will play at the wrong speed and pitch, and "
                "new recordings will not match the old ones. Switch the device "
                "back to " + juce::String ((int) session.sessionSampleRate)
                + " Hz in Settings unless this is deliberate.");
        }
    }
}

bool MainComponent::currentSessionDirty()
{
    const auto dir = session.getSessionDirectory();
    if (dir == juce::File()) return false;

    // Publish live plugin + transport/tape state into the Session model first -
    // the serializer reads only from Session, so without this a just-touched
    // plugin/tape param stays at its last-published value and the compare would
    // falsely read clean. Audio is live here so no detach.
    engine.publishPluginStateForSave (/*audioCallbackDetached*/ false);
    engine.publishTransportStateForSave();

    // Compare with volatile state stripped (playhead / view / timestamps)
    // so transient fields don't flip the raw JSON on an otherwise-clean session.
    const auto strippedCurrent = stripVolatileStateForDirtyCompare (SessionSerializer::serialize (session));
    const auto strippedSaved   = stripVolatileStateForDirtyCompare (lastSavedSessionJson);
    return (! lastSavedSessionJson.isEmpty() && strippedCurrent != strippedSaved)
         || autosaveIsNewerThan (dir.getChildFile ("session.json"));
}

void MainComponent::requestQuit()
{
    // Take the window back before the dirty check, and WITHOUT saving: the
    // titlebar X reaches here past both the dim and the native child, and the
    // sidecar write has to wait for the user's answer or Don't Save and Cancel
    // would each commit the notepad behind their backs. notepadDirty survives
    // into the check below, and the modal hook then finds nothing left to close.
    yieldNotepadWindow (/*saveChanges*/ false);

    // Industry-standard dirty-only prompt. Compare the live serialized
    // session JSON against the snapshot we took at the last successful
    // save (or session load) - any single-knob / fader / region edit
    // diverges the JSON immediately, so this catches changes that the
    // autosave-timestamp check below would miss (autosave fires every
    // 30 s; closing within that window with a moved fader used to skip
    // the prompt and silently lose the change). autosaveIsNewerThan
    // stays as a belt-and-braces fallback for sessions where we somehow
    // didn't seed lastSavedSessionJson.
    const bool dirty = currentSessionDirty() || notepadDirty;

    if (! dirty)
    {
        // No unsaved changes - quit through the same staged-shutdown
        // path the dirty flow uses, so the engine is quiesced and
        // window peers tear down in a deterministic order. Bypassing
        // beginSafeShutdown here historically left the audio callback
        // attached during MainWindow destruction, which raced plugin
        // editor / native-peer teardown.
        beginSafeShutdown();
        return;
    }

    if (quitModal.isOpen()) return;  // user double-clicked the X

    auto dialog = std::make_unique<QuitConfirmDialog>();
    dialog->setSize (440, 200);

    // Each handler defers its body via callAsync so the button-click
    // call stack fully unwinds before we destruct the dialog. Closing
    // an EmbeddedModal that owns the dialog from inside the dialog's
    // own button callback was a use-after-free against the std::function
    // backing the button's onClick, and the resulting message-thread
    // corruption was the trigger for the GNOME compositor crash on
    // Save / Don't Save. EmbeddedModal::close defends against this on
    // its end too (it move-captures the body into a callAsync), but
    // deferring at the call site is the canonical idiom.
    //
    // Save / Don't Save also detach the audio callback up front. This
    // does two things on the quit path:
    //   - the save below can call publishPluginStateForSave with the
    //     "audio detached" fast path (no atomic-park sleeps), which is
    //     the difference between a snappy save and several hundred
    //     milliseconds of message-thread blocking on a session with
    //     multiple heavy plugins;
    //   - plugin getStateInformation runs with no concurrent
    //     processBlock, which side-steps the data race in plugins that
    //     don't honour JUCE's "must not overlap" contract on Linux.
    juce::Component::SafePointer<MainComponent> safeThis (this);

    dialog->onCancel = [safeThis]
    {
        dusk::callAsync ([safeThis]
        {
            if (auto* self = safeThis.getComponent())
                self->quitModal.close();
        });
    };
    dialog->onDontSave = [safeThis]
    {
        // "Don't save" means discard - delete the autosave too, so the next
        // launch doesn't offer to recover the work the user just discarded.
        // Recovery is then reserved for an actual crash (unclean exit, where
        // this clean-shutdown path never runs and the autosave survives).
        dusk::callAsync ([safeThis]
        {
            if (auto* self = safeThis.getComponent())
            {
                self->deleteAutosaveFor (self->session.getSessionDirectory());
                // The user explicitly chose Don't Save for every dirty part of
                // the session, including the notepad sidecar. Prevent the
                // staged shutdown's normal sidecar flush from overriding that
                // choice.
                self->notepadDirty = false;
                self->quitModal.close();
                self->beginSafeShutdown();
            }
        });
    };
    dialog->onSave = [safeThis]
    {
        // saveSessionAndThen handles both the sync (existing dir) and
        // async (Save As file chooser) paths. Close the modal first so
        // the chooser, if it opens, isn't fighting our overlay for
        // input. On save success, quit; on failure (chooser cancel,
        // disk error) the user is left in the app and can retry.
        dusk::callAsync ([safeThis]
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr) return;
            self->quitModal.close();

            // Quiesce engine BEFORE saveSessionAndThen so the save's
            // publishPluginStateForSave skips the atomic-park sleeps
            // and plugins see no concurrent processBlock during state
            // I/O. stopTimer prevents the autosave timer from re-
            // entering a save mid-shutdown.
            self->stopTimer();
            self->engine.detachAudioCallback();
            self->engineDetached = true;

            self->saveSessionAndThen ([safeThis] (bool ok)
            {
                if (! ok) return;
                if (auto* s = safeThis.getComponent())
                    s->beginSafeShutdown();
            });
        });
    };

    // Focus-locked: save-before-quit MUST go through Save / Don't Save /
    // Cancel. Esc and click-outside would let the user dismiss with no
    // decision, leaving the dirty state ambiguous and the X-button quit
    // request silently swallowed.
    quitModal.show (*this, std::move (dialog), /*onDismiss*/ {},
                       /*dismissOnClickOutside*/ false,
                       /*dismissOnEscape*/        false);
}

void MainComponent::leakAllPluginInstancesForShutdown()
{
    engine.leakAllPluginInstancesForShutdown();
}

void MainComponent::beginSafeShutdown()
{
    // Quiesce the engine + tear down native window peers in an order
    // the host windowing system can keep up with. Several earlier
    // shutdown variants were observed to crash Mutter on Linux/
    // Wayland; this one walks a deliberately conservative sequence
    // and prints a stderr marker before each phase so a future crash
    // gives us a precise line number ("which phase did we die in?").
    //
    // Idempotent on the autosave timer + audio callback removal so
    // the quit-save path can detach those up front (to make the
    // intervening save fast) and still call beginSafeShutdown() to
    // finish the teardown. That stays a single entry into the sequence:
    // the phases below span message-loop ticks, and a second entry would
    // re-run them over a tree the first one has already dismantled.
    auto markPhase = [] (const char* msg)
    {
        std::fprintf (stderr, "[Dusk Studio/shutdown] %s\n", msg);
        std::fflush (stderr);
    };

    if (shutdownInProgress)
    {
        markPhase ("re-entry ignored: shutdown already in progress");
        return;
    }
    shutdownInProgress = true;

    markPhase ("phase 1: stop autosave timer");
    stopTimer();

    markPhase ("phase 1b: close native session notepad");
    dismissNotepad (true);

    markPhase ("phase 2: stop transport (commits in-flight recording)");
    auto& transport = engine.getTransport();
    if (transport.isRecording() || transport.isPlaying())
        engine.stop();

    if (! engineDetached)
    {
        markPhase ("phase 3: detach audio callback");
        engine.detachAudioCallback();
        engineDetached = true;
    }
    else
    {
        markPhase ("phase 3: audio callback already detached (skipping)");
    }

    markPhase ("phase 3b: release plugin resources (setActive(false) on each)");
    // Quiesce every plugin BEFORE editor windows + engine destructors
    // start running. Diva's terminate() (called inside its destructor)
    // tries to talk back to the host's VST3 context; that's only safe
    // when the plugin already considers itself inactive. Without this,
    // ~VST3PluginInstance has been observed to abort with
    // __cxa_pure_virtual on session shutdown.
    engine.releaseAllPluginResources();

    markPhase ("phase 4: drop plugin editor windows");
    if (consoleView != nullptr)
        consoleView->dropAllPluginEditors (NativeEditorTeardown::LeakForExit);
    // JUCE AUX plugin editors tear down fine with the normal ~MainWindow -> ~AuxView
    // cascade. NATIVE editors (CLAP, LV2, VST3) do NOT: each opens its own X11
    // Display + a host window parented into the main peer, and its close (plugin-UI
    // destroy + XCloseDisplay) hangs if it runs in the late cascade after the main
    // peer is gone - CLAP/LV2 additionally leak their plugin UIs there. Tear them
    // down here - main peer + message loop alive, audio callback detached (phase 3).
    if (auxView != nullptr)
        auxView->dropAllNativeEditors (NativeEditorTeardown::LeakForExit);

    markPhase ("phase 5: flush window operations");
    duskstudio::platform::flushWindowOperations();

    // Walk every juce::TopLevelWindow so any future window class
    // (mastering popout, file dialog left open) inherits the
    // protection without per-site plumbing.
    markPhase ("phase 5b: clear keyboard focus from every top-level window");
    for (int i = juce::TopLevelWindow::getNumTopLevelWindows(); --i >= 0;)
        if (auto* w = juce::TopLevelWindow::getTopLevelWindow (i))
            duskstudio::platform::prepareForTopLevelDestruction (*w);

    // Yield to mutter's compositor loop so it ticks a focus-out /
    // focus-in cycle and updates its internal focus_window tracker
    // before phase 6's unmap arrives. Without this gap, mutter sees
    // the unmap before processing the EWMH activate above and trips
    // meta_window_unmanage.
    juce::Component::SafePointer<MainComponent> safeThis (this);
    dusk::callAsync ([safeThis]
    {
        auto* self = safeThis.getComponent();
        if (self == nullptr) return;

        auto mark = [] (const char* msg)
        {
            std::fprintf (stderr, "[Dusk Studio/shutdown] %s\n", msg);
            std::fflush (stderr);
        };

        mark ("phase 6: hide main window");
        if (auto* tlw = self->getTopLevelComponent())
            tlw->setVisible (false);
        duskstudio::platform::flushWindowOperations();

        duskstudio::platform::clearXInputFocus();

        mark ("phase 7: defer systemRequestedQuit to next message-loop tick");
        dusk::callAsync ([]
        {
            std::fprintf (stderr,
                          "[Dusk Studio/shutdown] phase 7b: posting systemRequestedQuit\n");
            std::fflush (stderr);
            if (auto* app = juce::JUCEApplicationBase::getInstance())
                app->systemRequestedQuit();
        });
    });

    markPhase ("phase 8: beginSafeShutdown returning to message loop (yield to mutter)");
}

void MainComponent::saveSessionAndThen (std::function<void(bool)> onComplete)
{
    const auto dir = session.getSessionDirectory();
    if (dir.getChildFile ("session.json").existsAsFile())
    {
        // Sync save into the existing dir.
        const bool ok = saveSessionTo (dir);
        if (onComplete) onComplete (ok);
        return;
    }

    // No prior save - open a Save As dialog where the user types the session
    // name and picks a parent directory in one step. The chooser is in
    // saveMode | canSelectFiles so the OS shows a filename text field; we
    // then treat the resulting "file" path as the session folder we'll
    // create (saveSessionTo already creates the directory if missing).
    auto startDir = session.getSessionDirectory().getParentDirectory();
    if (! startDir.isDirectory())
        startDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                       .getChildFile ("Dusk Studio");
    if (! startDir.exists()) startDir.createDirectory();

    juce::String defaultName = session.getSessionDirectory().getFileName();
    if (defaultName.isEmpty() || defaultName == "Untitled") defaultName = "MySong";

    filebrowser::open (*this, {
        /*title*/                  "Save session as...",
        /*initialFileOrDirectory*/ startDir.getChildFile (defaultName),
        /*filePatternsAllowed*/    juce::String(),
        /*mode*/                   filebrowser::Mode::Save,
        /*warnAboutOverwriting*/   true,
        /*selectDirectories*/      false,
    },
    [this, onComplete = std::move (onComplete)] (juce::File chosen)
    {
        if (chosen == juce::File())
        {
            if (onComplete) onComplete (false);
            return;
        }
        const bool ok = saveSessionTo (chosen);
        if (onComplete) onComplete (ok);
    });
}

void MainComponent::saveAsPrompt()
{
    // Single-dialog Save As: filename text field + folder browser in one
    // step. The typed name becomes the session folder; the navigated
    // directory becomes its parent. Replaces the old two-step modal-then-
    // chooser flow which only let the user browse, never type.
    auto startDir = session.getSessionDirectory().getParentDirectory();
    if (! startDir.isDirectory())
        startDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                       .getChildFile ("Dusk Studio");
    if (! startDir.exists()) startDir.createDirectory();

    juce::String defaultName = session.getSessionDirectory().getFileName();
    if (defaultName.isEmpty() || defaultName == "Untitled") defaultName = "MySong";

    filebrowser::open (*this, {
        /*title*/                  "Save session as...",
        /*initialFileOrDirectory*/ startDir.getChildFile (defaultName),
        /*filePatternsAllowed*/    juce::String(),
        /*mode*/                   filebrowser::Mode::Save,
        /*warnAboutOverwriting*/   true,
        /*selectDirectories*/      false,
    },
    [this] (juce::File chosen)
    {
        if (chosen == juce::File()) return;
        saveSessionTo (chosen);
    });
}

void MainComponent::openSessionPath (const juce::File& path)
{
    // Accept either the session.json itself or the session directory holding it.
    juce::File sessionJson;
    if (path.isDirectory())
        sessionJson = path.getChildFile ("session.json");
    else if (path.hasFileExtension ("json"))
        sessionJson = path;

    if (sessionJson.existsAsFile())
    {
        // Clear the startup New / Open-recent flow first so a CLI / file-manager
        // open doesn't stack a session-load (recovery) modal over the startup
        // dialog. dismissStartupDialog tears down asynchronously, so defer the
        // load into its completion callback - otherwise the recovery prompt
        // would open on top of the still-present startup dialog.
        juce::Component::SafePointer<MainComponent> safeThis (this);
        dismissStartupDialog ([safeThis, sessionJson]
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr) return;

            // Unannounced - a double-click in a file manager, on a window that
            // may hold hours of unsaved work - so it takes the same route the
            // in-app open paths take.
            self->guardSessionSwitchThen (
                "Save changes before opening another session?",
                "Another session was opened from outside Dusk Studio, and your "
                "current session has unsaved changes. If you don't save, those "
                "changes are discarded when the other session opens.",
                [safeThis, sessionJson]
            {
                if (auto* s = safeThis.getComponent())
                    s->loadSessionFromJson (sessionJson);   // shows its own alert on failure
            });
        });
    }
}

bool MainComponent::loadSessionFromJson (const juce::File& sessionJson)
{
    if (! sessionJson.existsAsFile())
    {
        setStatusForPath ("No session at", sessionJson);
        return false;
    }

    const auto dir = sessionJson.getParentDirectory();

    // Recovery prompt path. The dialog is async so we hand it a continuation
    // that runs the actual load once the user has picked. The synchronous
    // bool return reflects only "we accepted the load attempt"; the load
    // itself completes after the dialog dismisses.
    if (autosaveIsNewerThan (sessionJson))
    {
        const auto autosave = getAutosaveFileFor (dir);
        juce::Component::SafePointer<MainComponent> safe (this);

        auto body = std::make_unique<AutosaveRecoveryDialog> (
            dir,
            autosave  .getLastModificationTime(),
            sessionJson.getLastModificationTime());
        body->setSize (560, 280);

        auto* raw = body.get();
        raw->onRecover = [safe, sessionJson, dir, autosave]
        {
            if (auto* self = safe.getComponent())
            {
                self->recoveryModal.close();
                self->finishLoadingSessionFrom (autosave, dir);
                self->maybeStartStartupPluginScan();   // deferred past the recovery prompt
            }
        };
        raw->onLoad = [safe, sessionJson, dir]
        {
            if (auto* self = safe.getComponent())
            {
                self->recoveryModal.close();
                self->finishLoadingSessionFrom (sessionJson, dir);
                self->maybeStartStartupPluginScan();
            }
        };
        raw->onCancel = [safe]
        {
            if (auto* self = safe.getComponent())
            {
                self->recoveryModal.close();
                self->maybeStartStartupPluginScan();
            }
        };

        recoveryModal.show (*this, std::move (body),
                              [safe]
                              {
                                  if (auto* self = safe.getComponent())
                                  {
                                      self->recoveryModal.close();
                                      self->maybeStartStartupPluginScan();
                                  }
                              });
        return true;
    }

    return finishLoadingSessionFrom (sessionJson, dir);
}

bool MainComponent::finishLoadingSessionFrom (const juce::File& sourceJson,
                                                 const juce::File& dir)
{
    // Flush the outgoing session's notepad while getSessionDirectory() still
    // points at it. A write failure leaves the outgoing document dirty and
    // aborts the switch instead of replacing its in-memory text.
    if (! saveNotepadNow())
    {
        showDuskAlert (*this, "Notepad save failed",
                       "Dusk Studio could not write the current session's notepad, so it "
                       "did not switch sessions - loading now would discard your notes.\n\n"
                       "They are still open in memory. If this session has never been "
                       "saved, save it first; otherwise check disk space and folder "
                       "permissions, then try again.");
        return false;
    }

    // Every session switch lands here, and none may move the directory with a
    // take still running - RecordManager resolves its writers against it.
    stopTransportForSessionSwitch();

    const auto t0 = juce::Time::getMillisecondCounterHiRes();

    // load() resolves the session's relative audio and mastering paths against
    // getSessionDirectory(), so the directory moves first. It also returns
    // false before touching the model, and a session describing one folder
    // while pointing at another writes itself, and its notepad, over the folder
    // it failed to read.
    const auto previousDir = session.getSessionDirectory();
    session.setSessionDirectory (dir);

    if (! SessionSerializer::load (session, sourceJson))
    {
        session.setSessionDirectory (previousDir);
        setStatusForPath ("Load failed", sourceJson);
        return false;
    }
    const auto tAfterParse = juce::Time::getMillisecondCounterHiRes();

    // The notepad is session-bound: drop any open editor from the previous
    // session and adopt the new session's sidecar.
    dismissNotepad (false);
    notepadText  = SessionSerializer::loadNotepad (dir);
    notepadDirty = false;

    const bool loadedFromAutosave =
        sourceJson.getFileName().endsWithIgnoreCase (".autosave");

    // When the user chose "load saved session" the autosave's job is done -
    // they made a deliberate choice to discard it, so clean up for the next
    // load. When RECOVERING from the autosave, keep it: until the recovered
    // state is persisted to session.json (at the tail of this function) the
    // autosave file is the only durable copy. saveSessionTo deletes it on
    // success.
    if (! loadedFromAutosave)
        deleteAutosaveFor (dir);

    // Sample-rate policy: region WAVs play 1:1 (no SRC in the playback path),
    // so the device must run at the session's canonical rate for correct
    // speed and pitch. Legacy sessions (no stored rate) adopt the device's.
    // On a mismatch, try to switch the device; when that fails, warn loudly -
    // the session still opens, but audio runs fast/slow until the rates
    // match.
    warnedSampleRateMismatch = false;
    {
        const double deviceSr = engine.getCurrentSampleRate();
        if (session.sessionSampleRate <= 0.0)
        {
            session.sessionSampleRate = deviceSr;
        }
        else if (deviceSr > 0.0
                 && std::abs (session.sessionSampleRate - deviceSr) > 0.5)
        {
            auto setup = engine.getDeviceManager().getSetup();
            setup.sampleRate = session.sessionSampleRate;
            const auto err = engine.getDeviceManager().setSetup (setup, true);
            const double after = engine.getDeviceManager().getSetup().sampleRate;
            if (! err.empty() || std::abs (after - session.sessionSampleRate) > 0.5)
            {
                warnedSampleRateMismatch = true;
                const auto body =
                    "This session's audio was made at "
                    + juce::String ((int) session.sessionSampleRate) + " Hz, but the "
                    "audio device is running at " + juce::String ((int) after) + " Hz "
                    "and could not be switched"
                    + (! err.empty() ? juce::String (":\n\n    " + err) : juce::String (".")) +
                    "\n\nEvery recorded track will play at the wrong speed and pitch "
                    "until the device runs at the session's rate. Pick a device or "
                    "rate of " + juce::String ((int) session.sessionSampleRate)
                    + " Hz in Settings, or expect to re-record.";
                juce::Component::SafePointer<MainComponent> safeThis (this);
                dusk::callAsync ([body, safeThis]
                {
                    if (auto* self = safeThis.getComponent())
                        showDuskAlert (*self, "Sample-rate mismatch", body);
                });
            }
            else
            {
                setStatusForPath ("Audio device switched to "
                                    + juce::String ((int) session.sessionSampleRate)
                                    + " Hz to match", sourceJson);
            }
        }
    }

    // Note: lastSavedSessionJson is seeded later in this function, after
    // consumePluginStateAfterLoad has filled in plugin/transport state.
    // Seeding it here would miss those fields and the first autosave tick
    // would still fire (snapshot mismatch). The seed lives at the tail.

    // After deserialisation, the track descriptor/state fields are populated;
    // ask the engine to
    // re-instantiate each track's plugin from those.
    // Drop the prior session's undo stack BEFORE consuming plugin
    // state. Without this, hitting Cmd+Z right after a session load
    // would replay edits from the OLD session against the NEW one's
    // region indices - either a no-op or a use-after-free on the
    // referenced AudioRegion. Belt-and-suspenders: also resets redo.
    engine.getUndoManager().clearUndoHistory();

    // Native plugin editors (CLAP/LV2/VST3) reference the instances the
    // reload below evicts. Tear every editor down NOW, while the instances
    // are still alive (same order the strip loaders use). The console is
    // rebuilt after the load anyway; aux lanes lazily re-embed on next show.
    // AbandonInstance, not the exit path: the app runs on, so leaking each
    // editor's private display connection would cost an X client slot per load.
    if (consoleView != nullptr)
        consoleView->dropAllPluginEditors (NativeEditorTeardown::AbandonInstance);
    if (auxView != nullptr)
        auxView->dropAllNativeEditors (NativeEditorTeardown::AbandonInstance);

    engine.consumePluginStateAfterLoad();
    // Surface any plugin restore failures as a single summary dialog so
    // the user doesn't think a saved-with-Diva mix is intact when Diva
    // failed to instantiate. Deferred via callAsync so the load path
    // can finish drawing the freshly-loaded session before the modal
    // pops on top.
    {
        const auto& failures = engine.getLastPluginLoadFailures();
        if (! failures.empty())
        {
            auto body = pluginRestoreFailureBody (failures);
            juce::Component::SafePointer<MainComponent> safeThis (this);
            dusk::callAsync (
                [body = std::move (body), safeThis]
                {
                    if (auto* self = safeThis.getComponent())
                        showDuskAlert (*self, "Plug-in unavailable", body);
                });
        }
    }

    // Same surface for unresolved audio files - without it a moved or
    // hand-edited session loads "successfully" and plays silence with no
    // hint why.
    if (! session.missingAudioFilesAfterLoad.empty())
    {
        juce::String body =
            "These audio files referenced by the session could not be found:\n\n";
        for (const auto& p : session.missingAudioFilesAfterLoad)
            body += "    " + p + "\n";
        body += "\nTheir regions will play silent. If the session folder was "
                "moved, copy the files back into its audio/ subfolder and "
                "reload the session.";
        juce::Component::SafePointer<MainComponent> safeThis (this);
        dusk::callAsync (
            [body = std::move (body), safeThis]
            {
                if (auto* self = safeThis.getComponent())
                    showDuskAlert (*self, "Missing audio files", body);
            });
    }
    const auto tAfterPlugins = juce::Time::getMillisecondCounterHiRes();
    engine.consumeTransportStateAfterLoad();
    // Bypass instruments on tracks the session restored as frozen (frozenRegion
    // was rebuilt during deserialize; this is the engine-side half).
    engine.reapplyFreezeState();

    // Re-resolve MIDI input routing from the just-loaded session. The load
    // restores the saved identifiers (MCU input, sync source, per-track MIDI
    // in) but NOT the runtime index each maps to, so without this the MCU
    // surface (and track MIDI inputs) stay dead until the user re-picks the
    // device in Settings. Use the LIGHTWEIGHT per-track re-resolve - the full
    // refreshMidiInputs() hot-plug path detaches/reattaches the audio callback
    // and disables/re-enables every MIDI device, which froze the UI for a few
    // seconds on every load. The physical devices are unchanged since startup,
    // so the banks are valid; MCU + sync re-resolve happens in
    // openConfiguredMidiOutputs() below.
    engine.reresolveTrackMidiFromSession();

    // Open MIDI output ports for any tracks the loaded session had
    // routed. Done here (not in the engine constructor) so startup
    // never blocks on snd_seq_connect_to for ports nobody uses.
    // Safely variant: the audio callback is live on the load path (the
    // lightweight reresolve above doesn't detach), so the bank open must.
    engine.openConfiguredMidiOutputsSafely();
    const auto tAfterMidiOuts = juce::Time::getMillisecondCounterHiRes();

    // Reconstruct the console so all controls reflect the freshly
    // loaded values (atomic state is in `session`, but UI widgets
    // captured initial values in their constructors).
    consoleView.reset();
    consoleView = std::make_unique<ConsoleView> (session, engine);
    addAndMakeVisible (consoleView.get());
    consoleView->setOnStripFocusRequested ([this] (int t)
    {
        if (tapeStrip != nullptr) tapeStrip->setSelectedTrack (t);
    });
    if (consoleView != nullptr && transportBar != nullptr)
        consoleView->setStripsCompactMode (tapeStripExpanded);

    // Restore the saved UI stage. switchToStage() early-returns when the
    // engine is already on the requested stage, but the freshly-rebuilt
    // consoleView (above) still needs the per-stage sync (strip mixing
    // mode, mastering/aux view construction, toggle button state). Call
    // syncStageUi unconditionally so the on-screen stage always matches
    // session.uiStage after load.
    {
        const int loaded = jlimit (0, 3, session.uiStage.load (std::memory_order_relaxed));
        const auto wantStage = (AudioEngine::Stage) loaded;
        if (engine.getStage() != wantStage)
            engine.setStage (wantStage);
        syncStageUi (wantStage);
    }
    refreshSnapUi();   // snap on/off + resolution are serialized - reflect the loaded values
    resized();
    // resized()'s indirect refresh of the tape strip (setConsoleVisibleRange /
    // setBounds) no-ops when the reopened session has the same track layout +
    // window size, so the freshly-loaded regions would never get drawn. Force an
    // explicit rebuild + refit + repaint. Safe in a fullscreen stage too: the
    // strip is hidden now but laid out (and repainted) on the next stage switch.
    if (tapeStrip != nullptr)
        tapeStrip->refreshAfterSessionLoad();
    const auto tAfterConsole = juce::Time::getMillisecondCounterHiRes();

    RecentSessions::add (toPath (dir));
    setStatusForPath ("Loaded", sourceJson, /*isAutosave*/ loadedFromAutosave);

    // Seed the saved-state snapshot from the live in-memory session now
    // that plugin + transport state have been consumed. The next autosave
    // tick compares against this snapshot and skips the write while the
    // session remains untouched.
    setLastSavedSessionJson    (SessionSerializer::serialize (session));
    setLastWrittenAutosaveJson (juce::String());

    // Recovery must end with the recovered state on disk. Without this,
    // the snapshot seeded above makes the session look clean: the quit
    // dirty-check passes, the autosave tick skips, and the recovered
    // work exists nowhere durable - quit + relaunch would land on the
    // stale session.json. saveSessionTo persists it and (only on
    // success) deletes the autosave; on failure the autosave survives
    // and the save-failed alert fires.
    if (loadedFromAutosave)
        saveSessionTo (dir);

    std::fprintf (stderr,
                  "[Dusk Studio/Load] %s: parse=%dms plugins=%dms midiOuts=%dms console=%dms total=%dms\n",
                  sourceJson.getFileName().toRawUTF8(),
                  (int) (tAfterParse    - t0),
                  (int) (tAfterPlugins  - tAfterParse),
                  (int) (tAfterMidiOuts - tAfterPlugins),
                  (int) (tAfterConsole  - tAfterMidiOuts),
                  (int) (tAfterConsole  - t0));

    // Loading churns the component tree (console + stage rebuild) and, on
    // XWayland, can leave the main window without input focus - the toolbar then
    // renders in its inactive/dimmed state and the first click only re-focuses.
    // Reassert front + keyboard focus once the rebuild settles.
    juce::Component::SafePointer<MainComponent> safeThis (this);
    dusk::callAsync ([safeThis]
    {
        auto* self = safeThis.getComponent();
        if (self == nullptr) return;
        if (auto* peer = self->getPeer())
            duskstudio::platform::bringWindowToFront (*peer);
        // Bring the window forward always, but don't yank keyboard focus when a modal
        // is up (a missing-plugin / missing-audio alert raised by the load) - that
        // would steal focus from the dialog the user needs to dismiss.
        if (juce::ModalComponentManager::getInstance()->getNumModalComponents() == 0)
            self->grabKeyboardFocus();

        // Pre-warm the AUX view if this session has any aux insert loaded. Building it
        // here (hidden) moves the AuxView construction + the native-CLAP gui->create off
        // the first Mixing->Aux switch, so that switch isn't stalled building plugin
        // editors. Deferred to this post-load callAsync so the main window paints first;
        // the native slots are already loaded (consumePluginStateAfterLoad ran above).
        bool anyAuxInsert = false;
        for (int a = 0; a < Session::kNumAuxLanes && ! anyAuxInsert; ++a)
        {
            const auto& lane = self->session.auxLane (a);
            for (int s = 0; s < AuxLaneParams::kMaxLanePlugins; ++s)
                if (lane.nativeClapPath[(size_t) s].isNotEmpty()
                    || lane.nativeLv2Path[(size_t) s].isNotEmpty()
                    || lane.nativeVst3Path[(size_t) s].isNotEmpty()
                    || lane.nativeAuIdentifier[(size_t) s].isNotEmpty()
                    || lane.pluginDescriptor[(size_t) s].has_value()
                    || lane.pluginLegacyDescriptionXml[(size_t) s].isNotEmpty())
                { anyAuxInsert = true; break; }
        }
        if (anyAuxInsert)
            self->ensureAuxView();   // bounds + X11 embed stay deferred to the first switch
    });
    return true;
}

void MainComponent::openFromFilePrompt()
{
    // Opening another session discards the current one - guard unsaved work
    // before the browser appears (mirrors New Session / Open Recent).
    guardSessionSwitchThen (
        "Save changes before opening another session?",
        "Your current session has unsaved changes. If you don't save, "
        "those changes are discarded when the other session opens.",
        [this]
    {
        auto startDir = session.getSessionDirectory();
        if (! startDir.isDirectory())
            startDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory);

        filebrowser::open (*this, {
            /*title*/                  "Open session.json",
            /*initialFileOrDirectory*/ startDir.getChildFile ("session.json"),
            /*filePatternsAllowed*/    "*.json",
            /*mode*/                   filebrowser::Mode::Open,
            /*warnAboutOverwriting*/   false,
            /*selectDirectories*/      false,
        },
        [this] (juce::File chosen)
        {
            if (chosen == juce::File()) return;
            loadSessionFromJson (chosen);
        });
    });
}

void MainComponent::openBounceDialog()
{
    auto defaultDir = session.getSessionDirectory();
    if (! defaultDir.isDirectory())
        defaultDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    const auto defaultFile = defaultDir.getChildFile ("bounce.wav");

   #if DUSKSTUDIO_HAS_LAME
    const juce::String patterns = "*.wav;*.mp3";   // name it .mp3 to bounce MP3
   #else
    const juce::String patterns = "*.wav";
   #endif

    filebrowser::open (*this, {
        /*title*/                  "Bounce master mix",
        /*initialFileOrDirectory*/ defaultFile,
        /*filePatternsAllowed*/    patterns,
        /*mode*/                   filebrowser::Mode::Save,
        /*warnAboutOverwriting*/   true,
        /*selectDirectories*/      false,
    },
    [this] (juce::File out)
    {
        if (out == juce::File()) return;  // user cancelled
        auto outFile = out;
       #if DUSKSTUDIO_HAS_LAME
        const bool mp3 = outFile.hasFileExtension ("mp3");
       #else
        const bool mp3 = false;   // no encoder in this build - a typed .mp3 falls back to WAV
       #endif
        if (! mp3 && ! outFile.hasFileExtension ("wav"))
            outFile = outFile.withFileExtension ("wav");
        const auto fmt = mp3 ? BounceEngine::Format::Mp3 : BounceEngine::Format::Wav;

        askBounceRealtime ([this, outFile, fmt] (bool realtime)
        {
            // A realtime capture is always WAV (BounceEngine forces the
            // format), so a typed .mp3 name must follow - otherwise the file
            // is WAV data under an .mp3 extension.
            const auto target = realtime ? outFile.withFileExtension ("wav") : outFile;
            const auto format = realtime ? BounceEngine::Format::Wav : fmt;

            auto launchBounce = [this, target, format, realtime]
            {
                auto panel = std::make_unique<BounceDialog> (engine, session,
                                                               target,
                                                               BounceEngine::Mode::MasterMix, format,
                                                               320, 0.0, 24, realtime);
                panel->setSize (520, 200);
                juce::Component::SafePointer<MainComponent> safeThis (this);
                panel->onRequestClose = [safeThis] { if (safeThis != nullptr) safeThis->bounceModal.close(); };
                bounceModal.show (*this, std::move (panel), {}, false, false);
            };

            // The .mp3 -> .wav retarget skips the file browser's overwrite
            // check (it inspected the .mp3 name), so re-check the real target.
            if (target != outFile && target.existsAsFile())
            {
                juce::Component::SafePointer<MainComponent> safe (this);
                showDuskConfirm (*this, "Overwrite file?",
                                 target.getFileName()
                                   + " already exists (realtime bounces are written as WAV). "
                                     "Overwrite it?",
                                 "Overwrite",
                                 [safe, launchBounce] { if (safe != nullptr) launchBounce(); },
                                 "Cancel", {}, /*destructive*/ true);
                return;
            }
            launchBounce();
        });
    });
}

void MainComponent::openBounceStemsDialog()
{
    // Pick a base WAV; per-stem filenames derive from it via
    // BounceEngine::stemOutputFile (<base>_<NN>_<sanitized-track>.wav).
    // Default into a stems/ subfolder - a full set is 20+ files and would
    // bury session.json and mixdown.wav in the session root. The browser can
    // still navigate anywhere.
    auto defaultDir = session.getSessionDirectory();
    if (! defaultDir.isDirectory())
        defaultDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    else
        defaultDir = defaultDir.getChildFile ("stems");
    defaultDir.createDirectory();
    const auto defaultFile = defaultDir.getChildFile ("stems.wav");

    // Stems are WAV-only - MP3 encoder delay/padding would misalign them for
    // re-import, so don't offer .mp3 here (the master / mastering bounces do).
    filebrowser::open (*this, {
        /*title*/                  "Bounce stems (one WAV per track)",
        /*initialFileOrDirectory*/ defaultFile,
        /*filePatternsAllowed*/    "*.wav",
        /*mode*/                   filebrowser::Mode::Save,
        /*warnAboutOverwriting*/   true,
        /*selectDirectories*/      false,
    },
    [this] (juce::File out)
    {
        if (out == juce::File()) return;
        auto outFile = out;
        if (! outFile.hasFileExtension ("wav"))
            outFile = outFile.withFileExtension ("wav");

        // Preflight: the base WAV is never written for stems - the real
        // targets are the derived stem files. Ask BounceEngine for the exact
        // set it would write (tracks + bus / aux stems) and warn if any
        // already exist, so the file browser's base-file overwrite check
        // doesn't give false comfort.
        juce::StringArray conflicts;
        for (const auto& tgt : BounceEngine::collectStemTargets (session, outFile))
            if (tgt.file.existsAsFile())
                conflicts.add (tgt.file.getFileName());

        auto launch = [this, outFile] (bool realtime)
        {
            auto panel = std::make_unique<BounceDialog> (engine, session,
                                                           outFile,
                                                           BounceEngine::Mode::Stems,
                                                           BounceEngine::Format::Wav,
                                                           320, 0.0, 24, realtime);
            panel->setSize (520, 200);
            juce::Component::SafePointer<MainComponent> safeThis (this);
            panel->onRequestClose = [safeThis] { if (safeThis != nullptr) safeThis->bounceModal.close(); };
            bounceModal.show (*this, std::move (panel), {}, false, false);
        };

        if (conflicts.isEmpty())
        {
            askBounceRealtime (launch);
            return;
        }

        const auto msg = "These stem files already exist and will be overwritten:\n\n"
                       + conflicts.joinIntoString ("\n")
                       + "\n\nContinue?";
        juce::Component::SafePointer<MainComponent> safe (this);
        showDuskConfirm (*this, "Overwrite stems?", msg,
                         "Overwrite",
                         [safe, launch]() mutable
                         {
                             if (safe.getComponent() != nullptr)
                                 safe->askBounceRealtime (launch);
                         },
                         "Cancel", {}, /*destructive*/ true);
    });
}

// True when the track's name is empty or still the default "N" string.
// Used by the import flow to decide whether to auto-rename the track to
// the imported file's basename (preserves user-renamed tracks).
static bool trackHasDefaultName (const Track& t, int idx)
{
    const auto trimmed = t.name.trim();
    return trimmed.isEmpty() || trimmed == juce::String (idx + 1);
}

static void maybeRenameTrackFromFile (Track& t, int idx, const juce::File& f)
{
    if (trackHasDefaultName (t, idx))
        t.name = f.getFileNameWithoutExtension();
}

void MainComponent::runAudioImportFlow (const juce::File& source,
                                            std::int64_t timelineStart,
                                            int trackHint)
{
    auto reader = dusk::audio::FileReader::open (toPath (source));
    if (reader == nullptr)
    {
        showImportError ("Import audio",
                          "Unsupported or unreadable audio file: " + source.getFileName());
        return;
    }

    ImportTargetPicker::FileSummary summary;
    summary.file          = source;
    summary.sampleRate    = reader->info().sampleRate;
    summary.numChannels   = std::min (2, reader->info().numChannels);
    summary.lengthSamples = reader->info().numFrames;
    summary.isMidi        = false;
    reader.reset();   // close before the picker calls FileImporter

    auto picker = std::make_unique<ImportTargetPicker> (
        session,
        std::move (summary),
        timelineStart,
        engine.getCurrentSampleRate(),
        session.tempoBpm.load (std::memory_order_relaxed),
        session.beatsPerBar.load (std::memory_order_relaxed),
        session.timeDisplayMode.load (std::memory_order_relaxed),
        trackHint,
        [safeThis = juce::Component::SafePointer<MainComponent> (this),
         source, timelineStart] (int trackIndex)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr) return;
            self->importTargetModal.close();

            // Re-check transport state - the user could have hit Play
            // between opening the picker and confirming a target. The
            // success path mutates Track::regions in place, which is
            // only safe with playback halted. Abort the WHOLE chain on
            // mid-batch play so the user sees one error rather than
            // silently dropping every remaining queued file.
            if (! self->engine.getTransport().isStopped())
            {
                showImportError ("Import audio",
                                         "Stop playback before importing files.");
                self->cancelImportChain();
                return;
            }

            auto& track = self->session.track (trackIndex);
            // A frozen track's playback is its baked WAV; adding a region would
            // desync it. Same guard as the editor entry points.
            if (track.frozen.load (std::memory_order_relaxed))
            {
                showImportError ("Import audio",
                                 "This track is frozen. Unfreeze it before importing onto it.");
                self->cancelImportChain();
                return;
            }
            const auto mode = (Track::Mode) track.mode.load (std::memory_order_relaxed);

            duskstudio::fileimport::AudioImportRequest req;
            req.source            = source;
            req.audioDir          = self->session.getAudioDirectory();
            req.trackIndex        = trackIndex;
            req.sessionSampleRate = self->engine.getCurrentSampleRate();
            req.targetChannels    = (mode == Track::Mode::Stereo) ? 2 : 1;
            req.timelineStart     = timelineStart;

            auto res = duskstudio::fileimport::importAudio (req);
            if (! res.ok)
            {
                showImportError ("Import audio failed", res.errorMessage);
                return;
            }

            // Transport is stopped (re-checked above), so PlaybackEngine
            // isn't iterating Track::regions on the audio thread - mutating
            // in place is safe; the next play() pulls the new layout via
            // preparePlayback.
            track.regions.push_back (std::move (res.region));
            duskstudio::maybeRenameTrackFromFile (track, trackIndex, source);
            if (self->tapeStrip != nullptr) self->tapeStrip->repaint();
            self->pendingImportLastCommitted = trackIndex;
            self->kickNextImport();
        },
        [safeThis = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (auto* self = safeThis.getComponent())
                self->cancelImportChain();
        });

    importTargetModal.show (*this, std::move (picker));
}

void MainComponent::runMidiImportFlow (const juce::File& source,
                                          std::int64_t timelineStart,
                                          int trackHint)
{
    ImportTargetPicker::FileSummary summary;
    summary.file        = source;
    summary.isMidi      = true;
    summary.numChannels = -1;

    if (! peekMidiSummary (source, summary))
    {
        showImportError ("Import MIDI", "Could not read MIDI file.");
        return;
    }

    auto picker = std::make_unique<ImportTargetPicker> (
        session,
        std::move (summary),
        timelineStart,
        engine.getCurrentSampleRate(),
        session.tempoBpm.load (std::memory_order_relaxed),
        session.beatsPerBar.load (std::memory_order_relaxed),
        session.timeDisplayMode.load (std::memory_order_relaxed),
        trackHint,
        [safeThis = juce::Component::SafePointer<MainComponent> (this),
         source, timelineStart] (int trackIndex)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr) return;
            self->importTargetModal.close();

            // Mirror the audio-import recheck: the user could have hit
            // Play between opening the picker and confirming a target.
            // MidiRegions is AtomicSnapshot-mutated (RT-safe by design)
            // so this isn't a strict data-race guard, but importing a
            // region mid-playback produces confusing UX (notes appear
            // partway through the take). Bail consistently with the
            // audio path - cancel the whole chain so a multi-file batch
            // doesn't silently drop everything after the first play-edge.
            if (! self->engine.getTransport().isStopped())
            {
                showImportError ("Import MIDI",
                                         "Stop playback before importing files.");
                self->cancelImportChain();
                return;
            }

            auto& track = self->session.track (trackIndex);
            // A frozen track's playback is its baked WAV; adding a region would
            // desync it. Same guard as the audio-import path.
            if (track.frozen.load (std::memory_order_relaxed))
            {
                showImportError ("Import MIDI",
                                 "This track is frozen. Unfreeze it before importing onto it.");
                self->cancelImportChain();
                return;
            }

            duskstudio::fileimport::MidiImportRequest req;
            req.source            = source;
            req.sessionSampleRate = self->engine.getCurrentSampleRate();
            req.sessionBpm        = self->session.tempoBpm.load (std::memory_order_relaxed);
            req.timelineStart     = timelineStart;

            auto res = duskstudio::fileimport::importMidi (req);
            if (! res.ok)
            {
                showImportError ("Import MIDI failed", res.errorMessage);
                return;
            }

            track.midiRegions.mutate ([&] (std::vector<MidiRegion>& v)
            {
                v.push_back (std::move (res.region));
            });
            duskstudio::maybeRenameTrackFromFile (track, trackIndex, source);
            if (self->tapeStrip != nullptr) self->tapeStrip->repaint();
            self->pendingImportLastCommitted = trackIndex;
            self->kickNextImport();
        },
        [safeThis = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (auto* self = safeThis.getComponent())
                self->cancelImportChain();
        });

    importTargetModal.show (*this, std::move (picker));
}

void MainComponent::importPrompt()
{
    if (! engine.getTransport().isStopped())
    {
        showImportError ("Import", "Stop playback before importing files.");
        return;
    }

    // One picker for both kinds. enqueueImports / openMultiImportPicker route
    // each chosen file by extension (audio -> reader peek, MIDI -> file
    // peek) and the target picker flips a track's mode to match the dropped
    // file, so a mixed audio+MIDI selection is handled in a single batch.
    const auto startDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    filebrowser::openMulti (*this, {
        /*title*/                  "Import audio or MIDI file(s)",
        /*initialFileOrDirectory*/ startDir,
        /*filePatternsAllowed*/    "*.wav;*.aiff;*.aif;*.flac;*.mid;*.midi",
        /*mode*/                   filebrowser::Mode::Open,
        /*warnAboutOverwriting*/   false,
        /*selectDirectories*/      false,
    },
    [safeThis = juce::Component::SafePointer<MainComponent> (this)]
    (juce::Array<juce::File> chosen)
    {
        auto* self = safeThis.getComponent();
        if (self == nullptr || chosen.isEmpty()) return;
        const auto t = self->engine.getTransport().getPlayhead();
        if (chosen.size() > 1)
            self->openMultiImportPicker (chosen, t);
        else
            self->enqueueImports (chosen, t, -1);
    });
}

namespace
{
// Interleave two equal-rate mono fragments (_1 / _2) into one temporary 2-ch
// WAV so the stereo pair travels through FileImporter::importAudio as a single
// source. Returns an empty File on failure; caller deletes the temp afterwards.
juce::File makeStereoTempWav (const juce::File& left, const juce::File& right)
{
    auto rl = dusk::audio::FileReader::open (toPath (left));
    auto rr = dusk::audio::FileReader::open (toPath (right));
    if (rl == nullptr || rr == nullptr) return {};

    const std::int64_t len = std::min (rl->info().numFrames, rr->info().numFrames);
    const double sampleRate = rl->info().sampleRate;
    if (len <= 0 || sampleRate <= 0.0 || sampleRate != rr->info().sampleRate) return {};

    const auto tmp = juce::File::createTempFile (".wav");
    dusk::audio::WriteSpec spec;
    spec.sampleRate = sampleRate;
    spec.numChannels = 2;
    spec.bitsPerSample = 24;
    spec.format = dusk::audio::WriteSpec::Format::Wav;
    auto writer = dusk::audio::FileWriter::create (toPath (tmp), spec);
    if (writer == nullptr) { tmp.deleteFile(); return {}; }

    // Each fragment is a mono reader (channel 0). Interleave the two halves into
    // a 2-ch stream in fixed-size chunks so a long take doesn't allocate the
    // whole file up front.
    constexpr int kChunk = 1 << 16;   // 64k samples per pass
    dusk::audio::PlanarBuffer lbuf, rbuf, out;
    if (! lbuf.setSize (1, kChunk) || ! rbuf.setSize (1, kChunk)
        || ! out.setSize (2, kChunk))
    {
        writer.reset(); tmp.deleteFile(); return {};
    }
    for (std::int64_t pos = 0; pos < len; pos += kChunk)
    {
        const int n = (int) std::min ((std::int64_t) kChunk, len - pos);
        if (rl->read (lbuf.data(), 1, pos, n) != n
            || rr->read (rbuf.data(), 1, pos, n) != n)
        {
            writer.reset(); tmp.deleteFile(); return {};
        }
        dusk::audio::vecCopy (out.channel (0), lbuf.channel (0), n);
        dusk::audio::vecCopy (out.channel (1), rbuf.channel (0), n);
        if (! writer->write (out.data(), 2, n))
        {
            writer.reset(); tmp.deleteFile(); return {};
        }
    }
    writer.reset();
    return tmp;
}
} // namespace

void MainComponent::importDpSongPrompt()
{
    if (! engine.getTransport().isStopped())
    {
        showImportError ("Import DP Song", "Stop playback before importing.");
        return;
    }

    // Selecting a folder-of-files is unintuitive in directory-pick mode (the
    // browser only lists subfolders, so a song folder looks empty when you open
    // it). Instead let the user pick ANY file inside the song folder and import
    // its parent - they navigate in, see the ZZ/.sys files, pick one.
    const auto startDir = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    filebrowser::open (*this, {
        /*title*/                  "Open any file inside the DP song folder",
        /*initialFileOrDirectory*/ startDir,
        /*filePatternsAllowed*/    "*.wav;*.sys",
        /*mode*/                   filebrowser::Mode::Open,
        /*warnAboutOverwriting*/   false,
        /*selectDirectories*/      false,
    },
    [safeThis = juce::Component::SafePointer<MainComponent> (this)]
    (juce::File chosen)
    {
        auto* self = safeThis.getComponent();
        if (self == nullptr || chosen == juce::File()) return;
        const auto folder = chosen.isDirectory() ? chosen : chosen.getParentDirectory();

        const auto scan = dp::scanSongFolder (folder);
        if (! scan.ok)
        {
            showImportError ("Import DP Song",
                             scan.warnings.empty()
                                 ? juce::String ("No DP song folder found at that location.")
                                 : juce::String (scan.warnings));
            return;
        }

        auto dialog = std::make_unique<DpImportDialog> (
            scan, Session::kNumTracks,
            [safeThis, scan] (bool importMixer, bool importTimeline)
            {
                if (auto* s = safeThis.getComponent())
                {
                    s->importTargetModal.close();
                    s->runDpImport (scan, importMixer, importTimeline);
                }
            },
            [safeThis]
            {
                if (auto* s = safeThis.getComponent())
                    s->importTargetModal.close();
            });

        self->importTargetModal.show (*self, std::move (dialog), {},
                                      /*dismissOnClickOutside*/ false);
    });
}

void MainComponent::runDpImport (const dp::SongScan& scan,
                                 bool importMixer, bool importTimeline)
{
    if (! engine.getTransport().isStopped())
    {
        showImportError ("Import DP Song", "Stop playback before importing.");
        return;
    }
    if (dpImportJob != nullptr) return;   // one import at a time

    double sr = engine.getCurrentSampleRate();
    // Device not open yet -> SR reads 0; FileImporter resamples content to 48k in
    // that case, so use the same fallback here or every clip/marker would be
    // placed at 0 (timeline offsets multiplied by sr).
    if (sr <= 0.0) sr = 48000.0;

    std::array<bool, Session::kNumTracks> frozen {};
    for (int i = 0; i < Session::kNumTracks; ++i)
        frozen[(size_t) i] = session.track (i).frozen.load (std::memory_order_relaxed);

    dpImportWantMixer = importMixer;
    auto job = std::make_unique<DpImportJob> (scan, sr, session.getAudioDirectory(),
                                               frozen, importTimeline,
                                               &makeStereoTempWav);

    auto panel = std::make_unique<DpImportProgressPanel> (*job);
    panel->setSize (460, 130);
    juce::Component::SafePointer<MainComponent> safeThis (this);
    panel->onFinished = [safeThis]
    {
        if (auto* self = safeThis.getComponent())
            self->finishDpImport();
    };

    dpImportJob = std::move (job);
    dpImportProgressModal.show (*this, std::move (panel), {}, false, false);
    static_cast<DpImportJob*> (dpImportJob.get())->startThread();
}

void MainComponent::finishDpImport()
{
    auto* job = static_cast<DpImportJob*> (dpImportJob.get());
    if (job == nullptr) return;

    dpImportProgressModal.close();

    if (job->cancelled.load (std::memory_order_acquire))
    {
        dpImportJob.reset();
        setStatusForPath ("DP import cancelled", session.getSessionDirectory());
        return;
    }

    const auto& scan = job->scan;
    const bool importMixer = dpImportWantMixer;
    const double sr = job->sr;

    int imported = 0;
    juce::StringArray skipped;

    for (auto& out : job->outcomes)
    {
        if (! out.imported)
        {
            skipped.add (out.skipReason);
            continue;
        }
        const auto& it   = scan.tracks[(size_t) out.index];
        const auto& frag = it.fragment;
        auto& track      = session.track (out.index);

        // Transport was stopped at kickoff and the modal blocked edits, so
        // PlaybackEngine isn't iterating regions - mutating in place is safe
        // (mirrors runAudioImportFlow).
        track.regions.push_back (std::move (out.res.region));
        track.name = it.name;
        track.mode.store ((int) (frag.stereo ? Track::Mode::Stereo : Track::Mode::Mono),
                          std::memory_order_relaxed);
        if (importMixer && it.mixer.valid)
        {
            auto& s = track.strip;
            s.faderDb.store (it.mixer.faderDb, std::memory_order_relaxed);
            s.pan    .store (it.mixer.pan,     std::memory_order_relaxed);
            s.mute   .store (it.mixer.mute,    std::memory_order_relaxed);

            // Map the DP 3-band EQ onto Dusk's 4-band: Low->LF, High->HF, and
            // Mid->LM or HM by frequency (the unused band stays flat).
            const auto& m = it.mixer;
            s.eqEnabled.store (m.eqOn, std::memory_order_relaxed);
            s.lfGainDb.store (jlimit (-15.0f, 15.0f, m.lowGainDb), std::memory_order_relaxed);
            s.lfFreq  .store (jlimit (20.0f, 400.0f, m.lowFreqHz), std::memory_order_relaxed);
            // DP has one mid band -> Dusk has two (LM/HM). Drive the band that
            // matches the frequency and reset the other to flat defaults, so an
            // import into a non-empty session doesn't leave stale EQ behind.
            if (m.midFreqHz < 1500.0f)
            {
                s.lmGainDb.store (jlimit (-15.0f, 15.0f, m.midGainDb), std::memory_order_relaxed);
                s.lmFreq  .store (jlimit (100.0f, 4000.0f, m.midFreqHz), std::memory_order_relaxed);
                s.lmQ     .store (jlimit (0.4f, 4.0f, m.midQ), std::memory_order_relaxed);
                s.hmGainDb.store (0.0f, std::memory_order_relaxed);
                s.hmFreq  .store (2000.0f, std::memory_order_relaxed);
                s.hmQ     .store (0.7f, std::memory_order_relaxed);
            }
            else
            {
                s.hmGainDb.store (jlimit (-15.0f, 15.0f, m.midGainDb), std::memory_order_relaxed);
                s.hmFreq  .store (jlimit (600.0f, 13000.0f, m.midFreqHz), std::memory_order_relaxed);
                s.hmQ     .store (jlimit (0.4f, 4.0f, m.midQ), std::memory_order_relaxed);
                s.lmGainDb.store (0.0f, std::memory_order_relaxed);
                s.lmFreq  .store (600.0f, std::memory_order_relaxed);
                s.lmQ     .store (0.7f, std::memory_order_relaxed);
            }
            s.hfGainDb.store (jlimit (-15.0f, 15.0f, m.highGainDb), std::memory_order_relaxed);
            s.hfFreq  .store (jlimit (1000.0f, 20000.0f, m.highFreqHz), std::memory_order_relaxed);
        }
        ++imported;
    }

    // Song markers (intro/verse/chorus/punch/end) -> session markers.
    int markersAdded = 0;
    {
        const double songSr = scan.sampleRate > 0.0 ? scan.sampleRate : sr;
        for (const auto& mk : scan.markers)
        {
            const auto at = (std::int64_t) std::llround ((double) mk.positionSamples * sr / songSr);
            session.addMarker (at, "DP Mark " + juce::String (mk.index));
            ++markersAdded;
        }
    }

    // Song tempo (song.sys 0x6d8, BPM).
    if (scan.tempoBpm > 0)
        session.tempoBpm.store ((float) scan.tempoBpm, std::memory_order_relaxed);

    // Song time signature (song.sys 0x6d9).
    if (scan.timeSigNum > 0 && scan.timeSigDen > 0)
    {
        // Clamp to the same [1, 32] range SessionSerializer enforces so the
        // in-memory model can't hold a value the serializer would reject.
        session.beatsPerBar.store (jlimit (1, 32, scan.timeSigNum), std::memory_order_relaxed);
        session.beatUnit   .store (jlimit (1, 32, scan.timeSigDen), std::memory_order_relaxed);
        // The ruler / grid read these on repaint, but the transport bar's
        // time-sig button caches its text and the 20 Hz refresh doesn't touch
        // it - re-sync it so it doesn't contradict the import.
        if (transportBar != nullptr) transportBar->refreshTimeSigButton();
    }

    if (tapeStrip != nullptr) tapeStrip->repaint();

    juce::String msg;
    msg << "Imported " << imported << (imported == 1 ? " track." : " tracks.");
    if (scan.discardedTakes > 0)
        msg << "\nSkipped " << scan.discardedTakes << " discarded take(s).";
    if (job->importTimeline && job->placedCount > 0)
        msg << "\nPlaced " << job->placedCount << " region(s) on the timeline; the rest at song start.";
    if (markersAdded > 0)
        msg << "\nImported " << markersAdded << " song marker(s).";
    if (scan.tempoBpm > 0)
        msg << "\nSet tempo to " << scan.tempoBpm << " BPM.";
    if (scan.timeSigNum > 0 && scan.timeSigDen > 0)
        msg << "\nSet time signature to " << scan.timeSigNum << "/" << scan.timeSigDen << ".";
    if (! skipped.isEmpty())
        msg << "\n\nSkipped " << skipped.size() << ":\n" << skipped.joinIntoString ("\n");
    dpImportJob.reset();
    showDuskAlert (*this, "Import DP Song", msg);
}

void MainComponent::enqueueImports (juce::Array<juce::File> files,
                                       std::int64_t timelineStart,
                                       int trackHint)
{
    pendingImportQueue.clear();
    pendingImportLastCommitted = -2;
    pendingImportInitialHint   = trackHint;
    pendingImportTimelineStart = timelineStart;
    for (const auto& f : files)
    {
        PendingImport p;
        p.file       = f;
        p.trackIndex = -1;   // needs picker
        const auto ext = f.getFileExtension().toLowerCase();
        p.isMidi     = (ext == ".mid" || ext == ".midi");
        pendingImportQueue.push_back (std::move (p));
    }
    kickNextImport();
}

void MainComponent::enqueueImportsWithTargets (
    std::vector<MultiImportTargetPicker::Assignment> assignments,
    std::int64_t timelineStart)
{
    pendingImportQueue.clear();
    pendingImportLastCommitted = -2;
    pendingImportInitialHint   = -1;
    pendingImportTimelineStart = timelineStart;
    for (const auto& a : assignments)
    {
        PendingImport p;
        p.file       = a.file;
        p.trackIndex = a.trackIndex;   // pre-assigned, picker skipped
        p.isMidi     = a.isMidi;
        pendingImportQueue.push_back (std::move (p));
    }
    kickNextImport();
}

void MainComponent::kickNextImport()
{
    if (pendingImportQueue.empty()) return;
    auto entry = pendingImportQueue.front();
    pendingImportQueue.erase (pendingImportQueue.begin());

    // Pre-assigned (multi-picker path): skip the per-file modal and
    // dispatch straight to the importer.
    if (entry.trackIndex >= 0)
    {
        MultiImportTargetPicker::Assignment a;
        a.file       = entry.file;
        a.trackIndex = entry.trackIndex;
        a.isMidi     = entry.isMidi;
        commitImportNoModal (a, pendingImportTimelineStart);
        return;
    }

    // Sequential hint: after the first commit, push subsequent files to
    // adjacent tracks so a drop on track 2 fills 2/3/4 unless the user
    // overrides each pick in the modal.
    const int hint = pendingImportLastCommitted >= 0
                       ? std::min (pendingImportLastCommitted + 1,
                                       Session::kNumTracks - 1)
                       : pendingImportInitialHint;

    if (entry.isMidi)
        runMidiImportFlow (entry.file, pendingImportTimelineStart, hint);
    else
        runAudioImportFlow (entry.file, pendingImportTimelineStart, hint);
}

void MainComponent::cancelImportChain()
{
    pendingImportQueue.clear();
    pendingImportLastCommitted = -2;
    importTargetModal.close();
}

void MainComponent::commitImportNoModal (
    const MultiImportTargetPicker::Assignment& a, std::int64_t timelineStart)
{
    // Mirror the per-file picker's onCommit body but without re-opening
    // a modal. Mid-batch transport state changes abort the whole queue,
    // same as the single-file path.
    if (! engine.getTransport().isStopped())
    {
        showImportError (a.isMidi ? "Import MIDI" : "Import audio",
                          "Stop playback before importing files.");
        cancelImportChain();
        return;
    }

    auto& track = session.track (a.trackIndex);
    // A frozen track's playback is its baked WAV; reject the commit (skip this
    // file, continue the batch) rather than desync it.
    if (track.frozen.load (std::memory_order_relaxed))
    {
        showImportError (a.isMidi ? "Import MIDI" : "Import audio",
                          "This track is frozen. Unfreeze it before importing onto it.");
        kickNextImport();
        return;
    }

    if (a.isMidi)
    {
        duskstudio::fileimport::MidiImportRequest req;
        req.source            = a.file;
        req.sessionSampleRate = engine.getCurrentSampleRate();
        req.sessionBpm        = session.tempoBpm.load (std::memory_order_relaxed);
        req.timelineStart     = timelineStart;

        auto res = duskstudio::fileimport::importMidi (req);
        if (! res.ok)
        {
            showImportError ("Import MIDI failed", res.errorMessage);
            kickNextImport();
            return;
        }
        track.midiRegions.mutate ([&] (std::vector<MidiRegion>& v)
        {
            v.push_back (std::move (res.region));
        });
    }
    else
    {
        const auto mode = (Track::Mode) track.mode.load (std::memory_order_relaxed);

        duskstudio::fileimport::AudioImportRequest req;
        req.source            = a.file;
        req.audioDir          = session.getAudioDirectory();
        req.trackIndex        = a.trackIndex;
        req.sessionSampleRate = engine.getCurrentSampleRate();
        req.targetChannels    = (mode == Track::Mode::Stereo) ? 2 : 1;
        req.timelineStart     = timelineStart;

        auto res = duskstudio::fileimport::importAudio (req);
        if (! res.ok)
        {
            showImportError ("Import audio failed", res.errorMessage);
            kickNextImport();
            return;
        }
        track.regions.push_back (std::move (res.region));
    }

    duskstudio::maybeRenameTrackFromFile (track, a.trackIndex, a.file);
    if (tapeStrip != nullptr) tapeStrip->repaint();
    pendingImportLastCommitted = a.trackIndex;
    kickNextImport();
}

void MainComponent::openMultiImportPicker (juce::Array<juce::File> files,
                                              std::int64_t timelineStart)
{
    // Peek each file to build a FileSummary the picker can render. Audio
    // peek opens an audio reader; MIDI peek parses the file and counts
    // notes. Both happen synchronously on the message thread - the user
    // clicked Import and is waiting for the modal to appear.
    std::vector<ImportTargetPicker::FileSummary> summaries;
    summaries.reserve ((size_t) files.size());
    for (const auto& f : files)
    {
        ImportTargetPicker::FileSummary s;
        s.file = f;
        const auto ext = f.getFileExtension().toLowerCase();
        s.isMidi = (ext == ".mid" || ext == ".midi");

        if (s.isMidi)
        {
            if (! peekMidiSummary (f, s))
                continue;
            s.numChannels = -1;
        }
        else
        {
            auto reader = dusk::audio::FileReader::open (toPath (f));
            if (reader == nullptr)
                continue;
            s.sampleRate    = reader->info().sampleRate;
            s.numChannels   = std::min (2, reader->info().numChannels);
            s.lengthSamples = reader->info().numFrames;
        }
        summaries.push_back (std::move (s));
    }

    // Every file failed to open (no reader, malformed MIDI). Surface the
    // error and bail rather than opening a multi-import modal with zero
    // rows and a permanently-disabled Import button.
    if (summaries.empty())
    {
        showImportError ("Import",
                          "None of the selected files could be opened. "
                          "Check that they are valid audio or MIDI files.");
        return;
    }

    auto picker = std::make_unique<MultiImportTargetPicker> (
        session, std::move (summaries), timelineStart,
        [safeThis = juce::Component::SafePointer<MainComponent> (this),
         timelineStart]
        (std::vector<MultiImportTargetPicker::Assignment> assignments)
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr) return;
            self->importTargetModal.close();
            self->enqueueImportsWithTargets (std::move (assignments), timelineStart);
        },
        [safeThis = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (auto* self = safeThis.getComponent())
                self->cancelImportChain();
        });

    // Modal stays open on click-outside - the user must explicitly
    // Cancel or Import. Esc still routes to the body's key handling
    // (no onDismiss installed -> Esc is currently a no-op too).
    importTargetModal.show (*this, std::move (picker), {}, /*dismissOnClickOutside*/ false);
}

// MenuBarModel
//
// Two top-level menus drive every header action that used to be a separate
// TextButton. Item IDs are namespaced per-menu so menuItemSelected can
// dispatch with a single switch, no need to also branch on the top-level
// menu index.
namespace
{
enum MenuItemId
{
    kMenuFileNew      = 1001,
    kMenuFileOpen     = 1002,
    kMenuFileSave     = 1003,
    kMenuFileSaveAs   = 1004,
    kMenuFileImport   = 1006,
    kMenuFileImportDp = 1007,
    kMenuFileMixdown  = 1010,
    kMenuFileBounce   = 1011,
    kMenuFileCleanOut = 1012,
    kMenuFileOptimizeAutomation = 1013,
    kMenuFileBounceStems        = 1014,
    kMenuFileQuit     = 1099,
    // Reserved range for the "Open Recent" submenu (one entry per
    // RecentSessions::kMaxEntries slot, indexed off this base). Plus a
    // clear-all sentinel just above the per-entry range.
    kMenuFileRecentBase  = 1100,
    kMenuFileRecentClear = 1180,
    // Reserved range for template entries (one per SessionTemplate enum
    // value, indexed off this base). Stays well above the file-action IDs
    // so future additions don't collide.
    kMenuFileTemplateBase = 1200,
    kMenuViewFullScreen = 3001,
    kMenuViewTimeline   = 3002,
    kMenuSettingsAudio = 2001,
    kMenuSettingsAbout = 2002,
    kMenuSettingsShortcuts = 2003,
    kMenuSettingsSupporters = 2004,
};
}

juce::StringArray MainComponent::getMenuBarNames()
{
    return { "File", "View", "Settings" };
}

juce::PopupMenu MainComponent::getMenuForIndex (int topLevelMenuIndex,
                                                  const juce::String& /*name*/)
{
    juce::PopupMenu menu;
    if (topLevelMenuIndex == 0)   // File
    {
        // Right-aligned accelerator hints (⌘ on macOS, Ctrl+ elsewhere via
        // getTextDescriptionWithIcons). The shortcuts are handled in
        // keyPressed; this just makes them discoverable in the menu.
        auto addAccel = [&menu] (int id, const juce::String& text,
                                  int keyCode, int modFlags)
        {
            juce::PopupMenu::Item item;
            item.itemID = id;
            item.text   = text;
            item.shortcutKeyDescription = juce::KeyPress (
                keyCode, juce::ModifierKeys (modFlags), 0).getTextDescriptionWithIcons();
            menu.addItem (item);
        };

        addAccel (kMenuFileNew, "New session...", 'N',
                  juce::ModifierKeys::commandModifier);

        // New From Template submenu - drops opinionated track names /
        // colours / modes onto the live session so the user is one
        // arm-click from recording. Iterates the SessionTemplate enum so
        // a new template appears here automatically.
        juce::PopupMenu templates;
        for (int i = 0; i < (int) SessionTemplate::kCount; ++i)
            templates.addItem (kMenuFileTemplateBase + i,
                                nameForTemplate ((SessionTemplate) i));
        menu.addSubMenu ("New from template", templates);

        addAccel (kMenuFileOpen, "Open...", 'O',
                  juce::ModifierKeys::commandModifier);

        // "Recent Sessions" submenu: cached on the way out so the click
        // handler can resolve by index. Capped at RecentSessions::kMaxEntries
        // (the on-disk file is also capped, so this match isn't load-bearing).
        menuRecentSessions = toFileArray (RecentSessions::load());
        juce::Logger::writeToLog ("[Dusk Studio/MainComponent] Recent Sessions submenu: "
                                     + juce::String (menuRecentSessions.size())
                                     + " entries from RecentSessions::load()");
        juce::PopupMenu recents;
        if (menuRecentSessions.isEmpty())
        {
            recents.addItem (-1, "(none)", false, false);
        }
        else
        {
            for (int i = 0; i < menuRecentSessions.size()
                              && i < RecentSessions::kMaxEntries; ++i)
            {
                const auto& dir    = menuRecentSessions.getReference (i);
                const auto  json   = dir.getChildFile ("session.json");
                const auto  parent = dir.getParentDirectory().getFullPathName();
                // Always enabled - load() already pruned dirs that have
                // disappeared, and a missing session.json inside a
                // surviving dir is rare enough that we'd rather let the
                // user click + see the error than greyout an entry that
                // looks broken without explanation. The click handler
                // shows the alert if loadSessionFromJson fails.
                recents.addItem (kMenuFileRecentBase + i,
                                  dir.getFileName()
                                    + juce::String (juce::CharPointer_UTF8 ("  \xe2\x80\x94  "))
                                    + parent);
                juce::ignoreUnused (json);
            }
            recents.addSeparator();
            recents.addItem (kMenuFileRecentClear, "Clear recent sessions");
        }
        menu.addSubMenu ("Recent Sessions", recents);

        addAccel (kMenuFileSave, "Save", 'S',
                  juce::ModifierKeys::commandModifier);
        addAccel (kMenuFileSaveAs, "Save as...", 'S',
                  juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier);
        menu.addSeparator();
        addAccel (kMenuFileImport, "Import Audio or MIDI...", 'I',
                  juce::ModifierKeys::commandModifier);
        menu.addItem (kMenuFileImportDp, "Import DP Song (experimental)...");
        menu.addSeparator();
        menu.addItem (kMenuFileMixdown, "Mixdown");
        addAccel (kMenuFileBounce, "Bounce...", 'B',
                  juce::ModifierKeys::commandModifier);
        menu.addItem (kMenuFileBounceStems, "Bounce stems...");
        menu.addSeparator();
        menu.addItem (kMenuFileCleanOut, "Clean out unreferenced files...");
        menu.addItem (kMenuFileOptimizeAutomation, "Optimize automation...");
        menu.addSeparator();
        addAccel (kMenuFileQuit, "Quit", 'Q',
                  juce::ModifierKeys::commandModifier);
    }
    else if (topLevelMenuIndex == 1)   // View
    {
        const auto* peer = getPeer();
        const bool isFullScreen = peer != nullptr && peer->isFullScreen();
        menu.addItem (kMenuViewFullScreen, "Full Screen", true, isFullScreen);
        menu.addSeparator();
        const auto stage = engine.getStage();
        const bool timelineAvailable = stage != AudioEngine::Stage::Mastering
                                    && stage != AudioEngine::Stage::Aux;
        menu.addItem (kMenuViewTimeline, "Show Timeline",
                      timelineAvailable, tapeStripExpanded);
    }
    else if (topLevelMenuIndex == 2)   // Settings
    {
        menu.addItem (kMenuSettingsAudio, "Settings...");
        menu.addItem (kMenuSettingsShortcuts, "Keyboard Shortcuts  (?)");
        menu.addSeparator();
       #if DUSKSTUDIO_HAS_PATREON_CREDITS
        menu.addItem (kMenuSettingsSupporters, "Supporters");
       #endif
        menu.addItem (kMenuSettingsAbout, "About Dusk Studio");
    }
    return menu;
}

void MainComponent::menuItemSelected (int menuItemID, int /*topLevelMenuIndex*/)
{
    switch (menuItemID)
    {
        case kMenuFileNew:    newSessionPrompt();       break;
        case kMenuFileOpen:   openFromFilePrompt();     break;
        case kMenuFileSave:
        {
            // Mirror the Save button's smart behavior: redirect to Save As
            // if the session has never been saved (no session.json yet) so
            // the user picks a real destination instead of clobbering the
            // bootstrap "Untitled" dir.
            const auto dir = session.getSessionDirectory();
            if (! dir.getChildFile ("session.json").existsAsFile())
                saveAsPrompt();
            else
                saveSessionTo (dir);
            break;
        }
        case kMenuFileSaveAs: saveAsPrompt();           break;
        case kMenuFileImport: importPrompt(); break;
        case kMenuFileImportDp: importDpSongPrompt(); break;
        case kMenuFileMixdown: doMixdown();             break;
        case kMenuFileBounce:  openBounceDialog();      break;
        case kMenuFileBounceStems: openBounceStemsDialog(); break;
        case kMenuFileCleanOut: cleanOutUnreferencedFiles(); break;
        case kMenuFileOptimizeAutomation:
        {
            // Safety contract from Session.h: handleWritePassComplete is
            // NOT lock-free against audio-thread lane reads, so we only
            // call it when no strip is in Read/Touch and the transport
            // is stopped (no audio routing through evaluateLane). The
            // dialog enforces this explicitly so a user who hasn't read
            // the spec can't trigger a race.
            const bool playing = engine.getTransport().isPlaying();
            bool anyActive = false;
            auto isActive = [] (int amode) noexcept
            {
                return amode == (int) AutomationMode::Read
                    || amode == (int) AutomationMode::Write
                    || amode == (int) AutomationMode::Touch;
            };
            for (int t = 0; t < Session::kNumTracks && ! anyActive; ++t)
                if (isActive (session.track (t).automationMode.load (std::memory_order_relaxed)))
                    anyActive = true;
            for (int a = 0; a < Session::kNumAuxLanes && ! anyActive; ++a)
                if (isActive (session.auxLane (a).params.automationMode.load (std::memory_order_relaxed)))
                    anyActive = true;
            if (! anyActive
                && isActive (session.master().automationMode.load (std::memory_order_relaxed)))
                anyActive = true;

            if (playing || anyActive)
            {
                showDuskAlert (*this, "Optimize automation",
                                  playing
                                    ? juce::String ("Stop playback before optimising automation. "
                                                      "The optimiser rewrites every lane's point "
                                                      "data; running it while the audio thread "
                                                      "may be reading the lanes is unsafe.")
                                    : juce::String ("Set every strip's automation mode to Off "
                                                      "before optimising. The optimiser rewrites "
                                                      "lane data; doing it while a strip is in "
                                                      "Read or Touch can race the audio thread."));
                break;
            }

            // Count points before / after so the user sees what the
            // optimiser actually did.
            auto countPoints = [this]() noexcept
            {
                std::size_t total = 0;
                for (int t = 0; t < Session::kNumTracks; ++t)
                    for (int p = 0; p < kNumAutomationParams; ++p)
                        total += session.track (t).automationLanes[(size_t) p].pointsConst().size();
                for (int a = 0; a < Session::kNumAuxLanes; ++a)
                    for (int p = 0; p < kNumAutomationParams; ++p)
                        total += session.auxLane (a).params.automationLanes[(size_t) p].pointsConst().size();
                for (int p = 0; p < kNumAutomationParams; ++p)
                    total += session.master().automationLanes[(size_t) p].pointsConst().size();
                return total;
            };
            const auto before = countPoints();
            handleWritePassComplete (session);
            const auto after = countPoints();

            showDuskAlert (*this, "Optimize automation",
                              juce::String ("Thinned ")
                                + juce::String ((std::int64_t) before)
                                + " automation points down to "
                                + juce::String ((std::int64_t) after) + ".");
            break;
        }
        case kMenuFileQuit:
            if (auto* app = juce::JUCEApplicationBase::getInstance())
                app->systemRequestedQuit();
            break;
        case kMenuViewFullScreen: toggleFullScreen(); break;
        case kMenuViewTimeline: setTimelineVisible (! tapeStripExpanded); break;
        case kMenuSettingsAudio: openAudioSettings();   break;
        case kMenuSettingsShortcuts: openShortcuts();   break;
       #if DUSKSTUDIO_HAS_PATREON_CREDITS
        case kMenuSettingsSupporters:
        {
            if (! supportersModal.isOpen())
            {
                auto panel = std::make_unique<SupportersPanel>();
                panel->onCloseRequested = [this] { supportersModal.close(); };
                supportersModal.show (*this, std::move (panel));
            }
            break;
        }
       #endif
        case kMenuSettingsAbout:
        {
            // Pull the version string from the JUCE_APPLICATION_VERSION_STRING
            // compile define (wired through CMakeLists from PROJECT_VERSION).
            // Always matches what's in Info.plist so a bug report's reported
            // version can be cross-checked.
            const auto body =
                juce::String ("Dusk Studio ") + JUCE_APPLICATION_VERSION_STRING + "\n\n"
                "Portastudio-style DAW.\n"
                "Built " __DATE__ " " __TIME__;
            showDuskAlert (*this, "About Dusk Studio", body);
            break;
        }
        case kMenuFileRecentClear:
            RecentSessions::clear();
            menuRecentSessions.clear();
            break;
        default:
            // "Open Recent" picks live in [kMenuFileRecentBase, +kMaxEntries).
            // Resolve via the snapshot taken when the menu was built.
            if (menuItemID >= kMenuFileRecentBase
                && menuItemID < kMenuFileRecentBase + RecentSessions::kMaxEntries)
            {
                const int idx = menuItemID - kMenuFileRecentBase;
                if (idx >= 0 && idx < menuRecentSessions.size())
                {
                    const auto dir = menuRecentSessions.getReference (idx);
                    guardSessionSwitchThen (
                        "Save changes before opening another session?",
                        "Your current session has unsaved changes. If you don't save, "
                        "those changes are discarded when the other session opens.",
                        [this, dir] { loadSessionFromJson (dir.getChildFile ("session.json")); });
                }
                break;
            }
            // Template menu items live in [kMenuFileTemplateBase, +kCount).
            if (menuItemID >= kMenuFileTemplateBase
                && menuItemID < kMenuFileTemplateBase + (int) SessionTemplate::kCount)
            {
                const auto tmpl = (SessionTemplate) (menuItemID - kMenuFileTemplateBase);
                // Applying a template rewrites the current session in place -
                // guard unsaved work first (mirrors New / Open).
                guardSessionSwitchThen (
                    "Save changes before applying a template?",
                    "Your current session has unsaved changes. If you don't save, "
                    "those changes are discarded when the template is applied.",
                    [this, tmpl]
                {
                    applyTemplate (session, tmpl);
                    // Rebuild the console view so the new track names / colours
                    // / modes propagate into existing strip components - the
                    // simplest way to pick up name + colour + mode in one shot.
                    consoleView.reset();
                    consoleView = std::make_unique<ConsoleView> (session, engine);
                    addAndMakeVisible (consoleView.get());
                    consoleView->setOnStripFocusRequested ([this] (int t)
                    {
                        if (tapeStrip != nullptr) tapeStrip->setSelectedTrack (t);
                    });
                    consoleView->setStripsMixingMode (
                        engine.getStage() == AudioEngine::Stage::Mixing);
                    // Re-apply the compact/expanded strip mode the rebuilt view
                    // would otherwise default away from (mirrors the ctor + load).
                    consoleView->setStripsCompactMode (tapeStripExpanded);
                    if (tapeStrip != nullptr) tapeStrip->repaint();
                    resized();
                });
            }
            break;
    }
}

void MainComponent::cleanOutUnreferencedFiles()
{
    // Build the set of WAVs the session is currently using - both the
    // live region's `file` and every previousTakes entry. Anything in
    // the audio dir not in this set is fair game for deletion.
    auto audioDir = session.getAudioDirectory();
    if (! audioDir.isDirectory())
    {
        showDuskAlert (*this, "Clean out",
                          "This session has no audio directory yet, "
                          "so there's nothing to clean.");
        return;
    }

    juce::StringArray referenced;   // full paths
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        for (const auto& r : session.track (t).regions)
        {
            referenced.addIfNotAlreadyThere (r.file.getFullPathName());
            for (const auto& take : r.previousTakes)
                referenced.addIfNotAlreadyThere (take.file.getFullPathName());
        }
    }
    // A bounce loaded into the Mastering stage may live in audio/ too.
    referenced.addIfNotAlreadyThere (session.mastering().sourceFile.getFullPathName());

    // Walk the audio directory for .wav files. Anything outside the
    // referenced set is a candidate. Subdirectories are intentionally
    // skipped so external WAVs the user dropped in by hand don't get
    // touched.
    juce::Array<juce::File> candidates;
    std::int64_t totalBytes = 0;
    for (const auto& f : audioDir.findChildFiles (juce::File::findFiles, false, "*.wav"))
    {
        if (! referenced.contains (f.getFullPathName()))
        {
            candidates.add (f);
            totalBytes += f.getSize();
        }
    }

    if (candidates.isEmpty())
    {
        showDuskAlert (*this, "Clean out",
                          "No unreferenced files found. The audio "
                          "directory is already clean.");
        return;
    }

    const auto sizeMB = (double) totalBytes / (1024.0 * 1024.0);
    const auto msg = "Found " + juce::String (candidates.size())
                   + " unreferenced .wav file(s) totalling "
                   + juce::String (sizeMB, 1) + " MB.\n\n"
                   + "These were created by past record passes that no "
                     "longer have any region or take pointing at them. "
                     "Deleting cannot be undone, and the session's undo "
                     "history will be cleared (undone edits may still "
                     "reference these files).";

    juce::Component::SafePointer<MainComponent> safeThis (this);
    showDuskConfirm (*this, "Clean out unreferenced files", msg,
                       /*primary*/   "Delete",
                       /*onPrimary*/ [safeThis, candidates]
                       {
                           int deleted = 0;
                           for (const auto& f : candidates)
                               if (f.deleteFile()) ++deleted;
                           if (auto* self = safeThis.getComponent())
                           {
                               // The undo stack holds full before-states of
                               // deleted regions; Ctrl+Z after this would
                               // restore a region whose WAV is gone. Nothing
                               // deleted = nothing dangling, keep the history.
                               if (deleted > 0)
                                   self->engine.getUndoManager().clearUndoHistory();
                               self->statusLabel.setText (
                                   "Deleted " + juce::String (deleted)
                                       + " unreferenced file(s).",
                                   juce::dontSendNotification);
                           }
                       },
                       /*secondary*/   "Cancel",
                       /*onSecondary*/ {},
                       /*destructive*/ true);
}

namespace
{
// Editor expand/collapse timing. Open is a touch slower than close so the
// panel "blooms" out of the region but dismisses crisply.
constexpr int kEditorOpenMs  = 180;
constexpr int kEditorCloseMs = 150;

// A start rect is only animatable when it's on-screen and big enough that the
// editor's resized() math has something to chew on; otherwise snap to final.
bool editorStartRectUsable (juce::Rectangle<int> r) noexcept
{
    return ! r.isEmpty() && r.getWidth() >= 2 && r.getHeight() >= 2;
}

// One-shot timer that fires `fn` once after `ms` then idles. Owned by
// MainComponent so it outlives the ComponentAnimator's collapse run.
class OneShotTimer final : public dusk::Timer
{
public:
    OneShotTimer (int ms, std::function<void()> f) : fn (std::move (f)) { startTimer (ms); }
    void timerCallback() override
    {
        stopTimer();
        auto f = fn;
        fn = nullptr;
        if (f) f();
    }
private:
    std::function<void()> fn;
};

// Fade the dim in; scale the editor out of `startRect` into `finalBounds`.
// Editor + dim must already be visible children. Falls back to a hard snap
// when the source rect is off-screen / degenerate.
void animateEditorOpen (juce::Component& editor, juce::Component& dim,
                        juce::Rectangle<int> finalBounds,
                        juce::Rectangle<int> startRect)
{
    auto& animator = juce::Desktop::getInstance().getAnimator();
    dim.setAlpha (0.0f);
    animator.animateComponent (&dim, dim.getBounds(), 1.0f, kEditorOpenMs, false, 1.0, 0.0);

    if (editorStartRectUsable (startRect))
    {
        editor.setBounds (startRect);
        animator.animateComponent (&editor, finalBounds, 1.0f, kEditorOpenMs, false, 1.0, 0.0);
    }
    else
    {
        editor.setBounds (finalBounds);
        editor.setAlpha (1.0f);
    }
}
} // namespace

void MainComponent::openPianoRoll (int trackIdx, int regionIdx)
{
    // Frozen tracks are edit-locked: their MIDI is baked into the rendered WAV,
    // so editing notes would silently desync the audio. Block the editor (the
    // single entry point for every MIDI-edit gesture) until the user unfreezes.
    if (trackIdx >= 0 && trackIdx < Session::kNumTracks
        && session.track (trackIdx).frozen.load (std::memory_order_relaxed))
    {
        showDuskAlert (*this, "Track is frozen",
                        "Unfreeze this track to edit its MIDI.");
        return;
    }

    // Drop any pending collapse-teardown so a stale timer can't tear down
    // the editor we're about to open (swap mid-collapse).
    editorTeardownTimer.reset();

    // Remember the tapestrip's edit tool before any modal can change it.
    snapshotEditModeForModal();

    // Mutually exclusive with the audio editor. Opening the piano roll
    // tears down any open audio editor first.
    if (audioEditor != nullptr) closeAudioEditor();

    // Toggle vs swap. Clicking the SAME region while the roll is already
    // open dismisses it (a second click on a region is naturally read as
    // "I'm done"); clicking a DIFFERENT region tears the current roll
    // down and re-opens on the new target so the user doesn't have to
    // dismiss-then-click.
    if (pianoRoll != nullptr)
    {
        const bool sameRegion = (pianoRollTrackIdx == trackIdx
                                  && pianoRollRegionIdx == regionIdx);
        if (sameRegion) { closePianoRollAnimated(); return; }
        closePianoRoll();
    }

    pianoRoll = std::make_unique<PianoRollComponent> (session, engine, trackIdx, regionIdx);
    pianoRollTrackIdx  = trackIdx;
    pianoRollRegionIdx = regionIdx;
    pianoRollDim = std::make_unique<DimOverlay> (0.80f);

    // Sized as a centred panel that leaves a small inset on each side so
    // the dimmed backdrop is still visible (helps users see they're in a
    // modal). The inset shrinks on small windows so the roll always has
    // a workable surface even on a 1280-wide screen.
    const auto bounds = getLocalBounds();
    const int inset = std::max (24, std::min (bounds.getWidth(), bounds.getHeight()) / 16);
    const auto rollBounds = bounds.reduced (inset);

    juce::Rectangle<int> startRect;
    if (tapeStrip != nullptr)
    {
        const auto rr = tapeStrip->midiRegionScreenRect (trackIdx, regionIdx);
        if (! rr.isEmpty())
            startRect = getLocalArea (tapeStrip.get(), rr);
    }

    pianoRollDim->setBounds (bounds);
    pianoRollDim->onClick = [this] { closePianoRollAnimated(); };
    addAndMakeVisible (pianoRollDim.get());

    pianoRoll->onCloseRequested = [this] { closePianoRollAnimated(); };
    pianoRoll->onMouseMovedForCursor =
        [this] (juce::Component& src, juce::Point<int> localInSrc, EditMode m,
                juce::Range<int> cutLine)
        {
            if (cursorOverlay != nullptr)
                cursorOverlay->setMousePosition (src, localInSrc, m, cutLine);
        };
    pianoRoll->onMouseExitedForCursor =
        [this] { if (cursorOverlay != nullptr) cursorOverlay->clearMousePosition(); };
    pianoRoll->onNavigateToRegion = [this] (int t, int newIdx)
    {
        // Close + reopen on the new region. Same-track only; the
        // editor already validated the bounds before calling.
        // Deferred: the call arrives from the editor's own keyPressed
        // frame - closing in place would destroy the component (and the
        // invoked std::function) while both are still on the stack.
        juce::Component::SafePointer<MainComponent> safe (this);
        dusk::callAsync ([safe, t, newIdx]
        {
            if (auto* self = safe.getComponent())
            {
                self->closePianoRoll();
                self->openPianoRoll (t, newIdx);
            }
        });
    };
    addAndMakeVisible (pianoRoll.get());
    pianoRoll->grabKeyboardFocus();

    animateEditorOpen (*pianoRoll, *pianoRollDim, rollBounds, startRect);
}

void MainComponent::closePianoRoll()
{
    // Cancel any in-flight collapse animation BEFORE resetting - the
    // ComponentAnimator holds raw pointers and would tick into freed
    // memory otherwise (false = leave at current pos, don't snap).
    auto& animator = juce::Desktop::getInstance().getAnimator();
    if (pianoRoll != nullptr) animator.cancelAnimation (pianoRoll.get(), false);
    if (pianoRollDim != nullptr) animator.cancelAnimation (pianoRollDim.get(), false);

    // Clear the shared CursorOverlay first - JUCE doesn't fire
    // mouseExit on a component being removed, so the editor's
    // onMouseExitedForCursor callback won't run on its own. Without
    // this, the painted glyph stays stuck at the last position and
    // (on Linux) the native cursor stays hidden because the
    // setNativeCursorVisible(true) transition never fires.
    if (cursorOverlay != nullptr) cursorOverlay->clearMousePosition();
    if (pianoRoll != nullptr) removeChildComponent (pianoRoll.get());
    if (pianoRollDim != nullptr) removeChildComponent (pianoRollDim.get());
    pianoRoll.reset();
    pianoRollDim.reset();
    pianoRollTrackIdx  = -1;
    pianoRollRegionIdx = -1;
    // The editor's toolbar may have changed session.editMode while open;
    // refresh the strip's native cursor so it reflects the mode immediately
    // rather than waiting for the first mouse move over the strip.
    if (tapeStrip != nullptr) { tapeStrip->refreshModeCursor(); tapeStrip->repaint(); }
    scheduleEditModeRestore();
}

void MainComponent::closePianoRollAnimated()
{
    if (pianoRoll == nullptr) return;

    juce::Rectangle<int> startRect;
    if (tapeStrip != nullptr)
    {
        const auto rr = tapeStrip->midiRegionScreenRect (pianoRollTrackIdx, pianoRollRegionIdx);
        if (! rr.isEmpty())
            startRect = getLocalArea (tapeStrip.get(), rr);
    }
    // No usable target rect (region scrolled off): skip the collapse and
    // tear down immediately.
    if (! editorStartRectUsable (startRect)) { closePianoRoll(); return; }

    if (cursorOverlay != nullptr) cursorOverlay->clearMousePosition();
    auto& animator = juce::Desktop::getInstance().getAnimator();
    animator.animateComponent (pianoRoll.get(), startRect, 0.0f, kEditorCloseMs, false, 1.0, 0.0);
    if (pianoRollDim != nullptr)
        animator.animateComponent (pianoRollDim.get(), pianoRollDim->getBounds(), 0.0f,
                                   kEditorCloseMs, false, 1.0, 0.0);
    editorTeardownTimer = std::make_unique<OneShotTimer> (kEditorCloseMs, [this] { closePianoRoll(); });
}

void MainComponent::openAudioEditor (int trackIdx, int regionIdx)
{
    // Frozen tracks are edit-locked: the audio is baked into the rendered WAV,
    // so trimming/fading a region would silently desync it. Block the editor
    // until the user unfreezes (mirrors openPianoRoll for MIDI).
    if (trackIdx >= 0 && trackIdx < Session::kNumTracks
        && session.track (trackIdx).frozen.load (std::memory_order_relaxed))
    {
        showDuskAlert (*this, "Track is frozen",
                        "Unfreeze this track to edit its audio.");
        return;
    }

    // Drop any pending collapse-teardown (see openPianoRoll).
    editorTeardownTimer.reset();

    // Remember the tapestrip's edit tool before any modal can change it.
    snapshotEditModeForModal();

    // Mutual exclusion with the piano roll - opening the audio editor
    // tears down any open piano roll first.
    if (pianoRoll != nullptr) closePianoRoll();

    // Toggle vs swap, mirroring openPianoRoll's semantics. Same target
    // region on a re-double-click closes; a different region swaps.
    if (audioEditor != nullptr)
    {
        const bool sameRegion = (audioEditorTrackIdx == trackIdx
                                  && audioEditorRegionIdx == regionIdx);
        if (sameRegion) { closeAudioEditorAnimated(); return; }
        closeAudioEditor();
    }

    audioEditor = std::make_unique<AudioRegionEditor> (session, engine, trackIdx, regionIdx);
    audioEditorTrackIdx  = trackIdx;
    audioEditorRegionIdx = regionIdx;
    audioEditorDim = std::make_unique<DimOverlay> (0.80f);

    const auto bounds = getLocalBounds();
    const int inset = std::max (24, std::min (bounds.getWidth(), bounds.getHeight()) / 16);
    const auto editorBounds = bounds.reduced (inset);

    juce::Rectangle<int> startRect;
    if (tapeStrip != nullptr)
    {
        const auto rr = tapeStrip->audioRegionScreenRect (trackIdx, regionIdx);
        if (! rr.isEmpty())
            startRect = getLocalArea (tapeStrip.get(), rr);
    }

    audioEditorDim->setBounds (bounds);
    audioEditorDim->onClick = [this] { closeAudioEditorAnimated(); };
    addAndMakeVisible (audioEditorDim.get());

    audioEditor->onCloseRequested = [this] { closeAudioEditorAnimated(); };
    audioEditor->onMouseMovedForCursor =
        [this] (juce::Component& src, juce::Point<int> localInSrc, EditMode m,
                juce::Range<int> cutLine)
        {
            if (cursorOverlay != nullptr)
                cursorOverlay->setMousePosition (src, localInSrc, m, cutLine);
        };
    audioEditor->onMouseExitedForCursor =
        [this] { if (cursorOverlay != nullptr) cursorOverlay->clearMousePosition(); };
    audioEditor->onNavigateToRegion = [this] (int t, int newIdx)
    {
        // Deferred - see the piano-roll navigate handler.
        juce::Component::SafePointer<MainComponent> safe (this);
        dusk::callAsync ([safe, t, newIdx]
        {
            if (auto* self = safe.getComponent())
            {
                self->closeAudioEditor();
                self->openAudioEditor (t, newIdx);
            }
        });
    };
    addAndMakeVisible (audioEditor.get());
    audioEditor->grabKeyboardFocus();

    animateEditorOpen (*audioEditor, *audioEditorDim, editorBounds, startRect);
}

void MainComponent::closeAudioEditor()
{
    // Cancel any in-flight collapse animation BEFORE resetting - the
    // ComponentAnimator holds raw pointers (see closePianoRoll).
    auto& animator = juce::Desktop::getInstance().getAnimator();
    if (audioEditor    != nullptr) animator.cancelAnimation (audioEditor.get(), false);
    if (audioEditorDim != nullptr) animator.cancelAnimation (audioEditorDim.get(), false);

    // Same reasoning as closePianoRoll - clear the overlay first so
    // the painted glyph + Linux native-cursor hide don't outlive the
    // editor.
    if (cursorOverlay != nullptr) cursorOverlay->clearMousePosition();
    if (audioEditor    != nullptr) removeChildComponent (audioEditor.get());
    if (audioEditorDim != nullptr) removeChildComponent (audioEditorDim.get());
    audioEditor.reset();
    audioEditorDim.reset();
    audioEditorTrackIdx  = -1;
    audioEditorRegionIdx = -1;
    // Refresh the strip cursor for any mode change made in the editor (see
    // closePianoRoll).
    if (tapeStrip != nullptr) { tapeStrip->refreshModeCursor(); tapeStrip->repaint(); }
    scheduleEditModeRestore();
}

void MainComponent::snapshotEditModeForModal()
{
    // Snapshot only on the first modal open. A swap (audio<->piano) or a
    // region-navigate re-enters here with a modal still tracked, so the guard
    // preserves the original tapestrip tool instead of capturing the modal's.
    if (! modalEditModeSaved)
    {
        savedEditMode      = session.editMode;
        modalEditModeSaved = true;
    }
}

void MainComponent::scheduleEditModeRestore()
{
    if (! modalEditModeSaved) return;
    // Defer: a swap / navigate closes then re-opens within the same call
    // stack, so by the time this fires a modal is open again and the restore
    // is correctly skipped. A genuine close leaves both editors null.
    juce::Component::SafePointer<MainComponent> safe (this);
    dusk::callAsync ([safe]
    {
        if (auto* s = safe.getComponent()) s->restoreEditModeIfModalClosed();
    });
}

void MainComponent::restoreEditModeIfModalClosed()
{
    if (! modalEditModeSaved) return;
    if (audioEditor != nullptr || pianoRoll != nullptr) return;   // reopened (swap / navigate)
    modalEditModeSaved = false;
    if (session.editMode == savedEditMode) return;
    session.editMode = savedEditMode;
    if (tapeStrip != nullptr) { tapeStrip->refreshModeCursor(); tapeStrip->repaint(); }
}

void MainComponent::closeAudioEditorAnimated()
{
    if (audioEditor == nullptr) return;

    juce::Rectangle<int> startRect;
    if (tapeStrip != nullptr)
    {
        const auto rr = tapeStrip->audioRegionScreenRect (audioEditorTrackIdx, audioEditorRegionIdx);
        if (! rr.isEmpty())
            startRect = getLocalArea (tapeStrip.get(), rr);
    }
    if (! editorStartRectUsable (startRect)) { closeAudioEditor(); return; }

    if (cursorOverlay != nullptr) cursorOverlay->clearMousePosition();
    auto& animator = juce::Desktop::getInstance().getAnimator();
    animator.animateComponent (audioEditor.get(), startRect, 0.0f, kEditorCloseMs, false, 1.0, 0.0);
    if (audioEditorDim != nullptr)
        animator.animateComponent (audioEditorDim.get(), audioEditorDim->getBounds(), 0.0f,
                                   kEditorCloseMs, false, 1.0, 0.0);
    editorTeardownTimer = std::make_unique<OneShotTimer> (kEditorCloseMs, [this] { closeAudioEditor(); });
}

namespace
{
// Tiny Timer subclass used by the tuner overlay to poll the engine's
// pitch atoms at 30 Hz on the message thread. Public-internal because
// the unique_ptr in MainComponent.h holds it as dusk::Timer*.
class TunerPoller final : public dusk::Timer
{
public:
    TunerPoller (Session& s, TunerOverlay& o) : session (s), overlay (o)
    {
        startTimerHz (30);
    }
    void timerCallback() override
    {
        const float hz = session.tuneLatestHz   .load (std::memory_order_relaxed);
        const float lv = session.tuneLatestLevel.load (std::memory_order_relaxed);
        overlay.setDetected (hz, lv);
    }
private:
    Session& session;
    TunerOverlay& overlay;
};
} // namespace

void MainComponent::toggleTuner()
{
    if (tuner != nullptr) { closeTuner(); return; }

    // Pick the track to tune. Prefers the user's most-recent selection
    // (the same selectedTrack the keyboard shortcuts use); falls back
    // to track 0 when nothing's selected so the button always does
    // something rather than silently failing.
    int trackIdx = 0;
    if (tapeStrip != nullptr)
    {
        const int sel = tapeStrip->getSelectedTrack();
        if (sel >= 0 && sel < Session::kNumTracks) trackIdx = sel;
    }
    session.tuneTrackIndex.store (trackIdx, std::memory_order_relaxed);
    session.tuneLatestHz   .store (0.0f,    std::memory_order_relaxed);
    session.tuneLatestLevel.store (0.0f,    std::memory_order_relaxed);

    tuner = std::make_unique<TunerOverlay>();
    tuner->onDismiss = [this] { closeTuner(); };
    tunerDim = std::make_unique<DimOverlay>();
    tunerDim->setBounds (getLocalBounds());
    tunerDim->onClick = [this] { closeTuner(); };
    addAndMakeVisible (tunerDim.get());

    tuner->setBounds (getLocalBounds());
    addAndMakeVisible (tuner.get());

    tunerPoller = std::make_unique<TunerPoller> (session, *tuner);
}

void MainComponent::closeTuner()
{
    tunerPoller.reset();
    if (tuner    != nullptr) removeChildComponent (tuner.get());
    if (tunerDim != nullptr) removeChildComponent (tunerDim.get());
    // Deferred destruction - closeTuner is reached from tunerDim's own
    // mouseDown and the overlay's onDismiss, so resetting in place would
    // destroy the component whose mouse handler is still on the stack
    // (the EmbeddedModal::close() teardown pattern).
    if (tuner != nullptr || tunerDim != nullptr)
    {
        std::shared_ptr<juce::Component> trashTuner (tuner.release());
        std::shared_ptr<juce::Component> trashDim   (tunerDim.release());
        dusk::callAsync ([trashTuner, trashDim]() mutable {});
    }
    session.tuneTrackIndex.store (-1, std::memory_order_relaxed);
}

void MainComponent::closeVirtualKeyboard()
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    if (virtualKeyboardWindow == nullptr || ! virtualKeyboardWindow->isOpen())
        return;

    // Every route out of the keyboard ends here - the toggle, Escape and the click
    // outside the plate, the modal yield, and the close a graphics failure forces -
    // so the in-flight step-record chord state is reset here rather than at one of
    // them. Stale held-counters would otherwise survive to the next open.
    if (pianoRoll != nullptr)
        pianoRoll->resetStepRecordState();
    virtualKeyboardWindow->close();
   #endif
}

void MainComponent::openVirtualKeyboardForCapture (const std::string& capturePath)
{
    toggleVirtualKeyboard();
   #if DUSKSTUDIO_HAS_NATIVE_UI
    if (virtualKeyboardWindow != nullptr && virtualKeyboardWindow->isOpen())
        virtualKeyboardWindow->captureNextFrameTo (capturePath);
   #else
    (void) capturePath;
   #endif
}

void MainComponent::toggleVirtualKeyboard()
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    setStatusText ("Virtual keyboard unavailable: built without the native UI");
   #else
    if (virtualKeyboardWindow != nullptr && virtualKeyboardWindow->isOpen())
    {
        closeVirtualKeyboard();
        return;
    }

    auto* const topLevel = getTopLevelComponent();
    const auto parentHandle = topLevel != nullptr
                            ? embedscale::nativeParentHandle (*topLevel) : 0;
    if (parentHandle == 0)
    {
        setStatusText ("Virtual keyboard unavailable: main window is not ready");
        return;
    }

    if (auto hook = EmbeddedModal::beforeModalShown())
        hook();

    if (virtualKeyboardWindow == nullptr)
    {
        virtualKeyboardWindow = std::make_unique<imgui::DuskPanelWindow> (
            "dusk-studio-virtual-keyboard", "virtual-keyboard", "Virtual keyboard");

        imgui::DuskPanelWindow::Callbacks callbacks;
        callbacks.dismissed = [this] { closeVirtualKeyboard(); };
        callbacks.closed = [this]
        {
            virtualKeyboardDim.reset();
            virtualKeyboardHider.restore();
            reclaimFocusFromNotepad();
        };
        callbacks.shortcut = [] (imgui::ShellShortcut shortcut)
        {
            return dispatchShellShortcut (shortcut);
        };
        callbacks.geometry = [this]
        {
            auto* const top = getTopLevelComponent();
            if (top == nullptr || virtualKeyboardWindow == nullptr)
                return imgui::DuskPanelWindow::Geometry {};
            const auto plate = virtualKeyboardWindow->plateSize();
            const auto bounds = embedscale::centredChildBounds (*top, plate.width,
                                                                plate.height);
            if (virtualKeyboardDim != nullptr)
            {
                virtualKeyboardDim->setBounds (top->getLocalBounds());
                virtualKeyboardDim->setNativeChildArea (bounds.expanded (1));
            }
            const auto g = embedscale::childGeometryFor (*top, bounds);
            return imgui::DuskPanelWindow::Geometry { g.x, g.y, g.width, g.height, g.scale };
        };
        virtualKeyboardWindow->setCallbacks (std::move (callbacks));
    }

    // Step-record wiring: while the piano roll is open, each typed note also lands as
    // a MidiNote at the playhead. The roll's stepRecordNoteOn / Off handle the
    // chord-aware playhead advance.
    juce::Component::SafePointer<MainComponent> safeThis (this);
    virtualKeyboardWindow->setView (imgui::makeVirtualKeyboardView (
        engine,
        [safeThis] (int note, int velocity, int)
        {
            if (auto* self = safeThis.getComponent())
                if (self->pianoRoll != nullptr)
                    self->pianoRoll->stepRecordNoteOn (note, velocity);
        },
        [safeThis] (int note, int)
        {
            if (auto* self = safeThis.getComponent())
                if (self->pianoRoll != nullptr)
                    self->pianoRoll->stepRecordNoteOff (note);
        }));

    const auto plate = virtualKeyboardWindow->plateSize();
    const auto logical = embedscale::centredChildBounds (*topLevel, plate.width,
                                                         plate.height);

    virtualKeyboardDim = std::make_unique<DimOverlay> (virtualKeyboardWindow->dimAlpha());
    virtualKeyboardDim->setBounds (topLevel->getLocalBounds());
    virtualKeyboardDim->setNativeChildArea (logical.expanded (1));
    virtualKeyboardDim->onClick = [this] { closeVirtualKeyboard(); };
    topLevel->addAndMakeVisible (virtualKeyboardDim.get());
    virtualKeyboardHider.hideUnder (*topLevel, { virtualKeyboardDim.get() });

    const auto geometry = embedscale::childGeometryFor (*topLevel, logical);
    if (! virtualKeyboardWindow->open (parentHandle,
                                       { geometry.x, geometry.y, geometry.width,
                                         geometry.height, geometry.scale }))
    {
        virtualKeyboardDim.reset();
        virtualKeyboardHider.restore();
        const auto& why = virtualKeyboardWindow->lastOpenFailure();
        setStatusText (why.empty()
                           ? "Virtual keyboard unavailable: this display cannot carry it"
                           : why.c_str());
    }
   #endif
}

void MainComponent::toggleNotepad()
{
#if ! DUSKSTUDIO_HAS_NATIVE_UI
    setStatusText ("Notepad unavailable: built without the native notepad UI");
#else
    if (notepadWindow != nullptr && notepadWindow->isOpen())
    {
        // The close callback restores the DAW and flushes the sidecar once the
        // embedded DAF child has actually disappeared.
        notepadWindow->close();
        return;
    }

    if (! appconfig::getNotepadEnabled())
    {
        setStatusText ("Notepad is switched off in app-config.properties (notepad_enabled)");
        return;
    }

    if (notepadWindow == nullptr)
        notepadWindow = std::make_unique<NativeNotepadWindow>();

    auto* const topLevel = getTopLevelComponent();
    auto* const peer = topLevel != nullptr ? topLevel->getPeer() : nullptr;
    if (peer == nullptr || peer->getNativeHandle() == nullptr)
    {
        setStatusText ("Notepad unavailable: main window is not ready");
        return;
    }

    juce::Component::SafePointer<MainComponent> safeThis (this);
    notepadWindow->setCallbacks (
        [safeThis] (const std::string& text, bool dirty)
        {
            if (auto* self = safeThis.getComponent())
            {
                self->notepadText = juce::String::fromUTF8 (text.c_str());
                self->notepadDirty = dirty;
            }
        },
        [safeThis]
        {
            if (auto* self = safeThis.getComponent())
            {
                self->notepadDim.reset();
                self->notepadEditorHider.restore();
                self->reclaimFocusFromNotepad();
                self->saveNotepadNow();
            }
        },
        [safeThis] (const std::string& target)
        {
            if (auto* self = safeThis.getComponent())
            {
                const auto value = juce::String::fromUTF8 (target.c_str());
                if (value.startsWithIgnoreCase ("https://")
                    || value.startsWithIgnoreCase ("http://")
                    || value.startsWithIgnoreCase ("mailto:"))
                    juce::URL (value).launchInDefaultBrowser();
                else
                    self->setStatusText ("Link not opened: only http, https, and mailto links are supported");
            }
        });

    // A JUCE sibling beneath the embedded native child blocks the DAW and
    // receives clicks outside the notepad. Keep dismissal at this native host
    // boundary: the DAF/ImGui editor owns document editing only, while close()
    // preserves the deferred native teardown -> onClosed -> saveNotepadNow()
    // sequence above.
    notepadDim = std::make_unique<DimOverlay> (0.80f);
    notepadDim->setBounds (getLocalBounds());
    // One pixel of slack absorbs the rounding between the logical overlay and
    // the physical child.
    notepadDim->setNativeChildArea (
        getLocalArea (topLevel, notepadChildBounds (*topLevel)).expanded (1));
    notepadDim->onClick = [safeThis]
    {
        if (auto* self = safeThis.getComponent())
            if (self->notepadWindow != nullptr && self->notepadWindow->isOpen())
                self->notepadWindow->close();
    };
    addAndMakeVisible (notepadDim.get());
    // The dim only covers JUCE's z-order. A native plugin editor keeps painting
    // over both it and the notepad's own child, so it hides for the notepad's
    // lifetime exactly as it does under an embedded modal.
    notepadEditorHider.hideUnder (*this, { notepadDim.get() });

    const bool opened = notepadWindow->open (
        reinterpret_cast<std::uintptr_t> (peer->getNativeHandle()),
        notepadGeometryFor (*topLevel),
        notepadText.toStdString(),
        session.getSessionDirectory() != juce::File(),
        notepadDirty);

    if (! opened)
    {
        notepadDim.reset();
        notepadEditorHider.restore();
        const auto& why = notepadWindow->lastOpenFailure();
        setStatusText (why.empty()
                           ? "Unable to embed session notepad with the current display backend"
                           : why.c_str());
    }
#endif
}

void MainComponent::dismissNotepad (bool saveChanges)
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    const bool wasOpen = notepadDim != nullptr;
    if (notepadWindow != nullptr)
    {
        // close() commits an open chord slot, which reports the new text through
        // onTextChanged; clearing the callbacks first would drop that final edit.
        notepadWindow->close();
        notepadWindow->setCallbacks ({}, {}, {});
        notepadWindow.reset();
    }
    notepadDim.reset();
    notepadEditorHider.restore();
    // wasOpen: only a dismissal that actually tore down a live notepad needs
    // the focus handed back; the deferred closed callback that normally does
    // it never runs on this route.
    if (wasOpen)
        reclaimFocusFromNotepad();
   #endif

    if (saveChanges)
        (void) saveNotepadNow();
}

void MainComponent::reclaimFocusFromNotepad()
{
    // Quit paths that bypass requestQuit (SIGTERM, logout) reach this from the
    // destructor cascade, where a WM round-trip would poke a half-destroyed
    // top-level window.
    if (tearingDown)
        return;
    // The native child held keyboard focus at the X11 level; pull it back so
    // transport / edit shortcuts work without a stray click first (same reason
    // as dismissStartupDialog).
    if (auto* window = getTopLevelComponent())
        window->toFront (true);
    if (isShowing())
        grabKeyboardFocus();
}

void MainComponent::yieldNotepadWindow (bool saveChanges)
{
   #if DUSKSTUDIO_HAS_NATIVE_UI
    // The dim exists for exactly as long as the notepad does, including the
    // deferred native teardown between close() and its closed callback.
    if (notepadDim != nullptr)
        dismissNotepad (saveChanges);
   #else
    (void) saveChanges;
   #endif
}

bool MainComponent::saveNotepadNow()
{
    if (! notepadDirty) return true;
    const auto dir = session.getSessionDirectory();
    if (dir == juce::File())
    {
       #if DUSKSTUDIO_HAS_NATIVE_UI
        if (notepadWindow != nullptr)
            notepadWindow->markSaveFailed();
       #endif
        setStatusText ("Notepad unsaved: save the session first");
        return false;
    }
    if (SessionSerializer::saveNotepad (dir, notepadText))
    {
        notepadDirty = false;
       #if DUSKSTUDIO_HAS_NATIVE_UI
        if (notepadWindow != nullptr)
            notepadWindow->markSaved();
       #endif
        return true;
    }

   #if DUSKSTUDIO_HAS_NATIVE_UI
    if (notepadWindow != nullptr)
        notepadWindow->markSaveFailed();
   #endif
    setStatusForPath ("Notepad save failed", dir.getChildFile ("notepad.md"));
    return false;
}
} // namespace duskstudio
