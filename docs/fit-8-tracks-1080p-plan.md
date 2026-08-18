# Eight-track 1080p console plan

## Goal

At a maximized 1920x1080 display at 100% UI scaling, show one complete
eight-track bank together with all four buses and the Master strip. Keep the
expanded channel EQ available on the strip, keep the buses and Master readable,
and preserve the existing narrower-window fallbacks.

## Current constraint

The maximized MainComponent gives ConsoleView about 1904 px after the existing
8 px outer inset on each side. The current documented minimums need 2206 px for
eight channels:

```text
8 channels at 154 px                       1232
4 buses at 172 px                           688
Master at 210 px                             210
inter-strip and section gaps                  64
ConsoleView outer padding                     12
                                             ----
                                             2206 px
```

The four buses and Master already sit at widths chosen to keep their long value
labels readable. Compressing those strips enough to recover the missing 302 px
would damage the part of the console with the least horizontal flexibility.

The channel-strip layout has a better breakpoint. At 116 px, the existing
geometry fits exactly beside the current bus and Master minimums:

```text
8 channels at 116 px                        928
4 buses at 172 px                           688
Master at 210 px                             210
inter-strip and section gaps                  64
ConsoleView outer padding                     12
                                             ----
                                             1902 px
```

That leaves 2 px of safety inside the expected 1904 px ConsoleView. It also
preserves the 12 px visual breaks before the buses and Master, so the console
does not become one undifferentiated row of narrow strips.

## Recommended design

Add an explicit horizontal `EightUp` density tier. It is separate from the
existing vertical compact/TIMELINE mode: EightUp keeps the EQ and compressor
directly operable, while compact mode continues to collapse them into pills
when vertical space is scarce.

At a ConsoleView width of 1902 px or greater:

- prefer at least eight visible channels;
- keep each bus at 172 px and Master at 210 px;
- allow channel strips to reach 116 px;
- show three screen pages: `1-8`, `9-16`, and `17-24`;
- keep buses and Master anchored at the right edge;
- retain the existing 4 px strip gaps and 12 px section gaps.

This also makes the screen pages match the eight-channel MCU hardware banks,
removing the current mismatch between visible pages and control-surface banks.

## Channel-strip adaptation at 116 px

The widened EQ should remain inline. Use width-aware layout changes only where
the current controls would become cramped:

1. Reduce the EQ band-name gutter from 28 px to 24 px in EightUp. The remaining
   width provides three 26 px GAIN/FREQ/Q cells, retaining the current knob size.
2. Reflow the four compressor controls from one 4-across row to a 2x2 grid in
   EightUp. This keeps labels and values readable instead of squeezing each
   control to about 25 px. The 1080p console has enough vertical room for the
   extra row.
3. Keep HPF/LPF, the four staggered AUX sends, I/O controls, pan, fader, meters,
   bus assignments, and M/S/phase controls visible. The existing narrow-fader
   path already prevents the fader cap and meter cluster from overlapping.
4. Do not shorten labels as the first response. If visual verification finds a
   specific value still clipped, adapt only that value formatter in EightUp.

## Implementation sequence

1. Add named EightUp constants and a density result to `ConsoleLayout.h`,
   including the 1902 px threshold and 116 px channel floor.
2. Make `channelsThatFitForWidth()` prefer eight channels once the EightUp
   threshold is available, while retaining the existing behavior below it and
   the wider-screen behavior above it.
3. Have `ConsoleView` pass the horizontal density to channel strips without
   changing the existing user/auto compact state for buses or Master.
4. Add the narrow EQ gutter and 2x2 compressor layout to
   `ChannelStripComponent`. Keep the bus and Master layouts unchanged.
5. Update layout and bank-mapping tests before visual tuning.

## Acceptance checks

- A maximized 1920x1080 window at 100% scaling shows tracks 1-8, all four buses,
  and Master simultaneously with no overlap or horizontal scrolling.
- Page buttons read `1-8`, `9-16`, and `17-24`; keyboard focus and MCU bank
  changes cross 8/9 and 16/17 correctly.
- EQ knobs and values remain visible on every channel; compressor labels and
  values do not clip in any compressor mode.
- Bus and Master labels are unchanged and remain readable.
- Existing 1366 px and minimum-window tests still pass, including the no-overlap
  guarantee before Bus 1.
- A screenshot comparison at 1920x1080 checks alignment, control clipping,
  fader/meter separation, and the visual distinction between channels, buses,
  and Master.
- Keyboard traversal, focus indication, and popup access are tested directly;
  screenshots alone cannot confirm those accessibility behaviors.

## Trade-off

EightUp is denser than the comfortable layout, so channel compressor controls
use two rows and the strips have less whitespace. That is preferable to shrinking
the buses/Master, collapsing the newly widened EQ, or hiding three tracks behind
an extra page. Wider windows can continue to use the existing comfortable
geometry.
