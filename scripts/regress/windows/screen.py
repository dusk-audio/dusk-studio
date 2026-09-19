"""Capture the guest framebuffer and report how bright it is.

virsh returns a black frame once the guest display has blanked, which is
indistinguishable from a hung guest unless the mean level is checked. The
runner uses the value to decide whether a wake key is needed, and keeps the
PNG so a human can see what the guest was actually showing.

    screen.py --domain win11 --out /tmp/run/shot-01.png
"""

import argparse
import subprocess
import sys
import tempfile
import os


def capture_ppm(domain, connect, path):
    proc = subprocess.run(['virsh', '-c', connect, 'screenshot', domain, path],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        print(proc.stderr, file=sys.stderr, end='')
        raise RuntimeError(f'virsh screenshot for domain {domain!r} failed '
                           f'with exit status {proc.returncode}')


def mean_level(path):
    from PIL import Image

    with Image.open(path) as img:
        grey = img.convert('L')
        histogram = grey.histogram()
        total = sum(histogram)
        if total == 0:
            return 0.0
        return sum(i * n for i, n in enumerate(histogram)) / total


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--domain', default='win11')
    ap.add_argument('--connect', default='qemu:///system')
    ap.add_argument('--out', required=True, help='PNG to write')
    args = ap.parse_args()

    fd, ppm = tempfile.mkstemp(suffix='.ppm')
    os.close(fd)
    try:
        capture_ppm(args.domain, args.connect, ppm)
        try:
            from PIL import Image

            with Image.open(ppm) as img:
                img.save(args.out)
            level = mean_level(args.out)
        except ImportError:
            os.replace(ppm, args.out + '.ppm')
            print('mean=unknown file={}.ppm (install python3-pillow for PNG)'
                  .format(args.out))
            return 0
    finally:
        if os.path.exists(ppm):
            os.unlink(ppm)

    print('mean={:.1f} file={}'.format(level, args.out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
