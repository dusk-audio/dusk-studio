#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../session/Session.h"

namespace duskstudio
{
juce::MouseCursor cursorForEditMode (EditMode m);

// JUCE's default-constructed MouseCursor compares as NormalCursor (NOT
// ParentCursor), so a parent's setMouseCursor is silently shadowed by
// every label / static child the mouse hovers. Walk the tree and flip
// any descendant whose cursor is still the default NormalCursor over
// to ParentCursor so the parent's stored cursor is what actually
// renders. Children that explicitly chose a cursor (resize handles,
// fader caps, learn buttons, etc.) are left alone.
void inheritCursorOnDescendants (juce::Component& root);
} // namespace duskstudio
