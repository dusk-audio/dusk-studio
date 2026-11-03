"""Read a Screen Sharing capture of the macOS node for the window-activation leg.

    screen.py lights shot.png X Y W H
    screen.py panel shot.png CX CY WIDTH MIN_HEIGHT MAX_HEIGHT

lights looks in the title bar of the window at X Y W H (window-server points,
which are capture pixels on the node's 1x desktop) for its close, minimize
and zoom buttons drawn in colour. AppKit draws them in colour only on the
window of the active application that takes the keyboard, and grey on every
other, so this is the key-window evidence a capture can give without the
Accessibility access that reading it directly needs. It prints the three
button centres and exits 0, or says what it found and exits 1.

panel looks for one of the app's in-window modal panels (the file browser, an
alert, the save prompt) centred on CX CY: a solid dark rectangle WIDTH wide
whose height lies between the two bounds. The panels paint one flat colour
behind their text, inset at least 14 px, so a column just inside the left
edge and a row just under the top edge of a panel that is visible on top are
that colour from end to end. It prints the rectangle and exits 0, or says
what it found instead and exits 1.
"""

import argparse
import sys

from PIL import Image

INSET = 8
TOLERANCE = 4


def near(a, b, tol=TOLERANCE):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def light_kind(p):
    r, g, b = p
    hi, lo = max(p), min(p)
    if hi < 110 or hi - lo < 70:
        return None
    if hi == r and g <= 0.5 * r and b <= 0.5 * r:
        return "red"
    if hi == r and g >= 0.55 * r and b <= 0.3 * r:
        return "yellow"
    if hi == g and r <= 0.5 * g and b <= 0.5 * g:
        return "green"
    return None


def lights(img, x, y, w, h):
    """Centres of the three title-bar buttons in colour, or why not.

    The pointer resting on a button draws its glyph in the middle of it, so
    the buttons are found by hue over their whole area, not by one pixel.
    """
    sw, sh = img.size
    x0, y0 = max(0, x), max(0, y)
    x1, y1 = min(sw, x + min(w, 150)), min(sh, y + min(h, 40))
    found = {"red": [], "yellow": [], "green": []}
    for py in range(y0, y1):
        for px in range(x0, x1):
            kind = light_kind(img.getpixel((px, py)))
            if kind:
                found[kind].append((px, py))
    centres = {}
    for kind, pts in found.items():
        if len(pts) < 30:
            return None, (f"no {kind} button in colour in ({x0},{y0})-({x1},{y1}): "
                          f"{', '.join(f'{k} {len(v)} px' for k, v in found.items())}")
        centres[kind] = (sum(p[0] for p in pts) // len(pts), sum(p[1] for p in pts) // len(pts))
    r, ye, g = centres["red"], centres["yellow"], centres["green"]
    if not (r[0] < ye[0] < g[0]) or max(r[1], ye[1], g[1]) - min(r[1], ye[1], g[1]) > 3:
        return None, f"coloured pixels in the title bar are not three buttons in a row: {centres}"
    return centres, None


def panel(img, cx, cy, width, hmin, hmax):
    """The (left, top, width, height) of the panel centred on cx, cy, or why not."""
    sw, sh = img.size
    left = cx - width // 2
    x = left + INSET
    if not (0 <= x < sw and 0 <= cy < sh):
        return None, f"({x},{cy}) is off the {sw}x{sh} capture"
    ref = img.getpixel((x, cy))
    if max(ref) > 60:
        return None, f"({x},{cy}) is {ref}, not a dark panel"
    top = cy
    while top > 0 and near(img.getpixel((x, top - 1)), ref):
        top -= 1
    bottom = cy
    while bottom < sh - 1 and near(img.getpixel((x, bottom + 1)), ref):
        bottom += 1
    height = bottom - top + 1
    if not hmin - 4 <= height <= hmax + 4:
        return None, f"the {ref} run at x={x} is {height} px tall ({top}-{bottom}), not {hmin}-{hmax}"
    if abs((top + bottom) / 2 - cy) > 4:
        return None, f"the {ref} run at x={x} ({top}-{bottom}) is not centred on y={cy}"
    row = top + INSET
    span = range(left + INSET, left + width - INSET)
    hits = sum(1 for px in span if near(img.getpixel((px, row)), ref))
    if hits < 0.95 * len(span):
        return None, f"row y={row} is {ref} for only {hits} of {len(span)} px"
    return (left, top, width, height), None


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="cmd", required=True)
    lp = sub.add_parser("lights")
    lp.add_argument("shot")
    for name in ("x", "y", "w", "h"):
        lp.add_argument(name, type=int)
    pp = sub.add_parser("panel")
    pp.add_argument("shot")
    for name in ("cx", "cy", "width", "hmin", "hmax"):
        pp.add_argument(name, type=int)
    args = parser.parse_args()

    with Image.open(args.shot) as raw:
        img = raw.convert("RGB")
    if args.cmd == "lights":
        centres, reason = lights(img, args.x, args.y, args.w, args.h)
        if reason:
            print(f"grey: {reason}")
            return 1
        print("colour: " + " ".join(f"{k}={v[0]},{v[1]}" for k, v in centres.items()))
        return 0
    rect, reason = panel(img, args.cx, args.cy, args.width, args.hmin, args.hmax)
    if reason:
        print(f"absent: {reason}")
        return 1
    print("present: {}x{} at {},{}".format(rect[2], rect[3], rect[0], rect[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
