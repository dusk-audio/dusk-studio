#include "AudioSettingsView.h"
#include "AudioDeviceSelector.h"
#include "DuskTheme.h"
#include "PanelControls.h"
#include "../AppConfig.h"
#include "../../dsp/OutputPairRouting.h"
#include "../../engine/AudioEngine.h"
#include "../../engine/device/DeviceManager.h"
#include "../../engine/device/IODevice.h"
#include "../../foundation/MessageThread.h"
#include "../../session/Session.h"
#if defined(__linux__)
 #include "../../engine/alsa/AlsaAudioIODevice.h"
#endif

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace duskstudio::imgui
{
namespace
{
namespace dw = DuskWidgets;

// AudioSettingsPanel::resized()'s grid, in design pixels.
constexpr float kPanelW = 820.0f;
constexpr float kInsetX = 12.0f;
constexpr float kInsetY = 8.0f;
constexpr float kLabelW = 220.0f;
constexpr float kHeaderH = 24.0f;
constexpr float kHeaderGapBottom = 4.0f;
constexpr float kRowH = 30.0f;
constexpr float kRowGap = 4.0f;
constexpr float kTallRowH = 32.0f;
constexpr float kSectionGap = 14.0f;
constexpr float kComboW = 320.0f;
constexpr float kSliderW = 260.0f;
constexpr float kControlInset = 4.0f;

constexpr float kSectionHeadH = kHeaderH + kHeaderGapBottom;
constexpr float kStdRow = kRowH + kRowGap;
constexpr float kTallRow = kTallRowH + kRowGap;

constexpr float kAudioSectionH = kSectionHeadH + kStdRow + kSectionGap;
constexpr float kControlSurfaceH = kSectionHeadH + 2.0f * kStdRow + kSectionGap;
constexpr float kBindingsH = kSectionHeadH + kStdRow + kSectionGap;
constexpr float kSyncH = kSectionHeadH + 4.0f * kStdRow + kSectionGap;
constexpr float kGeneralH = kSectionHeadH + 7.0f * kStdRow + kSectionGap;
constexpr float kAdvancedH = kSectionHeadH + 3.0f * kTallRow + kStdRow;

// Every section stacked, plus the device block, which sizes itself. Taller than most
// displays can grant, and the body scrolls when the window hands back less.
float panelHeight() noexcept
{
    return kInsetY * 2.0f + kAudioSectionH + AudioDeviceSelector::preferredHeight()
         + kControlSurfaceH + kBindingsH + kSyncH + kGeneralH + kAdvancedH;
}

constexpr unsigned int kHintText = 0x909094ff;

ImU32 rgba (unsigned int hex)
{
    return IM_COL32 ((hex >> 24) & 0xff, (hex >> 16) & 0xff, (hex >> 8) & 0xff, hex & 0xff);
}

const char* const kOversamplingItems[] = { "1\xc3\x97 (native)", "2\xc3\x97", "4\xc3\x97" };
constexpr int kOversamplingFactors[] = { 1, 2, 4 };

const char* const kFrameRateItems[] = { "24 fps", "25 fps", "29.97 DF", "30 fps" };

const char* const kStopBehaviorItems[] = { "Stay where it is (pause)",
                                           "Return to start (rewind to 0)",
                                           "Return to last clicked point" };

const char* const kAutosaveItems[] = { "15 seconds", "30 seconds", "1 minute",
                                       "2 minutes", "5 minutes" };
constexpr int kAutosaveSeconds[] = { 15, 30, 60, 120, 300 };

#if defined(__linux__)
// Sensible USB-audio range. 2 is the minimum that gives the kernel any slack at all;
// 4 is what most DAWs use; 8 and 16 add latency but give the kernel more headroom
// against scheduler jitter.
const char* const kPeriodItems[] = { "2", "3", "4", "8", "16" };
constexpr int kPeriodCounts[] = { 2, 3, 4, 8, 16 };
#endif

int indexOfNearest (const int* values, int count, int wanted)
{
    int best = 0;
    int bestDistance = std::numeric_limits<int>::max();
    for (int i = 0; i < count; ++i)
    {
        const int distance = std::abs (values[i] - wanted);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}

class AudioSettingsViewImpl final : public DuskPanelView
{
public:
    AudioSettingsViewImpl (device::DeviceManager& dm, AudioEngine& e, Session& s,
                           AudioSettingsHost hostCallbacks)
        : deviceManager (dm), engine (e), session (s), host (std::move (hostCallbacks))
    {
        selector = std::make_unique<AudioDeviceSelector> (
            dm,
            [this] (std::string title, std::string message)
            {
                if (host.alert)
                    host.alert (std::move (title), std::move (message));
            },
            [this]
            {
                engine.clearDeviceFallbackHold();
                populateMainOutput();
            });

        deviceManager.addChangeListener (this, [this] { handleDeviceListChange(); });
        engine.addChangeCallback (this, [this] { handleDeviceListChange(); });

        populateMainOutput();
        populateMidiCombos();
        populateMulticore();

        // A stored factor that is not one of the three offered reads as native rather
        // than snapping to whichever rung happens to be nearest.
        oversampling = 0;
        const int storedFactor = session.oversamplingFactor.load (std::memory_order_relaxed);
        for (int i = 0; i < 3; ++i)
            if (kOversamplingFactors[i] == storedFactor)
                oversampling = i;
        frameRate = std::clamp (
            session.syncOutputTimeCodeFrameRate.load (std::memory_order_relaxed), 0, 3);
        stopBehavior = std::clamp (static_cast<int> (appconfig::getStopBehavior()), 0, 2);
        // A hand-edited config value snaps to the nearest offered cadence rather than
        // leaving the dropdown blank.
        autosave = indexOfNearest (kAutosaveSeconds, 5,
                                   appconfig::getAutosaveIntervalSeconds());

        chaseClock = session.externalSyncChasesTransport.load (std::memory_order_relaxed);
        emitClock = session.syncOutputEmitClock.load (std::memory_order_relaxed);
        chaseTimeCode = session.externalTimeCodeChasesTransport.load (std::memory_order_relaxed);
        emitTimeCode = session.syncOutputEmitTimeCode.load (std::memory_order_relaxed);
        tapeStripExpanded = appconfig::getTapeStripExpandedDefault();
        followPlayhead = appconfig::getFollowPlayheadDefault();
        softTakeover = appconfig::getMidiSoftTakeover();
        scanOnStartup = appconfig::getScanPluginsOnStartup();
        uiScale = appconfig::getUiScaleOverride();
        recordOffset = static_cast<float> (appconfig::getRecordingLatencyOffsetSamples());

       #if defined(__linux__)
        // getRequestedPeriods() is clamped to [2,16] while the menu carries a curated
        // subset, so a value with no matching rung leaves the dropdown blank rather
        // than pointing at a nearby one and misstating the next open.
        periods = -1;
        const int requested = AlsaAudioIODevice::getRequestedPeriods();
        for (int i = 0; i < 5; ++i)
            if (kPeriodCounts[i] == requested)
                periods = i;
       #endif
    }

    ~AudioSettingsViewImpl() override
    {
        engine.removeChangeCallback (this);
        deviceManager.removeChangeListener (this);
    }

    ImVec2 preferredSize() const override { return ImVec2 (kPanelW, panelHeight()); }

    // Escape belongs to an open dropdown first. Dear ImGui closes that popup while it
    // processes the frame's input, before the view draws, so what the guard reads is
    // whether one was up when the key arrived rather than whether one is up now.
    bool escapeDismisses() const override { return ! popupWasOpen; }

    void draw (dw::Context& ctx, ImVec2 origin, ImVec2 size) override
    {
        popupWasOpen = popupOpen;

        ctx.dl->AddRectFilled (origin, ImVec2 (origin.x + size.x, origin.y + size.y),
                               ctx.theme->panelFill);

        const ScopedFormStyle style (ctx);

        ImGui::SetCursorScreenPos (origin);
        if (! ImGui::BeginChild ("##audio-settings-body", size, ImGuiChildFlags_None,
                                 ImGuiWindowFlags_NoBackground))
        {
            ImGui::EndChild();
            return;
        }

        ImDrawList* const outer = ctx.dl;
        ctx.dl = ImGui::GetWindowDrawList();

        // The child's cursor already carries the scroll offset, so every row below is
        // laid out against it rather than against the panel's own origin.
        const ImVec2 content = ImGui::GetCursorScreenPos();
        const float bodyW = ImGui::GetContentRegionAvail().x;
        drawSections (ctx, content, bodyW);

        ImGui::SetCursorScreenPos (ImVec2 (content.x, content.y + ctx.s (panelHeight())));
        ImGui::Dummy (ImVec2 (bodyW, 1.0f));

        ctx.dl = outer;
        ImGui::EndChild();

        popupOpen = ImGui::IsPopupOpen (nullptr, ImGuiPopupFlags_AnyPopupId
                                                 | ImGuiPopupFlags_AnyPopupLevel);
    }

private:
    void drawSections (dw::Context& ctx, ImVec2 content, float bodyW)
    {
        const float left = content.x + ctx.s (kInsetX);
        const float innerW = std::max (ctx.s (100.0f), bodyW - ctx.s (kInsetX) * 2.0f);
        const float right = left + innerW;
        float y = content.y + ctx.s (kInsetY);

        const auto heading = [&] (const char* label)
        {
            formHeading (ctx, ImVec2 (left + ctx.s (kControlInset), y), innerW,
                         ctx.s (kHeaderH), label);
            y += ctx.s (kHeaderH) + ctx.s (kHeaderGapBottom);
        };
        const auto endSection = [&]
        {
            // The rule runs the panel's full width less an 8 px margin, not the row
            // grid's inset, which is what AudioSettingsPanel::paint() drew.
            formRule (ctx, ImVec2 (content.x + ctx.s (8.0f), y + ctx.s (kSectionGap * 0.5f)),
                      bodyW - ctx.s (16.0f));
            y += ctx.s (kSectionGap);
        };
        const auto takeRow = [&] (float height)
        {
            const float top = y;
            y += ctx.s (height) + ctx.s (kRowGap);
            return top;
        };

        const auto labelled = [&] (float top, const char* caption)
        {
            formLabel (ctx, ImVec2 (left + ctx.s (kControlInset), top),
                       ctx.s (kLabelW) - ctx.s (kControlInset) * 2.0f, ctx.s (kRowH),
                       caption);
        };
        const auto comboAt = [&] (float top, const char* id, ComboModel& model, float width)
        {
            const float x = left + ctx.s (kLabelW) + ctx.s (kControlInset);
            return formCombo (ctx, id, ImVec2 (x, top + ctx.s (2.0f)),
                              ImVec2 (x + ctx.s (width) - ctx.s (kControlInset) * 2.0f,
                                      top + ctx.s (kRowH) - ctx.s (2.0f)),
                              model);
        };
        const auto staticComboAt = [&] (float top, const char* id, const char* const* items,
                                        int count, int& selected, float width)
        {
            const float x = left + ctx.s (kLabelW) + ctx.s (kControlInset);
            return formCombo (ctx, id, ImVec2 (x, top + ctx.s (2.0f)),
                              ImVec2 (x + ctx.s (width) - ctx.s (kControlInset) * 2.0f,
                                      top + ctx.s (kRowH) - ctx.s (2.0f)),
                              items, count, selected);
        };
        const auto toggleAt = [&] (float top, float x, const char* id, const char* label,
                                   bool& value)
        {
            return formCheckbox (ctx, id, ImVec2 (x, top), ctx.s (kRowH), label, value);
        };
        const auto buttonAt = [&] (const char* id, float top, float width, float height,
                                   const char* label)
        {
            return formButton (ctx, id,
                               ImVec2 (right - ctx.s (width) + ctx.s (kControlInset),
                                       top + ctx.s (kControlInset)),
                               ImVec2 (right - ctx.s (kControlInset),
                                       top + ctx.s (height) - ctx.s (kControlInset)),
                               label);
        };

        // Audio
        heading ("Audio");
        selector->draw (ctx, ImVec2 (left, y), innerW);
        y += ctx.s (AudioDeviceSelector::preferredHeight());
        {
            const float top = takeRow (kRowH);
            labelled (top, "Main output");
            if (comboAt (top, "##main-output", mainOutput, kComboW))
                applyMainOutput();
            formTooltip ("Physical output pair for the main mix. Defaults to outputs 1-2. "
                         "Open more output channels above to send the master to a "
                         "different pair (aux lanes can then take 1-2 as a separate "
                         "headphone / cue feed).");
            if (buttonAt ("##rescan", top, 140.0f, kRowH, "Rescan devices"))
                deferred ([this] { applyRescan(); });
            formTooltip ("Re-enumerate audio backends, devices and MIDI ports. Use after "
                         "plugging in or removing a USB / Thunderbolt audio interface. On "
                         "Linux, MIDI controllers are picked up on their own once the "
                         "transport is stopped.");
        }
        endSection();

        // Control Surface
        heading ("Control Surface");
        {
            const float top = takeRow (kRowH);
            labelled (top, "MCU Control Surface Input");
            if (comboAt (top, "##mcu-input", mcuInput, kComboW))
                applyMcuInput();
            formTooltip ("Pick the MIDI input port your Mackie Control / X-Touch / Tascam "
                         "Model 12 (MCU mode) is sending on. Faders, V-pot encoders, "
                         "transport buttons, and mute/solo/arm presses arrive on this "
                         "port. Setting this gates that device's MIDI from the generic "
                         "MIDI Learn surface so MCU traffic does not double-fire.");
        }
        {
            const float top = takeRow (kRowH);
            labelled (top, "MCU Control Surface Output");
            if (comboAt (top, "##mcu-output", mcuOutput, kComboW))
                applyMcuOutput();
            formTooltip ("Pick the MIDI output port for motorized fader / button LED / "
                         "LCD / timecode / meter feedback to the control surface. "
                         "Typically the same physical device as the MCU input.");
        }
        endSection();

        // MIDI Bindings
        heading ("MIDI Bindings");
        {
            const float top = takeRow (kRowH);
            const float x = left + ctx.s (kControlInset);
            if (formButton (ctx, "##midi-bindings", ImVec2 (x, top + ctx.s (2.0f)),
                            ImVec2 (x + ctx.s (kLabelW) - ctx.s (kControlInset) * 2.0f,
                                    top + ctx.s (kRowH) - ctx.s (2.0f)),
                            "MIDI Bindings..."))
                deferred ([this] { if (host.openMidiBindings) host.openMidiBindings(); });
            formTooltip ("Open the MIDI Bindings panel: list everything currently mapped, "
                         "remove individual bindings, or clear all. Use right-click on any "
                         "fader / knob / button to add new bindings.");
        }
        endSection();

        // MIDI Sync
        heading ("MIDI Sync");
        const float toggleX = left + ctx.s (kLabelW + kComboW + 8.0f);
        {
            const float top = takeRow (kRowH);
            labelled (top, "MIDI Sync Source");
            if (comboAt (top, "##sync-source", syncSource, kComboW))
                applySyncSource();
            formTooltip ("MIDI Clock sync source. When set, Dusk Studio's tempo follows "
                         "incoming MIDI Clock (24 PPQN) from the chosen input.");
            if (toggleAt (top, toggleX, "##chase-clock", "Chase transport (Start/Stop)",
                          chaseClock))
                session.externalSyncChasesTransport.store (chaseClock,
                                                           std::memory_order_relaxed);
            formTooltip ("When on, MIDI Start (FA / FB) plays Dusk Studio and MIDI Stop "
                         "(FC) stops it. Off = tempo-only sync; the transport stays under "
                         "your control.");
        }
        {
            const float top = takeRow (kRowH);
            if (toggleAt (top, toggleX, "##chase-mtc", "Chase transport from MTC",
                          chaseTimeCode))
                session.externalTimeCodeChasesTransport.store (chaseTimeCode,
                                                               std::memory_order_relaxed);
            formTooltip ("When on, the transport follows incoming MTC absolute time. "
                         "Initial lock on the master's Play edge; freewheels within about "
                         "two frames of drift; soft re-locates on sustained drift.");
        }
        {
            const float top = takeRow (kRowH);
            labelled (top, "MIDI Sync Output");
            if (comboAt (top, "##sync-output", syncOutput, kComboW))
                applySyncOutput();
            formTooltip ("Pick a MIDI output port to send Clock + Start/Stop to. Used when "
                         "Dusk Studio acts as the master. Emission only fires while the "
                         "'Emit clock' toggle is on.");
            if (toggleAt (top, toggleX, "##emit-clock", "Emit clock (Dusk Studio as master)",
                          emitClock))
                session.syncOutputEmitClock.store (emitClock, std::memory_order_relaxed);
            formTooltip ("When on, Dusk Studio emits 24-PPQN MIDI Clock + Start/Stop "
                         "transport bytes to the chosen output.");
        }
        {
            const float top = takeRow (kRowH);
            labelled (top, "MTC frame rate");
            if (staticComboAt (top, "##mtc-rate", kFrameRateItems, 4, frameRate, 140.0f))
                session.syncOutputTimeCodeFrameRate.store (frameRate,
                                                            std::memory_order_relaxed);
            formTooltip ("SMPTE frame rate used when emitting MTC. 29.97 DF for NTSC video "
                         "sync (drop-frame); 25 fps for PAL; 24 fps for film; 30 fps "
                         "non-drop for audio-only workflows.");
            if (toggleAt (top, toggleX, "##emit-mtc", "Emit MTC (Dusk Studio as master)",
                          emitTimeCode))
                session.syncOutputEmitTimeCode.store (emitTimeCode,
                                                       std::memory_order_relaxed);
            formTooltip ("When on, Dusk Studio emits MTC quarter-frames + full-frame sysex "
                         "to the chosen Sync Output.");
        }
        endSection();

        // General
        heading ("General");
        const float generalToggleX = left + ctx.s (kLabelW + kControlInset);
        {
            const float top = takeRow (kRowH);
            if (toggleAt (top, generalToggleX, "##tape-strip",
                          "Expand tape strip by default", tapeStripExpanded))
                appconfig::setTapeStripExpandedDefault (tapeStripExpanded);
            formTooltip ("When on, the TIMELINE tape strip starts expanded on every app "
                         "launch. Saved per-machine; takes effect on next launch.");
        }
        {
            const float top = takeRow (kRowH);
            if (toggleAt (top, generalToggleX, "##follow-playhead",
                          "Follow playhead by default", followPlayhead))
                appconfig::setFollowPlayheadDefault (followPlayhead);
            formTooltip ("When on, the timeline and editors start with Chase engaged, "
                         "scrolling to keep the playhead in view during playback. Saved "
                         "per-machine; takes effect on next launch.");
        }
        {
            const float top = takeRow (kRowH);
            if (toggleAt (top, generalToggleX, "##soft-takeover",
                          "MIDI soft takeover (pickup)", softTakeover))
            {
                appconfig::setMidiSoftTakeover (softTakeover);
                engine.setMidiSoftTakeover (softTakeover);
            }
            formTooltip ("When on, a knob or fader bound by MIDI Learn stays dormant until "
                         "it crosses the parameter's current position, instead of snapping "
                         "the parameter to the controller on first touch.");
        }
        {
            const float top = takeRow (kRowH);
            if (toggleAt (top, generalToggleX, "##scan-startup", "Scan plugins on startup",
                          scanOnStartup))
                appconfig::setScanPluginsOnStartup (scanOnStartup);
            formTooltip ("When on, every app launch synchronously scans every installed "
                         "plugin format and refreshes the plugin cache. Saved per-machine; "
                         "takes effect on next launch.");
        }
        {
            const float top = takeRow (kRowH);
            labelled (top, "Playhead on Stop");
            if (staticComboAt (top, "##stop-behavior", kStopBehaviorItems, 3, stopBehavior,
                               kComboW))
            {
                const auto value = static_cast<appconfig::StopBehavior> (stopBehavior);
                appconfig::setStopBehavior (value);
                session.stopBehavior.store (stopBehavior, std::memory_order_relaxed);
            }
            formTooltip ("What the playhead does on Stop. \"Stay\" matches the "
                         "commercial-DAW pause-in-place. \"Return to start\" rewinds every "
                         "time. \"Last clicked\" jumps to the most recent ruler click so "
                         "Stop then Play recycles a region you just auditioned.");
        }
        {
            const float top = takeRow (kRowH);
            labelled (top, "Autosave every");
            if (staticComboAt (top, "##autosave", kAutosaveItems, 5, autosave, 200.0f))
                appconfig::setAutosaveIntervalSeconds (kAutosaveSeconds[autosave]);
            formTooltip ("How often the session autosaves for crash recovery. Saved "
                         "per-machine; applies when this panel closes.");
        }
        {
            const float top = takeRow (kRowH);
            labelled (top, "UI scale");
            const float x = left + ctx.s (kLabelW) + ctx.s (kControlInset);
            const auto moved = formSlider (
                ctx, "##ui-scale", ImVec2 (x, top + ctx.s (2.0f)),
                ImVec2 (x + ctx.s (kSliderW) - ctx.s (kControlInset) * 2.0f,
                        top + ctx.s (kRowH) - ctx.s (2.0f)),
                uiScale, appconfig::kUiScaleMin, appconfig::kUiScaleMax, "%.2fx");
            formTooltip ("Multiplier applied on top of the OS-reported display DPI. 1.00x "
                         "= follow the OS. Range 0.50x to 2.00x.");
            applyUiScale (moved);
            drawHint (ctx, ImVec2 (x + ctx.s (kSliderW), top),
                      right - (x + ctx.s (kSliderW)),
                      "Previews live; saved per-machine when released.");
        }
        endSection();

        // Advanced
        heading ("Advanced");
        {
            const float top = takeRow (kTallRowH);
           #if defined(__linux__)
            labelled (top, "Periods (ALSA)");
            if (staticComboAt (top, "##periods", kPeriodItems, 5, periods, 100.0f))
                deferred ([this] { applyPeriods(); });
            formTooltip ("ALSA period count. Only applies to the ALSA backend. Increase if "
                         "you hear xruns or distortion at low buffer sizes; decrease for "
                         "lower latency.");
           #endif
            if (buttonAt ("##self-test", top, 160.0f, kTallRowH, "Run Self-Test..."))
                deferred ([this] { if (host.openSelfTest) host.openSelfTest(); });
            formTooltip ("Open the headless audio pipeline self-test panel - synthetic "
                         "engine tests plus a backend cycle.");
        }
        {
            const float top = takeRow (kTallRowH);
            labelled (top, "Effect Oversampling");
            if (staticComboAt (top, "##oversampling", kOversamplingItems, 3, oversampling,
                               140.0f))
                applyOversampling();
            formTooltip ("Global effect oversampling. 1\xc3\x97 is native rate (lowest "
                         "CPU). 2\xc3\x97 / 4\xc3\x97 raise the internal rate of every "
                         "channel, bus and master EQ + compressor and the tape saturation "
                         "- roughly 2-3\xc3\x97 the mix-engine CPU at 4\xc3\x97. The "
                         "status bar shows an @2x / @4x badge while engaged.");
        }
        {
            const float top = takeRow (kTallRowH);
            labelled (top, "Multicore DSP");
            if (comboAt (top, "##multicore", multicore, 220.0f))
                applyMulticore();
            formTooltip ("Spread the 24 channel strips' per-block DSP across real-time "
                         "worker threads so the mixer uses more than one CPU core. Off is "
                         "the single-core path. Auto uses (cores - 2) workers, leaving a "
                         "core for the UI and OS. Per-machine - it is not saved in the "
                         "session.");
        }
        {
            const float top = takeRow (kRowH);
            labelled (top, "Recording offset");
            const float x = left + ctx.s (kLabelW) + ctx.s (kControlInset);
            const auto moved = formSlider (
                ctx, "##record-offset", ImVec2 (x, top + ctx.s (2.0f)),
                ImVec2 (x + ctx.s (kSliderW) - ctx.s (kControlInset) * 2.0f,
                        top + ctx.s (kRowH) - ctx.s (2.0f)),
                recordOffset, static_cast<float> (appconfig::kRecordingLatencyOffsetMin),
                static_cast<float> (appconfig::kRecordingLatencyOffsetMax), "%.0f smp");
            formTooltip ("Samples subtracted from each recorded audio take's timeline "
                         "start, to compensate for input round-trip latency (converters, "
                         "external gear, unreported plugin delay). Calibrate by recording a "
                         "loopback of the metronome click and entering the measured error. "
                         "Saved per-machine; applies to the next take.");
            if (moved.released)
            {
                const int samples = static_cast<int> (recordOffset);
                appconfig::setRecordingLatencyOffsetSamples (samples);
                engine.setRecordingLatencyOffsetSamples (samples);
            }
            drawHint (ctx, ImVec2 (x + ctx.s (kSliderW), top),
                      right - (x + ctx.s (kSliderW)),
                      "Saved per-machine; applies to the next take.");
        }
    }

    void drawHint (const dw::Context& ctx, ImVec2 at, float width, const char* text)
    {
        ImFont* const font = ctx.fonts->label;
        dw::text (ctx, font, font->FontSize,
                  ImVec2 (at.x + ctx.s (kControlInset),
                          at.y + (ctx.s (kRowH) - font->FontSize) * 0.5f),
                  width, rgba (kHintText), text, dw::Align::left);
    }

    void deferred (std::function<void()> action)
    {
        // Anything that reopens a device, rescans the backends or takes the panel down
        // has to happen between frames rather than inside the one that asked for it.
        std::weak_ptr<int> alive = life;
        dusk::callAsync ([alive, action = std::move (action)]
        {
            if (alive.expired())
                return;
            action();
        });
    }

    void handleDeviceListChange()
    {
        populateMidiCombos();
        populateMainOutput();
    }

    void populateMainOutput()
    {
        mainOutput.clear();
        mainOutputIds.clear();
        mainOutput.add ("1-2 (default)");
        mainOutputIds.push_back (1);

        auto* const device = deviceManager.getCurrentDevice();
        if (device != nullptr)
        {
            // Only offer pairs the engine can actually write to: both channels must be
            // in the device's active-output set, or the master would appear to vanish.
            const auto active = device->getActiveOutputChannels();
            const int count = static_cast<int> (device->getOutputChannelNames().size());
            for (int i = 2; i + 1 < count; i += 2)
            {
                if (! active[i] || ! active[i + 1])
                    continue;
                mainOutput.add ("Out " + std::to_string (i + 1) + "-"
                                + std::to_string (i + 2));
                mainOutputIds.push_back (outputpair::encodePair (i, i + 1));
            }
        }

        const int stored = session.master().outputPair.load (std::memory_order_relaxed);
        int selected = 0;
        for (std::size_t i = 0; i < mainOutputIds.size(); ++i)
            if (mainOutputIds[i] == stored)
                selected = static_cast<int> (i);
        mainOutput.finish (selected);

        // Persist the fallback so the engine does not keep routing the master to a pair
        // that is no longer active while the panel shows "1-2 (default)". Only while a
        // device is there to say which pairs are active: with none, every pair but the
        // default is missing because nothing has been enumerated, and writing the
        // fallback then would drop a stored routing the next open would have honoured.
        if (device == nullptr)
            return;

        const int normalized = mainOutputIds[static_cast<std::size_t> (selected)] <= 1
                             ? -1 : mainOutputIds[static_cast<std::size_t> (selected)];
        if (normalized != stored)
            session.master().outputPair.store (normalized, std::memory_order_relaxed);
    }

    void applyMainOutput()
    {
        if (mainOutput.selected < 0
            || mainOutput.selected >= static_cast<int> (mainOutputIds.size()))
            return;
        const int id = mainOutputIds[static_cast<std::size_t> (mainOutput.selected)];
        session.master().outputPair.store (id <= 1 ? -1 : id, std::memory_order_relaxed);
    }

    void populateMidiCombos()
    {
        const auto fill = [] (ComboModel& model,
                              const std::vector<midi::MidiDeviceInfo>& devices,
                              const std::string& chosen)
        {
            model.clear();
            model.add ("(none)");
            int selected = 0;
            for (std::size_t i = 0; i < devices.size(); ++i)
            {
                if (devices[i].identifier == chosen)
                    selected = static_cast<int> (i) + 1;
                model.add (devices[i].name);
            }
            model.finish (selected);
        };

        fill (syncSource, engine.getMidiInputDevices(),
              session.syncSourceInputIdentifier.toStdString());
        fill (syncOutput, engine.getMidiOutputDevices(),
              session.syncOutputIdentifier.toStdString());
        fill (mcuInput, engine.getMidiInputDevices(),
              session.mcu.inputIdentifier.toStdString());
        fill (mcuOutput, engine.getMidiOutputDevices(),
              session.mcu.outputIdentifier.toStdString());
    }

    void populateMulticore()
    {
        multicore.clear();
        multicoreIds.clear();
        const int maxWorkers = appconfig::maxMulticoreWorkers();

        multicore.add ("Off (serial)");
        multicoreIds.push_back (0);
        multicore.add (maxWorkers > 0
                           ? "Auto (" + std::to_string (maxWorkers) + " workers)"
                           : std::string ("Auto (needs 4+ cores)"));
        multicoreIds.push_back (-1);
        for (int n = 1; n <= maxWorkers; ++n)
        {
            multicore.add (std::to_string (n) + (n == 1 ? " worker" : " workers"));
            multicoreIds.push_back (n);
        }

        int selected = 1;   // Auto
        switch (appconfig::getMulticoreDspMode())
        {
            case appconfig::MulticoreDspMode::Off:  selected = 0; break;
            case appconfig::MulticoreDspMode::Auto: selected = 1; break;
            case appconfig::MulticoreDspMode::Manual:
                // A host with no manual range falls back to Auto.
                selected = maxWorkers > 0
                         ? 1 + std::clamp (appconfig::getMulticoreManualWorkers(), 1,
                                           maxWorkers)
                         : 1;
                break;
        }
        multicore.finish (selected);
    }

    void applyMulticore()
    {
        if (multicore.selected < 0
            || multicore.selected >= static_cast<int> (multicoreIds.size()))
            return;

        const int workers = multicoreIds[static_cast<std::size_t> (multicore.selected)];
        if (workers == 0)
            appconfig::setMulticoreDspMode (appconfig::MulticoreDspMode::Off);
        else if (workers < 0)
            appconfig::setMulticoreDspMode (appconfig::MulticoreDspMode::Auto);
        else
        {
            appconfig::setMulticoreDspMode (appconfig::MulticoreDspMode::Manual);
            appconfig::setMulticoreManualWorkers (workers);
        }

        engine.setDesiredWorkers (appconfig::resolveWorkerCount());
        // The pool is reconfigured only inside the engine's DSP restart, which is
        // deferred to the next stop while the transport rolls.
        engine.restartDspWhenIdle();
    }

    void applyOversampling()
    {
        if (oversampling < 0 || oversampling > 2)
            return;
        session.oversamplingFactor.store (kOversamplingFactors[oversampling],
                                           std::memory_order_relaxed);
        // The factor is read when the engine prepares its DSP, so it only takes effect
        // on a restart - deferred to the next stop while the transport rolls.
        engine.restartDspWhenIdle();
    }

    void applyUiScale (const FormSliderResult& moved)
    {
        if (moved.changed)
            ++uiScaleFrames;

        // 20 Hz rather than every frame: a preview re-lays out the whole window behind
        // the panel, and the slider has to stay draggable through it.
        constexpr int kFramesPerPreview = 3;
        if (! moved.released && uiScaleFrames < kFramesPerPreview)
            return;
        if (! moved.changed && ! moved.released)
            return;

        uiScaleFrames = 0;
        // Between frames, not inside one: a preview re-lays out the whole JUCE tree the
        // panel is floating over, and this is running from the framework's event pump.
        const float scale = uiScale;
        deferred ([this, scale] { if (host.previewUiScale) host.previewUiScale (scale); });
        if (moved.released)
            appconfig::setUiScaleOverride (uiScale);
    }

    void applySyncSource()
    {
        const auto& devices = engine.getMidiInputDevices();
        const int index = syncSource.selected - 1;
        if (index < 0 || index >= static_cast<int> (devices.size()))
        {
            session.syncSourceInputIdentifier.clear();
            session.syncSourceInputIdx.store (-1, std::memory_order_release);
            return;
        }
        session.syncSourceInputIdentifier = devices[static_cast<std::size_t> (index)].identifier;
        session.syncSourceInputIdx.store (index, std::memory_order_release);
    }

    void applySyncOutput()
    {
        const auto& devices = engine.getMidiOutputDevices();
        const int index = syncOutput.selected - 1;
        if (index < 0 || index >= static_cast<int> (devices.size()))
        {
            session.syncOutputIdentifier.clear();
            session.syncOutputIdx.store (-1, std::memory_order_release);
            return;
        }
        session.syncOutputIdentifier = devices[static_cast<std::size_t> (index)].identifier;
        // Open the port BEFORE publishing the index: the audio thread's acquire-load of
        // syncOutputIdx expects the opened-port state to be visible, and the eager open
        // keeps the first emission clear of a synchronous sequencer connect.
        engine.ensureMidiOutputOpen (index);
        session.syncOutputIdx.store (index, std::memory_order_release);
    }

    void applyMcuInput()
    {
        const auto& devices = engine.getMidiInputDevices();
        const int index = mcuInput.selected - 1;
        if (index < 0 || index >= static_cast<int> (devices.size()))
        {
            session.mcu.inputIdentifier.clear();
            session.mcu.resolvedInputIdx.store (-1, std::memory_order_release);
            return;
        }
        session.mcu.inputIdentifier = devices[static_cast<std::size_t> (index)].identifier;
        session.mcu.resolvedInputIdx.store (index, std::memory_order_release);
    }

    void applyMcuOutput()
    {
        const auto& devices = engine.getMidiOutputDevices();
        const int index = mcuOutput.selected - 1;
        if (index < 0 || index >= static_cast<int> (devices.size()))
        {
            session.mcu.outputIdentifier.clear();
            session.mcu.resolvedOutputIdx.store (-1, std::memory_order_release);
            return;
        }
        session.mcu.outputIdentifier = devices[static_cast<std::size_t> (index)].identifier;
        engine.ensureMidiOutputOpen (index);
        session.mcu.resolvedOutputIdx.store (index, std::memory_order_release);
    }

    void applyRescan()
    {
        // Every registered backend, not only the current one, so switching backend after
        // a hot-plug still sees the freshly-enumerated devices on the new one.
        deviceManager.scanAllDeviceTypes();
        // Some backends only broadcast on a real diff, so force the refresh either way.
        deviceManager.notifyChange();
        // The same rescan re-enumerates MIDI inputs so freshly plugged-in controllers
        // appear in the per-strip dropdowns.
        engine.refreshMidiInputs();
    }

   #if defined(__linux__)
    void applyPeriods()
    {
        if (periods < 0 || periods > 4)
            return;
        const int wanted = kPeriodCounts[periods];
        const int previous = AlsaAudioIODevice::getRequestedPeriods();
        AlsaAudioIODevice::setRequestedPeriods (wanted);

        // The period count is read in the ALSA device's open(), so the device has to be
        // recreated for it to apply, and re-submitting the same setup short-circuits
        // while the device is live. Closing first makes the re-apply a real open - but
        // only for ALSA, where the setting means anything.
        const auto* const type = deviceManager.getCurrentDeviceType();
        if (type == nullptr || type->getTypeName() != "ALSA")
            return;
        if (deviceManager.getCurrentDevice() == nullptr)
            return;

        const auto setup = deviceManager.getSetup();
        deviceManager.closeDevice();

        const auto error = deviceManager.setSetup (setup, /*treatAsChosen*/ true);
        if (error.empty())
            return;

        // The device would not come back at the new count. Put the count back and
        // reopen at the old one; the rollback can fail too, and that outcome is worse
        // than the one that started this, so it has to reach the user as well.
        AlsaAudioIODevice::setRequestedPeriods (previous);
        const auto rollbackError = deviceManager.setSetup (setup, /*treatAsChosen*/ true);
        periods = -1;
        for (int i = 0; i < 5; ++i)
            if (kPeriodCounts[i] == AlsaAudioIODevice::getRequestedPeriods())
                periods = i;

        std::string message = "Could not reopen the audio device with "
                            + std::to_string (wanted) + " periods:\n\n" + error;
        if (rollbackError.empty())
            message += "\n\nReverted to " + std::to_string (previous) + " periods.";
        else
            message += "\n\nReverting to " + std::to_string (previous)
                     + " periods also failed:\n\n" + rollbackError
                     + "\n\nNo audio device is open. Pick one in Audio Settings.";

        if (host.alert)
            host.alert ("Audio device error", message);
    }
   #endif

    device::DeviceManager& deviceManager;
    AudioEngine& engine;
    Session& session;
    AudioSettingsHost host;
    std::unique_ptr<AudioDeviceSelector> selector;

    // Handed to every deferred action so one queued behind a panel that closes first
    // does not run against a destroyed view.
    std::shared_ptr<int> life = std::make_shared<int> (0);

    ComboModel mainOutput, mcuInput, mcuOutput, syncSource, syncOutput, multicore;
    std::vector<int> mainOutputIds;
    std::vector<int> multicoreIds;

    int oversampling = 0;
    int frameRate = 0;
    int stopBehavior = 0;
    int autosave = 1;
   #if defined(__linux__)
    int periods = -1;
   #endif

    bool chaseClock = false;
    bool emitClock = false;
    bool chaseTimeCode = false;
    bool emitTimeCode = false;
    bool tapeStripExpanded = false;
    bool followPlayhead = false;
    bool softTakeover = false;
    bool scanOnStartup = false;

    float uiScale = 1.0f;
    float recordOffset = 0.0f;
    int uiScaleFrames = 0;
    bool popupOpen = false;
    bool popupWasOpen = false;
};
} // namespace

std::unique_ptr<DuskPanelView> makeAudioSettingsView (device::DeviceManager& deviceManager,
                                                      AudioEngine& engine,
                                                      Session& session,
                                                      AudioSettingsHost host)
{
    return std::unique_ptr<DuskPanelView> (
        new AudioSettingsViewImpl (deviceManager, engine, session, std::move (host)));
}
} // namespace duskstudio::imgui
