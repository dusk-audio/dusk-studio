#include "AudioSettingsView.h"
#include "AudioDeviceSelector.h"
#include "PanelControls.h"
#include "DuskTheme.h"
#include "../AppConfig.h"
#include "../../dsp/OutputPairRouting.h"
#include "../../engine/AudioEngine.h"
#include "../../engine/PluginSearchPaths.h"
#include "../../engine/sfz/SfzLibraryService.h"
#include "../../engine/device/DeviceManager.h"
#include "../../engine/device/IODevice.h"
#include "../../foundation/MessageThread.h"
#include "../../session/Session.h"
#if defined(__linux__)
 #include "../../engine/alsa/AlsaAudioIODevice.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace duskstudio::imgui
{
namespace
{
namespace dw = DuskWidgets;

constexpr float kPanelW = 1040.0f;
constexpr float kInsetX = 12.0f;
constexpr float kInsetY = 8.0f;
constexpr float kLabelW = 220.0f;
constexpr float kHeaderH = 24.0f;
constexpr float kHeaderGapBottom = 4.0f;
constexpr float kRowH = 36.0f;
constexpr float kRowGap = 8.0f;
constexpr float kTallRowH = 32.0f;
constexpr float kComboW = 320.0f;
constexpr float kSliderW = 260.0f;
constexpr float kControlInset = 4.0f;

constexpr std::array<const char*, 8> kTabLabels {
    "Audio", "Control Surface", "MIDI Bindings", "MIDI Sync", "General", "Advanced", "SFZ Library", "Plug-ins"
};
constexpr std::array<const char*, 8> kTabControls {
    "tab-audio", "tab-control-surface", "tab-midi-bindings", "tab-midi-sync", "tab-general", "tab-advanced", "tab-sfz-library", "tab-plugins"
};

constexpr unsigned int kHintText = 0x909094ff;

ImU32 rgba (unsigned int hex)
{
    return IM_COL32 ((hex >> 24) & 0xff, (hex >> 16) & 0xff, (hex >> 8) & 0xff, hex & 0xff);
}

const char* const kOversamplingItems[] = { "1\xc3\x97 (native)", "2\xc3\x97", "4\xc3\x97" };
constexpr int kOversamplingFactors[] = { 1, 2, 4 };

const char* const kFrameRateItems[] = { "24 fps", "25 fps", "29.97 DF", "30 fps" };

// Indexed by the persisted StopBehavior value.
const char* const kStopBehaviorItems[] = { "Stay where it is (pause)",
                                           "Return to start (rewind to 0)",
                                           "Return to last clicked point",
                                           "Return to where play or record started" };
constexpr int kStopBehaviorCount = static_cast<int> (std::size (kStopBehaviorItems));

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
        pluginFolders = pluginpaths::load();
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
                // An explicit pick settles the backend question, so the bar's
                // startup fallback notice has said all it has to say.
                engine.clearBackendFallbackNotice();
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
        stopBehavior = std::clamp (static_cast<int> (appconfig::getStopBehavior()), 0,
                                   kStopBehaviorCount - 1);
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
        recordOffset = appconfig::getRecordingLatencyOffsetSamples();
        pendingTab = std::clamp (host.initialTab, 0, static_cast<int> (kTabLabels.size()) - 1);

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

    bool controlPointForScenario (const std::string& control, ImVec2& point, float position) const override
    {
        for (std::size_t i = 0; i < kTabControls.size(); ++i)
            if (control == kTabControls[i])
            {
                point = scenarioTabs[i];
                return point.x > 0.0f;
            }
        if (control == "record-offset") point = scenarioOffset;
        else if (control == "sfz-folder") point = scenarioSfzFolder;
        else if (control == "sfz-move") point = scenarioSfzMove;
        else if (control == "record-offset-minus") point = scenarioOffsetMinus;
        else if (control == "record-offset-plus") point = scenarioOffsetPlus;
        else if (control == "midi-bindings") point = scenarioMidiBindings;
        else if (control == "rescan") point = scenarioRescan;
        else if (control == "autosave") point = scenarioAutosave;
        else if (control == "tape-default") point = scenarioTape;
        else if (control == "follow-default") point = scenarioFollow;
        else if (control == "plugin-folder-add") point = scenarioPluginAdd;
        else if (control == "plugin-scan") point = scenarioPluginScan;
        else if (control == "plugin-folder-remove") point = scenarioPluginRemove;
        else if (control == "ui-scale")
            point = { scenarioScaleFirst.x + (scenarioScaleLast.x - scenarioScaleFirst.x)
                        * std::clamp (position, 0.0f, 1.0f),
                      (scenarioScaleFirst.y + scenarioScaleLast.y) * 0.5f };
        else return false;
        return point.x > 0.0f;
    }

    ImVec2 scenarioMidiBindings {};
    ImVec2 scenarioRescan {};
    ImVec2 scenarioAutosave {};
    ImVec2 scenarioTape {}, scenarioFollow {};
    ImVec2 scenarioScaleFirst {}, scenarioScaleLast {};
    ImVec2 scenarioOffset {}, scenarioOffsetMinus {}, scenarioOffsetPlus {};
    ImVec2 scenarioSfzFolder {}, scenarioSfzMove {};
    ImVec2 scenarioPluginAdd {}, scenarioPluginScan {}, scenarioPluginRemove {};
    std::array<ImVec2, kTabLabels.size()> scenarioTabs {};

    ImVec2 preferredSize() const override { return ImVec2 (kPanelW, 680.0f); }

    // Input processing closes popups and cancels text edits before draw observes them.
    bool capturesKeyboard() const override { return keyboardActive; }
    bool escapeDismisses() const override { return ! popupWasOpen && ! keyboardWasActive; }

    void draw (dw::Context& ctx, ImVec2 origin, ImVec2 size) override
    {
        popupWasOpen = popupOpen;
        keyboardWasActive = keyboardActive;
        scenarioTabs.fill ({});
        scenarioMidiBindings = scenarioRescan = scenarioAutosave = scenarioTape = scenarioFollow = {};
        scenarioScaleFirst = scenarioScaleLast = scenarioOffset = scenarioOffsetMinus = scenarioOffsetPlus = {};
        scenarioSfzFolder = scenarioSfzMove = {};
        scenarioPluginAdd = scenarioPluginScan = scenarioPluginRemove = {};

        ctx.dl->AddRectFilled (origin, ImVec2 (origin.x + size.x, origin.y + size.y),
                               ctx.theme->panelFill);

        auto fonts = *ctx.fonts;
        fonts.band = fonts.label = fonts.textEntry != nullptr ? fonts.textEntry : ImGui::GetFont();
        auto formContext = ctx;
        formContext.fonts = &fonts;
        const ScopedFormStyle style (formContext);
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, { ctx.s (18), ctx.s (16) });
        ImGui::PushStyleVar (ImGuiStyleVar_ItemSpacing, { ctx.s (8), ctx.s (4) });
        ImGui::PushStyleVar (ImGuiStyleVar_FramePadding, { ctx.s (12), ctx.s (9) });

        ImGui::SetCursorScreenPos (origin);
        if (! ImGui::BeginChild ("##audio-settings-body", size, ImGuiChildFlags_AlwaysUseWindowPadding,
                                 ImGuiWindowFlags_NoBackground))
        {
            ImGui::EndChild();
            ImGui::PopStyleVar (3);
            return;
        }

        ImDrawList* const outer = ctx.dl;
        ctx.dl = ImGui::GetWindowDrawList();

        const auto title = ImGui::GetCursorScreenPos();
        ctx.dl->AddText (fonts.title, ctx.s (22), title, ctx.theme->textBright, "Settings");
        ImGui::Dummy ({ 1, ctx.s (30) });
        if (pendingTab >= 0) selectedTab = pendingTab;
        pendingTab = -1;
        const float available = ImGui::GetContentRegionAvail().x;
        float used = 0;
        for (std::size_t i = 0; i < kTabLabels.size(); ++i)
        {
            const float width = ImGui::CalcTextSize (kTabLabels[i]).x + ctx.s (28);
            if (used > 0 && used + ctx.s (8) + width <= available) ImGui::SameLine();
            else used = 0;
            const bool active = selectedTab == static_cast<int> (i);
            if (active)
            {
                const auto accent = consolePalette().focusRing;
                ImGui::PushStyleColor (ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4 (accent));
                ImGui::PushStyleColor (ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4 (dw::brighter (accent, 0.15f)));
                ImGui::PushStyleColor (ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4 (dw::darker (accent, 0.1f)));
                ImGui::PushStyleColor (ImGuiCol_Text, ImVec4 (0.07f, 0.07f, 0.08f, 1));
            }
            if (ImGui::Button (kTabLabels[i], { width, ctx.s (38) }))
            {
                selectedTab = static_cast<int> (i);
                if (host.tabChanged) host.tabChanged (selectedTab);
            }
            const auto first = ImGui::GetItemRectMin(), last = ImGui::GetItemRectMax();
            if (ImGui::IsItemVisible()) scenarioTabs[i] = { (first.x + last.x) * 0.5f, (first.y + last.y) * 0.5f };
            if (active) ImGui::PopStyleColor (4);
            used += width + (used > 0 ? ctx.s (8) : 0);
        }
        ImGui::Separator();
        ImGui::PushID (selectedTab);
        if (ImGui::BeginChild ("##settings-page", { 0, 0 }, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground))
        {
            formContext.dl = ImGui::GetWindowDrawList();
            drawSections (formContext, ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail().x);
        }
        ImGui::EndChild();
        ImGui::PopID();

        ctx.dl = outer;
        ImGui::EndChild();
        ImGui::PopStyleVar (3);

        popupOpen = ImGui::IsPopupOpen (nullptr, ImGuiPopupFlags_AnyPopupId
                                                 | ImGuiPopupFlags_AnyPopupLevel);
        keyboardActive = popupOpen || ImGui::IsAnyItemActive();
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

        if (selectedTab == 0)
        {
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
                const auto first = ImGui::GetItemRectMin();
                const auto last = ImGui::GetItemRectMax();
                scenarioRescan = { (first.x + last.x) * 0.5f, (first.y + last.y) * 0.5f };
                formTooltip ("Re-enumerate audio backends, devices and MIDI ports. Use after "
                             "plugging in or removing a USB / Thunderbolt audio interface. On "
                             "Linux, MIDI controllers are picked up on their own once the "
                             "transport is stopped.");
            }
        }

        if (selectedTab == 1)
        {
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
        }

        if (selectedTab == 2)
        {
            heading ("MIDI Bindings");
            {
                const float top = takeRow (kRowH);
                const float x = left + ctx.s (kControlInset);
                if (formButton (ctx, "##midi-bindings", ImVec2 (x, top + ctx.s (2.0f)),
                                ImVec2 (x + ctx.s (kLabelW) - ctx.s (kControlInset) * 2.0f,
                                        top + ctx.s (kRowH) - ctx.s (2.0f)),
                                "MIDI Bindings..."))
                    deferred ([this] { if (host.openMidiBindings) host.openMidiBindings(); });
                const auto first = ImGui::GetItemRectMin();
                const auto last = ImGui::GetItemRectMax();
                scenarioMidiBindings = { (first.x + last.x) * 0.5f, (first.y + last.y) * 0.5f };
                formTooltip ("Open the MIDI Bindings panel: list everything currently mapped, "
                             "remove individual bindings, or clear all. Use right-click on any "
                             "fader / knob / button to add new bindings.");
            }
        }

        if (selectedTab == 3)
        {
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
        }

        if (selectedTab == 4)
        {
            heading ("General");
            const float generalToggleX = left + ctx.s (kLabelW + kControlInset);
            {
                const float top = takeRow (kRowH);
                if (toggleAt (top, generalToggleX, "##tape-strip",
                              "Expand tape strip by default", tapeStripExpanded))
                    appconfig::setTapeStripExpandedDefault (tapeStripExpanded);
                const auto first = ImGui::GetItemRectMin();
                const auto last = ImGui::GetItemRectMax();
                scenarioTape = ImGui::IsItemVisible()
                    ? ImVec2 { (first.x + last.x) * 0.5f, (first.y + last.y) * 0.5f } : ImVec2 {};
                formTooltip ("When on, the TIMELINE tape strip starts expanded on every app "
                             "launch. Saved per-machine; takes effect on next launch.");
            }
            {
                const float top = takeRow (kRowH);
                if (toggleAt (top, generalToggleX, "##follow-playhead",
                              "Follow playhead by default", followPlayhead))
                    appconfig::setFollowPlayheadDefault (followPlayhead);
                const auto first = ImGui::GetItemRectMin();
                const auto last = ImGui::GetItemRectMax();
                scenarioFollow = ImGui::IsItemVisible()
                    ? ImVec2 { (first.x + last.x) * 0.5f, (first.y + last.y) * 0.5f } : ImVec2 {};
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
                if (staticComboAt (top, "##stop-behavior", kStopBehaviorItems, kStopBehaviorCount,
                                   stopBehavior, kComboW))
                {
                    const auto value = static_cast<appconfig::StopBehavior> (stopBehavior);
                    appconfig::setStopBehavior (value);
                    session.stopBehavior.store (stopBehavior, std::memory_order_relaxed);
                }
                formTooltip ("What the playhead does on Stop. \"Return to where play or record "
                             "started\" (the default) puts it back where playback or the take "
                             "began, ready to hear the take. \"Stay\" leaves it where it "
                             "stopped. \"Return to start\" rewinds every time. \"Last clicked\" "
                             "jumps to the most recent ruler click. Stop while already stopped "
                             "always returns to the start.");
            }
            {
                const float top = takeRow (kRowH);
                labelled (top, "Autosave every");
                if (staticComboAt (top, "##autosave", kAutosaveItems, 5, autosave, 200.0f))
                    appconfig::setAutosaveIntervalSeconds (kAutosaveSeconds[autosave]);
                const auto first = ImGui::GetItemRectMin();
                const auto last = ImGui::GetItemRectMax();
                scenarioAutosave = ImGui::IsItemVisible()
                    ? ImVec2 { (first.x + last.x) * 0.5f, (first.y + last.y) * 0.5f } : ImVec2 {};
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
                scenarioScaleFirst = ImGui::IsItemVisible() ? ImGui::GetItemRectMin() : ImVec2 {};
                scenarioScaleLast = ImGui::IsItemVisible() ? ImGui::GetItemRectMax() : ImVec2 {};
                formTooltip ("Multiplier applied on top of the OS-reported display DPI. 1.00x "
                             "= follow the OS. Range 0.50x to 2.00x.");
                applyUiScale (moved);
                drawHint (ctx, ImVec2 (x + ctx.s (kSliderW), top),
                          right - (x + ctx.s (kSliderW)),
                          "Previews live; saved per-machine when released.");
            }
        }

        if (selectedTab == 5)
        {
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
                bool commit = false;
                const auto remember = [] (ImVec2& point)
                {
                    const auto first = ImGui::GetItemRectMin(), last = ImGui::GetItemRectMax();
                    point = ImGui::IsItemVisible() ? ImVec2 { (first.x + last.x) * 0.5f, (first.y + last.y) * 0.5f } : ImVec2 {};
                };
                if (formButton (ctx, "##record-offset-minus", { x, top + ctx.s (2) },
                                { x + ctx.s (28), top + ctx.s (kRowH - 2) }, "-"))
                {
                    recordOffset = std::clamp (recordOffset, appconfig::kRecordingLatencyOffsetMin,
                                              appconfig::kRecordingLatencyOffsetMax);
                    if (recordOffset > appconfig::kRecordingLatencyOffsetMin) --recordOffset;
                    commit = true;
                }
                remember (scenarioOffsetMinus);
                ImGui::SetCursorScreenPos ({ x + ctx.s (34), top + ctx.s (2) });
                ImGui::SetNextItemWidth (ctx.s (kSliderW - 76));
                ImGui::PushFont (ctx.fonts->textEntry != nullptr ? ctx.fonts->textEntry : ImGui::GetFont());
                ImGui::PushStyleVar (ImGuiStyleVar_FramePadding,
                    { ctx.s (4), std::max (0.0f, (ctx.s (kRowH - 4) - ImGui::GetFontSize()) * 0.5f) });
                ImGui::InputInt ("##record-offset", &recordOffset, 0, 0);
                commit = ImGui::IsItemDeactivatedAfterEdit()
                    || (ImGui::IsItemActive() && (ImGui::IsKeyPressed (ImGuiKey_Enter)
                                                || ImGui::IsKeyPressed (ImGuiKey_KeypadEnter))) || commit;
                remember (scenarioOffset);
                ImGui::PopStyleVar();
                ImGui::PopFont();
                formTooltip ("Samples subtracted from each recorded audio take's timeline "
                             "start, to compensate for input round-trip latency (converters, "
                             "external gear, unreported plugin delay). Calibrate by recording a "
                             "loopback of the metronome click and entering the measured error. "
                             "Saved per-machine; applies to the next take.");
                if (formButton (ctx, "##record-offset-plus", { x + ctx.s (kSliderW - 36), top + ctx.s (2) },
                                { x + ctx.s (kSliderW - 8), top + ctx.s (kRowH - 2) }, "+"))
                {
                    recordOffset = std::clamp (recordOffset, appconfig::kRecordingLatencyOffsetMin,
                                              appconfig::kRecordingLatencyOffsetMax);
                    if (recordOffset < appconfig::kRecordingLatencyOffsetMax) ++recordOffset;
                    commit = true;
                }
                remember (scenarioOffsetPlus);
                if (commit)
                {
                    recordOffset = std::clamp (recordOffset, appconfig::kRecordingLatencyOffsetMin,
                                              appconfig::kRecordingLatencyOffsetMax);
                    appconfig::setRecordingLatencyOffsetSamples (recordOffset);
                    engine.setRecordingLatencyOffsetSamples (recordOffset);
                }
                drawHint (ctx, ImVec2 (x + ctx.s (kSliderW), top),
                          right - (x + ctx.s (kSliderW)),
                          "Samples. Saved per-machine; applies to the next take.");
            }
        }

        if (selectedTab == 6)
        {
            drawSfzLibrary (ctx, { left, y });
            return;
        }
        if (selectedTab == 7)
        {
            drawPluginFolders (ctx, { left, y });
            return;
        }
        ImGui::SetCursorScreenPos ({ content.x, y });
        ImGui::Dummy ({ bodyW, 1.0f });
    }

    void drawPluginFolders (dw::Context& ctx, ImVec2 at)
    {
        ImGui::PushStyleVar (ImGuiStyleVar_ItemSpacing, { ctx.s (8), ctx.s (12) });
        const auto remember = [] (ImVec2& point)
        {
            const auto first = ImGui::GetItemRectMin(), last = ImGui::GetItemRectMax();
            if (ImGui::IsItemVisible()) point = { (first.x + last.x) * 0.5f, (first.y + last.y) * 0.5f };
        };
        ImGui::SetCursorScreenPos (at);
        ImGui::PushTextWrapPos (0.0f);
        ImGui::TextUnformatted ("Plug-in search folders");
        ImGui::TextWrapped ("Standard plug-in locations are always scanned. Add folders here for CLAP, VST3 and LV2 plug-ins stored elsewhere; subfolders are included.");
       #if defined(__APPLE__)
        ImGui::TextWrapped ("Audio Units are discovered through macOS registration, independently of this list.");
       #endif
        ImGui::Spacing();
        if (ImGui::Button ("Add folder...", { ctx.s (150), ctx.s (36) }))
            deferred ([this] { if (host.choosePluginFolder) host.choosePluginFolder(); });
        remember (scenarioPluginAdd);
        ImGui::SameLine();
        if (ImGui::Button ("Scan plugins", { ctx.s (150), ctx.s (36) }))
            deferred ([this] { if (host.scanPlugins) host.scanPlugins(); });
        remember (scenarioPluginScan);
        ImGui::TextWrapped ("Changes apply to the next scan. Removing a folder does not delete or unload its plug-ins.");
        ImGui::TextWrapped ("Restart Dusk Studio after removing an LV2 folder to clear its loaded discovery metadata. Reconnect unavailable locations before scanning.");
        ImGui::Separator();
        const auto& roots = pluginFolders;
        if (roots.empty()) ImGui::TextDisabled ("No additional folders. Standard locations are enabled.");
        for (std::size_t i = 0; i < roots.size(); ++i)
        {
            ImGui::PushID (static_cast<int> (i));
            if (ImGui::BeginTable ("##plugin-folder", 2, ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn ("Folder", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn ("Action", ImGuiTableColumnFlags_WidthFixed, ctx.s (100));
                ImGui::TableNextColumn();
                ImGui::TextWrapped ("%s", roots[i].u8string().c_str());
                ImGui::TableNextColumn();
                if (ImGui::Button ("Remove", { ctx.s (96), ctx.s (34) }))
                {
                    auto remaining = roots;
                    remaining.erase (remaining.begin() + static_cast<std::ptrdiff_t> (i));
                    std::string message;
                    if (! pluginpaths::save (remaining, message))
                        deferred ([this, message] { if (host.alert) host.alert ("Plug-in folders", message); });
                    else deferred ([this, remaining] { pluginFolders = remaining; });
                }
                if (i == 0) remember (scenarioPluginRemove);
                ImGui::EndTable();
            }
            ImGui::PopID();
        }
        ImGui::PopTextWrapPos();
        ImGui::PopStyleVar();
    }

    void drawSfzLibrary (dw::Context& ctx, ImVec2 at)
    {
        ImGui::PushStyleVar (ImGuiStyleVar_ItemSpacing, { ctx.s (8), ctx.s (8) });
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextLibraryPoll)
        {
            librarySnapshot = engine.getSfzLibrary().snapshot();
            nextLibraryPoll = now + std::chrono::milliseconds (100);
        }
        ImGui::SetCursorScreenPos (at);
        ImGui::PushFont (ctx.fonts->textEntry != nullptr ? ctx.fonts->textEntry : ImGui::GetFont());
        ImGui::TextUnformatted ("SFZ download location");
        ImGui::Spacing();
        const auto path = librarySnapshot.locations.active.root.u8string();
        ImGui::PushTextWrapPos (0.0f);
        ImGui::TextWrapped ("%s", path.empty() ? "No library folder is configured." : path.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped ("Choose a folder on this computer or an external drive for downloaded SFZ packs.");
        ImGui::BeginDisabled (librarySnapshot.busy || librarySnapshot.state == sfz::LibraryState::Paused);
        ImGui::PushStyleVar (ImGuiStyleVar_FramePadding, { ctx.s (3), ctx.s (3) });
        ImGui::Checkbox ("Move existing installed packs", &moveLibraryPacks);
        ImGui::PopStyleVar();
        const auto remember = [] (ImVec2& point)
        {
            const auto first = ImGui::GetItemRectMin(), last = ImGui::GetItemRectMax();
            point = ImGui::IsItemVisible() ? ImVec2 { (first.x + last.x) * 0.5f, (first.y + last.y) * 0.5f } : ImVec2 {};
        };
        remember (scenarioSfzMove);
        ImGui::TextWrapped (moveLibraryPacks
            ? "Packs are copied and verified before originals are removed. Unload managed SFZ instruments before moving them."
            : "Only future downloads use the new folder. Existing packs stay available from their current folders.");
        if (ImGui::Button ("Change folder...", { ctx.s (180), ctx.s (34) }))
            deferred ([this] { if (host.chooseSfzFolder) host.chooseSfzFolder (moveLibraryPacks); });
        remember (scenarioSfzFolder);
        ImGui::EndDisabled();
        ImGui::Spacing();
        if (librarySnapshot.busy || librarySnapshot.state == sfz::LibraryState::Paused)
        {
            ImGui::TextUnformatted (librarySnapshot.state == sfz::LibraryState::Paused
                ? "Stop recording or rendering, then resume the library operation."
                : "Updating the SFZ library...");
            if (librarySnapshot.progress.completedBytes != 0)
                ImGui::Text ("%llu bytes processed", static_cast<unsigned long long> (librarySnapshot.progress.completedBytes));
            if (librarySnapshot.state == sfz::LibraryState::Paused && ImGui::Button ("Resume"))
                engine.getSfzLibrary().resume();
            if (librarySnapshot.state == sfz::LibraryState::Paused) ImGui::SameLine();
            if (ImGui::Button ("Cancel")) engine.getSfzLibrary().cancel();
        }
        if (! librarySnapshot.error.empty()) ImGui::TextWrapped ("%s", librarySnapshot.error.c_str());
        if (! librarySnapshot.locations.previous.empty())
        {
            ImGui::Separator();
            ImGui::TextUnformatted ("Previous library folders");
            for (const auto& location : librarySnapshot.locations.previous)
                ImGui::TextWrapped ("%s", location.root.u8string().c_str());
        }
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::PopStyleVar();
    }

    void drawHint (const dw::Context& ctx, ImVec2 at, float width, const char* text)
    {
        // The kit draws nothing on a null font; dereferencing FontSize here would crash
        // instead. Fall back to the frame's current font, which is never null.
        ImFont* font = ctx.fonts->label;
        if (font == nullptr)
            font = ImGui::GetFont();
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
    pluginpaths::Roots pluginFolders;
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
    int recordOffset = 0;
    int selectedTab = -1, pendingTab = 0;
    int uiScaleFrames = 0;
    bool popupOpen = false;
    bool popupWasOpen = false;
    bool keyboardActive = false, keyboardWasActive = false;
    bool moveLibraryPacks = false;
    sfz::LibrarySnapshot librarySnapshot;
    std::chrono::steady_clock::time_point nextLibraryPoll {};
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
