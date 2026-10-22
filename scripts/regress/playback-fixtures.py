#!/usr/bin/env python3
"""Generate distinct PCM16 sources for the device playback continuity oracle."""

import argparse
import hashlib
import json
from pathlib import Path
import wave

import numpy as np


TRACKS = 24
REGIONS = 15
REGION_SECONDS = 8
CHUNK_FRAMES = 65536
MASK = (1 << 64) - 1


def pcm16(track, region, channel, first_frame, count):
    seed = ((track * REGIONS + region) * 0x9E3779B97F4A7C15
            + (channel + 1) * 0xD1B54A32D192ED03) & MASK
    bits = np.arange(first_frame, first_frame + count, dtype=np.uint64) + np.uint64(seed)
    bits = (bits ^ (bits >> 30)) * np.uint64(0xBF58476D1CE4E5B9)
    bits = (bits ^ (bits >> 27)) * np.uint64(0x94D049BB133111EB)
    bits ^= bits >> 31
    return ((bits >> 48).astype(np.int32) - 32768).astype('<i2')


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def generate(directory, rate):
    directory.mkdir()
    tracks, files = [], []
    frames = REGION_SECONDS * rate
    for track in range(TRACKS):
        regions = []
        for region in range(REGIONS):
            name = f'track-{track + 1:02d}-region-{region + 1:02d}.wav'
            path = directory / name
            with wave.open(str(path), 'wb') as output:
                output.setparams((2, 2, rate, frames, 'NONE', 'not compressed'))
                for first in range(0, frames, CHUNK_FRAMES):
                    count = min(CHUNK_FRAMES, frames - first)
                    samples = np.empty((count, 2), dtype='<i2')
                    for channel in range(2):
                        samples[:, channel] = pcm16(track, region, channel, first, count)
                    output.writeframesraw(samples.tobytes())
            files.append({'path': name, 'bytes': path.stat().st_size, 'sha256': sha256(path)})
            regions.append({'file': name, 'timeline_start': region * frames, 'length': frames,
                            'source_offset': 0, 'num_channels': 2})
        tracks.append({'name': f'Perf {track + 1}', 'track_mode': 1, 'input_monitor': False,
                       'fader_db': -36.0, 'regions': regions})
    session = directory / 'session.json'
    session.write_text(json.dumps({'version': 11, 'session_sample_rate': rate,
                                   'tracks': tracks}, indent=2) + '\n', encoding='utf-8')
    return {'rate': rate, 'session': str(session), 'session_sha256': sha256(session), 'files': files}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path, help='new or empty fixture directory')
    parser.add_argument('--rates', nargs='+', type=int, choices=(48000, 96000, 192000),
                        default=[48000, 96000, 192000])
    args = parser.parse_args()
    root = args.output.resolve()
    if root.exists() and (not root.is_dir() or any(root.iterdir())):
        parser.error('output must be a new or empty directory')
    if len(set(args.rates)) != len(args.rates):
        parser.error('sample rates must be distinct')
    root.mkdir(parents=True, exist_ok=True)
    manifest = {'format': 'stereo PCM16', 'pattern': 'splitmix64-track-region-channel-frame-v1',
                'tracks': TRACKS, 'regions_per_track': REGIONS, 'region_seconds': REGION_SECONDS,
                'track_fader_db': -36, 'rates': []}
    for rate in args.rates:
        result = generate(root / str(rate), rate)
        manifest['rates'].append(result)
        print(f'{rate}: {TRACKS} tracks, {len(result["files"])} distinct files, '
              f'{sum(item["bytes"] for item in result["files"])} bytes', flush=True)
    (root / 'fixtures.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
