#include "DuskStudioApp.h"
#include "ui/AppConfig.h"
#if DUSKSTUDIO_HAS_NATIVE_CLAP
  #include "ui/ClapPluginEditorComponent.h"   // Linux-only native CLAP editor test
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
  #include "engine/lv2/NativeLv2Slot.h"       // Linux-only native LV2 editor test
  #include "ui/Lv2PluginEditorComponent.h"
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
  #include "engine/vst3/Vst3Bundle.h"         // Linux-only native VST3 editor test
  #include "engine/vst3/Vst3Instance.h"
  #include "ui/Vst3PluginEditorComponent.h"
#endif
#include "ui/ConsoleView.h"
#include "ui/MainComponent.h"
#include "ui/WindowState.h"
#include "engine/AudioEngine.h"
#include "engine/AudioPipelineSelfTest.h"
#include "engine/BounceEngine.h"
#include "engine/PluginManager.h"
#include "engine/PluginSlot.h"
#if defined(DUSKSTUDIO_HAS_AUDIOFILE)
 #include "engine/audiofile/FileReader.h"
 #include "engine/audiofile/FileWriter.h"
#endif
#include "session/SessionSerializer.h"
#include "util/CrashHandler.h"
#if JUCE_LINUX
 #include "engine/ipc/IpcSelfTest.h"
#endif
#if defined(__linux__)
 #include "engine/alsa/AlsaPerformanceTest.h"
 #include "engine/device/DeviceManager.h"
 #include "engine/device/IODeviceCallback.h"
 #include "foundation/Text.h"
#endif
#include "session/Session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

#if JUCE_LINUX
 #include <sys/mman.h>
 #include <sys/resource.h>
#endif
#include "ui/PlatformWindowing.h"

// Embedded brand icon - wired in CMakeLists.txt via juce_add_binary_data.
// Linux relies on this for runtime _NET_WM_ICON since JUCE 8 has no
// getApplicationIcon hook; macOS / Windows get it from the bundled
// .icns / PE .ico but setIcon is harmless there.
#if __has_include("BinaryData.h")
 #include "BinaryData.h"
 #define DUSKSTUDIO_HAS_BINARY_ICON 1
#else
 #define DUSKSTUDIO_HAS_BINARY_ICON 0
#endif

namespace duskstudio
{
class DuskStudioApp::MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow (juce::String name)
        : DocumentWindow (std::move (name),
                          juce::Desktop::getInstance().getDefaultLookAndFeel()
                              .findColour (juce::ResizableWindow::backgroundColourId),
                          DocumentWindow::allButtons,
                          /*addToDesktop*/ false)
    {
        // Defer addToDesktop until ALL style flags are set. Reason:
        // setUsingNativeTitleBar + setResizable destroy + recreate the
        // peer; if we add to desktop first, those calls each spawn a
        // fresh peer (= 3 peers across MainWindow ctor). Doing style
        // setup with no peer means flags are stashed and applied by
        // the single addToDesktop call at the end of this ctor.
        //
        // Linux: route the main peer to X11 (XWayland) instead of
        // wl_surface. Required for inline-embedded plugin editors -
        // VST3/LV2/CLAP SDKs hand back X11 Window IDs that can only
        // reparent into an X11 host. XWayland is transparent and ships
        // on every mainstream Wayland session.

        setUsingNativeTitleBar (true);
        setContentOwned (new MainComponent(), true);
        // Native title bar already provides edge resize handles on every OS
        // we ship to. Passing `true` for useBottomRightCornerResizer adds
        // JUCE's overlay resizer, which on macOS sits ON TOP of the OS
        // window's lower-right corner and intercepts the live-resize
        // events the system sends during the fullscreen animation,
        // leaving the content stuck at its pre-fullscreen size.
        setResizable (true, false);
        // Min height keeps the console usable; the tape strip is collapsible
        // so we don't need to budget for it in the floor.
        setResizeLimits (ConsoleView::minimumContentWidth() + 24, 750, 32768, 32768);

        // Restore prior session's window geometry. JUCE's
        // restoreWindowStateFromString rebuilds bounds + fullscreen state
        // from the same string getWindowStateAsString() produced. We then
        // sanity-check the restored bounds against connected displays -
        // if the saved monitor is gone, undo the restore and centre at
        // MainComponent's default size.
        bool restored = false;
        const auto savedState = WindowState::load();
        if (savedState.isNotEmpty() && restoreWindowStateFromString (savedState))
        {
            // Validate using the WINDOWED bounds (so a fullscreen-on-an-
            // unplugged-monitor case still falls back gracefully).
            const auto checkRect = isFullScreen()
                ? juce::Rectangle<int> (0, 0,
                                          std::max (getWidth(), 800),
                                          std::max (getHeight(), 600))
                : getBounds();
            if (WindowState::rectIsUsable (checkRect))
                restored = true;
        }

        if (! restored)
        {
            // Some tiling/Wayland WMs auto-maximize new windows. Explicitly
            // opt out of full-screen so we open at the size MainComponent
            // requested.
            setFullScreen (false);
            centreWithSize (getWidth(), getHeight());
        }

        // Single addToDesktop call with all style flags already set.
        // X11-latched on Linux so the peer factory in
        // Component::createNewPeer picks LinuxComponentPeer (XWayland)
        // instead of WaylandComponentPeer.
       #if defined(__linux__)
        duskstudio::platform::preferX11ForNextNativeWindow();
       #endif
        addToDesktop (getDesktopWindowStyleFlags());
       #if defined(__linux__)
        duskstudio::platform::clearPreferX11ForNativeWindow();
        // The peer now exists (JUCE's X display is up), so a non-fatal X error
        // handler installed here captures JUCE's handler as its chain target.
        // Stops a dying OOP plugin editor window from core-dumping the host.
        duskstudio::platform::installNonFatalXErrorHandler();
       #endif

        // Brand icon on the live peer. macOS / Windows already pick this
        // up from the bundled .icns / PE .ico, but JUCE 8 does NOT auto-
        // set _NET_WM_ICON on Linux so the WM / taskbar / Alt-Tab show a
        // default placeholder unless we call setIcon explicitly.
       #if DUSKSTUDIO_HAS_BINARY_ICON
        const auto brandIcon = juce::ImageCache::getFromMemory (
            BinaryData::dsicon_png, BinaryData::dsicon_pngSize);
        if (brandIcon.isValid())
            setIcon (brandIcon);
       #endif

        // Hide explicitly so the deferred setVisible(true) below gets
        // a clean transition - JUCE's Component default is visible=true,
        // and addToDesktop just made the peer visible.
        setVisible (false);

        // Combined async tick: setVisible(true) creates the peer at the
        // already-finalised bounds, then bringWindowToFront promotes
        // focus once the peer exists. bringWindowToFront is a no-op on
        // non-Linux builds.
        juce::Component::SafePointer<MainWindow> safeThis (this);
        juce::MessageManager::callAsync ([safeThis]
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr) return;
            self->setVisible (true);
            if (auto* peer = self->getPeer())
                duskstudio::platform::bringWindowToFront (*peer);
        });
    }

    void closeButtonPressed() override
    {
        // Delegate to MainComponent's requestQuit, which checks dirty
        // state (autosave-newer-than-saved) and shows the Dusk Studio-styled
        // Save / Don't Save / Cancel modal only when there are actual
        // unsaved changes. No dirty changes -> quit immediately.
        if (auto* main = dynamic_cast<MainComponent*> (getContentComponent()))
        {
            main->requestQuit();
            return;
        }
        // No MainComponent (shouldn't happen in normal use) - fall back
        // to immediate quit so the X still works.
        JUCEApplication::getInstance()->systemRequestedQuit();
    }
};

DuskStudioApp::DuskStudioApp() = default;
DuskStudioApp::~DuskStudioApp() = default;

// True when a native window can be created; on failure prints the XWayland
// guidance. Called before the main window AND at the top of each *_EDITOR_TEST
// gate (those open real X11 windows too) - but never on the headless paths,
// which must keep running without any display.
static bool displayUsableOrExplain()
{
    const char* waylandDisplay = std::getenv ("WAYLAND_DISPLAY");
    const bool onWayland = waylandDisplay != nullptr && waylandDisplay[0] != '\0';
    if (duskstudio::platform::hasUsableDisplay())
    {
        // One self-describing line so support reports name their session type.
        if (onWayland)
            std::fprintf (stderr, "Dusk Studio: Wayland session detected - using the X11 display.\n");
        return true;
    }
    const char* xDisplay = std::getenv ("DISPLAY");
    const bool haveDisplayVar = xDisplay != nullptr && xDisplay[0] != '\0';
    if (onWayland && ! haveDisplayVar)
        std::fprintf (stderr,
            "Dusk Studio needs an X11 display and could not open one.\n"
            "This is a Wayland session with no DISPLAY set - most likely XWayland\n"
            "is not running. Enable it, then relaunch:\n"
            "  sway:   add `xwayland enable` to your config and restart sway\n"
            "  niri:   run `xwayland-satellite` and export the DISPLAY it prints\n"
            "  labwc:  start labwc with XWayland enabled (the default build)\n"
            "  river / mango / other wlroots: ensure XWayland is installed and enabled\n");
    else if (onWayland)
        std::fprintf (stderr,
            "Dusk Studio needs an X11 display and could not open one.\n"
            "DISPLAY is set (%s) but connecting to it failed - XWayland may have\n"
            "stopped, or access was refused. Restart your session (or XWayland)\n"
            "and relaunch.\n",
            xDisplay);
    else if (haveDisplayVar)
        std::fprintf (stderr,
            "Dusk Studio needs an X11 display and could not open one.\n"
            "DISPLAY is set (%s) but connecting to it failed - the server may be\n"
            "unreachable or access refused (check XAUTHORITY / xhost). Point\n"
            "DISPLAY at a running X server, or launch from a graphical session.\n",
            xDisplay);
    else
        std::fprintf (stderr,
            "Dusk Studio needs an X11 display and could not open one.\n"
            "No display was found (DISPLAY is unset). Launch from a graphical\n"
            "session, or set DISPLAY to a reachable X server.\n");
    std::fflush (stderr);
    return false;
}

#if JUCE_LINUX
static void primeRealtimeAudio()
{
    // Pin every page of the process in physical RAM so the audio thread
    // never blocks on a page fault during a callback. Ardour, Bitwig, and
    // every other low-latency Linux DAW does this. Requires `memlock` rlimit
    // - typically `unlimited` for the audio group via /etc/security/limits.d.
    if (mlockall (MCL_CURRENT | MCL_FUTURE) != 0)
    {
        DBG ("mlockall failed (errno=" << errno
             << ") - audio thread may suffer page-fault stalls under memory pressure");
    }
}
#endif

// Headless self-test entry: triggered by setting DUSKSTUDIO_RUN_SELFTEST=1 in
// the environment before launching. Runs AudioPipelineSelfTest::runAll() at
// startup, writes the formatted report to stdout, and quits. The MainWindow
// (and the entire UI) is never created in this mode. Useful for automated
// loops without a display server.
static bool envFlagSet (const char* name)
{
    const char* v = std::getenv (name);
    if (v == nullptr) return false;
    const juce::String s (v);
    return s.getIntValue() != 0
        || s.equalsIgnoreCase ("true")
        || s.equalsIgnoreCase ("yes");
}

