#!/usr/bin/env bash
# Prove a published artifact runs on a machine that did not build it.
#
#   scripts/release-smoke-test.sh <linux|macos> <artifact> [expected-version]
#
#     linux   <dusk-studio-X.Y.Z-Linux-<arch>.tar.xz>
#     macos   <dusk-studio-X.Y.Z-macOS-arm64.dmg>
#
# The expected version defaults to the one in the artifact's file name, so a
# mismatch between the name and what the binary reports is itself a failure.
#
# Read-only with respect to the artifact: everything is unpacked or mounted
# into a scratch directory that is removed on exit. Nothing is installed for
# the user, and no system location is touched.
#
# On Linux the app is launched under a private Xvfb display with
# WAYLAND_DISPLAY cleared. Never run this against a live session: the binary
# takes an audio device and a GL context.
#
# Each check prints one PASS or FAIL line. A single FAIL fails the run, and
# every check still runs, so one invocation reports the whole picture.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONTENTS_CHECK="${REPO_ROOT}/scripts/verify-package-contents.sh"

usage() {
    echo "usage: $0 <linux|macos> <artifact> [expected-version]" >&2
    exit 2
}

[[ $# -ge 2 && $# -le 3 ]] || usage
PLATFORM="$1"
ARTIFACT="$2"
case "$PLATFORM" in linux|macos) ;; *) usage ;; esac
[[ -f "$ARTIFACT" ]] || { echo "error: no such artifact: $ARTIFACT" >&2; exit 2; }

# dusk-studio-0.13.3-Linux-x86_64.tar.xz -> 0.13.3
derive_version() {
    local base
    base="$(basename "$1")"
    base="${base#dusk-studio-}"
    printf '%s' "${base%%-*}"
}
EXPECTED_VERSION="${3:-$(derive_version "$ARTIFACT")}"
[[ -n "$EXPECTED_VERSION" ]] || { echo "error: could not derive a version" >&2; exit 2; }

failures=0
pass() { printf 'PASS  %s\n' "$1"; }
fail() { printf 'FAIL  %s\n' "$1"; failures=$((failures + 1)); }

WORK="$(mktemp -d)"
MOUNT=""
cleanup() {
    [[ -n "$MOUNT" ]] && hdiutil detach "$MOUNT" >/dev/null 2>&1
    [[ -n "${XVFB_PID:-}" ]] && kill "$XVFB_PID" >/dev/null 2>&1
    rm -rf "$WORK"
    return 0
}
trap cleanup EXIT

# --- Unpack -----------------------------------------------------------------
ROOT=""
APP=""
if [[ "$PLATFORM" == "linux" ]]; then
    if tar -xf "$ARTIFACT" -C "$WORK" 2>"$WORK/untar.err"; then
        ROOT="$(find "$WORK" -mindepth 1 -maxdepth 1 -type d -print -quit)"
        APP="$ROOT/DuskStudio/DuskStudio"
        pass "unpack: $(basename "$ARTIFACT")"
    else
        fail "unpack: $(cat "$WORK/untar.err")"
    fi
else
    MOUNT="$WORK/mnt"
    mkdir -p "$MOUNT"
    # The image carries a licence agreement, so hdiutil blocks for an answer and
    # closes stdin once it has one. Read hdiutil's status, not the pipeline's,
    # which would come from the SIGPIPEd yes.
    set +o pipefail
    yes | hdiutil attach -nobrowse -readonly -noverify -noautoopen \
                  -mountpoint "$MOUNT" "$ARTIFACT" >/dev/null 2>&1
    mount_rc=${PIPESTATUS[1]}
    set -o pipefail
    if [[ $mount_rc -eq 0 ]]; then
        ROOT="$MOUNT"
        APP="$MOUNT/DuskStudio.app/Contents/MacOS/DuskStudio"
        pass "mount: $(basename "$ARTIFACT")"
    else
        MOUNT=""
        fail "mount: hdiutil returned $mount_rc"
    fi
fi

# --- Package contents -------------------------------------------------------
if [[ -n "$ROOT" ]]; then
    if [[ -x "$CONTENTS_CHECK" ]]; then
        if out="$("$CONTENTS_CHECK" "$PLATFORM" "$ROOT" 2>&1)"; then
            pass "package contents"
        else
            fail "package contents: $(printf '%s' "$out" | tr '\n' ' ')"
        fi
    else
        fail "package contents: $CONTENTS_CHECK is missing"
    fi
else
    fail "package contents: nothing was unpacked"
fi

