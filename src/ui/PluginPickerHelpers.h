#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>

namespace duskstudio
{
class PluginManager;
class PluginSlot;

// Shared plugin-picker UX used by per-channel insert slots and per-aux send-FX
// slots. The picker, scan, and file-chooser flows are identical between the
// two surfaces; the only differences are which PluginSlot they target and
// what to refresh on success. Keeping the implementation here avoids two
// slowly-diverging copies of the same menu-building / load-from-description
// code.
namespace pluginpicker
{
// Filter applied to the installed-plugin list before the picker is
// shown. Effect slots (channel strip in Mono/Stereo mode, aux send-FX
// slots) want Effects only - instrument plugins need MIDI input to
// produce sound and render as no-op or unstable when loaded as audio
// inserts. Instrument slots (channel strip in MIDI mode) want
// Instruments only.
enum class PluginKind { Effects, Instruments };

// Open a popup menu of installed plugins anchored on `target`. On selection:
//   • 1..N → resolves to KnownPluginList types and loadFromDescription.
//   • "Scan plugins" → runs the synchronous scan + reopens the picker.
//   • "Browse for file..." → launches a juce::FileChooser owned by
//     `chooserOwner` (kept alive across the async callback).
// `onChange` runs on every successful change to the slot.
//
// `kind` filters the visible list: Effects hides instruments,
// Instruments hides effects. The Browse-for-file path is unfiltered -
// if the user explicitly browses to a file we trust their choice.
//
// `screenPosition` overrides the menu anchor. Pass the cursor's screen
// position when `target` is a large click-target (full-slot placeholder)
// so the menu appears at the click rather than at the component's
// top-left. Default {-1,-1} means "use target component bounds".
// `onPickHardwareInsert`, when set, adds a top-of-menu "External
// Hardware Insert..." item. Selection invokes the callback - the caller
// is expected to flip the strip's insertMode to Hardware and open the
// HardwareInsertEditor modal. Empty/null callback hides the menu entry
// (preserves the existing menu shape for surfaces that don't support
// hardware inserts).
void openPickerMenu (PluginSlot& slot,
                      juce::Component& target,
                      std::unique_ptr<juce::FileChooser>& chooserOwner,
                      std::function<void()> onChange,
                      PluginKind kind,
                      juce::Point<int> screenPosition = { -1, -1 },
                      std::function<void()> onPickHardwareInsert = {});

// Synchronous scan. Blocks the message thread during scanInstalledPlugins().
// Shows a Dusk in-window completion alert in `parent` (top-level Component)
// when done. Pass nullptr for `parent` to fall back to a JUCE AlertWindow
// (legacy path; new callsites should always provide a parent).
// `onAlertDismiss` fires after the user dismisses the completion alert;
// use it to chain subsequent UI work (e.g. reopening the picker) so the
// follow-up doesn't stack on top of the alert and hide it.
void runScanModal (PluginManager& manager, juce::Component* parent = nullptr,
                    std::function<void()> onAlertDismiss = {});

// Run a juce::FileChooser to pick a plugin file (.vst3 / .so / .lv2),
// loading on selection. `chooserOwner` keeps the chooser alive across the
// async callback. `onChange` runs on successful load. `expectedKind`
// gates the load: if the picked plugin's effect/instrument flag doesn't
// match (e.g. user browses to a synth on an audio track expecting an
// effect), the slot is unloaded and an alert is shown.
//
// `parentForLifetime` is a SafePointer to the UI component that owns
// `chooserOwner`. The async callback captures it and bails if the
// parent is destroyed while the dialog is still open (user switches
// stages, quits, etc.) - prevents a UAF deref on the now-dead unique_ptr.
void openFileChooser (PluginSlot& slot,
                       std::unique_ptr<juce::FileChooser>& chooserOwner,
                       std::function<void()> onChange,
                       juce::Component::SafePointer<juce::Component> parentForLifetime,
                       PluginKind expectedKind = PluginKind::Effects);
}
} // namespace duskstudio