// Headless tone-test probe: opens a chosen device at a chosen rate/buffer and
// renders a 440 Hz sine without attaching an AudioEngine. Useful for isolating
// the backend + driver path from engine DSP when diagnosing low-buffer noise.
//
// Env vars (all optional, sensible defaults):
//   DUSKSTUDIO_TONE_BACKEND     "ALSA" | "PipeWire"       (default "ALSA")
//   DUSKSTUDIO_TONE_DEVICE      output device name        (default "" = backend default)
//   DUSKSTUDIO_TONE_RATE        sample rate in Hz         (default 48000)
//   DUSKSTUDIO_TONE_BUFFER      buffer size in samples    (default 128)
//   DUSKSTUDIO_TONE_DURATION_MS playback duration (ms)    (default 2000)
#if defined(__linux__)
class HeadlessSineCallback final : public device::IODeviceCallback
{
public:
    void audioDeviceIOCallback (const float* const*, int,
                                float* const* outputChannelData, int numOutputChannels,
                                int numSamples, const device::CallbackContext&) override
    {
        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float value = 0.5f * (float) std::sin (phase);
            phase += phaseStep;
            if (phase >= twoPi) phase -= twoPi;

            for (int channel = 0; channel < numOutputChannels; ++channel)
                if (outputChannelData[channel] != nullptr)
                    outputChannelData[channel][sample] = value;
        }
        renderedFrames.fetch_add ((std::uint64_t) numSamples, std::memory_order_relaxed);
    }

    void audioDeviceAboutToStart (device::IODevice* dev) override
    {
        phase = 0.0;
        phaseStep = dev->getCurrentSampleRate() > 0.0
                  ? twoPi * 440.0 / dev->getCurrentSampleRate() : 0.0;
        renderedFrames.store (0, std::memory_order_relaxed);
    }

    void audioDeviceStopped() override {}

    std::uint64_t framesRendered() const noexcept
    {
        return renderedFrames.load (std::memory_order_relaxed);
    }

private:
    static constexpr double twoPi = 6.28318530717958647692;
    double phase = 0.0;
    double phaseStep = 0.0;
    std::atomic<std::uint64_t> renderedFrames { 0 };
};

static void runHeadlessToneTest()
{
    auto env = [] (const char* name) -> std::string {
        if (const char* v = std::getenv (name)) return v;
        return {};
    };
    const std::string backendEnv = env ("DUSKSTUDIO_TONE_BACKEND");
    const std::string rateEnv    = env ("DUSKSTUDIO_TONE_RATE");
    const std::string bufferEnv  = env ("DUSKSTUDIO_TONE_BUFFER");
    const std::string durationEnv = env ("DUSKSTUDIO_TONE_DURATION_MS");
    const std::string backendName = backendEnv.empty() ? "ALSA" : backendEnv;
    const std::string deviceName  = env ("DUSKSTUDIO_TONE_DEVICE");
    const double targetRate = rateEnv.empty() ? 48000.0 : dusk::text::getDoubleValue (rateEnv);
    const int targetBuf = bufferEnv.empty() ? 128 : dusk::text::getIntValue (bufferEnv);
    const int durationMs = durationEnv.empty() ? 2000 : dusk::text::getIntValue (durationEnv);

    device::DeviceManager dm;

    std::fprintf (stdout, "=== Dusk Studio Headless Tone Test ===\n");
    std::fprintf (stdout, "Requested: backend=%s device=\"%s\" rate=%.0f buf=%d duration=%dms\n",
                  backendName.c_str(), deviceName.c_str(), targetRate, targetBuf, durationMs);

    if (const auto err = dm.initialise (0, 2, {}, false); ! err.empty())
        std::fprintf (stdout, "init: %s\n", err.c_str());

    dm.setCurrentDeviceType (backendName, /*treatAsChosen*/ true);
    auto* type = dm.getCurrentDeviceType();
    if (type == nullptr || type->getTypeName() != backendName)
    {
        std::fprintf (stdout, "Open failed: backend \"%s\" is unavailable\n", backendName.c_str());
        std::fprintf (stdout, "=== End of Tone Test ===\n");
        std::fflush (stdout);
        return;
    }

    type->scanForDevices();
    const auto outputNames = type->getDeviceNames (false);
    device::DeviceSetup setup;
    setup.outputDeviceName = deviceName;
    if (setup.outputDeviceName.empty())
    {
        int defaultIndex = type->getDefaultDeviceIndex (false);
        if (defaultIndex < 0 && ! outputNames.empty()) defaultIndex = 0;
        if (defaultIndex >= 0 && defaultIndex < (int) outputNames.size())
            setup.outputDeviceName = outputNames[(std::size_t) defaultIndex];
    }
    setup.sampleRate = targetRate;
    setup.bufferSize = targetBuf;

    if (const auto err = dm.setSetup (setup, /*treatAsChosen*/ true); ! err.empty())
        std::fprintf (stdout, "setSetup: %s\n", err.c_str());

    if (auto* dev = dm.getCurrentDevice())
    {
        std::fprintf (stdout,
                      "Opened: \"%s\" type=%s rate=%.0f buf=%d activeOut=%d activeIn=%d bitDepth=%d\n",
                      dev->getName().c_str(), type->getTypeName().c_str(),
                      dev->getCurrentSampleRate(), dev->getCurrentBufferSizeSamples(),
                      dev->getActiveOutputChannels().count(), dev->getActiveInputChannels().count(),
                      dev->getCurrentBitDepth());
        const int xrunBefore = dev->getXRunCount();
        HeadlessSineCallback tone;
        dm.addCallback (&tone);
        std::this_thread::sleep_for (std::chrono::milliseconds (durationMs));
        dm.removeCallback (&tone);
        const int xrunAfter = dev->getXRunCount();

        std::fprintf (stdout, "Sine render: %llu frames\n",
                      (unsigned long long) tone.framesRendered());
        std::fprintf (stdout, "Backend xrun count: before=%d after=%d delta=%d\n",
                      xrunBefore, xrunAfter, xrunAfter - xrunBefore);
    }
    else
        std::fprintf (stdout, "Open failed: getCurrentDevice() returned nullptr\n");

    std::fprintf (stdout, "=== End of Tone Test ===\n");
    std::fflush (stdout);
}
#else
static void runHeadlessToneTest()
{
    auto env = [] (const char* name) -> juce::String {
        if (const char* v = std::getenv (name)) return juce::String (v);
        return {};
    };
    const juce::String backendName = env ("DUSKSTUDIO_TONE_BACKEND").isNotEmpty()
                                     ? env ("DUSKSTUDIO_TONE_BACKEND") : juce::String ("ALSA");
    const juce::String deviceName  = env ("DUSKSTUDIO_TONE_DEVICE");
    const double       targetRate  = env ("DUSKSTUDIO_TONE_RATE").isNotEmpty()
                                     ? env ("DUSKSTUDIO_TONE_RATE").getDoubleValue() : 48000.0;
    const int          targetBuf   = env ("DUSKSTUDIO_TONE_BUFFER").isNotEmpty()
                                     ? env ("DUSKSTUDIO_TONE_BUFFER").getIntValue()  : 128;
    const int          durationMs  = env ("DUSKSTUDIO_TONE_DURATION_MS").isNotEmpty()
                                     ? env ("DUSKSTUDIO_TONE_DURATION_MS").getIntValue() : 2000;

    juce::AudioDeviceManager dm;

    std::fprintf (stdout, "=== Dusk Studio Headless Tone Test ===\n");
    std::fprintf (stdout, "Requested: backend=%s device=\"%s\" rate=%.0f buf=%d duration=%dms\n",
                  backendName.toRawUTF8(), deviceName.toRawUTF8(),
                  targetRate, targetBuf, durationMs);

    if (const auto err = dm.initialiseWithDefaultDevices (0, 2); err.isNotEmpty())
        std::fprintf (stdout, "init: %s\n", err.toRawUTF8());

    dm.setCurrentAudioDeviceType (backendName, /*treatAsChosen*/ true);

    auto setup = dm.getAudioDeviceSetup();
    if (deviceName.isNotEmpty())
        setup.outputDeviceName = deviceName;
    setup.sampleRate            = targetRate;
    setup.bufferSize            = targetBuf;
    setup.useDefaultInputChannels  = true;
    setup.useDefaultOutputChannels = true;

    if (const auto err = dm.setAudioDeviceSetup (setup, /*treatAsChosen*/ true); err.isNotEmpty())
        std::fprintf (stdout, "setAudioDeviceSetup: %s\n", err.toRawUTF8());

    if (auto* dev = dm.getCurrentAudioDevice())
    {
        std::fprintf (stdout,
                      "Opened: \"%s\" type=%s rate=%.0f buf=%d activeOut=%d activeIn=%d bitDepth=%d\n",
                      dev->getName().toRawUTF8(), dm.getCurrentAudioDeviceType().toRawUTF8(),
                      dev->getCurrentSampleRate(), dev->getCurrentBufferSizeSamples(),
                      dev->getActiveOutputChannels().countNumberOfSetBits(),
                      dev->getActiveInputChannels().countNumberOfSetBits(),
                      dev->getCurrentBitDepth());
        const int xrunBefore = dev->getXRunCount();

        dm.playTestSound();
        juce::Thread::sleep (durationMs);

        const int xrunAfter = dev->getXRunCount();
        std::fprintf (stdout, "Backend xrun count: before=%d after=%d delta=%d\n",
                      xrunBefore, xrunAfter, xrunAfter - xrunBefore);
    }
    else
        std::fprintf (stdout, "Open failed: getCurrentAudioDevice() returned nullptr\n");

    std::fprintf (stdout, "=== End of Tone Test ===\n");
    std::fflush (stdout);
}
#endif

// Headless instrument-plugin test: load a single VST3 / LV2 / AU
// instrument, send a synthetic MIDI chord, and report whether the
// plugin produced audio. Exercises the same in-process PluginSlot path
// the GUI uses (loadFromFile + processStereoBlock) - distinct from
// DUSKSTUDIO_IPC_HOST_TEST which exercises the OOP dusk-studio-plugin-host path.
//
// Usage:
//   DUSKSTUDIO_INSTRUMENT_TEST=/home/marc/.vst3/u-he/Diva.vst3 ./Dusk Studio
static void runHeadlessInstrumentTest (const juce::String& pluginPath)
{
    constexpr double sampleRate = 48000.0;
    constexpr int    blockSize  = 256;
    constexpr int    numBlocks  = 200;          // ~1.07 s of audio
    constexpr int    chordHoldBlocks = 150;     // release before measurement ends

    std::fprintf (stdout, "=== Dusk Studio Headless Instrument Test ===\n");
    std::fprintf (stdout, "Plugin: %s\nSR=%.0f BS=%d Blocks=%d\n\n",
                  pluginPath.toRawUTF8(), sampleRate, blockSize, numBlocks);

    PluginManager manager;
    manager.scanInstalledPlugins();   // populates the cache; cheap if already cached

    PluginSlot slot;
    slot.setManager (manager);
    slot.prepareToPlay (sampleRate, blockSize);

    juce::String err;
    if (! slot.loadFromFile (juce::File (pluginPath), err))
    {
        std::fprintf (stderr, "FAIL: loadFromFile: %s\n", err.toRawUTF8());
        return;
    }

    // C-major triad on channel 1, MIDI velocity 100. Note On at sample 0
    // of the first block, Note Off at sample 0 of block kChordHoldBlocks.
    constexpr int kChordNotes[] = { 60, 64, 67 };

    std::vector<float> L ((size_t) blockSize), R ((size_t) blockSize);
    float peak = 0.0f;
    double rms  = 0.0;
    long long counted = 0;

    for (int b = 0; b < numBlocks; ++b)
    {
        std::fill (L.begin(), L.end(), 0.0f);
        std::fill (R.begin(), R.end(), 0.0f);

        juce::MidiBuffer midi;
        if (b == 0)
            for (int n : kChordNotes)
                midi.addEvent (juce::MidiMessage::noteOn (1, n, (std::uint8_t) 100), 0);
        if (b == chordHoldBlocks)
            for (int n : kChordNotes)
                midi.addEvent (juce::MidiMessage::noteOff (1, n), 0);

        slot.processStereoBlock (L.data(), R.data(), blockSize, midi);

        for (int s = 0; s < blockSize; ++s)
        {
            const float l = L[(size_t) s];
            const float r = R[(size_t) s];
            const float mag = std::max (std::abs (l), std::abs (r));
            if (mag > peak) peak = mag;
            rms += (double) l * (double) l + (double) r * (double) r;
            counted += 2;
        }
    }

    const double rmsVal = counted > 0 ? std::sqrt (rms / (double) counted) : 0.0;
    std::fprintf (stdout, "Result: peak=%.6f rms=%.6f bypassed=%d auto-bypassed=%d\n",
                  peak, rmsVal,
                  (int) slot.isBypassed(),
                  (int) slot.wasAutoBypassed());
    if (peak < 1.0e-6f)
        std::fprintf (stdout, "VERDICT: SILENCE - plugin produced no output.\n");
    else
        std::fprintf (stdout, "VERDICT: AUDIO PRESENT - plugin produced output.\n");
    std::fprintf (stdout, "=== End of Instrument Test ===\n");
    std::fflush (stdout);
}

