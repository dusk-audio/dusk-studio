#pragma once

namespace duskstudio::imgui
{
// The keys a native panel keeps live for the DAW behind it. The shell binds them; a
// panel has no way to name a JUCE key press, so it reports which of these the user
// asked for and the shell turns that back into its own shortcut. The set is the one
// any dialog lets through: transport, loop and punch, playhead home, fullscreen, and
// the session's Undo, Redo, Save, Save As and Quit. Nothing that edits a region or a
// marker the panel hides, which is why Delete and the clipboard keys are absent.
//
// Its own header so the JUCE side can name a shortcut without pulling in Dear ImGui.
enum class ShellShortcut
{
    playStop,
    record,
    playheadToZero,
    stopAndRewind,
    toggleLoop,
    togglePunch,
    setLoopIn,
    setLoopOut,
    setPunchIn,
    setPunchOut,
    toggleFullscreen,
    undo,
    redo,
    save,
    saveAs,
    quit,

    // Sentinel: the shell's binding table is checked against this, so adding a
    // shortcut without the key press it maps to fails the build rather than
    // reaching the panel and doing nothing.
    count
};

// What the shell's own shortcuts may do while a native panel is open. A modal panel
// is a dialog like any other; the startup dialog decides which session there is to
// act on, so it takes none; an inline editor, or a panel that rules its keys itself,
// leaves them all. Ordered by how much they hold back.
enum class PanelShellKeys
{
    all,
    dialog,
    none
};
} // namespace duskstudio::imgui
