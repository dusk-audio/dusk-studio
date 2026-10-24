#!/usr/bin/env python3
"""Portable Mach-O metadata fixtures; no compiled code is executed."""

import importlib.util
import json
import os
from pathlib import Path
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
source = Path(os.environ.get('DUSK_MACOS_BUNDLE_SUT',
                             Path(__file__).resolve().parents[1] / 'scripts/verify-macos-bundle.py'))
spec = importlib.util.spec_from_file_location('macos_bundle_verifier', source)
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)

ARM64, X86_64 = 0x100000C, 0x1000007
MAIN = 'Contents/MacOS/DuskStudio'
HELPER = 'Contents/MacOS/dusk-studio-plugin-host'
DIRECT = 'Contents/Frameworks/direct.dylib'
TRANSITIVE = 'Contents/Frameworks/transitive.dylib'
ORPHAN = 'Contents/Resources/no-extension'


def minimum(major=11, minor=0, patch=0, legacy=False, endian='<'):
    value = major << 16 | minor << 8 | patch
    if legacy:
        return struct.pack(endian + '4I', 0x24, 16, value, 15 << 16)
    return struct.pack(endian + '6I', 0x32, 24, 1, value, 15 << 16, 0)


def load(name, command=0xC):
    text = name.encode() + b'\0'
    size = (24 + len(text) + 7) // 8 * 8
    return struct.pack('<6I', command, size, 24, 0, 0, 0) + text + b'\0' * (size - 24 - len(text))


def thin(commands=None, cpu=ARM64, filetype=6, endian='<', bits=64):
    commands = [minimum(endian=endian)] if commands is None else commands
    header = struct.pack(endian + '7I', 0xFEEDFACF if bits == 64 else 0xFEEDFACE,
                         cpu, 0, filetype, len(commands), sum(map(len, commands)), 0)
    return header + (b'\0' * 4 if bits == 64 else b'') + b''.join(commands)


def fat(slices, endian='>', bits=32):
    wide = bits == 64
    offset = 8 + len(slices) * (32 if wide else 20)
    header = struct.pack(endian + '2I', 0xCAFEBABF if wide else 0xCAFEBABE, len(slices))
    bodies = b''
    for cpu, body in slices:
        fields = (cpu, 0, offset, len(body), 0) + ((0,) if wide else ())
        header += struct.pack(endian + ('2I2Q2I' if wide else '5I'), *fields)
        bodies += body
        offset += len(body)
    return header + bodies


def universal(commands, filetype=6):
    return fat([(cpu, thin(commands, cpu, filetype)) for cpu in (ARM64, X86_64)])