// Headless pipeline test: drive the full Engine + Session pipeline with
// an instrument plugin loaded on track 0, inject a MIDI chord through
// the same `perInputMidi` path live MIDI takes, and report stage-by-
// stage where signal exists in the chain. Used to bisect "GUI has Diva
// loaded but no audio" between PluginSlot (validated by the instrument-
// test path) and engine routing (this).
//
// Optionally loads a session file via DUSKSTUDIO_PIPELINE_TEST_SESSION so we
// exercise the user's actual saved fader / mute / bus / aux state.
//
// Usage:
//   DUSKSTUDIO_PIPELINE_TEST=/home/marc/.vst3/u-he/Diva.vst3 ./Dusk Studio
//   DUSKSTUDIO_PIPELINE_TEST=/home/marc/.vst3/u-he/Diva.vst3 \
//     DUSKSTUDIO_PIPELINE_TEST_SESSION=/home/marc/Music/Dusk Studio/Untitled/session.json.autosave \
//     ./Dusk Studio
static void runHeadlessPipelineTest (const juce::String& pluginPath)
{
    constexpr double sampleRate = 48000.0;
    constexpr int    blockSize  = 256;
    constexpr int    numInChannels  = 2;
    constexpr int    numOutChannels = 2;
    constexpr int    numBlocks       = 200;
    constexpr int    chordHoldBlocks = 150;

    std::fprintf (stdout, "=== Dusk Studio Headless Pipeline Test ===\n");
    std::fprintf (stdout, "Plugin: %s\nSR=%.0f BS=%d Blocks=%d\n\n",
                  pluginPath.toRawUTF8(), sampleRate, blockSize, numBlocks);

    auto session = std::make_unique<Session>();
    auto engine  = std::make_unique<AudioEngine> (*session);

    // The ctor may have opened a real device + added the engine as its callback.
    // This path drives the callback manually below (including the BS-cycle
    // re-prepares), so detach FIRST - before prepareForSelfTest mutates DSP
    // state - so the device thread can't process the engine mid-re-prepare
    // (matches runHeadlessSessionPerf).
    engine->detachAudioCallback();
    // Don't depend on a real device coming up - prepare directly.
    engine->prepareForSelfTest (sampleRate, blockSize);

    const char* sessionPath = std::getenv ("DUSKSTUDIO_PIPELINE_TEST_SESSION");
    const bool useSession = (sessionPath != nullptr && *sessionPath != '\0');

    if (useSession)
    {
        std::fprintf (stdout, "Loading session: %s\n", sessionPath);
        if (! SessionSerializer::load (*session, juce::File (sessionPath)))
        {
            std::fprintf (stderr, "FAIL: SessionSerializer::load returned false\n");
            return;
        }
        // Verify the description was deserialised before we ask the
        // engine to consume it. Empty here = the JSON didn't contain
        // plugin_desc_xml, which is a session-file regression.
        const auto& descXml = session->track (0).pluginDescriptionXml;
        const auto& stateB64 = session->track (0).pluginStateBase64;
        std::fprintf (stdout,
                      "After SessionSerializer::load: track[0] descXml.len=%d  state.len=%d  "
                      "descXml head=\"%.60s\"\n",
                      descXml.length(), stateB64.length(),
                      descXml.toRawUTF8());

        // Call restoreFromSavedState DIRECTLY here (instead of going via
        // engine->consumePluginStateAfterLoad) so we can see the error.
        // The engine wraps the same call but routes failures into DBG,
        // which is a no-op in release builds.
        juce::String restoreErr;
        const bool restored = engine->getStrip (0).getPluginSlot()
            .restoreFromSavedState (descXml, stateB64, restoreErr);
        if (! restored)
        {
            std::fprintf (stderr, "FAIL: restoreFromSavedState: %s\n",
                          restoreErr.toRawUTF8());
        }
        else
        {
            std::fprintf (stdout,
                          "restoreFromSavedState: ok (loaded=%d)\n",
                          (int) engine->getStrip (0).getPluginSlot().isLoaded());
        }

        // Run the rest of the engine's after-load housekeeping (other
        // tracks, aux-lane plugins, master tape state) - just call the
        // public consume method; track 0 will be re-restored as a no-op
        // since restoreFromSavedState is idempotent.
        engine->consumePluginStateAfterLoad();
        engine->consumeTransportStateAfterLoad();
        // Re-prepare so the just-loaded plugin sees the right SR/BS.
        engine->prepareForSelfTest (sampleRate, blockSize);
    }
    else
    {
        // Default-state path: track 0 in MIDI mode + load the plugin.
        session->track (0).mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
        juce::String err;
        if (! engine->getStrip (0).getPluginSlot().loadFromFile (juce::File (pluginPath), err))
        {
            std::fprintf (stderr, "FAIL: loadFromFile: %s\n", err.toRawUTF8());
            return;
        }
    }

    // Snapshot the relevant Track[0] + Master state so the user can see
    // exactly what we're testing against. This is what would be silencing
    // the strip if anything is misconfigured in the loaded session.
    {
        auto& t0 = session->track (0);
        std::fprintf (stdout, "\n--- Track 0 (UI: track 1) state ---\n");
        std::fprintf (stdout, "  mode=%d (0=Mono 1=Stereo 2=MIDI)\n",
                      t0.mode.load());
        std::fprintf (stdout, "  faderDb=%.2f  pan=%.2f  mute=%d  solo=%d  printEffects=%d\n",
                      t0.strip.faderDb.load(), t0.strip.pan.load(),
                      (int) t0.strip.mute.load(), (int) t0.strip.solo.load(),
                      (int) t0.printEffects.load());
        std::fprintf (stdout, "  liveFaderDb=%.2f  liveMute=%d  liveSolo=%d\n",
                      t0.strip.liveFaderDb.load(),
                      (int) t0.strip.liveMute.load(),
                      (int) t0.strip.liveSolo.load());
        std::fprintf (stdout, "  busAssign: A=%d B=%d C=%d D=%d\n",
                      (int) t0.strip.busAssign[0].load(), (int) t0.strip.busAssign[1].load(),
                      (int) t0.strip.busAssign[2].load(), (int) t0.strip.busAssign[3].load());
        std::fprintf (stdout, "  auxSendDb: 1=%.2f 2=%.2f 3=%.2f 4=%.2f\n",
                      t0.strip.auxSendDb[0].load(), t0.strip.auxSendDb[1].load(),
                      t0.strip.auxSendDb[2].load(), t0.strip.auxSendDb[3].load());
        std::fprintf (stdout, "  midiInputIndex=%d  midiInputId=\"%s\"  midiChannel=%d\n",
                      t0.midiInputIndex.load(),
                      t0.midiInputIdentifier.toRawUTF8(),
                      t0.midiChannel.load());
        std::fprintf (stdout, "  pluginLoaded=%d  pluginAutoBypassed=%d\n",
                      (int) engine->getStrip (0).getPluginSlot().isLoaded(),
                      (int) engine->getStrip (0).getPluginSlot().wasAutoBypassed());

        std::fprintf (stdout, "--- Master state ---\n");
        std::fprintf (stdout, "  faderDb=%.2f  tapeEnabled=%d  eqEnabled=%d  compEnabled=%d\n",
                      session->master().faderDb.load(),
                      (int) session->master().tapeEnabled.load(),
                      (int) session->master().eqEnabled.load(),
                      (int) session->master().compEnabled.load());
        std::fprintf (stdout, "  anyTrackSoloed=%d  anyBusSoloed=%d\n",
                      (int) session->anyTrackSoloed(),
                      (int) session->anyBusSoloed());
        std::fprintf (stdout, "\n");
    }

    if (! engine->getStrip (0).getPluginSlot().isLoaded())
    {
        std::fprintf (stderr, "FAIL: track 0 has no plugin loaded after setup; aborting.\n");
        return;
    }

    // Decide which input index to inject MIDI on. Both the test driver
    // (this code) and the per-track filter (engine) must agree on the
    // same index so the staged events flow through to the strip.
    //
    // - If the session was loaded and its saved midiInputIndex points at
    //   a real device in the engine's current bank, use that. We're then
    //   testing the session's exact wiring.
    // - Otherwise pick index 0 if any MIDI input exists, and force track
    //   0's midiInputIndex to match so the test can inject end-to-end.
    int midiInputIdx = -1;
    const int numMidiInputs = (int) engine->getMidiInputDevices().size();
    if (numMidiInputs == 0)
    {
        std::fprintf (stderr, "WARN: no MIDI inputs available; injection will be a no-op.\n");
    }
    else if (useSession)
    {
        const int saved = session->track (0).midiInputIndex.load (std::memory_order_relaxed);
        if (saved >= 0 && saved < numMidiInputs)
        {
            midiInputIdx = saved;
            std::fprintf (stdout, "Using session's saved midiInputIndex=%d for injection.\n",
                          midiInputIdx);
        }
        else
        {
            midiInputIdx = 0;
            session->track (0).midiInputIndex.store (midiInputIdx, std::memory_order_relaxed);
            std::fprintf (stdout, "Session midiInputIndex=%d invalid (numInputs=%d); "
                                  "overriding to %d for injection.\n",
                          saved, numMidiInputs, midiInputIdx);
        }
    }
    else
    {
        midiInputIdx = 0;
        session->track (0).midiInputIndex.store (midiInputIdx, std::memory_order_relaxed);
        session->track (0).midiChannel.store   (0,            std::memory_order_relaxed);  // Omni
    }

    constexpr int kChordNotes[] = { 60, 64, 67 };

    // I/O buffers for the engine callback.
    std::vector<std::vector<float>> inputs ((size_t) numInChannels,
                                              std::vector<float> ((size_t) blockSize, 0.0f));
    std::vector<const float*> inputPtrs ((size_t) numInChannels, nullptr);
    for (int c = 0; c < numInChannels; ++c)
        inputPtrs[(size_t) c] = inputs[(size_t) c].data();

    std::vector<std::vector<float>> outputs ((size_t) numOutChannels,
                                               std::vector<float> ((size_t) blockSize, 0.0f));
    std::vector<float*> outputPtrs ((size_t) numOutChannels, nullptr);
    for (int c = 0; c < numOutChannels; ++c)
        outputPtrs[(size_t) c] = outputs[(size_t) c].data();

    duskstudio::device::CallbackContext ctx {};

    // Two probes:
    //   - Master output peak/RMS - what the device would hear.
    //   - Strip-level peak via Track::peakDb meter, polled every block.
    //     The strip writes meterPeakDbL/R from inside processAndAccumulate
    //     (see ChannelStrip), so it reflects post-pan / post-fader state.
    float  masterPeak = 0.0f;
    double masterRms  = 0.0;
    long long counted = 0;

    // Strip output peak: scan the strip's post-DSP buffer directly via
    // getLastProcessedMono() / getLastProcessedR(). Those pointers are
    // valid for the duration of the block we just processed.
    float stripPeak = 0.0f;

    for (int b = 0; b < numBlocks; ++b)
    {
        // Stage MIDI for this block: chord on at b==0, off at chordHoldBlocks.
        if (midiInputIdx >= 0)
        {
            juce::MidiBuffer midi;
            if (b == 0)
                for (int n : kChordNotes)
                    midi.addEvent (juce::MidiMessage::noteOn (1, n, (std::uint8_t) 100), 0);
            if (b == chordHoldBlocks)
                for (int n : kChordNotes)
                    midi.addEvent (juce::MidiMessage::noteOff (1, n), 0);
            if (! midi.isEmpty())
                engine->stageTestMidiInjection (midiInputIdx, std::move (midi));
        }

        for (auto& o : outputs) std::fill (o.begin(), o.end(), 0.0f);
        engine->audioDeviceIOCallback (
            inputPtrs.data(), numInChannels,
            outputPtrs.data(), numOutChannels,
            blockSize, ctx);

        for (int s = 0; s < blockSize; ++s)
        {
            const float l = outputs[0][(size_t) s];
            const float r = outputs[1][(size_t) s];
            const float mag = std::max (std::abs (l), std::abs (r));
            if (mag > masterPeak) masterPeak = mag;
            masterRms += (double) l * (double) l + (double) r * (double) r;
            counted += 2;
        }

        // Read the strip's post-DSP buffer pointers and scan for peak.
        // lastProcessedMono is the L (or mono) channel; lastProcessedR is
        // the R channel (set on stereo / MIDI tracks, null on mono). For
        // a MIDI track Diva fills both L and R via processStereoBlock so
        // both pointers are non-null.
        if (auto* lp = engine->getStrip (0).getLastProcessedMono())
        {
            const int n = engine->getStrip (0).getLastProcessedSamples();
            if (n > 0)
            {
                const auto rng = juce::FloatVectorOperations::findMinAndMax (lp, n);
                const float p = std::max (std::abs (rng.getStart()), std::abs (rng.getEnd()));
                if (p > stripPeak) stripPeak = p;
            }
        }
        if (auto* rp = engine->getStrip (0).getLastProcessedR())
        {
            const int n = engine->getStrip (0).getLastProcessedSamples();
            if (n > 0)
            {
                const auto rng = juce::FloatVectorOperations::findMinAndMax (rp, n);
                const float p = std::max (std::abs (rng.getStart()), std::abs (rng.getEnd()));
                if (p > stripPeak) stripPeak = p;
            }
        }
    }

    const double masterRmsVal = counted > 0 ? std::sqrt (masterRms / (double) counted) : 0.0;
    std::fprintf (stdout,
                  "Track 1 strip:  peak (linear, post-DSP) = %.6f  (~%.1f dBFS)\n",
                  stripPeak,
                  stripPeak > 0.0f ? juce::Decibels::gainToDecibels (stripPeak) : -120.0f);
    std::fprintf (stdout,
                  "Master output:  peak = %.6f  rms = %.6f  (~%.1f dBFS peak)\n",
                  masterPeak, masterRmsVal,
                  masterPeak > 0.0f ? juce::Decibels::gainToDecibels (masterPeak) : -120.0f);

    if (stripPeak > 1.0e-4f && masterPeak > 1.0e-4f)
        std::fprintf (stdout, "VERDICT: PASS - audio reaches both the strip and the master.\n");
    else if (stripPeak > 1.0e-4f && masterPeak <= 1.0e-4f)
        std::fprintf (stdout, "VERDICT: FAIL - strip has audio but master is silent. "
                              "Check master fader / mute / bus routing.\n");
    else if (stripPeak <= 1.0e-4f)
        std::fprintf (stdout, "VERDICT: FAIL - strip is silent. "
                              "MIDI not reaching plugin OR strip fader/mute is silencing it.\n");

    // DUSKSTUDIO_PIPELINE_TEST_BS_CYCLE="512,1024,256": after the main run,
    // re-prepare at each listed block size and drive blocks with a held chord -
    // reproduces the buffer-size-change crash path (device reopen at a new
    // period) headlessly, plugin and all, without touching a real device.
    if (const char* cyc = std::getenv ("DUSKSTUDIO_PIPELINE_TEST_BS_CYCLE"))
    {
        juce::StringArray sizes = juce::StringArray::fromTokens (juce::String (cyc), ",", {});
        for (const auto& tok : sizes)
        {
            const int bs = std::clamp (tok.trim().getIntValue(), 32, 8192);
            std::fprintf (stdout, "BS-CYCLE: re-prepare at %d samples + 64 blocks\n", bs);
            std::fflush (stdout);

            engine->prepareForSelfTest (sampleRate, bs);

            std::vector<std::vector<float>> ins ((size_t) numInChannels,
                                                 std::vector<float> ((size_t) bs, 0.0f));
            std::vector<const float*> inP ((size_t) numInChannels);
            for (int c = 0; c < numInChannels; ++c) inP[(size_t) c] = ins[(size_t) c].data();
            std::vector<std::vector<float>> outs ((size_t) numOutChannels,
                                                  std::vector<float> ((size_t) bs, 0.0f));
            std::vector<float*> outP ((size_t) numOutChannels);
            for (int c = 0; c < numOutChannels; ++c) outP[(size_t) c] = outs[(size_t) c].data();

            for (int b = 0; b < 64; ++b)
            {
                if (b == 0 && midiInputIdx >= 0)
                {
                    juce::MidiBuffer midi;
                    for (int n : kChordNotes)
                        midi.addEvent (juce::MidiMessage::noteOn (1, n, (std::uint8_t) 100), 0);
                    engine->stageTestMidiInjection (midiInputIdx, std::move (midi));
                }
                for (auto& o : outs) std::fill (o.begin(), o.end(), 0.0f);
                engine->audioDeviceIOCallback (inP.data(), numInChannels,
                                                          outP.data(), numOutChannels, bs, ctx);
            }
            if (midiInputIdx >= 0)
            {
                juce::MidiBuffer midi;
                for (int n : kChordNotes)
                    midi.addEvent (juce::MidiMessage::noteOff (1, n), 0);
                engine->stageTestMidiInjection (midiInputIdx, std::move (midi));
                for (auto& o : outs) std::fill (o.begin(), o.end(), 0.0f);
                engine->audioDeviceIOCallback (inP.data(), numInChannels,
                                                          outP.data(), numOutChannels, bs, ctx);
            }
            std::fprintf (stdout, "BS-CYCLE: %d samples OK\n", bs);
            std::fflush (stdout);
        }
        std::fprintf (stdout, "BS-CYCLE: all sizes survived\n");
    }

    std::fprintf (stdout, "=== End of Pipeline Test ===\n");
    std::fflush (stdout);
}

