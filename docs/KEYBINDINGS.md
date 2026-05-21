# Dusk Studio — Keyboard Shortcuts

All shortcuts route through `MainComponent::keyPressed`. Modifier
`Cmd` is `Ctrl` on Linux/Windows, `Command` on macOS.

## Transport

| Key | Action |
| --- | --- |
| `Space` | Play / Stop |
| `R` | Record (toggle) |
| `.` | Stop + rewind playhead to 0 |
| `Home` | Move playhead to 0 (no stop) |
| `L` | Loop on/off |
| `P` | Punch on/off |
| `[` | Set loop start at playhead |
| `]` | Set loop end at playhead |
| `Shift+[` | Set punch in at playhead |
| `Shift+]` | Set punch out at playhead |
| `C` | Metronome click on/off |
| `M` | Drop marker at playhead |

## Track (selected, via tape strip)

| Key | Action |
| --- | --- |
| `A` | Arm track for record |
| `S` | Solo track |
| `X` | Mute track |
| `K` | Toggle virtual MIDI keyboard |

## Edit

| Key | Action |
| --- | --- |
| `Cmd+Z` | Undo |
| `Cmd+Shift+Z` / `Cmd+Y` | Redo |
| `Cmd+C` / `Cmd+X` / `Cmd+V` | Copy / cut / paste region |
| `Cmd+D` | Duplicate region |
| `Delete` / `Backspace` | Delete selected region |
| `T` | Split region at playhead (razor) |
| `Alt+T` | Cycle to next take (history) |
| `Alt+Shift+T` | Cycle to previous take |
| `Cmd+←` / `Cmd+→` | Nudge selected region by one beat |
| `Cmd+Shift+←` / `Cmd+Shift+→` | Nudge by one bar |
| `G` | Edit mode = Grab (default click-drag) |

## File

| Key | Action |
| --- | --- |
| `Cmd+S` | Save session |
| `Cmd+Shift+S` | Save as |
| `Cmd+O` | Open session |
| `Cmd+B` | Bounce to file |

## View

| Key | Action |
| --- | --- |
| `=` / `+` | Zoom in tape strip |
| `-` | Zoom out tape strip |
| `0` | Zoom fit |
| `F11` | Toggle fullscreen |

## Piano Roll (modal — when open)

The piano roll captures its own keypresses first. See `PianoRollComponent::keyPressed`. Highlights: `Q` opens quantize menu, `V` opens velocity menu, `Cmd+A` select-all, `Cmd+D` duplicate, `Esc` close.

## Notes

- Shortcuts that conflict with a focused text field defer to the text
  field. Edit a label or BPM spinner safely without accidental
  transport actions.
- `M` drops a marker; per-track mute is `X` to avoid the clash.
- `B` alone is not used; `Cmd+B` triggers Bounce (Logic convention).
