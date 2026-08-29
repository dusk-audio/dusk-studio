#!/usr/bin/env python3
"""Run one Dusk Studio commit through Linux, macOS, and Windows validation.

The macOS and Windows checkouts are expected to be dedicated validation
worktrees.  Pass --sync to move those clean worktrees to the requested commit;
the script refuses to do that unless each checkout has this local git setting:

    git config dusk.validationWorktree true

Build configuration and third-party dependency provisioning remain explicit,
one-time machine setup.  This driver verifies, builds, and tests the configured
trees without touching a developer's normal (possibly dirty) checkout.
"""

from __future__ import annotations

import argparse
import base64
import os
from pathlib import Path, PureWindowsPath
import shlex
import subprocess
import sys
import time


class ValidationFailure(RuntimeError):
    pass


def shell_quote(value: str | Path) -> str:
    return shlex.quote(str(value))


def ps_quote(value: str | PureWindowsPath) -> str:
    return "'" + str(value).replace("'", "''") + "'"


def ps_join(root: str, child: str) -> str:
    return str(PureWindowsPath(root) / PureWindowsPath(child))


class Driver:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.platform_results: list[tuple[str, str, float, str]] = []

    def command(
        self,
        label: str,
        argv: list[str],
        *,
        capture: bool = False,
        display: str | None = None,
    ) -> subprocess.CompletedProcess[str]:
        print(f"\n==> {label}", flush=True)
        print("    " + (display or shlex.join(argv)), flush=True)
        if self.args.dry_run:
            return subprocess.CompletedProcess(argv, 0, "", "")

        result = subprocess.run(
            argv,
            text=True,
            stdout=subprocess.PIPE if capture else None,
            stderr=subprocess.PIPE if capture else None,
            check=False,
        )
        if capture:
            if result.stdout:
                print(result.stdout, end="" if result.stdout.endswith("\n") else "\n")
            if result.stderr:
                print(
                    result.stderr,
                    end="" if result.stderr.endswith("\n") else "\n",
                    file=sys.stderr,
                )
        if result.returncode != 0:
            raise ValidationFailure(f"{label} exited with {result.returncode}")
        return result

    def local_bash(self, label: str, script: str, *, capture: bool = False) -> None:
        self.command(label, ["bash", "-lc", script], capture=capture)

    def mac_bash(self, label: str, script: str, *, capture: bool = False) -> None:
        remote = f"bash -lc {shlex.quote(script)}"
        self.command(label, ["ssh", self.args.mac_host, remote], capture=capture)

    def windows_ps(self, label: str, script: str, *, capture: bool = False) -> None:
        encoded = base64.b64encode(script.encode("utf-16le")).decode("ascii")
        argv = ["ssh"]
        if self.args.windows_key:
            argv.extend(["-i", self.args.windows_key])
        argv.extend(
            [
                self.args.windows_host,
                "powershell.exe",
                "-NoProfile",
                "-NonInteractive",
                "-EncodedCommand",
                encoded,
            ]
        )
        display_parts = ["ssh"]
        if self.args.windows_key:
            display_parts.extend(["-i", shell_quote(self.args.windows_key)])
        display_parts.extend(
            [
                shell_quote(self.args.windows_host),
                "powershell.exe -NoProfile -NonInteractive -EncodedCommand <script>",
            ]
        )
        display = " ".join(display_parts)
        display += "\n" + "\n".join(f"      {line}" for line in script.splitlines())
        self.command(label, argv, capture=capture, display=display)

    def platform(self, name: str, work) -> None:
        started = time.monotonic()
        try:
            work()
        except (OSError, ValidationFailure) as exc:
            elapsed = time.monotonic() - started
            self.platform_results.append((name, "FAIL", elapsed, str(exc)))
            print(f"\n{name}: FAIL: {exc}", file=sys.stderr, flush=True)
        else:
            elapsed = time.monotonic() - started
            self.platform_results.append((name, "PASS", elapsed, ""))
            print(f"\n{name}: PASS ({elapsed:.1f}s)", flush=True)