// DUSKSTUDIO_PERF_SESSION=<path>: load a real session and play it
// headlessly at 48 kHz / 256 samples, paced to realtime so the playback
// prefetch behaves as it does live, then print the per-section perf
// table. Measures the full mixer path (region playback, automation,
// buses, aux, master) with no audio device - for attributing a DSP-load
// report to a specific section of the callback.
static void runHeadlessSessionPerf (const juce::String& sessionPath)
{
    constexpr double sampleRate = 48000.0;
    constexpr int    blockSize  = 256;
    constexpr int    numBlocks  = 2000;   // ~10.7 s at 48k/256
    constexpr int    numInChannels  = 2;
    constexpr int    numOutChannels = 2;

    std::fprintf (stdout, "=== Dusk Studio Headless Session Perf ===\n");
    std::fprintf (stdout, "Session: %s\nSR=%.0f BS=%d Blocks=%d (realtime-paced)\n",
                  sessionPath.toRawUTF8(), sampleRate, blockSize, numBlocks);

    auto session = std::make_unique<Session>();
    auto engine  = std::make_unique<AudioEngine> (*session);
    // The AudioEngine ctor registered with its AudioDeviceManager (and may have
    // opened a real device). This perf path drives the callback MANUALLY below,
    // so detach FIRST - before prepareForSelfTest mutates DSP state - so the
    // device thread can't process the engine while that state is in flux.
    engine->detachAudioCallback();
    engine->prepareForSelfTest (sampleRate, blockSize);

    juce::File f (sessionPath);
    if (f.isDirectory()) f = f.getChildFile ("session.json");
    if (! f.existsAsFile())
    {
        std::fprintf (stderr, "FAIL: no session.json at %s\n", f.getFullPathName().toRawUTF8());
        return;
    }
    session->setSessionDirectory (f.getParentDirectory());
    if (! SessionSerializer::load (*session, f))
    {
        std::fprintf (stderr, "FAIL: SessionSerializer::load returned false\n");
        return;
    }
    engine->consumePluginStateAfterLoad();
    engine->consumeTransportStateAfterLoad();
    // Re-resolve saved MIDI routing AND republish the tempo-map snapshot
    // (reresolveTrackMidiFromSession does both) so the MIDI scheduler +
    // metronome run against the loaded map, matching the live path's cost.
    engine->reresolveTrackMidiFromSession();
    // Re-prepare so just-loaded plugins see the right SR/BS.
    engine->prepareForSelfTest (sampleRate, blockSize);

    int regions = 0, midiRegions = 0, inserts = 0, hwInserts = 0;
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        regions     += (int) session->track (t).regions.size();
        midiRegions += (int) session->track (t).midiRegions.current().size();
        if (engine->getStrip (t).getPluginSlot().isLoaded())          ++inserts;
        if (session->track (t).hardwareInsert.enabled.load())          ++hwInserts;
    }
    std::fprintf (stdout, "Loaded: %d audio regions, %d MIDI regions, %d plugin inserts, %d HW inserts\n",
                  regions, midiRegions, inserts, hwInserts);

    engine->setPerfCaptureEnabled (true);
    engine->getTransport().setPlayhead (0);
    engine->play();

    std::vector<std::vector<float>> inputs ((size_t) numInChannels,
                                              std::vector<float> ((size_t) blockSize, 0.0f));
    std::vector<const float*> inputPtrs ((size_t) numInChannels, nullptr);
    for (int c = 0; c < numInChannels; ++c)
        inputPtrs[(size_t) c] = inputs[(size_t) c].data();

    std::vector<std::vector<float>> outputs ((size_t) numOutChannels,
                                               std::vector<float> ((size_t) blockSize, 0.0f));
    std::vector<float*> outputPtrs ((size_t) numOutChannels, nullptr);
    for (int c = 0; c < numOutChannels; ++c)
        outputPtrs[(size_t) c] = outputs[(size_t) c].data();

    duskstudio::device::CallbackContext ctx {};

    const double blockMs = 1000.0 * blockSize / sampleRate;
    const auto   wall0   = juce::Time::getMillisecondCounterHiRes();
    float masterPeak = 0.0f;

    for (int b = 0; b < numBlocks; ++b)
    {
        for (auto& o : outputs) std::fill (o.begin(), o.end(), 0.0f);
        engine->audioDeviceIOCallback (
            inputPtrs.data(), numInChannels,
            outputPtrs.data(), numOutChannels,
            blockSize, ctx);

        for (int s = 0; s < blockSize; ++s)
            masterPeak = std::max ({ masterPeak,
                                     std::abs (outputs[0][(size_t) s]),
                                     std::abs (outputs[1][(size_t) s]) });

        // Pace to realtime so the prefetch thread fills readers the way a
        // live device run would; free-running would outrun it and the
        // resulting silence would let strips skip, undercounting cost.
        const double target = wall0 + (double) (b + 1) * blockMs;
        while (juce::Time::getMillisecondCounterHiRes() < target)
            juce::Thread::sleep (1);
    }

    engine->stop();
    std::fprintf (stdout, "Master peak over run: %.4f %s\n", masterPeak,
                  masterPeak <= 1.0e-6f ? "(SILENT - check playhead/regions; costs below undercount the live case)"
                                        : "");
    engine->printPerfTable();
    std::fprintf (stdout, "=== End of Session Perf ===\n");
    std::fflush (stdout);
}

static void runHeadlessSelfTest()
{
    // Heap-allocated so destruction order matches the GUI path: AudioEngine
    // first, then Session, before this function returns and DuskStudioApp::quit()
    // tears down the message loop.
    auto session = std::make_unique<Session>();
    auto engine  = std::make_unique<AudioEngine> (*session);

    // Poll for engine readiness instead of a fixed sleep. AudioEngine's
    // constructor calls initialiseWithDefaultDevices(16, 2) and adds itself
    // as an AudioDeviceCallback; the audio thread then fires
    // audioDeviceAboutToStart, which sets currentSampleRate to a non-zero
    // value as a side effect of preparing strip/aux/master state. So
    // sampleRate > 0 is the load-bearing readiness signal.
    //
    // 5-second timeout so a stuck-or-failing device-open can't hang the
    // headless test indefinitely (relevant on CI / slow boxes / contended
    // PipeWire setups). If we time out, we still proceed - the synthetic
    // tests don't strictly require a real device since they call
    // prepareForSelfTest() with their own SR/BS.
    constexpr int maxWaitMs       = 5000;
    constexpr int pollIntervalMs  = 10;
    int waited = 0;
    while (engine->getCurrentSampleRate() <= 0.0 && waited < maxWaitMs)
    {
        juce::Thread::sleep (pollIntervalMs);
        waited += pollIntervalMs;
    }
    if (engine->getCurrentSampleRate() <= 0.0)
        std::fprintf (stderr,
                      "[Dusk Studio/selftest] WARNING: audio engine not ready after %d ms - "
                      "synthetic tests will still run, backend tests may show degraded info\n",
                      maxWaitMs);

    AudioPipelineSelfTest test (*engine, engine->getDeviceManager(), *session);
    const auto report = test.runAll();

    std::fprintf (stdout, "%s\n", report.c_str());
    std::fflush (stdout);
}

