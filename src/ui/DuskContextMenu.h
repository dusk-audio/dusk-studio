#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace duskstudio
{
// Dusk-native context-menu show path. Callers keep building their menus
// with juce::PopupMenu (full addItem / addSeparator / addSectionHeader /
// addSubMenu API), then call showContextMenu instead of
// PopupMenu::showMenuAsync. Items render inside an EmbeddedModal-hosted
// panel — no OS-native popup, no XWayland flash/dismiss, no Wayland
// parent-stacking weirdness.
//
// The menu walk uses PopupMenu::MenuItemIterator so submenus + section
// headers + separators all carry over without per-call-site rewrites.
//
// Behaviour:
//   • Anchored under `target` if it fits; flipped above if it would
//     clip the host's bottom edge. `screenPos` (if provided, non-{-1,-1})
//     overrides the anchor — used by right-clicks where the cursor's
//     screen position is the natural anchor.
//   • Per-item callback (set via addItem with std::function) fires when
//     the user picks that item. `onResult` (if set) fires AFTER any
//     per-item callback with the chosen item ID, matching the legacy
//     PopupMenu::showMenuAsync contract.
//   • Clicking outside the panel / pressing Esc dismisses with
//     `onResult (0)` (same sentinel JUCE uses for cancellation).
//
// Note: per-item std::function callbacks set on the source PopupMenu
// ARE invoked. JUCE PopupMenu stores them on the item; we capture them
// via MenuItemIterator and fire them on pick.
void showContextMenu (const juce::PopupMenu& menu,
                       juce::Component& target,
                       std::function<void (int)> onResult = {},
                       juce::Point<int> screenPos = { -1, -1 });

// Convenience overload — anchors at the screen position only, no
// target component. Used by call sites that already capture the cursor
// position (e.g. right-clicks routed through MouseEvent::getScreenPosition).
void showContextMenu (const juce::PopupMenu& menu,
                       juce::Component& hostParent,
                       juce::Point<int> screenPos,
                       std::function<void (int)> onResult);
} // namespace duskstudio
