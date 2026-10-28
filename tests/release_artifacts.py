#!/usr/bin/env python3
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


repo = Path(sys.argv[1]).resolve()


def executable(path, contents):
    path.write_text(contents)
    path.chmod(0o755)


def package_macos(root):
    scripts = root / "scripts"
    scripts.mkdir(parents=True)
    shutil.copyfile(repo / "scripts/package-macos.sh", scripts / "package-macos.sh")
    entitlements = root / "packaging/macos/entitlements.plist"
    entitlements.parent.mkdir(parents=True)
    entitlements.write_text("fixture entitlements\n")
    build = root / "build with spaces"
    app = build / "DuskStudio_artefacts/Release/DuskStudio.app"
    app.mkdir(parents=True)
    old = build / "dusk-studio-0.0.0-macOS-arm64.dmg"
    old.write_text("stale package\n")
    mocks = root / "bin"
    mocks.mkdir()
    executable(mocks / "codesign", f"#!{sys.executable}\n" + """
import json, os, sys
with open(os.environ['PACKAGE_TEST_CODESIGN'], 'a') as out:
    out.write(json.dumps(sys.argv[1:]) + '\\n')
""")
    executable(mocks / "cpack", f"#!{sys.executable}\n" + """
import os, pathlib, sys
args = sys.argv[1:]
out = pathlib.Path(args[args.index('-B') + 1]) if '-B' in args else pathlib.Path.cwd()
out.mkdir(parents=True, exist_ok=True)
mode = os.environ['PACKAGE_TEST_MODE']
if mode == 'failed': sys.exit(19)
if mode != 'none': (out / 'dusk-studio-9.9.9-macOS-arm64.dmg').write_text('fresh package\\n')
if mode == 'multiple': (out / 'extra.dmg').write_text('ambiguous package\\n')
""")
    trace = root / "codesign.jsonl"
    environment = dict(os.environ, PATH=f"{mocks}:{os.environ['PATH']}",
                       BUILD_DIR=str(build), PACKAGE_TEST_CODESIGN=str(trace))
    destination = root / "dusk-studio-9.9.9-macOS-arm64.dmg"
    for mode in ("fresh", "none", "multiple", "failed"):
        result = subprocess.run(
            ["bash", str(scripts / "package-macos.sh")],
            env=dict(environment, PACKAGE_TEST_MODE=mode), capture_output=True,
            text=True, timeout=10,
        )
        if mode == "fresh":
            assert result.returncode == 0, (result.stdout, result.stderr)
            assert destination.exists(), "the current CPack DMG was not published"
            assert destination.read_text() == "fresh package\n", destination.read_text()
            checksums = (root / "SHA256SUMS.macos").read_text()
            expected_hash = hashlib.sha256(destination.read_bytes()).hexdigest()
            assert checksums == f"{expected_hash}  {destination.name}\n", checksums
            destination.unlink()
            (root / "SHA256SUMS.macos").unlink()
        else:
            assert result.returncode != 0, (mode, result.stdout, result.stderr)
            assert not destination.exists(), "failed CPack output was published"
            assert not (root / "SHA256SUMS.macos").exists(), "failed package was hashed"
        assert old.read_text() == "stale package\n", "a previous build DMG was consumed"
    calls = [json.loads(line) for line in trace.read_text().splitlines()]
    assert len(calls) == 8 and all(call[-1] == str(app) for call in calls), calls


def smoke_checksum(root):
    scripts = root / "scripts"
    scripts.mkdir(parents=True)
    shutil.copyfile(repo / "scripts/release-smoke-test.sh", scripts / "release-smoke-test.sh")
    executable(scripts / "verify-package-contents.sh", "#!/bin/sh\nexit 0\n")
    packaging = root / "packaging"
    packaging.mkdir()
    (packaging / "release-signing.pub").write_text("-----BEGIN PGP PUBLIC KEY BLOCK-----\nfixture\n")
    mocks = root / "bin"
    mocks.mkdir()
    executable(mocks / "gpg", "#!/bin/sh\nexit \"${SMOKE_TEST_GPG_STATUS:-0}\"\n")
    executable(mocks / "hdiutil", f"#!{sys.executable}\n" + """
import pathlib, sys
if sys.argv[1] == 'attach':
    mount = pathlib.Path(sys.argv[sys.argv.index('-mountpoint') + 1])
    app = mount / 'DuskStudio.app/Contents/MacOS/DuskStudio'
    app.parent.mkdir(parents=True)
    app.write_text('#!/bin/sh\\necho Dusk Studio 9.9.9\\n')
    app.chmod(0o755)
""")
    artifact_dir = root / "downloads with spaces"
    artifact_dir.mkdir()
    covered = artifact_dir / "dusk-studio-9.9.9-macOS-arm64.dmg"
    selected = artifact_dir / "dusk-studio-9.9.9-macOS-x86_64.dmg"
    covered.write_bytes(b"covered artifact A\n")
    selected.write_bytes(b"selected artifact B\n")
    covered_row = f"{hashlib.sha256(covered.read_bytes()).hexdigest()}  {covered.name}\n"
    digest = hashlib.sha256(selected.read_bytes()).hexdigest()
    selected_row = f"{digest}  {selected.name}\n"
    manifest = artifact_dir / "SHA256SUMS"
    (artifact_dir / "SHA256SUMS.asc").write_text("fixture signature\n")
    environment = dict(os.environ, PATH=f"{mocks}:{os.environ['PATH']}")
    cases = (
        ("unlisted", covered_row, False, "0"),
        ("listed", covered_row + selected_row, True, "0"),
        ("binary-marker", f"{digest} *{selected.name}\n", True, "0"),
        ("duplicate", selected_row + selected_row, False, "0"),
        ("wrong-hash", covered_row + f"{'0' * 64}  {selected.name}\n", False, "0"),
        ("malformed", f"{'z' * 64}  {selected.name}\n", False, "0"),
        ("bad-signature", selected_row, False, "1"),
    )
    for name, contents, succeeds, gpg_status in cases:
        manifest.write_text(contents)
        result = subprocess.run(
            ["bash", str(scripts / "release-smoke-test.sh"), "macos", str(selected)],
            env=dict(environment, SMOKE_TEST_GPG_STATUS=gpg_status),
            capture_output=True, text=True, timeout=10,
        )
        assert (result.returncode == 0) == succeeds, (name, result.stdout, result.stderr)
        assert "SKIP  checksum signature" not in result.stdout, result.stdout
        if name in ("unlisted", "duplicate", "wrong-hash", "malformed"):
            assert "FAIL  " in result.stdout and "checksum" in result.stdout, result.stdout


checks = {"package-macos": package_macos, "smoke-checksum": smoke_checksum}
for name in sys.argv[2:] or checks:
    with tempfile.TemporaryDirectory(prefix=f"dusk-{name}-") as scratch:
        checks[name](Path(scratch))
    print(f"{name}: synthetic artifact regression passed")