// Headless bounce regression harness: DUSKSTUDIO_BOUNCE_TEST=<plugin path>.
//
// Builds a two-track session with synthesised audio, loads the given plugin
// (.clap / .vst3 / .lv2, dispatched to the matching NATIVE host slot exactly as
// ChannelStrip does live) as an insert on track 0, then drives a full
// BounceEngine MasterMix render to a temp WAV THROUGH THE REAL ASYNC WORKER -
// start() spins the worker thread and the worker marshals its engine re-prepare
// back to the message thread via BounceEngine::runOnMessageThread. This harness
// must therefore keep the message loop pumping while it waits, which it does by
// polling from a juce::Timer (fired by the app dispatch loop) rather than
// blocking - a blocking wait would deadlock the worker's callAsync marshalling.
//
// The u-he-Satin-native-CLAP crash lived precisely in that worker/message-thread
// interaction (plugin deactivate off the main thread tripped u-he's contract
// checker abort()). This path is the automated proof the fix holds.
//
// Also renders a FREEZE of the plugin track (second [PASS] line): freeze captures
// the strip's pre-fader tap, so nonzero freeze output proves the plugin actually
// processed through the native insert.
//
// Exits nonzero on any failure. Runs offline (device detached), and closes the
// audio device up front so no real hardware is held while it renders - safe to
// run on a machine that is actively making sound elsewhere.
//
// Usage:
//   DUSKSTUDIO_BOUNCE_TEST=/home/marc/.clap/u-he/Satin.64.clap ./DuskStudio
struct DuskStudioApp::BounceTest : private juce::Timer
{
    BounceTest (DuskStudioApp& a, juce::String p)
        : app (a), pluginPath (std::move (p)) {}

    ~BounceTest() override { stopTimer(); }

    // Synchronous setup on the message thread. Returns false if setup failed
    // (return value + [FAIL] already emitted); the caller then quits immediately.
    bool begin()
    {
        std::fprintf (stdout, "=== Dusk Studio Headless Bounce Test ===\n");
        std::fprintf (stdout, "Plugin: %s\nSR=%.0f BS=%d\n\n", pluginPath.toRawUTF8(), sr, bs);
        std::fflush (stdout);

        tmpDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                     .getChildFile ("duskstudio-bounce-test");
        tmpDir.createDirectory();
        mixFile    = tmpDir.getChildFile ("mix.wav");
        freezeFile = tmpDir.getChildFile ("freeze.wav");
        mixFile.deleteFile();
        freezeFile.deleteFile();

        session = std::make_unique<Session>();
        engine  = std::make_unique<AudioEngine> (*session);

        // The engine ctor opened a real device + attached itself. Detach AND close
        // the device now so no hardware is held during the (potentially slow) native
        // plugin load + offline render on a machine in active use. The offline
        // render never needs a device; the bounce re-attaches at the end, harmless
        // against a closed device.
        engine->detachAudioCallback();
        engine->getDeviceManager().closeDevice();
        engine->prepareForSelfTest (sr, bs);

        synthesiseContent();

        if (! loadPluginByExtension())
        {
            // loadPluginByExtension already printed the [FAIL] line.
            exitCode = 1;
            return false;
        }

        std::fprintf (stdout, "[INFO] loaded %s insert on track 0: %s\n\n",
                      loadedFormat.toRawUTF8(), pluginPath.toRawUTF8());
        std::fflush (stdout);

        bounce = std::make_unique<BounceEngine> (*engine, *session);
        bounce->onFinished = [this] (bool ok, std::string err)
        {
            // Worker thread. Record + let the poll timer pick it up on the message
            // thread. masterDone is the release fence.
            masterOk  = ok;
            masterErr = err;
            masterDone.store (true, std::memory_order_release);
        };

        // Real async path: start() spins the worker; tail kept short for speed.
        if (! bounce->start (mixFile, sr, bs, 1.0, BounceEngine::Mode::MasterMix,
                             BounceEngine::Format::Wav))
        {
            std::fprintf (stdout, "[FAIL] MasterMix bounce start() returned false: %s\n",
                          bounce->getLastError().c_str());
            exitCode = 1;
            return false;
        }

        phase = Phase::WaitMaster;
        phaseStartMs = juce::Time::getMillisecondCounter();
        startTimer (25);
        return true;
    }

private:
    // Two detuned sines with an amplitude envelope; a plausible recorded take.
    juce::File writeDemoWav (const juce::File& file, double fL, double fR)
    {
        const int numCh = 2;
        const int numFrames = (int) (sr * kContentSeconds);
        juce::AudioBuffer<float> buf (numCh, numFrames);
        for (int n = 0; n < numFrames; ++n)
        {
            const double t   = (double) n / sr;
            const double env = std::sin (juce::MathConstants<double>::pi * (double) n / numFrames);
            buf.setSample (0, n, (float) (env * 0.6 * std::sin (2.0 * juce::MathConstants<double>::pi * fL * t)));
            buf.setSample (1, n, (float) (env * 0.6 * std::sin (2.0 * juce::MathConstants<double>::pi * fR * t)));
        }
        file.deleteFile();
       #if defined(DUSKSTUDIO_HAS_AUDIOFILE)
        dusk::audio::WriteSpec spec;
        spec.sampleRate = sr;
        spec.numChannels = numCh;
        spec.bitsPerSample = 24;
        spec.format = dusk::audio::WriteSpec::Format::Wav;
        if (auto writer = dusk::audio::FileWriter::create (
                std::filesystem::u8path (file.getFullPathName().toStdString()), spec))
            writer->write (buf.getArrayOfReadPointers(), numCh, numFrames);
       #else
        juce::WavAudioFormat wav;
        if (auto os = std::unique_ptr<juce::FileOutputStream> (file.createOutputStream()))
        {
            // createWriterFor takes ownership of the stream only on success.
            if (auto* writer = wav.createWriterFor (os.get(), sr, (unsigned) numCh, 24, {}, 0))
            {
                os.release();
                std::unique_ptr<juce::AudioFormatWriter> w (writer);
                w->writeFromAudioSampleBuffer (buf, 0, numFrames);
            }
        }
       #endif
        return file;
    }

    void synthesiseContent()
    {
        const std::int64_t lenSamples = (std::int64_t) (sr * kContentSeconds);
        auto makeRegion = [&] (const juce::File& f)
        {
            AudioRegion r;
            r.file            = f;
            r.timelineStart   = 0;
            r.lengthInSamples = lenSamples;
            r.numChannels     = 2;
            r.fadeInSamples   = (std::int64_t) (sr * 0.02);
            r.fadeOutSamples  = (std::int64_t) (sr * 0.05);
            return r;
        };
        const auto wav0 = writeDemoWav (tmpDir.getChildFile ("take0.wav"), 196.0, 198.0);
        const auto wav1 = writeDemoWav (tmpDir.getChildFile ("take1.wav"), 261.0, 264.0);

        session->track (0).name = "Plugin Trk";
        session->track (0).mode.store ((int) Track::Mode::Stereo, std::memory_order_relaxed);
        session->track (0).regions = { makeRegion (wav0) };

        session->track (1).name = "Dry Trk";
        session->track (1).mode.store ((int) Track::Mode::Stereo, std::memory_order_relaxed);
        session->track (1).regions = { makeRegion (wav1) };
    }

    // Dispatch to the native host slot by file extension - exactly the ChannelStrip
    // path the live UI uses, and the path the bounce crash lived in. Prints a
    // [FAIL] line on failure. Falls back to a clear message when the format's
    // native host isn't compiled into this build.
    bool loadPluginByExtension()
    {
        const juce::File f (pluginPath);
        if (! f.exists())
        {
            std::fprintf (stdout, "[FAIL] plugin path does not exist: %s\n", pluginPath.toRawUTF8());
            return false;
        }

        auto& strip = engine->getChannelStrip (0);
        std::string err;
        bool ok = false;

        if (f.getFileName().endsWithIgnoreCase (".clap"))
        {
           #if DUSKSTUDIO_HAS_NATIVE_CLAP
            loadedFormat = "CLAP";
            ok = strip.loadNativeClap (f, err);
           #else
            std::fprintf (stdout, "[FAIL] .clap given but DUSKSTUDIO_HAS_NATIVE_CLAP is off\n");
            return false;
           #endif
        }
        else if (f.getFileName().endsWithIgnoreCase (".vst3"))
        {
           #if DUSKSTUDIO_HAS_NATIVE_VST3
            loadedFormat = "VST3";
            ok = strip.loadNativeVst3 (f, err);
           #else
            std::fprintf (stdout, "[FAIL] .vst3 given but DUSKSTUDIO_HAS_NATIVE_VST3 is off\n");
            return false;
           #endif
        }
        else if (f.getFileName().endsWithIgnoreCase (".lv2"))
        {
           #if DUSKSTUDIO_HAS_NATIVE_LV2
            loadedFormat = "LV2";
            ok = strip.loadNativeLv2 (f, err);
           #else
            std::fprintf (stdout, "[FAIL] .lv2 given but DUSKSTUDIO_HAS_NATIVE_LV2 is off\n");
            return false;
           #endif
        }
        else
        {
            std::fprintf (stdout, "[FAIL] unrecognised plugin extension: %s\n",
                          f.getFileName().toRawUTF8());
            return false;
        }

        if (! ok)
            std::fprintf (stdout, "[FAIL] native %s load failed: %s\n",
                          loadedFormat.toRawUTF8(), err.c_str());
        return ok;
    }

    // Read a rendered WAV back and report nonzero + finite. peakOut set to the
    // linear peak. Returns false with a printed [FAIL] reason on any problem.
    bool validateWav (const juce::File& file, const char* label, float& peakOut,
                      std::int64_t* lengthOut = nullptr)
    {
        peakOut = 0.0f;
        if (lengthOut != nullptr) *lengthOut = 0;
        if (! file.existsAsFile())
        {
            std::fprintf (stdout, "[FAIL] %s: output file not written: %s\n",
                          label, file.getFullPathName().toRawUTF8());
            return false;
        }
       #if defined(DUSKSTUDIO_HAS_AUDIOFILE)
        auto reader = dusk::audio::FileReader::open (
            std::filesystem::u8path (file.getFullPathName().toStdString()));
        const auto readerLength = reader != nullptr ? reader->info().numFrames : 0;
        const auto readerChannels = reader != nullptr ? reader->info().numChannels : 0;
       #else
        auto stream = file.createInputStream();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> reader (
            stream != nullptr ? wav.createReaderFor (stream.release(), true) : nullptr);
        const auto readerLength = reader != nullptr ? reader->lengthInSamples : 0;
        const auto readerChannels = reader != nullptr ? (int) reader->numChannels : 0;
       #endif
        if (reader == nullptr)
        {
            std::fprintf (stdout, "[FAIL] %s: could not open output WAV for readback\n", label);
            return false;
        }
        if (lengthOut != nullptr) *lengthOut = readerLength;
        const int n = (int) readerLength;
        if (n <= 0)
        {
            std::fprintf (stdout, "[FAIL] %s: output WAV has zero samples\n", label);
            return false;
        }
        juce::AudioBuffer<float> buf (std::max (1, readerChannels), n);
       #if defined(DUSKSTUDIO_HAS_AUDIOFILE)
        if (reader->read (buf.getArrayOfWritePointers(), buf.getNumChannels(), 0, n) != n)
        {
            std::fprintf (stdout, "[FAIL] %s: short read from output WAV\n", label);
            return false;
        }
       #else
        reader->read (&buf, 0, n, 0, true, true);
       #endif
        bool allFinite = true;
        float peak = 0.0f;
        for (int c = 0; c < buf.getNumChannels(); ++c)
        {
            const float* d = buf.getReadPointer (c);
            for (int i = 0; i < n; ++i)
            {
                if (! std::isfinite (d[i])) { allFinite = false; break; }
                peak = std::max (peak, std::abs (d[i]));
            }
            if (! allFinite) break;
        }
        peakOut = peak;
        if (! allFinite)
        {
            std::fprintf (stdout, "[FAIL] %s: output WAV contains non-finite samples\n", label);
            return false;
        }
        if (peak <= 1.0e-6f)
        {
            std::fprintf (stdout, "[FAIL] %s: output WAV is silent (peak=%.3e)\n", label, peak);
            return false;
        }
        return true;
    }

