#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == --namespace ]]; then
    test_binary="${2:?}"
    capacity_root="${3:?}"
    mounted=false
    trap 'if [[ "$mounted" == true ]]; then umount -- "$capacity_root"; fi' EXIT
    for test_filter in '[sfz-disk-full]' '[sfz-service-disk-full]' '[sfz-location-disk-full]'; do
        for mode in inodes bytes; do
            inodes=64
            if [[ "$mode" == inodes ]]; then inodes=1; fi
            if ! mount -t tmpfs -o "size=64k,nr_inodes=$inodes,mode=700" tmpfs "$capacity_root"; then
                echo 'skip: cannot mount the private capacity fixture'
                exit 77
            fi
            mounted=true
            status=0
            DUSK_SFZ_CAPACITY_ROOT="$capacity_root" DUSK_SFZ_CAPACITY_MODE="$mode" \
                "$test_binary" "$test_filter" || status=$?
            umount -- "$capacity_root"
            mounted=false
            if [[ "$status" -ne 0 ]]; then exit "$status"; fi
        done
    done
    exit 0
fi

test_binary="${1:?usage: sfz_disk_full.sh <test-binary>}"
if ! command -v unshare >/dev/null 2>&1 \
    || ! unshare --map-root-user --mount --propagation private true 2>/dev/null; then
    echo 'skip: no usable private user and mount namespace'
    exit 77
fi
capacity_root="$(mktemp -d "${TMPDIR:-/tmp}/dusk-sfz-capacity.XXXXXXXX")"
capacity_root="$(cd "$capacity_root" && pwd -P)"
trap 'rmdir -- "$capacity_root"' EXIT
unshare --map-root-user --mount --propagation private \
    bash "$0" --namespace "$test_binary" "$capacity_root"