class MacOSBundleContract(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='dusk macos bundle ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.app = self.root / 'Dusk fixture.app'
        (self.root / 'outside.dylib').write_bytes(thin())
        self.reset()

    def put(self, relative, contents):
        path = self.app / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)

    def reset(self):
        if self.app.exists():
            shutil.rmtree(self.app)
        self.put('Contents/Info.plist', plistlib.dumps({
            'CFBundleExecutable': 'DuskStudio', 'LSMinimumSystemVersion': '11.0'}))
        self.put(MAIN, universal([minimum(), load('@executable_path/../Frameworks/direct.dylib'),
                                  load('/usr/lib/libSystem.B.dylib')], filetype=2))
        self.put(HELPER, thin([minimum(10, 15, legacy=True),
                               load('@executable_path/../Frameworks/direct.dylib')], filetype=2))
        self.put(DIRECT, universal([minimum(), load('/unbundled/own-install-id.dylib', 0xD),
                                    load('@loader_path/transitive.dylib')]))
        self.put(TRANSITIVE, universal([minimum(10, 9, legacy=True), load('@loader_path/direct.dylib')]))
        self.put(ORPHAN, thin([minimum(10, 15)]))

    def check_pass(self):
        report = verifier.verify(self.app, (11, 0, 0))
        self.assertEqual(report['errors'], [])
        self.assertEqual(report['status'], 'PASS')
        return report

    def check_fail(self, reason):
        report = verifier.verify(self.app, (11, 0, 0))
        self.assertEqual(report['status'], 'FAIL', report)
        self.assertTrue(any(reason in error for error in report['errors']), report)
        return report

    def test_inventory_cycle_identifiers_and_sdk(self):
        report = self.check_pass()
        self.assertEqual(report['macho_files'], 5)
        self.assertEqual(report['architecture_slices'], 8)
        self.assertEqual(report['dependency_edges'], 9)
        self.assertEqual({Path(image['path']).as_posix() for image in report['images']},
                         {MAIN, HELPER, DIRECT, TRANSITIVE, ORPHAN})
        self.assertEqual({image['minimum_macos'] for image in report['images']}, {'11.0', '10.15', '10.9'})

    def test_minimum_not_sdk_and_numeric_floor(self):
        for command in [minimum(11, 1), minimum(11, 0, 1), minimum(15, legacy=True)]:
            with self.subTest(command=command):
                self.put(ORPHAN, thin([command]))
                self.check_fail('exceeds 11.0')
                self.reset()
                self.check_pass()
        self.put(ORPHAN, thin([minimum(10, 15)]))
        self.check_pass()
        self.assertEqual(verifier.version('11.0.1'), (11, 0, 1))

    def test_every_slice_and_unlinked_resource(self):
        self.put(TRANSITIVE, fat([(ARM64, thin()), (X86_64, thin([minimum(15)], X86_64))]))
        self.check_fail('x86_64/subtype-0')
        self.reset()
        self.check_pass()
        self.put(ORPHAN, thin([minimum(15)]))
        self.check_fail('no-extension')
        self.reset()
        self.check_pass()

    def test_transitive_load_commands_and_restoration(self):
        for command in [0xC, 0x80000018, 0x8000001F, 0x20, 0x80000023]:
            with self.subTest(command=command):
                self.put(DIRECT, universal([minimum(), load('@loader_path/transitive.dylib', command)]))
                self.check_pass()
                saved = (self.app / TRANSITIVE).read_bytes()
                (self.app / TRANSITIVE).unlink()
                self.check_fail('transitive.dylib')
                self.put(TRANSITIVE, saved)
                self.check_pass()

    def test_external_unresolved_and_escaping_paths(self):
        for path in ['@rpath/transitive.dylib', '/opt/homebrew/lib/transitive.dylib', 'transitive.dylib']:
            with self.subTest(path=path):
                self.put(DIRECT, universal([minimum(), load(path)]))
                self.check_fail('unresolved or external dependency')
                self.reset()
                self.check_pass()
        for path in ['@loader_path/../../../outside.dylib', '@executable_path/../../../outside.dylib']:
            with self.subTest(path=path):
                self.put(DIRECT, universal([minimum(), load(path)]))
                self.check_fail('resolved target escapes the bundle')
                self.reset()
                self.check_pass()

    def test_arm64_filetypes_and_dependency_cpu(self):
        for path, body, reason in [
            (ORPHAN, thin(cpu=X86_64), 'has no arm64 slice'),
            (MAIN, thin(filetype=6), 'must use MH_EXECUTE'),
            (HELPER, thin(filetype=8), 'must use MH_EXECUTE'),
            (TRANSITIVE, universal([minimum()], filetype=8), 'must use MH_DYLIB'),
            (TRANSITIVE, thin(), 'target has no matching CPU architecture'),
            ('Contents/MacOS/tools/nested', thin(filetype=2), 'unsupported nested executable'),
        ]:
            with self.subTest(path=path, reason=reason):
                self.put(path, body)
                self.check_fail(reason)
                self.reset()
                self.check_pass()

    def test_required_images_and_advertised_floor(self):
        for path in [MAIN, HELPER]:
            with self.subTest(path=path):
                (self.app / path).unlink()
                self.check_fail('required executable is missing')
                self.reset()
                self.put(path, b'not Mach-O')
                self.check_fail('required executable is not a valid Mach-O')
                self.reset()
                self.check_pass()
        for value, reason in [('15.0', 'Info.plist advertises macOS 15.0'),
                              ('10.15', 'exceeds 10.15'), ('11x0', 'numeric macOS version')]:
            with self.subTest(value=value):
                self.put('Contents/Info.plist', plistlib.dumps({
                    'CFBundleExecutable': 'DuskStudio', 'LSMinimumSystemVersion': value}))
                self.check_fail(reason)
                self.reset()
                self.check_pass()

    def test_malformed_images_and_metadata(self):
        for body, reason in [
            (b'\xcf\xfa\xed\xfe', 'truncated Mach-O header'),
            (b'\xca\xfe\xba\xbe', 'truncated universal header'),
            (struct.pack('>7I', 0xCAFEBABE, 1, ARM64, 0, 65535, 32, 0), 'outside the file'),
            (fat([(X86_64, thin())]), 'does not match its slice header'),
            (thin([struct.pack('<2I', 0x32, 7)]), 'invalid load-command size'),
            (thin([]), 'exactly one nonzero'),
            (thin([minimum(), minimum(10, 15, legacy=True)]), 'exactly one nonzero'),
            (thin([struct.pack('<6I', 0x32, 24, 2, 11 << 16, 15 << 16, 0)]), 'platform 2 is not macOS'),
            (thin([minimum(), struct.pack('<6I', 0xC, 32, 24, 0, 0, 0) + b'abcdefgh']), 'unterminated dylib name'),
        ]:
            with self.subTest(reason=reason):
                self.put(ORPHAN, body)
                self.check_fail(reason)
                self.reset()
                self.check_pass()
        self.put('Contents/Info.plist', b'<?xml version="1.0"?><plist version="1.0"><dict>')
        self.check_fail('Contents/Info.plist')
        self.reset()
        self.check_pass()

    def test_binary_encodings(self):
        for endian in ['<', '>']:
            for bits in [32, 64]:
                with self.subTest(endian=endian, bits=bits):
                    self.put(ORPHAN, fat([(ARM64, thin()), (X86_64, thin(cpu=X86_64))], endian, bits))
                    self.check_pass()
                    body = thin([minimum(endian=endian)], cpu=7 if bits == 32 else ARM64,
                                endian=endian, bits=bits)
                    self.put(ORPHAN, fat([(ARM64, thin()), (7, body)]) if bits == 32 else body)
                    self.check_pass()

    def test_symlink_containment(self):
        alias = self.app / 'Contents/Frameworks/alias.dylib'
        try:
            alias.symlink_to('transitive.dylib')
        except OSError as error:
            self.skipTest(f'symbolic links unavailable: {error}')
        self.put(DIRECT, universal([minimum(), load('@loader_path/alias.dylib')]))
        self.check_pass()
        alias.unlink()
        alias.symlink_to(self.root / 'outside.dylib')
        self.check_fail('symbolic link escapes the bundle')
        alias.unlink()
        alias.symlink_to('transitive.dylib')
        self.check_pass()

    def test_cli_default_and_custom_floor(self):
        for extra, expected in [([], 0), (['--max-macos', '10.15'], 1)]:
            result = subprocess.run([sys.executable, str(source), str(self.app), *extra],
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, expected, result.stderr)
            report = json.loads(result.stdout)
            self.assertEqual(report['status'], 'PASS' if expected == 0 else 'FAIL')
            self.assertEqual(report['max_macos'], '11.0' if expected == 0 else '10.15')


if __name__ == '__main__':
    unittest.main()