    // After the render restores the engine, drive one manual callback block and
    // confirm the engine still processes to finite output - proves the device
    // callback path came back to a working state post-bounce.
    bool driveOnePostBounceBlock()
    {
        constexpr int kIn = 16, kOut = 2;
        std::vector<std::vector<float>> in ((size_t) kIn, std::vector<float> ((size_t) bs, 0.0f));
        std::vector<const float*> inP ((size_t) kIn);
        for (int c = 0; c < kIn; ++c) inP[(size_t) c] = in[(size_t) c].data();
        std::vector<std::vector<float>> out ((size_t) kOut, std::vector<float> ((size_t) bs, 0.0f));
        std::vector<float*> outP ((size_t) kOut);
        for (int c = 0; c < kOut; ++c) outP[(size_t) c] = out[(size_t) c].data();

        duskstudio::device::CallbackContext ctx {};
        engine->audioDeviceIOCallback (inP.data(), kIn, outP.data(), kOut, bs, ctx);

        for (int c = 0; c < kOut; ++c)
            for (int i = 0; i < bs; ++i)
                if (! std::isfinite (out[(size_t) c][(size_t) i]))
                    return false;
        return true;
    }

    void timerCallback() override
    {
        const auto now = juce::Time::getMillisecondCounter();
        const bool timedOut = (now - phaseStartMs) > (juce::uint32) kTimeoutMs;

        switch (phase)
        {
            case Phase::WaitMaster:
            {
                if (masterDone.load (std::memory_order_acquire))
                {
                    float peak = 0.0f;
                    const bool wav = validateWav (mixFile, "MasterMix", peak);
                    const bool cb  = driveOnePostBounceBlock();
                    if (! cb)
                        std::fprintf (stdout, "[FAIL] MasterMix: engine produced non-finite output "
                                              "on the post-bounce callback\n");
                    const bool ok = masterOk && masterErr.empty() && wav && cb;
                    if (! masterOk || ! masterErr.empty())
                        std::fprintf (stdout, "[FAIL] MasterMix: worker reported ok=%d err=\"%s\"\n",
                                      (int) masterOk, masterErr.c_str());
                    if (ok)
                        std::fprintf (stdout,
                                      "[PASS] MasterMix render OK (peak=%.4f, engine reattached + "
                                      "processes finite)\n", peak);
                    else
                        exitCode = 1;
                    std::fflush (stdout);

                    freezeAttempts = 0;
                    phase = Phase::StartFreeze;
                    phaseStartMs = now;
                }
                else if (timedOut)
                {
                    std::fprintf (stdout, "[FAIL] MasterMix render did not finish within %d ms\n",
                                  kTimeoutMs);
                    exitCode = 1;
                    finish();
                }
                break;
            }

            case Phase::StartFreeze:
            {
                // Recreate the engine so the previous instance's dtor joins the
                // master worker before we touch onFinished: the worker sets
                // masterDone from inside that std::function and is still unwinding
                // out of it, so reassigning the target here would free it under the
                // worker's feet. A fresh instance also isn't rendering, so
                // startFreeze() starts on the first tick.
                bounce = std::make_unique<BounceEngine> (*engine, *session);
                bounce->onFinished = [this] (bool ok, std::string err)
                {
                    freezeOk  = ok;
                    freezeErr = err;
                    freezeDone.store (true, std::memory_order_release);
                };
                const std::int64_t freezeLen = (std::int64_t) (sr * 1.5);
                if (bounce->startFreeze (0, freezeFile, freezeLen, sr, bs))
                {
                    phase = Phase::WaitFreeze;
                    phaseStartMs = now;
                }
                else if (++freezeAttempts > 40)
                {
                    std::fprintf (stdout, "[FAIL] Freeze: startFreeze() would not start: %s\n",
                                  bounce->getLastError().c_str());
                    exitCode = 1;
                    finish();
                }
                break;
            }

            case Phase::WaitFreeze:
            {
                if (freezeDone.load (std::memory_order_acquire))
                {
                    float peak = 0.0f;
                    const bool wav = validateWav (freezeFile, "Freeze", peak);
                    const bool ok  = freezeOk && freezeErr.empty() && wav;
                    if (! freezeOk || ! freezeErr.empty())
                        std::fprintf (stdout, "[FAIL] Freeze: worker reported ok=%d err=\"%s\"\n",
                                      (int) freezeOk, freezeErr.c_str());
                    if (ok)
                        std::fprintf (stdout,
                                      "[PASS] Freeze render OK (peak=%.4f, plugin processed through "
                                      "the native insert)\n", peak);
                    else
                        exitCode = 1;
                    std::fflush (stdout);
                    phase = Phase::StartStems;
                    phaseStartMs = now;
                }
                else if (timedOut)
                {
                    std::fprintf (stdout, "[FAIL] Freeze render did not finish within %d ms\n",
                                  kTimeoutMs);
                    exitCode = 1;
                    finish();
                }
                break;
            }

            case Phase::StartStems:
            {
                // Route track 1 into bus 1 and send it to aux 1 so the single
                // pass has to produce every stem kind: two track stems, a bus
                // stem, and an aux stem. Master leg already ran, so mutating
                // the routing here can't disturb its output.
                session->track (1).strip.busAssign[0].store (true);
                session->track (1).strip.auxSendDb[0].store (0.0f);

                stemsBase = tmpDir.getChildFile ("stems.wav");
                for (const auto& tgt : BounceEngine::collectStemTargets (*session, stemsBase))
                    tgt.file.deleteFile();

                bounce = std::make_unique<BounceEngine> (*engine, *session);
                bounce->onFinished = [this] (bool ok, std::string err)
                {
                    stemsOk  = ok;
                    stemsErr = err;
                    stemsDone.store (true, std::memory_order_release);
                };
                if (bounce->start (stemsBase, sr, bs, 1.0, BounceEngine::Mode::Stems,
                                   BounceEngine::Format::Wav))
                {
                    phase = Phase::WaitStems;
                    phaseStartMs = now;
                }
                else if (++stemsAttempts > 40)
                {
                    std::fprintf (stdout, "[FAIL] Stems: start() would not start: %s\n",
                                  bounce->getLastError().c_str());
                    exitCode = 1;
                    finish();
                }
                break;
            }

            case Phase::WaitStems:
            {
                if (stemsDone.load (std::memory_order_acquire))
                {
                    bool ok = stemsOk && stemsErr.empty();
                    if (! ok)
                        std::fprintf (stdout, "[FAIL] Stems: worker reported ok=%d err=\"%s\"\n",
                                      (int) stemsOk, stemsErr.c_str());

                    const auto targets = BounceEngine::collectStemTargets (*session, stemsBase);
                    int trackStems = 0, busStems = 0, auxStems = 0;
                    for (const auto& tgt : targets)
                        switch (tgt.kind)
                        {
                            case BounceEngine::StemTarget::Kind::Track: ++trackStems; break;
                            case BounceEngine::StemTarget::Kind::Bus:   ++busStems;   break;
                            case BounceEngine::StemTarget::Kind::Aux:   ++auxStems;   break;
                            case BounceEngine::StemTarget::Kind::Mix:   break;
                        }
                    if (trackStems != 2 || busStems != 1 || auxStems != 1)
                    {
                        std::fprintf (stdout, "[FAIL] Stems: expected 2 track + 1 bus + 1 aux "
                                              "targets, got %d/%d/%d\n",
                                      trackStems, busStems, auxStems);
                        ok = false;
                    }

                    std::int64_t commonLen = -1;
                    for (const auto& tgt : targets)
                    {
                        float peak = 0.0f;
                        std::int64_t len = 0;
                        const auto label = "Stems: " + tgt.file.getFileName();
                        if (! validateWav (tgt.file, label.toRawUTF8(), peak, &len))
                        {
                            ok = false;
                            continue;
                        }
                        if (commonLen < 0) commonLen = len;
                        else if (len != commonLen)
                        {
                            std::fprintf (stdout, "[FAIL] Stems: %s length %lld != %lld - "
                                                  "set is not sample-aligned\n",
                                          tgt.file.getFileName().toRawUTF8(),
                                          (long long) len,
                                          (long long) commonLen);
                            ok = false;
                        }
                    }

                    if (ok)
                        std::fprintf (stdout,
                                      "[PASS] Stems single-pass render OK (%d files, equal length, "
                                      "all nonzero + finite)\n", (int) targets.size());
                    else
                        exitCode = 1;
                    std::fflush (stdout);
                    finish();
                }
                else if (timedOut)
                {
                    std::fprintf (stdout, "[FAIL] Stems render did not finish within %d ms\n",
                                  kTimeoutMs);
                    exitCode = 1;
                    finish();
                }
                break;
            }

            case Phase::Done:
                break;
        }
    }

    void finish()
    {
        stopTimer();
        phase = Phase::Done;
        std::fprintf (stdout, "\n%s\n", exitCode == 0 ? "RESULT: ALL PASS" : "RESULT: FAIL");
        std::fprintf (stdout, "=== End of Bounce Test ===\n");
        std::fflush (stdout);

        // Tear the bounce down before the engine/session (worker joined in its dtor).
        bounce.reset();
        app.setApplicationReturnValue (exitCode);
        app.quit();
    }

    static constexpr double kContentSeconds = 2.0;
    static constexpr int    kTimeoutMs = 60000;

    enum class Phase { WaitMaster, StartFreeze, WaitFreeze, StartStems, WaitStems, Done };

    DuskStudioApp& app;
    juce::String   pluginPath;
    juce::String   loadedFormat;

    std::unique_ptr<Session>      session;
    std::unique_ptr<AudioEngine>  engine;
    std::unique_ptr<BounceEngine> bounce;

    juce::File tmpDir, mixFile, freezeFile, stemsBase;
    double sr = 48000.0;
    int    bs = 1024;

    Phase        phase = Phase::WaitMaster;
    juce::uint32 phaseStartMs = 0;
    int          freezeAttempts = 0;
    int          stemsAttempts = 0;
    int          exitCode = 0;

    std::atomic<bool> masterDone { false };
    std::atomic<bool> freezeDone { false };
    std::atomic<bool> stemsDone  { false };
    bool masterOk = false, freezeOk = false, stemsOk = false;
    std::string  masterErr, freezeErr, stemsErr;
};

// Resolve a session path passed on the command line / by the file manager.
// Prefer a token that clearly names a session (session.json, a directory holding
// one, or any .json file); otherwise fall back to the first existing token so a
// bare path still works. Relative paths resolve against the working directory;
// quoted paths (spaces) survive.
static juce::File sessionPathFromCommandLine (const juce::String& commandLine)
{
    juce::File firstExisting;
    for (const auto& tok : juce::StringArray::fromTokens (commandLine, true))
    {
        const auto t = tok.unquoted();
        if (t.isEmpty() || t.startsWithChar ('-')) continue;
        const juce::File f = juce::File::isAbsolutePath (t)
                               ? juce::File (t)
                               : juce::File::getCurrentWorkingDirectory().getChildFile (t);
        if (! f.exists()) continue;
        if (f.getFileName() == "session.json"
            || (f.isDirectory() && f.getChildFile ("session.json").existsAsFile())
            || f.hasFileExtension ("json"))
            return f;
        if (firstExisting == juce::File())
            firstExisting = f;
    }
    return firstExisting;
}

#if DUSKSTUDIO_HAS_NATIVE_LV2
struct DuskStudioApp::Lv2EditorTest
{
    duskstudio::lv2::NativeLv2Slot slot;
    std::unique_ptr<juce::DocumentWindow> window;   // declared last -> destroyed first
};
#endif

#if DUSKSTUDIO_HAS_NATIVE_VST3
struct DuskStudioApp::Vst3EditorTest
{
    duskstudio::vst3::Vst3Bundle bundle;            // backs the instance's vtables
    duskstudio::vst3::Vst3Instance instance;
    std::unique_ptr<juce::DocumentWindow> window;   // declared last -> destroyed first
};
#endif

