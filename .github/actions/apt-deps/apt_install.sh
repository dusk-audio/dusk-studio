#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# One hardened apt front end for every workflow in this repo. Ported from
# dusk-audio-plugins .github/scripts/apt_install.sh, where its two defences
# were earned the hard way:
#
#   STALL. An unreachable or byte-dribbling mirror does not fail apt, it
#   hangs it. Acquire::http::Timeout only bounds socket inactivity; a mirror
#   trickling bytes never trips it, so a retry loop alone never gets its
#   second attempt (run 32272023755 sat in apt-get for 70+ minutes with
#   retries "enabled"). Hence timeout(1) around every apt invocation.
#
#   WEDGED DPKG. A timed-out install can kill dpkg mid-transaction; every
#   later attempt then dies on "dpkg was interrupted" no matter how healthy
#   the network is. Hence a bounded `dpkg --configure -a` before each retry.
#
# Mirror policy (owner decision, 2026-08-19): stock Ubuntu repos ONLY
# (archive.ubuntu.com, ports.ubuntu.com, security.ubuntu.com). Azure's
# mirror is purged up front from every mirror file, including the hosted
# runners' /etc/apt/apt-mirrors.txt mirrorlist, which plain sources rewrites
# miss entirely. No third-party mirrors; Ubuntu 22.04 is supported until
# 2027 and the stock archive is the reference. arm64 keeps IPv4 forced,
# which raspberry-pi-build had established as necessary for ports.
#
# Usage:
#   apt_install.sh pkg [pkg...]   update, then install those packages
#   apt_install.sh --update-only  refresh the lists only
set -uo pipefail

ATTEMPTS="${APT_ATTEMPTS:-3}"
UPDATE_TIMEOUT="${APT_UPDATE_TIMEOUT:-120}"
INSTALL_TIMEOUT="${APT_INSTALL_TIMEOUT:-300}"
RETRY_SLEEP="${APT_RETRY_SLEEP:-10}"
APT_OPTS=(-o Acquire::Retries=3 -o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30)

# A typo'd override must fail loudly, not turn the installer into a no-op
# that exits 0 having installed nothing (seq on a non-number runs the loop
# body zero times).
require_positive_int() {
    case "$2" in
        '' | *[!0-9]* ) ;;
        * ) [ "$2" -gt 0 ] && return 0 ;;
    esac
    echo "::error::$1 must be a positive integer, got '$2'" >&2
    exit 2
}
require_positive_int APT_ATTEMPTS        "$ATTEMPTS"
require_positive_int APT_UPDATE_TIMEOUT  "$UPDATE_TIMEOUT"
require_positive_int APT_INSTALL_TIMEOUT "$INSTALL_TIMEOUT"
case "$RETRY_SLEEP" in
    '' | *[!0-9]* )
        echo "::error::APT_RETRY_SLEEP must be a non-negative integer, got '$RETRY_SLEEP'" >&2
        exit 2 ;;
esac

STOCK_MIRROR="http://archive.ubuntu.com/ubuntu"
STOCK_PORTS="http://ports.ubuntu.com/ubuntu-ports"

if [ "$(id -u)" -eq 0 ]; then
    SUDO=()
else
    SUDO=(sudo)
fi

update_only=0
if [ "${1:-}" = "--update-only" ]; then
    update_only=1
    shift
fi
packages=("$@")

if [ "$update_only" -eq 0 ] && [ ${#packages[@]} -eq 0 ]; then
    echo "apt_install: no packages given" >&2
    exit 2
fi

# All the places a mirror can hide: 24.04 images ship deb822 .sources files
# and no sources.list, and hosted runners route apt through the MIRRORLIST
# at /etc/apt/apt-mirrors.txt (sources say "mirror+file:..."), which a
# sources-only rewrite silently misses.
mirror_files() {
    local f
    for f in /etc/apt/sources.list \
             /etc/apt/sources.list.d/*.list \
             /etc/apt/sources.list.d/*.sources \
             /etc/apt/apt-mirrors.txt; do
        [ -f "$f" ] && printf '%s\n' "$f"
    done
}

sources_match() {
    local f
    while read -r f; do
        grep -q "$1" "$f" 2>/dev/null && return 0
    done < <(mirror_files)
    return 1
}

rewrite_sources() {
    local from="$1" to="$2"
    mirror_files | while read -r f; do
        "${SUDO[@]}" sed -i -e "s|${from}|${to}|g" "$f" || true
    done
}

mirror_family() {
    if sources_match "ports\.ubuntu\.com"; then
        echo ports
    else
        echo archive
    fi
}

# Purge azure from every mirror file (including the runner mirrorlist) so
# apt talks only to the stock Ubuntu archive; arm64 keeps IPv4 forced.
if [ "$(mirror_family)" = "ports" ]; then
    rewrite_sources "http://azure.ports.ubuntu.com/ubuntu-ports" "$STOCK_PORTS"
    echo 'Acquire::ForceIPv4 "true";' | "${SUDO[@]}" tee /etc/apt/apt.conf.d/99force-ipv4 >/dev/null
else
    rewrite_sources "http://azure.archive.ubuntu.com/ubuntu" "$STOCK_MIRROR"
fi

# A timed-out install can kill dpkg mid-transaction; repair before retrying.
repair_dpkg() {
    "${SUDO[@]}" timeout 120 env DEBIAN_FRONTEND=noninteractive \
        dpkg --configure -a || true
}

attempt() {
    "${SUDO[@]}" timeout "$UPDATE_TIMEOUT" apt-get update "${APT_OPTS[@]}" || return 1
    [ "$update_only" -eq 1 ] && return 0
    "${SUDO[@]}" timeout "$INSTALL_TIMEOUT" env DEBIAN_FRONTEND=noninteractive \
        apt-get install -y --no-install-recommends "${APT_OPTS[@]}" "${packages[@]}"
}

for i in $(seq 1 "$ATTEMPTS"); do
    if attempt; then
        # apt exiting 0 is not proof; verify the packages are present so a
        # miss fails here with a clear message, not three steps later.
        missing=()
        for pkg in ${packages+"${packages[@]}"}; do
            dpkg -s "$pkg" >/dev/null 2>&1 || missing+=("$pkg")
        done
        if [ ${#missing[@]} -eq 0 ]; then
            exit 0
        fi
        echo "::warning::apt reported success but these are missing: ${missing[*]}"
    fi

    if [ "$i" -eq "$ATTEMPTS" ]; then
        echo "::error::apt failed after ${ATTEMPTS} attempts (stock Ubuntu mirror)"
        exit 1
    fi

    echo "::warning::apt attempt ${i} failed or timed out; retrying in ${RETRY_SLEEP}s"
    repair_dpkg
    sleep "$RETRY_SLEEP"
done
