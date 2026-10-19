#!/usr/bin/env python3
"""Reproduce SFZ cutouts from the hash-pinned inputs in credits.json.

Install the recorded Pillow version, download each preparation.input.url into
an input directory under its recorded name, and pass separate --input-dir and
--output-dir paths. Collection components without preparation use their pinned
PNG inputs. This tool has no network access and does not change credit records.
"""

import argparse
import hashlib
import io
import json
from pathlib import Path

import PIL
from PIL import Image, ImageChops, ImageDraw, ImageEnhance, ImageFilter


def digest(data):
    return hashlib.sha256(data).hexdigest()


def load_input(directory, source):
    with (directory / source['file']).open('rb') as stream:
        data = stream.read(4 * 1024 * 1024 + 1)
    if len(data) > 4 * 1024 * 1024 or digest(data) != source['sha256']:
        raise ValueError('Input hash or size differs: ' + source['file'])
    return Image.open(io.BytesIO(data)).convert('RGBA')


def cutout(image, preparation):
    mask = Image.new('L', image.size)
    draw = ImageDraw.Draw(mask)
    for polygon in preparation['keep_polygons']:
        draw.polygon([tuple(point) for point in polygon], fill=255)
    mask = mask.filter(ImageFilter.GaussianBlur(0.35))
    image.putalpha(ImageChops.multiply(image.getchannel('A'), mask))
    mask = image.getchannel('A')
    for polygon in preparation.get('clear_polygons', []):
        ImageDraw.Draw(mask).polygon([tuple(point) for point in polygon], fill=0)
    image.putalpha(mask)
    return image


def remove_background(image, background):
    target, tolerance = background['color'], background['tolerance']
    width, height = image.size
    pixels = image.load()
    visited = set()
    pending = [(x, y) for x in range(width) for y in (0, height - 1)]
    pending += [(x, y) for y in range(height) for x in (0, width - 1)]
    while pending:
        x, y = pending.pop()
        if (x, y) in visited:
            continue
        visited.add((x, y))
        pixel = pixels[x, y]
        if max(abs(pixel[c] - target[c]) for c in range(3)) > tolerance:
            continue
        pixels[x, y] = (*pixel[:3], 0)
        for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
            if 0 <= nx < width and 0 <= ny < height:
                pending.append((nx, ny))
    return image


def retouch(image, edits):
    for edit in edits:
        mask = Image.new('L', image.size)
        ImageDraw.Draw(mask).polygon([tuple(point) for point in edit['polygon']], fill=255)
        rgb = image.convert('RGB')
        if 'source_offset' in edit:
            dx, dy = edit['source_offset']
            replacement = rgb.transform(image.size, Image.Transform.AFFINE, (1, 0, dx, 0, 1, dy))
        else:
            replacement = Image.new('RGB', image.size, tuple(edit['color']))
        rgb.paste(replacement, (0, 0), mask.filter(ImageFilter.GaussianBlur(0.6)))
        rgb.putalpha(image.getchannel('A'))
        image = rgb
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input-dir', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--manifest', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'assets/sfz-instruments/credits.json')
    args = parser.parse_args()
    if args.input_dir.resolve() == args.output_dir.resolve():
        parser.error('Keep the pinned inputs separate from the output directory')
    records = json.loads(args.manifest.read_text(encoding='utf-8'))
    prepared = {}
    for record in records:
        preparation = record.get('preparation')
        if not preparation:
            continue
        if preparation['pillow_version'] != PIL.__version__:
            raise ValueError('Install Pillow==' + preparation['pillow_version'])
        if 'components' in preparation:
            image = Image.new('RGBA', tuple(preparation['canvas']))
            for component in preparation['components']:
                name = component['file']
                part = prepared[name].copy() if name in prepared else load_input(args.input_dir, component)
                part.thumbnail(tuple(component['size']), Image.Resampling.LANCZOS)
                image.alpha_composite(part, tuple(component['at']))
        else:
            image = load_input(args.input_dir, preparation['input'])
            if 'background' in preparation:
                image = remove_background(image, preparation['background'])
            if 'keep_polygons' in preparation:
                image = cutout(image, preparation)
            image = retouch(image, preparation.get('patches', []))
            if 'saturation' in preparation:
                image = ImageEnhance.Color(image).enhance(preparation['saturation'])
            if 'tint' in preparation:
                tinted = Image.new('RGBA', image.size, tuple(preparation['tint']) + (255,))
                tinted.putalpha(image.getchannel('A'))
                image = tinted
            if 'rotate' in preparation:
                image = image.rotate(preparation['rotate'], resample=Image.Resampling.BICUBIC, expand=True)
            if 'fit' in preparation:
                image = image.crop(image.getchannel('A').getbbox())
                image.thumbnail(tuple(preparation['fit']), Image.Resampling.LANCZOS)
        encoded = io.BytesIO()
        image.save(encoded, format='PNG', compress_level=9)
        if digest(encoded.getvalue()) != record['sha256']:
            raise ValueError('Prepared output differs from its recorded hash: ' + record['file'])
        prepared[record['file']] = image
        args.output_dir.mkdir(parents=True, exist_ok=True)
        (args.output_dir / record['file']).write_bytes(encoded.getvalue())
    if not prepared:
        raise ValueError('The manifest has no preparation records')
    print('Prepared ' + str(len(prepared)) + ' hash-matched SFZ category images')


if __name__ == '__main__':
    main()
