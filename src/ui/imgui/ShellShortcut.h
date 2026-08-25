#pragma once

namespace duskstudio::imgui
{
// The transport and navigation keys a native panel keeps live for the DAW behind it.
// The shell binds them; a panel has no way to name a JUCE key press, so it reports
// which of these the user asked for and the shell turns that back into its own
// shortcut. The set is the one EmbeddedModal forwards: transport, loop and punch,
// playhead home, fullscreen. Nothing that edits the arrangement hidden behind the
// panel, which is why Delete and the clipboard keys are absent.
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
    toggleFullscreen
};
} // namespace duskstudio::imgui