# --- Checksum signature -----------------------------------------------------
# Only when the artifact was downloaded beside its checksum file: the smoke test
# takes one artifact, so this is skipped rather than failed when the release's
# SHA256SUMS and signature are not next to it.
ART_DIR="$(cd "$(dirname "$ARTIFACT")" && pwd)"
PUBKEY="${REPO_ROOT}/packaging/release-signing.pub"
if [[ -f "$ART_DIR/SHA256SUMS" && -f "$ART_DIR/SHA256SUMS.asc" ]]; then
    if ! grep -q 'BEGIN PGP PUBLIC KEY BLOCK' "$PUBKEY" 2>/dev/null; then
        echo "SKIP  checksum signature: no release key in packaging/release-signing.pub yet"
    elif ! command -v gpg >/dev/null 2>&1; then
        echo "SKIP  checksum signature: gpg is not installed"
    else
        keyring="$WORK/gnupg"
        mkdir -p "$keyring"
        chmod 700 "$keyring"
        if GNUPGHOME="$keyring" gpg --batch --quiet --import "$PUBKEY" 2>/dev/null \
           && GNUPGHOME="$keyring" gpg --batch --verify \
                "$ART_DIR/SHA256SUMS.asc" "$ART_DIR/SHA256SUMS" >/dev/null 2>&1; then
            pass "checksum signature"
        else
            fail "checksum signature does not verify against packaging/release-signing.pub"
        fi
        # The signature covers the checksum file; this is what ties the artifact
        # in hand to it.
        if ( cd "$ART_DIR" && sha256sum --ignore-missing --check SHA256SUMS >/dev/null 2>&1 ); then
            pass "checksum matches the artifact"
        else
            fail "the artifact's checksum is not the one SHA256SUMS records"
        fi
    fi
else
    echo "SKIP  checksum signature: SHA256SUMS and SHA256SUMS.asc are not beside the artifact"
fi

# --- Launch under a private display where one is needed ---------------------
run_app() {
    if [[ "$PLATFORM" == "linux" ]]; then
        env -u WAYLAND_DISPLAY DISPLAY="$DISPLAY_NUM" "$@"
    else
        "$@"
    fi
}

if [[ "$PLATFORM" == "linux" && -n "$ROOT" ]]; then
    if command -v Xvfb >/dev/null 2>&1; then
        DISPLAY_NUM=":$(( 90 + RANDOM % 8 ))"
        Xvfb "$DISPLAY_NUM" -screen 0 1280x800x24 >/dev/null 2>&1 &
        XVFB_PID=$!
        sleep 3
    else
        fail "Xvfb is required to launch the app headlessly"
        ROOT=""
    fi
fi

# --- Version ----------------------------------------------------------------
if [[ -n "$ROOT" && -x "$APP" ]]; then
    reported="$(run_app "$APP" --version 2>&1 | tr -d '\r')"
    version_rc=$?
    if [[ $version_rc -ne 0 ]]; then
        fail "--version exited $version_rc"
    elif [[ "$reported" == *"$EXPECTED_VERSION"* ]]; then
        pass "--version reports $EXPECTED_VERSION"
    else
        fail "--version reported \"$reported\", expected $EXPECTED_VERSION"
    fi
elif [[ -n "$ROOT" ]]; then
    fail "--version: no executable at $APP"
fi

# --- Headless self-test, bounded ---------------------------------------------
# Drives the full AudioPipelineSelfTest against the PACKAGED binary, not the
# build tree, which is the point of this script. The run also covers the plugin
# scan: a scan that hangs is the failure worth catching, and this is the run it
# would hang. One invocation, two verdicts.
#
# Bounded with a wait loop rather than `timeout`: macOS has no GNU timeout, and
# a check that cannot run on one of the two platforms is not much of a check.
wait_bounded() {
    local pid="$1" limit="$2" waited=0
    while kill -0 "$pid" 2>/dev/null; do
        if [[ $waited -ge $limit ]]; then
            kill -9 "$pid" 2>/dev/null
            wait "$pid" 2>/dev/null
            return 124
        fi
        sleep 1
        waited=$((waited + 1))
    done
    wait "$pid"
}

if [[ -n "$ROOT" && -x "$APP" ]]; then
    selftest_log="$WORK/selftest.log"
    run_app env DUSKSTUDIO_RUN_SELFTEST=1 "$APP" >"$selftest_log" 2>&1 &
    app_pid=$!
    wait_bounded "$app_pid" 180
    selftest_rc=$?

    if [[ $selftest_rc -eq 124 ]]; then
        fail "self-test did not terminate within 180s (a hung plugin scan looks like this)"
        fail "plugin scan terminates"
    else
        pass "plugin scan terminates"
        if [[ $selftest_rc -ne 0 ]]; then
            fail "self-test exited $selftest_rc (see the log above)"
        elif grep -q '^\[FAIL\]' "$selftest_log"; then
            fail "self-test reported: $(grep -m3 '^\[FAIL\]' "$selftest_log" | tr '\n' ' ')"
        else
            pass "headless self-test"
        fi
    fi
fi

echo
if [[ $failures -eq 0 ]]; then
    echo "release smoke test: all checks passed ($PLATFORM, $EXPECTED_VERSION)"
    exit 0
fi
echo "release smoke test: $failures check(s) failed ($PLATFORM, $EXPECTED_VERSION)" >&2
exit 1
