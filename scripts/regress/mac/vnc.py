"""Drive the macOS node's console session over Screen Sharing (VNC).

ssh cannot see or touch the logged-in desktop, and the package leg needs it
for three things: unlocking a locked screen, answering the privacy prompts the
installed app raises, and keeping a picture of what the screen showed.

    vnc.py --host macbook-air.local --user marc cap out.png
    vnc.py --host macbook-air.local --user marc click 1019 408 --expect allow \
        --pre-cap before.png [--cap out.png]
    vnc.py --host macbook-air.local --user marc unlock [--cap out.png]

The password is read from a file and never printed. It is the account's login
password as well, which is what unlock types.

A click given --expect captures the screen into --pre-cap first and clicks only
when that capture shows the expected button at the point; otherwise it exits 3
without clicking.
"""

import argparse
import os
import sys
import time

NOT_ON_SCREEN = 3


def luma(p):
    return (299 * p[0] + 587 * p[1] + 114 * p[2]) // 1000


def close(a, b, tolerance=12):
    return sum(abs(i - j) for i, j in zip(a, b)) <= tolerance


def button_at(img, x, y):
    """The prompt button whose label is centred on (x, y), as (left, right,
    label_left, label_right), or a string saying why there is none.

    Measured on the node's microphone prompt: buttons about 110 x 28 px, 9 px
    apart, the label about 11 px tall and centred on the point; "Allow" is 32
    px wide and "Don't Allow" 69. In the light appearance the fill is light and
    the label dark; the node switches to the dark appearance at night, where
    the fill is dark grey and the label light.
    """
    w, h = img.size
    if not (100 <= x < w - 100 and 40 <= y < h - 40):
        return f"({x},{y}) is too near the edge of the {w}x{h} screen"
    px = img.getpixel
    row = y - 8
    fill = px((x, row))
    light = luma(fill) >= 150
    if not light and not 25 <= luma(fill) <= 110:
        return f"({x},{row}) is {fill}, neither a light nor a dark button fill"
    # The dark prompt is translucent: what lies under it tints the fill by up
    # to about 20, while the fill still stands about 50 off the prompt around it.
    tolerance = 12 if light else 30
    left = x
    while left > x - 90 and close(px((left - 1, row)), fill, tolerance):
        left -= 1
    right = x
    while right < x + 90 and close(px((right + 1, row)), fill, tolerance):
        right += 1
    if not 80 <= right - left <= 160:
        return f"the fill at ({x},{row}) is {right - left + 1} px wide, not a button"
    # A column clear of the label, and of the pointer a previous click left on it.
    col = left + 10
    if not close(px((col, y + 8)), fill, tolerance):
        return f"({col},{y + 8}) is {px((col, y + 8))}, not the button fill below the label"
    top = row
    while top > row - 30 and close(px((col, top - 1)), fill, tolerance):
        top -= 1
    bottom = y + 8
    while bottom < y + 30 and close(px((col, bottom + 1)), fill, tolerance):
        bottom += 1
    if not 20 <= bottom - top <= 40:
        return f"the fill at column {col} is {bottom - top + 1} px tall, not a button"
    def on_label(p):
        return luma(p) < luma(fill) - 60 if light else luma(p) > luma(fill) + 60

    cols = [cx for cx in range(left + 4, right - 3)
            if any(on_label(px((cx, cy))) for cy in range(top + 4, bottom - 3))]
    if not cols:
        return f"the button at ({x},{y}) has no label"
    return left, right, cols[0], cols[-1]


def check_allow(img, x, y):
    """None when (x, y) is the Allow button of a two-button privacy prompt
    with Don't Allow on its left, otherwise why it is not."""
    button = button_at(img, x, y)
    if isinstance(button, str):
        return button
    left, right, label_left, label_right = button
    width = label_right - label_left + 1
    if not 22 <= width <= 44:
        return f"the label at ({x},{y}) is {width} px wide, not \"Allow\""
    if abs((label_left + label_right) - (left + right)) > 16:
        return f"the label at ({x},{y}) is off the centre of its button"
    partner = "nothing"
    for gap in range(4, 20):
        partner = button_at(img, left - gap - (right - left) // 2, y)
        if not isinstance(partner, str):
            break
    else:
        return f"no button left of ({x},{y}): {partner}"
    width = partner[3] - partner[2] + 1
    if not 55 <= width <= 85:
        return f"the button left of ({x},{y}) has a {width} px label, not \"Don't Allow\""
    return None


CHECKS = {"allow": check_allow}


def connect(host, user, login):
    from vncdotool import api

    with open(os.path.expanduser(login), encoding="utf-8") as f:
        password = f.read().strip()
    client = api.connect(f"{host}::5900", username=user, password=password, timeout=30)
    return api, client, password


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--user", required=True)
    parser.add_argument("--login", default=os.environ.get(
        "DUSK_REGRESS_MAC_VNC_LOGIN", "~/.config/dusk-mac-vnc/login"))
    sub = parser.add_subparsers(dest="cmd", required=True)
    cap = sub.add_parser("cap")
    cap.add_argument("out")
    click = sub.add_parser("click")
    click.add_argument("x", type=int)
    click.add_argument("y", type=int)
    click.add_argument("--cap")
    click.add_argument("--expect", choices=sorted(CHECKS))
    click.add_argument("--pre-cap")
    unlock = sub.add_parser("unlock")
    unlock.add_argument("--cap")
    args = parser.parse_args()
    if args.cmd == "click" and args.expect and not args.pre_cap:
        parser.error("click --expect needs --pre-cap")

    api, client, password = connect(args.host, args.user, args.login)
    try:
        if args.cmd == "click":
            if args.expect:
                from PIL import Image

                client.captureScreen(args.pre_cap)
                with Image.open(args.pre_cap) as img:
                    reason = CHECKS[args.expect](img.convert("RGB"), args.x, args.y)
                if reason:
                    print(f"vnc.py: not clicked: {reason}", file=sys.stderr)
                    return NOT_ON_SCREEN
            client.mouseMove(args.x, args.y)
            time.sleep(0.2)
            client.mousePress(1)
            time.sleep(2)
        elif args.cmd == "unlock":
            client.keyPress("shift")
            time.sleep(1.5)
            for ch in password:
                client.keyPress(ch)
                time.sleep(0.05)
            time.sleep(0.3)
            client.keyPress("enter")
            time.sleep(4)
        out = args.out if args.cmd == "cap" else args.cap
        if out:
            client.captureScreen(out)
            print(out)
    finally:
        client.disconnect()
        api.shutdown()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ImportError:
        sys.exit("vnc.py: vncdotool is not importable by this python "
                 "(python3 -m venv <dir> && <dir>/bin/pip install vncdotool, "
                 "then point DUSK_REGRESS_VNC_PYTHON at <dir>/bin/python)")