def parse_args(repo_root: Path) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Build and validate the same Dusk Studio commit on local Linux, a "
            "headless macOS SSH host, and a Windows SSH VM."
        )
    )
    parser.add_argument(
        "--commit",
        default="HEAD",
        help="local git revision to validate (default: HEAD)",
    )
    parser.add_argument(
        "--filter",
        action="append",
        default=[],
        dest="filters",
        metavar="CATCH_FILTER",
        help="focused Catch2 filter to run on every platform; repeatable",
    )
    parser.add_argument(
        "--native-state",
        action="store_true",
        help="also require CLAP state harnesses, macOS AU tests, and live Windows VST3 state tests",
    )
    parser.add_argument(
        "--sync",
        action="store_true",
        help="fetch and detach dedicated remote validation worktrees at --commit",
    )
    parser.add_argument(
        "--skip-build", action="store_true", help="reuse existing binaries"
    )
    parser.add_argument(
        "--skip-full", action="store_true", help="skip each full CTest suite"
    )
    parser.add_argument("--skip-linux", action="store_true")
    parser.add_argument("--skip-macos", action="store_true")
    parser.add_argument("--skip-windows", action="store_true")
    parser.add_argument(
        "--allow-dirty",
        action="store_true",
        help="allow a dirty local tree (remote hosts still validate only the commit)",
    )
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument(
        "--dry-run", action="store_true", help="print commands without executing them"
    )

    parser.add_argument("--linux-source", default=str(repo_root))
    parser.add_argument("--linux-build-dir", default="build-tests")

    parser.add_argument(
        "--mac-host",
        default=os.environ.get("DUSK_VALIDATION_MAC_HOST", "marc@macbook-air.local"),
    )
    parser.add_argument(
        "--mac-source",
        default=os.environ.get(
            "DUSK_VALIDATION_MAC_SOURCE", "/Users/marc/dusk-validation/source"
        ),
    )
    parser.add_argument(
        "--mac-build-dir",
        default=os.environ.get("DUSK_VALIDATION_MAC_BUILD", "build-validation"),
    )
    parser.add_argument(
        "--mac-cmake",
        default=os.environ.get("DUSK_VALIDATION_MAC_CMAKE", "cmake"),
    )

    parser.add_argument(
        "--windows-host",
        default=os.environ.get("DUSK_VALIDATION_WINDOWS_HOST", "marc@192.168.122.85"),
    )
    parser.add_argument(
        "--windows-key",
        default=os.environ.get("DUSK_VALIDATION_WINDOWS_KEY", ""),
    )
    parser.add_argument(
        "--windows-source",
        default=os.environ.get(
            "DUSK_VALIDATION_WINDOWS_SOURCE", r"C:\Users\marc\dusk-validation\source"
        ),
    )
    parser.add_argument(
        "--windows-build-dir",
        default=os.environ.get("DUSK_VALIDATION_WINDOWS_BUILD", "build-validation"),
    )
    parser.add_argument(
        "--windows-cmake",
        default=os.environ.get(
            "DUSK_VALIDATION_WINDOWS_CMAKE", r"C:\Program Files\CMake\bin\cmake.exe"
        ),
    )
    parser.add_argument(
        "--windows-asio-sdk",
        default=os.environ.get("DUSK_VALIDATION_WINDOWS_ASIO_SDK", ""),
        help="ASIO SDK root; required for every Windows validation",
    )
    parser.add_argument(
        "--windows-vst3",
        default=os.environ.get("DUSK_VALIDATION_WINDOWS_VST3", ""),
        help="live VST3 fixture; required with --native-state on Windows",
    )
    parser.add_argument(
        "--windows-ctest-exclude",
        default=os.environ.get("DUSK_VALIDATION_WINDOWS_CTEST_EXCLUDE", ""),
        help="optional CTest exclusion regex (normally empty on the VM)",
    )

    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be a positive integer")
    if not args.skip_windows and not args.windows_asio_sdk:
        parser.error("--windows-asio-sdk is required unless --skip-windows is set")
    if args.native_state and not args.skip_windows and not args.windows_vst3:
        parser.error(
            "--native-state requires --windows-vst3 unless --skip-windows is set"
        )
    return args


def resolve_commit(source: Path, revision: str, dry_run: bool) -> str:
    if dry_run:
        try:
            return subprocess.check_output(
                ["git", "-C", str(source), "rev-parse", revision], text=True
            ).strip()
        except (OSError, subprocess.CalledProcessError):
            return revision
    try:
        return subprocess.check_output(
            ["git", "-C", str(source), "rev-parse", f"{revision}^{{commit}}"],
            text=True,
            stderr=subprocess.PIPE,
        ).strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise SystemExit(
            f"error: cannot resolve commit {revision!r} in {source}: {exc}"
        )