void DuskStudioApp::initialise (const juce::String& commandLine)
{
    // --version: print app + JUCE versions + platform string and exit
    // cleanly. Used by Patreon support flows (paste the output of
    // `Dusk Studio --version` into the support DM) and by the Linux CI smoke
    // launch (verifies the binary actually links + starts headless).
    // Check BEFORE any audio init so the path works on machines with
    // no audio device. Tokenize commandLine - substring match would
    // false-trip on session paths containing "--version" (e.g.
    // ~/Sessions/test--version-bug/session.json).
    const auto cliTokens = juce::StringArray::fromTokens (commandLine, true);
    if (cliTokens.contains ("--version") || cliTokens.contains ("-v"))
    {
        std::fprintf (stdout, "Dusk Studio %s\nJUCE %s\nPlatform: %s\n",
                      JUCE_APPLICATION_VERSION_STRING,
                      juce::SystemStats::getJUCEVersion().toRawUTF8(),
                      juce::SystemStats::getOperatingSystemName().toRawUTF8());
        std::fflush (stdout);
        quit();
        return;
    }

   #if JUCE_LINUX
    primeRealtimeAudio();
   #endif

    if (envFlagSet ("DUSKSTUDIO_RUN_SELFTEST"))
    {
        runHeadlessSelfTest();
        quit();
        return;
    }

    if (const char* path = std::getenv ("DUSKSTUDIO_INSTRUMENT_TEST"); path != nullptr && *path)
    {
        runHeadlessInstrumentTest (juce::String (path));
        quit();
        return;
    }

    if (const char* path = std::getenv ("DUSKSTUDIO_PIPELINE_TEST"); path != nullptr && *path)
    {
        runHeadlessPipelineTest (juce::String (path));
        quit();
        return;
    }

    // DUSKSTUDIO_BOUNCE_TEST renders through the REAL async worker, whose engine
    // re-prepare marshals back to this (message) thread - so it can't run + wait
    // synchronously here. begin() sets everything up and starts a poll timer, then
    // we RETURN and let the app dispatch loop pump the worker's marshalling. The
    // harness sets the return value + quits itself when both renders finish.
    if (const char* path = std::getenv ("DUSKSTUDIO_BOUNCE_TEST"); path != nullptr && *path)
    {
        bounceTest = std::make_unique<BounceTest> (*this, juce::String (path));
        if (! bounceTest->begin())
        {
            setApplicationReturnValue (1);
            quit();
        }
        return;
    }

    if (const char* path = std::getenv ("DUSKSTUDIO_PERF_SESSION"); path != nullptr && *path)
    {
        runHeadlessSessionPerf (juce::String (path));
        quit();
        return;
    }

    // DUSKSTUDIO_CLAP_EDITOR_TEST=/path/to.clap - standalone live-verification of
    // the native CLAP editor embed (no engine, no main window). Opens the plugin's
    // embedded-X11 editor through our own host in a plain window. Close it to quit.
    // Linux-only (the native CLAP host + X11 embed).
#if DUSKSTUDIO_HAS_NATIVE_CLAP
    if (const char* path = std::getenv ("DUSKSTUDIO_CLAP_EDITOR_TEST"); path != nullptr && *path)
    {
        if (! displayUsableOrExplain()) { setApplicationReturnValue (1); quit(); return; }
        duskstudio::platform::preferX11ForNextNativeWindow();   // editor host needs an X11 peer
        auto comp = std::make_unique<duskstudio::ClapPluginEditorComponent>();
        juce::String err;
        if (! comp->load (juce::File (juce::String (path)), err))
        {
            std::fprintf (stderr, "[clap editor test] load failed: %s\n", err.toRawUTF8());
            quit();
            return;
        }
        const int w = std::max (200, comp->getWidth());
        const int h = std::max (200, comp->getHeight());
        // Plain juce::DocumentWindow's title-bar close button is a no-op; this dev
        // harness is "close the window to quit", so route it through systemRequestedQuit.
        struct TestWindow final : juce::DocumentWindow
        {
            using juce::DocumentWindow::DocumentWindow;
            void closeButtonPressed() override
            { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
        };
        clapEditorTestWindow = std::make_unique<TestWindow> (
            "Dusk - CLAP editor test", juce::Colours::black, juce::DocumentWindow::allButtons);
        clapEditorTestWindow->setUsingNativeTitleBar (true);
        clapEditorTestWindow->setContentOwned (comp.release(), true);
        clapEditorTestWindow->centreWithSize (w, h);
        clapEditorTestWindow->setVisible (true);   // creates the peer -> consumes the X11 latch
        // Match MainWindow's native-window setup: release the X11 latch now the peer
        // exists, and install the non-fatal X error handler so a dying CLAP editor
        // window can't core-dump this harness.
        duskstudio::platform::clearPreferX11ForNativeWindow();
        duskstudio::platform::installNonFatalXErrorHandler();
        return;   // standalone - skip the normal engine + main-window startup
    }
#endif // DUSKSTUDIO_HAS_NATIVE_CLAP

    // DUSKSTUDIO_LV2_EDITOR_TEST=/path/to.lv2 - same standalone live-verification
    // for the native LV2 (suil) editor embed. Close the window to quit.
#if DUSKSTUDIO_HAS_NATIVE_LV2
    if (const char* path = std::getenv ("DUSKSTUDIO_LV2_EDITOR_TEST"); path != nullptr && *path)
    {
        if (! displayUsableOrExplain()) { setApplicationReturnValue (1); quit(); return; }
        duskstudio::platform::preferX11ForNextNativeWindow();   // editor host needs an X11 peer
        lv2EditorTest = std::make_unique<Lv2EditorTest>();
        std::string err;
        if (! lv2EditorTest->slot.load (std::filesystem::u8path (path), 48000.0, 1024, err))
        {
            std::fprintf (stderr, "[lv2 editor test] load failed: %s\n", err.c_str());
            quit();
            return;
        }
        auto comp = std::make_unique<duskstudio::Lv2PluginEditorComponent>();
        juce::String jerr;
        auto* inst = lv2EditorTest->slot.getInstance();
        if (inst == nullptr || ! comp->attach (*inst, jerr))
        {
            std::fprintf (stderr, "[lv2 editor test] attach failed: %s\n", jerr.toRawUTF8());
            quit();
            return;
        }
        const int w = std::max (200, comp->getWidth());
        const int h = std::max (200, comp->getHeight());
        struct TestWindow final : juce::DocumentWindow
        {
            using juce::DocumentWindow::DocumentWindow;
            void closeButtonPressed() override
            { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
        };
        lv2EditorTest->window = std::make_unique<TestWindow> (
            "Dusk - LV2 editor test", juce::Colours::black, juce::DocumentWindow::allButtons);
        lv2EditorTest->window->setUsingNativeTitleBar (true);
        lv2EditorTest->window->setContentOwned (comp.release(), true);
        lv2EditorTest->window->centreWithSize (w, h);
        lv2EditorTest->window->setVisible (true);   // creates the peer -> consumes the X11 latch
        duskstudio::platform::clearPreferX11ForNativeWindow();
        duskstudio::platform::installNonFatalXErrorHandler();
        return;
    }
#endif // DUSKSTUDIO_HAS_NATIVE_LV2

    // DUSKSTUDIO_VST3_EDITOR_TEST=/path/to.vst3 - same standalone live-verification
    // for the native VST3 (IPlugView) editor embed. Close the window to quit.
#if DUSKSTUDIO_HAS_NATIVE_VST3
    if (const char* path = std::getenv ("DUSKSTUDIO_VST3_EDITOR_TEST"); path != nullptr && *path)
    {
        if (! displayUsableOrExplain()) { setApplicationReturnValue (1); quit(); return; }
        duskstudio::platform::preferX11ForNextNativeWindow();   // editor host needs an X11 peer
        vst3EditorTest = std::make_unique<Vst3EditorTest>();
        std::string err;
        if (! vst3EditorTest->bundle.load (path, err))
        {
            std::fprintf (stderr, "[vst3 editor test] load failed: %s\n", err.c_str());
            quit();
            return;
        }
        std::string classId;
        for (const auto& d : vst3EditorTest->bundle.plugins())
            if (! d.isInstrument) { classId = d.id; break; }
        if (classId.empty()
            || ! vst3EditorTest->instance.create (vst3EditorTest->bundle, classId, err)
            || ! vst3EditorTest->instance.activate (48000.0, 1024, err))
        {
            std::fprintf (stderr, "[vst3 editor test] create failed: %s\n",
                          classId.empty() ? "no effect class in module" : err.c_str());
            quit();
            return;
        }
        auto comp = std::make_unique<duskstudio::Vst3PluginEditorComponent>();
        juce::String jerr;
        if (! comp->attach (vst3EditorTest->instance, jerr))
        {
            std::fprintf (stderr, "[vst3 editor test] attach failed: %s\n", jerr.toRawUTF8());
            quit();
            return;
        }
        const int w = std::max (200, comp->getWidth());
        const int h = std::max (200, comp->getHeight());
        struct TestWindow final : juce::DocumentWindow
        {
            using juce::DocumentWindow::DocumentWindow;
            void closeButtonPressed() override
            { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
        };
        vst3EditorTest->window = std::make_unique<TestWindow> (
            "Dusk - VST3 editor test", juce::Colours::black, juce::DocumentWindow::allButtons);
        vst3EditorTest->window->setUsingNativeTitleBar (true);
        vst3EditorTest->window->setContentOwned (comp.release(), true);
        vst3EditorTest->window->centreWithSize (w, h);
        vst3EditorTest->window->setVisible (true);   // creates the peer -> consumes the X11 latch
        duskstudio::platform::clearPreferX11ForNativeWindow();
        duskstudio::platform::installNonFatalXErrorHandler();
        return;
    }
#endif // DUSKSTUDIO_HAS_NATIVE_VST3

    // DUSKSTUDIO_REPLACE_TEST=A.vst3:B.vst3 - exercises the Replace plugin...
    // swap pattern under live processing. Loads A, runs audio, swaps to
    // B mid-stream via loadFromDescription, runs more audio. Mirrors the
    // user's GUI flow: right-click slot button -> Replace plugin -> pick
    // a different plugin. The colon-separated form lets us test ACROSS
    // distinct plugins, which is the actual crashing case (a single
    // plugin reload doesn't reproduce the same destructor-race surface).
    if (const char* path = std::getenv ("DUSKSTUDIO_REPLACE_TEST"); path != nullptr && *path)
    {
        constexpr double sampleRate = 48000.0;
        constexpr int    blockSize  = 256;

        auto session = std::make_unique<Session>();
        auto engine  = std::make_unique<AudioEngine> (*session);
        engine->prepareForSelfTest (sampleRate, blockSize);
        session->track (0).mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);

        const juce::String pathStr (path);
        const auto colon = pathStr.indexOfChar (':');
        const juce::String pathA = colon > 0 ? pathStr.substring (0, colon)  : pathStr;
        const juce::String pathB = colon > 0 ? pathStr.substring (colon + 1) : pathStr;

        auto& slot = engine->getStrip (0).getPluginSlot();
        juce::String err;
        std::fprintf (stdout, "[Replace] Loading A: %s\n", pathA.toRawUTF8());
        if (! slot.loadFromFile (juce::File (pathA), err))
        {
            std::fprintf (stderr, "FAIL: initial load: %s\n", err.toRawUTF8());
            quit();
            return;
        }

        // Build a description for plugin B by scanning its file via the
        // PluginManager so it's resolved through the same path the GUI
        // picker uses (cached KnownPluginList descriptions).
        juce::PluginDescription descB;
        {
            auto& mgr = engine->getPluginManager();
            juce::String scanErr;
            auto probe = mgr.createPluginInstance (juce::File (pathB), sampleRate,
                                                     blockSize, scanErr);
            if (probe != nullptr) probe->fillInPluginDescription (descB);
            else
            {
                std::fprintf (stderr, "FAIL: scan B: %s\n", scanErr.toRawUTF8());
                quit();
                return;
            }
            // probe goes out of scope - releases its instance immediately.
        }
        std::fprintf (stdout, "[Replace] B = \"%s\"\n", descB.name.toRawUTF8());

        // I/O buffers
        std::vector<std::vector<float>> inputs (2, std::vector<float> (blockSize, 0.0f));
        std::vector<const float*> inputPtrs { inputs[0].data(), inputs[1].data() };
        std::vector<std::vector<float>> outputs (2, std::vector<float> (blockSize, 0.0f));
        std::vector<float*> outputPtrs { outputs[0].data(), outputs[1].data() };
        duskstudio::device::CallbackContext ctx {};

        // Drive audio callbacks, then loadFromDescription with plugin B
        // mid-stream to swap. The previousInstance keep-alive in
        // PluginSlot defers A's destructor until the NEXT swap; an
        // immediate Diva->MininnDrum->ThirdPlugin sequence would
        // therefore destroy Diva from the message thread DURING the
        // third swap. Run extra blocks after each swap so the audio
        // thread has many chances to dereference a stale pointer.
        for (int b = 0; b < 200; ++b)
        {
            engine->audioDeviceIOCallback (
                inputPtrs.data(), 2, outputPtrs.data(), 2, blockSize, ctx);
            if (b == 50)
            {
                std::fprintf (stdout, "[Replace] swap A -> B...\n");
                if (! slot.loadFromDescription (descB, err))
                    std::fprintf (stderr, "FAIL: swap A->B: %s\n", err.toRawUTF8());
            }
            if (b == 120)
            {
                std::fprintf (stdout, "[Replace] swap B -> A (forces A's prev destructor in PluginSlot)...\n");
                juce::PluginDescription descA;
                if (auto* p = slot.getInstance())
                    p->fillInPluginDescription (descA);
                // Re-resolve A via the manager so we have a clean desc.
                auto& mgr = engine->getPluginManager();
                juce::String scanErr;
                auto probe = mgr.createPluginInstance (juce::File (pathA),
                                                          sampleRate, blockSize, scanErr);
                if (probe != nullptr)
                {
                    probe->fillInPluginDescription (descA);
                    probe.reset();
                }
                if (! slot.loadFromDescription (descA, err))
                    std::fprintf (stderr, "FAIL: swap B->A: %s\n", err.toRawUTF8());
            }
        }
        std::fprintf (stdout, "[Replace] survived 200 blocks across two swaps.\n");
        quit();
        return;
    }

   #if JUCE_LINUX
    if (envFlagSet ("DUSKSTUDIO_RUN_IPC_SELFTEST"))
    {
        // Out-of-process plugin hosting Phase 1 acceptance gate.
        // Validates the shm + futex round-trip against the
        // dusk-studio-plugin-host stub binary (which lives next to Dusk Studio in
        // the build output).
        const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        const auto host = exe.getSiblingFile ("dusk-studio-plugin-host");
        const auto rc = duskstudio::ipc::runIpcSelfTest (host.getFullPathName().toStdString());
        std::fflush (stdout);
        setApplicationReturnValue (rc);
        quit();
        return;
    }

    // Phase 2 acceptance gate. Pass DUSKSTUDIO_IPC_HOST_TEST=/path/to/plugin.vst3
    // (or .lv2) and Dusk Studio launches dusk-studio-plugin-host in --ipc-host mode,
    // loads the plugin, runs 1000 stereo blocks, asserts the signal was
    // modified. Use a real-world plugin like Multi-Q.vst3 to validate the
    // entire JUCE plugin loading + processBlock path through the IPC.
    if (const char* path = std::getenv ("DUSKSTUDIO_IPC_HOST_TEST"); path != nullptr && *path)
    {
        const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        const auto host = exe.getSiblingFile ("dusk-studio-plugin-host");
        const auto rc = duskstudio::ipc::runIpcHostTest (
            host.getFullPathName().toStdString(), std::string (path));
        std::fflush (stdout);
        setApplicationReturnValue (rc);
        quit();
        return;
    }
   #endif

    if (envFlagSet ("DUSKSTUDIO_RUN_TONE_TEST"))
    {
        runHeadlessToneTest();
        quit();
        return;
    }

   #if defined(__linux__)
    if (envFlagSet ("DUSKSTUDIO_RUN_ALSA_PERF"))
    {
        // Tier 1 ALSA backend perf test. Drives the backend directly (no
        // AudioDeviceManager involvement, no engine), output-only, silent.
        // Configurable via env vars - device picks default of hw:0,0 if
        // none set.
        duskstudio::AlsaPerformanceTest::Options opts;
        if (const auto* v = std::getenv ("DUSKSTUDIO_ALSA_PERF_DEVICE"))      opts.deviceId      = v;
        if (const auto* v = std::getenv ("DUSKSTUDIO_ALSA_PERF_RATE"))        opts.sampleRate    = (unsigned int) dusk::text::getIntValue (v);
        if (const auto* v = std::getenv ("DUSKSTUDIO_ALSA_PERF_DURATION_MS")) opts.durationMs    = dusk::text::getIntValue (v);
        if (const auto* v = std::getenv ("DUSKSTUDIO_ALSA_PERF_LOAD_US"))     opts.fakeDspLoadUs = dusk::text::getIntValue (v);
        if (const auto* v = std::getenv ("DUSKSTUDIO_ALSA_PERF_LOOPBACK"))    opts.runLoopback   = dusk::text::getIntValue (v) != 0;

        // Tier 2: comma-separated list, e.g. "44100,48000,96000". Empty
        // (or unset) keeps the single-rate Tier 1 behaviour.
        if (const auto* v = std::getenv ("DUSKSTUDIO_ALSA_PERF_RATES"))
        {
            for (const auto& t : dusk::text::split (v, ','))
            {
                const int rate = dusk::text::getIntValue (dusk::text::trim (t));
                if (rate > 0) opts.sampleRates.push_back ((unsigned int) rate);
            }
        }

        const auto report = duskstudio::AlsaPerformanceTest::runAll (opts);
        std::fprintf (stdout, "%s\n", report.c_str());
        std::fflush (stdout);

        quit();
        return;
    }
   #endif

    if (envFlagSet ("DUSKSTUDIO_RUN_PERF_TEST"))
    {
        // Headless engine-CPU benchmark. Builds a Session+AudioEngine the
        // same way the GUI path does, then drives many callbacks directly
        // and reports per-callback wall-clock vs. buffer budget across a
        // matrix of (sample rate, buffer size, channel load) configs.
        auto session = std::make_unique<Session>();
        auto engine  = std::make_unique<AudioEngine> (*session);

        // Wait briefly for device init so engine's DSP graph is warm.
        for (int waited = 0;
             engine->getCurrentSampleRate() <= 0.0 && waited < 5000;
             waited += 10)
            juce::Thread::sleep (10);

        AudioPipelineSelfTest test (*engine, engine->getDeviceManager(), *session);
        const auto report = test.runPerfSuite();
        std::fprintf (stdout, "%s\n", report.c_str());
        std::fflush (stdout);

        quit();
        return;
    }

    // Preflight the windowing system before touching anything that opens
    // a native window (Desktop display enumeration, the main window). Dusk
    // Studio's Linux UI is X11-only - the main window and every plugin-
    // editor peer are X11 surfaces, reached via XWayland on a Wayland
    // session - so a pure-Wayland session with XWayland disabled (sway
    // without `xwayland enable`, niri/labwc without an XWayland satellite)
    // has no display we can open. Without this guard JUCE null-derefs deep
    // inside window creation and the process core-dumps with no usable
    // message. All headless env-gate paths above have already returned, so
    // this is only reached by a real GUI launch.
    if (! displayUsableOrExplain())
    {
        setApplicationReturnValue (1);
        quit();
        return;
    }

    // Install crash handler + FileLogger AFTER every selftest env-gate
    // above has had its chance to quit. Self-test paths don't want
    // stray daily log files littering the user's data dir (or CI
    // runner $HOME). Normal-user launches fall through to here, so the
    // crash report path is established before the main window opens
    // and before any plugin scan / audio device init can fault.
    duskstudio::crash_handler::install (JUCE_APPLICATION_VERSION_STRING);

    // User UI-scale override. JUCE composes this with each display's own
    // OS-reported DPI scale, so 1.0 here means "let the OS decide" and
    // anything else is the user's manual zoom. Applied BEFORE creating
    // the main window so its initial layout uses the right metrics.
    juce::Desktop::getInstance().setGlobalScaleFactor (appconfig::getUiScaleOverride());

    mainWindow = std::make_unique<MainWindow> (getApplicationName());

    // Open a session passed on the command line (file-manager "open with",
    // `DuskStudio path/to/session.json`, or a session directory). Deferred so
    // it runs after MainComponent's own startup (recovery prompt / scan) has
    // settled, then loads the requested session over it.
    if (const auto sessionPath = sessionPathFromCommandLine (commandLine);
        sessionPath != juce::File())
    {
        juce::Component::SafePointer<MainWindow> safeWin (mainWindow.get());
        juce::MessageManager::callAsync ([safeWin, sessionPath]
        {
            if (safeWin == nullptr) return;
            if (auto* main = dynamic_cast<MainComponent*> (safeWin->getContentComponent()))
                main->openSessionPath (sessionPath);
        });
    }
}

void DuskStudioApp::shutdown()
{
    // Persist window geometry before tearing down the window. Reading
    // getWindowStateAsString() AFTER mainWindow.reset() would crash; doing
    // it here captures the user's last visible position/size/fullscreen.
    if (mainWindow != nullptr)
        WindowState::save (mainWindow->getWindowStateAsString());

    // Dismiss any still-open modal dialogs (e.g. the Audio Device selector)
    // BEFORE destroying mainWindow. The selector's `AudioDeviceSelectorComponent`
    // is registered as a change-listener on `AudioEngine::deviceManager`. If we
    // skip this, `mainWindow.reset()` destroys MainComponent -> AudioEngine ->
    // AudioDeviceManager, then ScopedJuceInitialiser_GUI's destructor (which
    // runs AFTER us, in JUCEApplicationBase::main) destroys ModalComponentManager,
    // which finally destroys the dialog - its destructor calls removeChangeListener
    // on the freed AudioDeviceManager -> SIGSEGV.
    //
    // The AudioSettingsPanel modal dialog is freed inside MainComponent's
    // destructor (via a tracked Component::SafePointer) so the
    // AudioDeviceSelectorComponent's listener-removal happens while
    // AudioDeviceManager is still alive. cancelAllModalComponents() here
    // is unhelpful - it only marks dialogs inactive and queues an async
    // delete that never fires before main() returns.

    // Bypass plugin destruction on the way out. Some Linux plugins
    // (notably u-he Diva) have buggy destructors that fail with
    // pure-virtual-method-called during their shutdown sequence,
    // aborting the process and leaving a coredump. Releasing the
    // unique_ptrs without destroying the underlying instances skips
    // the broken destructors entirely; the OS reclaims the memory
    // when the process exits a moment later. The plugins'
    // IPluginBase::terminate() hook does NOT run in this path - if a
    // plugin saves state in terminate (Diva writes its midiAssignFile
    // there), that state is the version from the previous successful
    // load. Acceptable trade-off for a clean exit code + no coredump.
    if (mainWindow != nullptr)
        if (auto* main = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
            main->leakAllPluginInstancesForShutdown();

    mainWindow.reset();
    clapEditorTestWindow.reset();   // dev CLAP-editor test path: release its component deterministically
#if DUSKSTUDIO_HAS_NATIVE_LV2
    lv2EditorTest.reset();          // dev LV2-editor test path: window first, then its slot
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
    vst3EditorTest.reset();         // dev VST3-editor test path: window first, then instance/bundle
#endif
    bounceTest.reset();             // headless bounce harness: worker joined, then engine/session

    // Tear down the FileLogger installed by crash_handler::install so
    // JUCE's leak detector doesn't complain at exit. The crash callback
    // installed via setApplicationCrashHandler is harmless if it stays
    // registered - process is exiting either way.
    duskstudio::crash_handler::uninstall();
}

void DuskStudioApp::systemRequestedQuit()
{
    quit();
}

void DuskStudioApp::anotherInstanceStarted (const juce::String& commandLine)
{
    // Single-instance app: a second launch (e.g. opening a session from the
    // file manager) routes here. Bring the window forward and open the path.
    if (mainWindow == nullptr) return;
    mainWindow->toFront (true);
    if (const auto sessionPath = sessionPathFromCommandLine (commandLine);
        sessionPath != juce::File())
        if (auto* main = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
            main->openSessionPath (sessionPath);
}
} // namespace duskstudio
