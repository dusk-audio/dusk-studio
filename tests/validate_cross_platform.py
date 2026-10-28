#!/usr/bin/env python3
import argparse
import base64
import importlib.util
import json
import os
from pathlib import Path, PureWindowsPath
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
repo = Path(__file__).resolve().parents[1]
source = Path(os.environ.get('DUSK_VALIDATOR_SUT', repo / 'scripts/validate-cross-platform.py'))
spec = importlib.util.spec_from_file_location('cross_platform_validator', source)
validator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(validator)
powershell = shutil.which('pwsh') or shutil.which('powershell.exe')


class ValidatorContract(unittest.TestCase):
    def test_capture_returns_native_result(self):
        args = argparse.Namespace(dry_run=False, windows_key='', windows_host='unused')
        expected = subprocess.CompletedProcess([], 0, 'captured output', '')
        with patch.object(validator.subprocess, 'run', return_value=expected) as run:
            actual = validator.Driver(args).windows_ps('capture', 'Write-Output hello', capture=True)
        self.assertIs(actual, expected)
        self.assertEqual(run.call_args.kwargs['stdout'], subprocess.PIPE)
        encoded = run.call_args.args[0][-1]
        self.assertEqual(base64.b64decode(encoded).decode('utf-16le'), 'Write-Output hello')
        args.dry_run = True
        with patch.object(validator.subprocess, 'run') as run:
            actual = validator.Driver(args).windows_ps('dry', 'throw "must not run"', capture=True)
        run.assert_not_called()
        self.assertEqual(actual.returncode, 0)

    def test_layout_and_cache(self):
        root = r"C:\owned validation\Marc's build"
        for generator, suffix in [('Visual Studio 17 2022', r'tests\Release'), ('Ninja', 'tests')]:
            layout = validator.windows_layout(root, generator)
            self.assertEqual(layout['test'], str(PureWindowsPath(root) / suffix / 'dusk-studio-tests.exe'))
            self.assertEqual(layout['fixtures'], root + ';' + str(PureWindowsPath(root) / 'Release'))
            self.assertEqual(layout['app'], str(PureWindowsPath(root) / 'DuskStudio_artefacts/Release/DuskStudio.exe'))
            probe = 'Release/dusk-windows-software-opengl-probe.exe' if generator.startswith('Visual') else 'dusk-windows-software-opengl-probe.exe'
            self.assertEqual(layout['probe'], str(PureWindowsPath(root) / probe))
        with self.assertRaises(validator.ValidationFailure):
            validator.windows_layout(root, 'Ninja Multi-Config')
        metadata = dict(generator='Ninja', mesa=r'C:\owned Mesa')
        line = 'DUSK-WINDOWS-CACHE ' + json.dumps(metadata)
        self.assertEqual(validator.windows_cache_result('noise\r\n' + line + '\r\n'), metadata)
        for text in ['', line + '\n' + line, 'DUSK-WINDOWS-CACHE {}', 'DUSK-WINDOWS-CACHE []', 'DUSK-WINDOWS-CACHE {"generator":1,"mesa":""}', 'DUSK-WINDOWS-CACHE malformed']:
            with self.subTest(text=text), self.assertRaises(validator.ValidationFailure):
                validator.windows_cache_result(text)

    def test_probe_report(self):
        good = 'renderer=llvmpipe (LLVM 20, 256 bits)\nversion=4.5 (Compatibility Profile) Mesa 26.2.0\n'
        validator.check_windows_probe(good)
        validator.check_windows_probe(good.replace('4.5', '3.0'))
        for text in ['', good.replace('llvmpipe', 'GDI Generic'), good.replace('4.5', '1.1'), good.replace('4.5', '(null)'), good.replace('4.5', '30garbage'), good + good, good.splitlines()[0], good.replace('renderer=llvmpipe (LLVM 20, 256 bits)', 'renderer=')]:
            with self.subTest(text=text), self.assertRaises(validator.ValidationFailure):
                validator.check_windows_probe(text)

    def test_windows_orchestration_uses_generator(self):
        for generator in ['Visual Studio 17 2022', 'Ninja']:
            for mesa in ['', r'C:\Mesa']:
                args = argparse.Namespace(windows_source=r'C:\owned source', windows_build_dir='build', windows_cmake='cmake.exe', windows_asio_sdk=r'C:\ASIO', windows_ctest_exclude='', windows_vst3=r'C:\fixture.vst3', sync=False, skip_build=False, native_state=True, filters=['[test]'], skip_full=False, dry_run=False, jobs=4)
                calls = []
                class FakeDriver:
                    def windows_ps(self, label, script, **kwargs):
                        calls.append((label, script, kwargs))
                        output = ''
                        if label.endswith('read build configuration'):
                            output = 'DUSK-WINDOWS-CACHE ' + json.dumps(dict(generator=generator, mesa=mesa))
                        elif label.endswith('packaged llvmpipe WGL'):
                            output = 'renderer=llvmpipe\nversion=4.5 Mesa\n'
                        return subprocess.CompletedProcess([], 0, output, '')
                driver = FakeDriver(); driver.args = args
                validator.run_windows(driver, '1' * 40)
                scripts = {label: script for label, script, _ in calls}
                layout = validator.windows_layout(r'C:\owned source\build', generator)
                self.assertIn(validator.ps_quote(layout['test']), scripts['Windows: focused tests'])
                self.assertIn(validator.ps_quote(layout['fixtures']), scripts['Windows: full-engine CLAP state'])
                self.assertEqual('Windows: packaged llvmpipe WGL' in scripts, bool(mesa))
                self.assertEqual('dusk-windows-software-opengl-probe' in scripts['Windows: build'], bool(mesa))
                if mesa:
                    self.assertIn(validator.ps_quote(layout['probe']), scripts['Windows: packaged llvmpipe WGL'])

    @unittest.skipUnless(powershell, 'PowerShell is required for generated Windows scripts')
    def test_generated_preflight(self):
        with tempfile.TemporaryDirectory(prefix="dusk validator's ") as temp:
            root = Path(temp); build = root / 'build with spaces'; build.mkdir()
            sdk = root / "SDK's directory"; (sdk / 'common').mkdir(parents=True)
            header = sdk / 'common/iasiodrv.h'; header.write_text('fixture')
            project = build / 'DuskStudio.vcxproj'; ninja = build / 'build.ninja'; cache = build / 'CMakeCache.txt'
            script = root / 'preflight.ps1'
            xml = '''<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003"><ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Release|x64'"><ClCompile><PreprocessorDefinitions>JUCE_ASIO=1;OTHER=1;%(PreprocessorDefinitions)</PreprocessorDefinitions></ClCompile></ItemDefinitionGroup></Project>'''
            edge = 'build CMakeFiles/DuskStudio.dir/src/Main.cpp.obj: CXX_COMPILER__DuskStudio_unscanned_Release C$:/source/src/Main.cpp\n  DEFINES = -DOTHER=1 -DJUCE_ASIO=1\n  DEP_FILE = main.d\n\n'
            def run(generator='Ninja', xml_text=xml, ninja_text=edge, extra='', sdk_path=None, expect=True, newline='\n'):
                text = f'CMAKE_GENERATOR:INTERNAL={generator}\nDUSKSTUDIO_RESOLVED_ASIO_SDK_PATH:INTERNAL={sdk_path or sdk}\n' + extra
                cache.write_bytes(text.replace('\n', newline).encode())
                project.write_bytes(xml_text.replace('\n', newline).encode()); ninja.write_bytes(ninja_text.replace('\n', newline).encode())
                if generator == 'Ninja':
                    project.unlink()
                elif generator.startswith('Visual Studio '):
                    ninja.unlink()
                script.write_text(validator.windows_asio_preflight_script(str(build), str(sdk)))
                result = subprocess.run([powershell, '-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', str(script)], capture_output=True, text=True)
                self.assertEqual(result.returncode == 0, expect, result.stdout + result.stderr)
                return result
            for generator in ['Visual Studio 17 2022', 'Ninja']:
                run(generator=generator); run(generator=generator, newline='\r\n')
                run(generator=generator, extra='DUSKSTUDIO_REQUIRE_ASIO:BOOL=OFF\n', expect=False)
                run(generator=generator, sdk_path=root / 'different-sdk', expect=False)
                run(generator=generator, extra=f'DUSKSTUDIO_RESOLVED_ASIO_SDK_PATH:INTERNAL={sdk}\n', expect=False)
                for macro in ['JUCE_ASIO=10', 'JUCE_ASIO=0', 'JUCE_ASIO=1;JUCE_ASIO=0', 'JUCE_ASIO=1;JUCE_ASIO=1', 'NOT_JUCE_ASIO=1']:
                    run(generator=generator, xml_text=xml.replace('JUCE_ASIO=1', macro), ninja_text=edge.replace('-DJUCE_ASIO=1', ' '.join('-D' + m for m in macro.split(';'))), expect=False)
            run(generator='Unix Makefiles', expect=False)
            run(extra='CMAKE_GENERATOR:INTERNAL=Ninja\n', expect=False)
            for broken in [xml.replace('Release|x64', 'Debug|x64'), xml.replace('JUCE_ASIO=1', 'OTHER=1') + '<!-- JUCE_ASIO=1 -->', xml.replace('</Project>', xml[xml.index('<ItemDefinitionGroup'):xml.index('</Project>')] + '</Project>')]:
                run(generator='Visual Studio 17 2022', xml_text=broken, expect=False)
            for broken in [edge.replace('DuskStudio.dir', 'other-target.dir'), edge.replace('Main.cpp.obj', 'Other.cpp.obj'), edge.replace('unscanned_Release', 'unscanned_Debug'), edge + edge, edge.replace('  DEFINES =', '  OTHER ='), edge.replace('-DJUCE_ASIO=1', '') + '# JUCE_ASIO=1\n']:
                run(ninja_text=broken, expect=False)
            header.unlink();run(expect=False);header.write_text('fixture')
            metadata_script = validator.windows_cache_script(str(build))
            script.write_text(metadata_script)
            result = subprocess.run([powershell, '-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', str(script)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(validator.windows_cache_result(result.stdout), dict(generator='Ninja', mesa=''))

    @unittest.skipUnless(powershell, 'PowerShell is required for generated Windows scripts')
    def test_generated_probe_deadline_and_exit(self):
        with tempfile.TemporaryDirectory(prefix='dusk-probe-contract-') as temp:
            script = Path(temp) / 'probe.ps1'
            for mode in ['pass', 'native-failure', 'timeout']:
                mock = '''function Start-Process {
param($FilePath, [switch]$PassThru, $RedirectStandardOutput, $RedirectStandardError)
[IO.File]::WriteAllText($RedirectStandardOutput, "renderer=llvmpipe`nversion=4.5 Mesa`n")
[IO.File]::WriteAllText($RedirectStandardError, '')
$script:handleCached = $false
$p=[pscustomobject]@{}
$p|Add-Member ScriptProperty Handle {$script:handleCached = $true; [IntPtr]1}
$p|Add-Member ScriptProperty ExitCode {if ($script:handleCached) {__CODE__} else {$null}}
$p|Add-Member ScriptMethod WaitForExit {param($ms) __WAIT__}
$p|Add-Member ScriptMethod Kill {Write-Output 'owned probe killed'}
return $p
}
'''.replace('__CODE__', '16' if mode == 'native-failure' else '0').replace('__WAIT__', 'return $false' if mode == 'timeout' else 'return $true')
                script.write_text(mock + validator.windows_probe_script("C:/owned/probe.exe"))
                env = dict(os.environ, TEMP=temp)
                result = subprocess.run([powershell, '-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', str(script)], env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode == 0, mode == 'pass', result.stdout + result.stderr)
                if mode == 'pass': validator.check_windows_probe(result.stdout)
                self.assertEqual(list(Path(temp).glob('dusk-wgl-*')), [])


if __name__ == '__main__':
    unittest.main()