def require_local_clean(source: Path, allow_dirty: bool, dry_run: bool) -> None:
    if allow_dirty or dry_run:
        return
    result = subprocess.run(
        ["git", "-C", str(source), "status", "--porcelain", "--untracked-files=all"],
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        raise SystemExit(
            f"error: git status failed in {source}: {result.stderr.strip()}"
        )
    if result.stdout.strip():
        raise SystemExit(
            "error: local source is dirty; commit/stash it for exact cross-platform "
            "validation, or pass --allow-dirty to validate only the committed revision remotely"
        )


def posix_build_path(source: str, build: str) -> str:
    return build if build.startswith("/") else f"{source.rstrip('/')}/{build}"


def windows_build_path(source: str, build: str) -> str:
    if PureWindowsPath(build).is_absolute():
        return build
    return ps_join(source, build)


def posix_checkout_script(
    source: str, commit: str, sync: bool, *, require_clean: bool = True
) -> str:
    src = shell_quote(source)
    lines = [
        "set -euo pipefail",
        f"git -C {src} rev-parse --is-inside-work-tree >/dev/null",
    ]
    if sync:
        lines.extend(
            [
                f'test "$(git -C {src} config --bool dusk.validationWorktree)" = true',
                f'test -z "$(git -C {src} status --porcelain --untracked-files=all)"',
                f"git -C {src} fetch origin {shell_quote(commit)}",
                f"git -C {src} checkout --detach {shell_quote(commit)}",
                f"git -C {src} submodule update --init --recursive",
            ]
        )
    lines.append(f'test "$(git -C {src} rev-parse HEAD)" = {shell_quote(commit)}')
    if require_clean:
        lines.append(
            f'test -z "$(git -C {src} status --porcelain --untracked-files=all)"'
        )
    return "\n".join(lines)


def windows_checkout_script(source: str, commit: str, sync: bool) -> str:
    src = ps_quote(source)
    lines = [
        "$ErrorActionPreference = 'Stop'",
        f"& git -C {src} rev-parse --is-inside-work-tree | Out-Null",
        "if ($LASTEXITCODE -ne 0) { throw 'missing git checkout' }",
    ]
    if sync:
        lines.extend(
            [
                f"$dedicated = (& git -C {src} config --bool dusk.validationWorktree).Trim()",
                "if ($LASTEXITCODE -ne 0 -or $dedicated -ne 'true') { throw 'remote checkout is not marked dusk.validationWorktree=true' }",
                f'$dirty = (& git -C {src} status --porcelain --untracked-files=all) -join "`n"',
                "if ($LASTEXITCODE -ne 0 -or $dirty) { throw 'remote validation checkout is dirty' }",
                f"& git -C {src} fetch origin {ps_quote(commit)}",
                "if ($LASTEXITCODE -ne 0) { throw 'git fetch failed' }",
                f"& git -C {src} checkout --detach {ps_quote(commit)}",
                "if ($LASTEXITCODE -ne 0) { throw 'git checkout failed' }",
                f"& git -C {src} submodule update --init --recursive",
                "if ($LASTEXITCODE -ne 0) { throw 'git submodule update failed' }",
            ]
        )
    lines.extend(
        [
            f"$actual = (& git -C {src} rev-parse HEAD).Trim()",
            "if ($LASTEXITCODE -ne 0) { throw 'git rev-parse failed' }",
            f'if ($actual -ne {ps_quote(commit)}) {{ throw "checkout $actual does not match requested commit" }}',
            f'$dirty = (& git -C {src} status --porcelain --untracked-files=all) -join "`n"',
            "if ($LASTEXITCODE -ne 0 -or $dirty) { throw 'remote validation checkout is dirty' }",
        ]
    )
    return "\n".join(lines)


def windows_asio_preflight_script(build: str, expected_sdk: str) -> str:
    cache = ps_join(build, "CMakeCache.txt")
    project = ps_join(build, "DuskStudio.vcxproj")
    expected_header = ps_join(expected_sdk, r"common\iasiodrv.h")
    return "\n".join(
        [
            "$ErrorActionPreference = 'Stop'",
            f"$expectedSdk = [IO.Path]::GetFullPath({ps_quote(expected_sdk)}).TrimEnd('\\')",
            f"if (-not (Test-Path -LiteralPath {ps_quote(expected_header)})) {{ throw 'configured ASIO SDK is missing common\\iasiodrv.h' }}",
            f"if (-not (Test-Path -LiteralPath {ps_quote(cache)})) {{ throw 'Windows CMake cache is missing' }}",
            f"$cacheText = Get-Content -LiteralPath {ps_quote(cache)} -Raw",
            "if ($cacheText -match '(?m)^DUSKSTUDIO_REQUIRE_ASIO:[^=]+=OFF\\r?$') { throw 'ASIO is explicitly disabled in the Windows CMake cache' }",
            "$asioMatch = [regex]::Match($cacheText, '(?m)^ASIOSDK_PATH:[^=]+=(.+)\\r?$')",
            "if (-not $asioMatch.Success) { throw 'ASIOSDK_PATH is absent from the Windows CMake cache' }",
            "$cachedSdk = [IO.Path]::GetFullPath($asioMatch.Groups[1].Value.Trim()).TrimEnd('\\')",
            "if ($cachedSdk -ne $expectedSdk) { throw \"CMake uses ASIO SDK '$cachedSdk', expected '$expectedSdk'\" }",
            f"if (-not (Test-Path -LiteralPath {ps_quote(project)})) {{ throw 'DuskStudio.vcxproj is missing' }}",
            f"$projectText = Get-Content -LiteralPath {ps_quote(project)} -Raw",
            "if (-not $projectText.Contains('JUCE_ASIO=1')) { throw 'DuskStudio target is missing JUCE_ASIO=1' }",
            'Write-Output "ASIO enabled: $cachedSdk"',
        ]
    )


def native_clap_posix_script(app: str, fixture: str, runner: str = "") -> str:
    launch = f"{shell_quote(runner)} {shell_quote(app)}" if runner else shell_quote(app)
    return "\n".join(
        [
            "set -euo pipefail",
            'stdout_file="$(mktemp)"',
            'stderr_file="$(mktemp)"',
            'cleanup() { rm -f "$stdout_file" "$stderr_file"; }',
            "trap cleanup EXIT",
            "status=0",
            "env DUSKSTUDIO_RUN_SELFTEST=1 DUSKSTUDIO_CLAP_STATE_TEST_ONLY=1 "
            f"DUSKSTUDIO_CLAP_STATE_FIXTURE={shell_quote(fixture)} {launch} "
            '>"$stdout_file" 2>"$stderr_file" || status=$?',
            'sed "s/^/[stdout] /" "$stdout_file"',
            'sed "s/^/[stderr] /" "$stderr_file" >&2',
            'if [ "$status" -ne 0 ]; then',
            '  echo "CLAP state harness exited $status" >&2',
            "  exit 1",
            "fi",
            "grep -Fq '[PASS] Native CLAP track + aux session state round-trip' \"$stdout_file\"",
            "! grep -Fq '[FAIL]' \"$stdout_file\"",
            "grep -Eq 'track CLAP .* rejected its saved state' \"$stderr_file\"",
            "grep -Eq 'aux CLAP .* rejected its saved state' \"$stderr_file\"",
        ]
    )


def run_linux(driver: Driver, commit: str) -> None:
    args = driver.args
    source = str(Path(args.linux_source).resolve())
    build = (
        str(Path(source, args.linux_build_dir).resolve())
        if not Path(args.linux_build_dir).is_absolute()
        else args.linux_build_dir
    )
    test = f"{build}/tests/dusk-studio-tests"
    app = f"{build}/DuskStudio_artefacts/Release/DuskStudio"
    fixture = f"{build}/dusk-studio-multi-bus-clap-fixture.clap"

    driver.local_bash(
        "Linux: verify checkout",
        posix_checkout_script(
            source, commit, False, require_clean=not args.allow_dirty
        ),
    )
    if not args.skip_build:
        targets = ["DuskStudio", "dusk-studio-tests"]
        if args.native_state:
            targets.append("dusk-studio-multi-bus-clap-fixture")
        driver.command(
            "Linux: build",
            ["cmake", "--build", build, "--target", *targets, f"-j{args.jobs}"],
        )
    for test_filter in args.filters:
        driver.command("Linux: focused tests", [test, test_filter])
    if args.native_state:
        script = native_clap_posix_script(
            app, fixture, f"{source}/scripts/run-selftest-xvfb.sh"
        )
        driver.local_bash("Linux: full-engine CLAP state", script)
    if not args.skip_full:
        driver.command(
            "Linux: full CTest", ["ctest", "--test-dir", build, "--output-on-failure"]
        )


def run_macos(driver: Driver, commit: str) -> None:
    args = driver.args
    source = args.mac_source.rstrip("/")
    build = posix_build_path(source, args.mac_build_dir)
    test = f"{build}/tests/dusk-studio-tests"
    app = (
        f"{build}/DuskStudio_artefacts/Release/DuskStudio.app/Contents/MacOS/DuskStudio"
    )
    fixture = f"{build}/dusk-studio-multi-bus-clap-fixture.clap"
    cmake = args.mac_cmake
    ctest = str(Path(cmake).with_name("ctest")) if "/" in cmake else "ctest"

    driver.mac_bash(
        "macOS: verify/sync checkout", posix_checkout_script(source, commit, args.sync)
    )
    if not args.skip_build:
        targets = ["DuskStudio", "dusk-studio-tests"]
        if args.native_state:
            targets.append("dusk-studio-multi-bus-clap-fixture")
        command = " ".join(
            [
                shell_quote(cmake),
                "--build",
                shell_quote(build),
                "--target",
                *(shell_quote(target) for target in targets),
                f"-j{args.jobs}",
            ]
        )
        driver.mac_bash("macOS: build", "set -euo pipefail\n" + command)
    for test_filter in args.filters:
        driver.mac_bash(
            "macOS: focused tests",
            f"set -euo pipefail\n{shell_quote(test)} {shell_quote(test_filter)}",
        )
    if args.native_state:
        driver.mac_bash(
            "macOS: stock Audio Unit tests",
            f"set -euo pipefail\n{shell_quote(test)} '[au]'",
        )
        driver.mac_bash(
            "macOS: full-engine CLAP state", native_clap_posix_script(app, fixture)
        )
    if not args.skip_full:
        driver.mac_bash(
            "macOS: full CTest",
            f"set -euo pipefail\n{shell_quote(ctest)} --test-dir {shell_quote(build)} --output-on-failure",
        )


def run_windows(driver: Driver, commit: str) -> None:
    args = driver.args
    source = args.windows_source
    build = windows_build_path(source, args.windows_build_dir)
    test = ps_join(build, r"tests\Release\dusk-studio-tests.exe")
    app = ps_join(build, r"DuskStudio_artefacts\Release\DuskStudio.exe")
    fixture = ps_join(build, r"Release\dusk-studio-multi-bus-clap-fixture.clap")
    cmake = args.windows_cmake
    ctest = str(PureWindowsPath(cmake).with_name("ctest.exe"))

    driver.windows_ps(
        "Windows: verify/sync checkout",
        windows_checkout_script(source, commit, args.sync),
    )
    if not args.skip_build:
        targets = ["DuskStudio", "dusk-studio-tests"]
        if args.native_state:
            targets.append("dusk-studio-multi-bus-clap-fixture")
        target_args = ", ".join(ps_quote(target) for target in targets)
        script = "\n".join(
            [
                "$ErrorActionPreference = 'Stop'",
                f"$arguments = @('--build', {ps_quote(build)}, '--config', 'Release', '--target', {target_args}, '-j{args.jobs}')",
                f"& {ps_quote(cmake)} @arguments",
                "if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }",
            ]
        )
        driver.windows_ps("Windows: build", script)
    driver.windows_ps(
        "Windows: require ASIO",
        windows_asio_preflight_script(build, args.windows_asio_sdk),
    )
    for test_filter in args.filters:
        script = "\n".join(
            [
                "$ErrorActionPreference = 'Stop'",
                f"& {ps_quote(test)} {ps_quote(test_filter)}",
                "exit $LASTEXITCODE",
            ]
        )
        driver.windows_ps("Windows: focused tests", script)
    if args.native_state:
        vst3 = ps_quote(args.windows_vst3)
        state_script = "\n".join(
            [
                "$ErrorActionPreference = 'Stop'",
                f"if (-not (Test-Path -LiteralPath {vst3})) {{ throw 'missing live VST3 fixture' }}",
                f"$env:DUSKSTUDIO_TEST_VST3 = {vst3}",
                f"& {ps_quote(test)} '[vst3][instance]' -c 'state round-trips into a fresh instance'",
                "if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }",
                f"& {ps_quote(test)} '[vst3][slot]' -c 'state round-trips through the slot'",
                "exit $LASTEXITCODE",
            ]
        )
        driver.windows_ps("Windows: live VST3 state", state_script)

        token = commit[:12]
        clap_script = "\n".join(
            [
                "$ErrorActionPreference = 'Stop'",
                f"$stdoutFile = Join-Path $env:TEMP {ps_quote(f'dusk-clap-{token}-stdout.txt')}",
                f"$stderrFile = Join-Path $env:TEMP {ps_quote(f'dusk-clap-{token}-stderr.txt')}",
                "Remove-Item -LiteralPath $stdoutFile,$stderrFile -Force -ErrorAction SilentlyContinue",
                f"$env:DUSKSTUDIO_CLAP_STATE_FIXTURE = {ps_quote(fixture)}",
                "$env:DUSKSTUDIO_RUN_SELFTEST = '1'",
                "$env:DUSKSTUDIO_CLAP_STATE_TEST_ONLY = '1'",
                f"$proc = Start-Process -FilePath {ps_quote(app)} -NoNewWindow -Wait -PassThru -RedirectStandardOutput $stdoutFile -RedirectStandardError $stderrFile",
                "$stdoutText = Get-Content -LiteralPath $stdoutFile -Raw",
                "$stderrText = Get-Content -LiteralPath $stderrFile -Raw",
                "Write-Output '[stdout]'",
                "Write-Output $stdoutText",
                "Write-Output '[stderr]'",
                "Write-Output $stderrText",
                'if ($proc.ExitCode -ne 0) { throw "CLAP state harness exited $($proc.ExitCode)" }',
                "if (-not $stdoutText.Contains('[PASS] Native CLAP track + aux session state round-trip')) { throw 'CLAP state PASS marker missing' }",
                "if ($stdoutText.Contains('[FAIL]')) { throw 'CLAP state harness printed FAIL' }",
                "if ($stderrText -notmatch 'track CLAP .* rejected its saved state') { throw 'track CLAP rejection diagnostic missing' }",
                "if ($stderrText -notmatch 'aux CLAP .* rejected its saved state') { throw 'aux CLAP rejection diagnostic missing' }",
                "Remove-Item -LiteralPath $stdoutFile,$stderrFile -Force -ErrorAction SilentlyContinue",
            ]
        )
        driver.windows_ps("Windows: full-engine CLAP state", clap_script)
    if not args.skip_full:
        ctest_args = [
            "'--test-dir'",
            ps_quote(build),
            "'-C'",
            "'Release'",
            "'--output-on-failure'",
        ]
        if args.windows_ctest_exclude:
            ctest_args.extend(["'-E'", ps_quote(args.windows_ctest_exclude)])
        script = "\n".join(
            [
                "$ErrorActionPreference = 'Stop'",
                f"& {ps_quote(ctest)} {' '.join(ctest_args)}",
                "exit $LASTEXITCODE",
            ]
        )
        driver.windows_ps("Windows: full CTest", script)


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    args = parse_args(repo_root)
    linux_source = Path(args.linux_source).resolve()
    commit = resolve_commit(linux_source, args.commit, args.dry_run)
    require_local_clean(linux_source, args.allow_dirty, args.dry_run)

    print(f"Dusk Studio cross-platform validation\ncommit: {commit}", flush=True)
    if args.allow_dirty:
        print(
            "warning: local dirty changes are not present on the remote hosts",
            file=sys.stderr,
        )

    driver = Driver(args)
    if not args.skip_linux:
        driver.platform("Linux", lambda: run_linux(driver, commit))
    if not args.skip_macos:
        driver.platform("macOS", lambda: run_macos(driver, commit))
    if not args.skip_windows:
        driver.platform("Windows", lambda: run_windows(driver, commit))

    print("\nValidation summary")
    for name, status, elapsed, detail in driver.platform_results:
        suffix = f": {detail}" if detail else ""
        print(f"  {name:<8} {status:<4} {elapsed:7.1f}s{suffix}")
    if not driver.platform_results:
        print(
            "error: every platform was skipped; nothing was validated", file=sys.stderr
        )
        return 1
    return (
        1 if any(status != "PASS" for _, status, _, _ in driver.platform_results) else 0
    )


if __name__ == "__main__":
    raise SystemExit(main())
