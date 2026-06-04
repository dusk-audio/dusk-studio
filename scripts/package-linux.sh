#!/usr/bin/env bash
# Build a Linux .deb or .rpm via CPack. Generators are configured in
# the top-level CMakeLists.txt; this script just dispatches cpack with
# the right -G flag and stages the output. Run on a Debian/Ubuntu host
# for .deb (uses dpkg-shlibdeps) or a Fedora/RHEL host for .rpm (uses
# rpmbuild). Cross-distro packaging needs Docker; out of scope here.
#
# Usage:    scripts/package-linux.sh deb
#           scripts/package-linux.sh rpm
#
# Prerequisite: build-linux/ already configured + built (Release).

set -euo pipefail

if [[ $# -ne 1 ]] || [[ "$1" != "deb" && "$1" != "rpm" ]]; then
    echo "usage: $0 <deb|rpm>" >&2
    exit 2
fi
FORMAT="$1"

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_DIR"

BUILD_DIR="${BUILD_DIR:-build-linux}"
if [[ ! -d "$BUILD_DIR" ]]; then
    echo "error: $BUILD_DIR missing - run: cmake -S . -B $BUILD_DIR -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD_DIR -j" >&2
    exit 1
fi

GENERATOR=""
case "$FORMAT" in
    deb) GENERATOR="DEB" ;;
    rpm) GENERATOR="RPM" ;;
esac

if [[ "$FORMAT" == "deb" ]] && ! command -v dpkg-shlibdeps >/dev/null 2>&1; then
    echo "error: dpkg-shlibdeps missing - on Debian/Ubuntu: sudo apt install dpkg-dev" >&2
    exit 1
fi
if [[ "$FORMAT" == "rpm" ]] && ! command -v rpmbuild >/dev/null 2>&1; then
    echo "error: rpmbuild missing - on Fedora/RHEL: sudo dnf install rpm-build" >&2
    exit 1
fi

pushd "$BUILD_DIR" >/dev/null
cpack -G "$GENERATOR" -C Release
popd >/dev/null

# Move output to repo root for predictable downstream paths.
shopt -s nullglob
case "$FORMAT" in
    deb) for f in "$BUILD_DIR"/*.deb; do mv "$f" .; echo "Built: $(basename "$f")"; sha256sum "$(basename "$f")" >> SHA256SUMS.linux; done ;;
    rpm) for f in "$BUILD_DIR"/*.rpm; do mv "$f" .; echo "Built: $(basename "$f")"; sha256sum "$(basename "$f")" >> SHA256SUMS.linux; done ;;
esac
