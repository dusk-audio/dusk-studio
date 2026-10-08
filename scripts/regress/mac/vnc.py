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


def label_runs(img, y, x0, x1):
    """Column runs (first, last) of label text on the line centred on y,
    between x0 and x1.

    The prompt is translucent and takes on whatever lies under it, a busy
    mixer as much as the desktop, so its buttons have no fill of their own to
    measure. The labels keep their contrast: a pixel is text when it stands
    well off the median of its own row around it. Letters of one label are a
    few pixels apart and the words of "Don't Allow" about five, so gaps of up
    to six columns stay inside a run.
    """
    px = img.getpixel
    rows = range(y - 6, y + 7)
    lum = {(cx, cy): luma(px((cx, cy))) for cy in rows for cx in range(x0 - 20, x1 + 21)}
    ink = []
    for cx in range(x0, x1 + 1):
        hit = False
        for cy in rows:
            window = sorted(lum[(wx, cy)] for wx in range(cx - 20, cx + 21, 2))
            if abs(lum[(cx, cy)] - window[len(window) // 2]) > 55:
                hit = True
                break
        ink.append(hit)
    runs, start, gap = [], None, 0
    for i, hit in enumerate(ink):
        if hit:
            if start is None:
                start = i
            gap = 0
        elif start is not None:
            gap += 1
            if gap > 6:
                runs.append((x0 + start, x0 + i - gap))
                start, gap = None, 0
    if start is not None:
        runs.append((x0 + start, x1 - gap))
    return runs


def check_allow(img, x, y):
    """None when (x, y) is the Allow button of a two-button privacy prompt
    with Don't Allow on its left, otherwise why it is not.

    Measured on the node's microphone prompt: the labels sit on one line,
    "Allow" about 32 px wide and centred on the point, "Don't Allow" about 69
    px wide with its centre about 118 px to the left, and nothing between.
    """
    w, h = img.size
    if not (220 <= x < w - 80 and 40 <= y < h - 40):
        return f"({x},{y}) is too near the edge of the {w}x{h} screen"
    runs = label_runs(img, y, x - 200, x + 60)
    allow = [r for r in runs if abs((r[0] + r[1]) / 2 - x) <= 10]
    if not allow:
        return f"no label centred on ({x},{y}); text runs {runs}"
    a = allow[0]
    if not 22 <= a[1] - a[0] + 1 <= 44:
        return f"the label at ({x},{y}) is {a[1] - a[0] + 1} px wide, not \"Allow\""
    left = [r for r in runs if r[1] < a[0] and 100 <= x - (r[0] + r[1]) / 2 <= 140]
    if not left or not 55 <= left[-1][1] - left[-1][0] + 1 <= 85:
        return f"no \"Don't Allow\" label left of ({x},{y}); text runs {runs}"
    if any(left[-1][1] < r[0] and r[1] < a[0] for r in runs):
        return f"text between the two labels at ({x},{y}); text runs {runs}"
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
            # A privacy prompt ignores a press that arrives with the pointer: it
            # takes the click only once the pointer has come onto the button and
            # rested there, and a press and release sent as one event does not
            # count. Approach from the side, wait, then press and let go.
            client.mouseMove(args.x - 30, args.y - 10)
            time.sleep(0.3)
            for step in range(1, 7):
                client.mouseMove(args.x - 30 + 5 * step, args.y - 10 + step * 10 // 6)
                time.sleep(0.05)
            client.mouseMove(args.x, args.y)
            time.sleep(1.0)
            client.mouseDown(1)
            time.sleep(0.15)
            client.mouseUp(1)
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
