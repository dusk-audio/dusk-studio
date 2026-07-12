#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>

namespace duskstudio
{
// Shared Dusk-styled in-window modal helpers. All four route through
// EmbeddedModal so the app never spawns a top-level popup peer that
// fights for focus on XWayland and stacks badly on small displays.
//
// Three modal interaction modes:
//   - Plugin-style (default for showDuskAlert): click-outside / Esc
//     dismiss as well as the OK button. Use for non-destructive info.
//   - Soft-confirm (showDuskConfirm with dismissOnOutside=true): two
//     buttons; click-outside fires the secondary (Cancel). Use for
//     "would you like to retime?" style prompts.
//   - Decision (showDuskConfirm with dismissOnOutside=false OR
//     showDuskDecision): focus-locked, no Esc / click-outside. Use for
//     destructive deletes, save-before-quit, mandatory choices.
//
// `parent` should be the top-level component (typically MainComponent)
// so the modal covers the whole window. Each helper owns a separate
// static EmbeddedModal so multiple modal kinds can stack.

void showDuskAlert (juce::Component& parent,
                     juce::String title,
                     juce::String message,
                     std::function<void()> onDismiss = {});

// Two-button confirm. primaryLabel = action (e.g. "Save", "Delete"),
// secondaryLabel = cancel-equivalent. If `destructive` is true the
// primary button paints red AND click-outside / Esc are disabled - the
// user must hit an explicit button.
void showDuskConfirm (juce::Component& parent,
                       juce::String title,
                       juce::String message,
                       juce::String primaryLabel,
                       std::function<void()> onPrimary,
                       juce::String secondaryLabel,
                       std::function<void()> onSecondary,
                       bool destructive = false);

// N-button decision panel, always focus-locked. Caller supplies a list
// of { label, onClick, destructive } actions; user MUST click one
// (Esc + click-outside swallowed). Buttons laid out right-to-left in
// the order given, so the rightmost button in the vector is rightmost
// on screen.
struct DuskDecisionAction
{
    juce::String          label;
    std::function<void()> onClick;
    bool                  destructive = false;
};

void showDuskDecision (juce::Component& parent,
                        juce::String title,
                        juce::String message,
                        std::vector<DuskDecisionAction> actions);

// Single text-input modal: prompt label + text editor + OK/Cancel.
// Replaces juce::AlertWindow::addTextEditor for rename / numeric
// prompts. onAccept fires with the trimmed text on OK; nothing fires
// on Cancel / Esc / click-outside.
void showDuskTextInput (juce::Component& parent,
                         juce::String title,
                         juce::String prompt,
                         juce::String initial,
                         std::function<void (const juce::String&)> onAccept);
} // namespace duskstudio
