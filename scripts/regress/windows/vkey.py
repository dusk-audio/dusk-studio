"""Type text into a libvirt guest with virsh send-key.

The win11 regression guest has no qemu-guest-agent and no SSH, so keystrokes
are the only input channel. Modifiers are extra keys in the same send-key call.

    vkey.py --domain win11 'iwr -useb http://192.168.122.1:8000/x.ps1|iex'
    vkey.py --domain win11 --raw KEY_LEFTMETA
"""

import argparse
import subprocess
import sys
import time

SHIFTED = {
    '!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7',
    '*': '8', '(': '9', ')': '0', '_': 'MINUS', '+': 'EQUAL',
    '{': 'LEFTBRACE', '}': 'RIGHTBRACE', '|': 'BACKSLASH', ':': 'SEMICOLON',
    '"': 'APOSTROPHE', '<': 'COMMA', '>': 'DOT', '?': 'SLASH', '~': 'GRAVE',
}
PLAIN = {
    ' ': 'SPACE', '-': 'MINUS', '=': 'EQUAL', '[': 'LEFTBRACE',
    ']': 'RIGHTBRACE', '\\': 'BACKSLASH', ';': 'SEMICOLON',
    "'": 'APOSTROPHE', ',': 'COMMA', '.': 'DOT', '/': 'SLASH', '`': 'GRAVE',
    '\n': 'ENTER', '\t': 'TAB',
}


def keys_for(ch):
    if ch.isascii() and ch.isalpha():
        return (['KEY_LEFTSHIFT'] if ch.isupper() else []) + ['KEY_' + ch.upper()]
    if ch.isascii() and ch.isdigit():
        return ['KEY_' + ch]
    if ch in SHIFTED:
        return ['KEY_LEFTSHIFT', 'KEY_' + SHIFTED[ch]]
    if ch in PLAIN:
        return ['KEY_' + PLAIN[ch]]
    raise SystemExit('vkey: no keycode for {!r}'.format(ch))


def send(domain, connect, keys, holdtime):
    result = subprocess.run(
        ['virsh', '-c', connect, 'send-key', domain, '--holdtime', str(holdtime)] + keys,
        capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit('vkey: send-key {} failed: {}'.format(
            ' '.join(keys), result.stderr.strip()))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--domain', default='win11')
    ap.add_argument('--connect', default='qemu:///system')
    ap.add_argument('--holdtime', type=int, default=40)
    ap.add_argument('--delay', type=float, default=0.06,
                    help='seconds between characters')
    ap.add_argument('--raw', action='store_true',
                    help='arguments are literal KEY_* names, sent in one call')
    ap.add_argument('text', nargs='+')
    args = ap.parse_args()

    if args.raw:
        send(args.domain, args.connect, args.text, args.holdtime)
        return 0
    for ch in ' '.join(args.text):
        send(args.domain, args.connect, keys_for(ch), args.holdtime)
        time.sleep(args.delay)
    return 0


if __name__ == '__main__':
    sys.exit(main())
