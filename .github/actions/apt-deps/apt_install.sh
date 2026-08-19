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
#   MIRROR PINNING. azure.archive.ubuntu.com and archive.ubuntu.com take
#   turns being the sick one; the arm64 ports archive adds ports.ubuntu.com
#   (under-resourced) vs mirrors.mit.edu. Pinning EITHER side is the bug, so
#   after the first failed attempt this script switches to the other mirror
#   in the machine's family and keeps going.
#
# Families:
#   x86 runners:  azure.archive.ubuntu.com <-> archive.ubuntu.com
#   arm64 runners (ports): mirrors.mit.edu <-> ports.ubuntu.com
#     ports.ubuntu.com is regularly unreachable from GitHub's ARM runners on
#     both stacks, so MIT is normalized to primary up front (preserving the
#     old raspberry-pi-build behaviour) and IPv4 is forced, which that
#     workflow had established as necessary.
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

AZURE_MIRROR="http://azure.archive.ubuntu.com/ubuntu"
STOCK_MIRROR="http://archive.ubuntu.com/ubuntu"
MIT_PORTS="http://mirrors.mit.edu/ubuntu-ports"
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

# Both list formats: 24.04 images ship deb822 .sources files and no
# sources.list.
mirror_files() {
    local f
    for f in /etc/apt/sources.list \
             /etc/apt/sources.list.d/*.list \
             /etc/apt/sources.list.d/*.sources; do
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
    if sources_match "ports\.ubuntu\.com" || sources_match "mirrors\.mit\.edu/ubuntu-ports"; then
        echo ports
    else
        echo archive
    fi
}

if [ "$(mirror_family)" = "ports" ]; then
    # arm64: normalize everything (azure.ports included) onto MIT up front and
    # force IPv4, matching what raspberry-pi-build.yml had proven necessary.
    rewrite_sources "http://azure.ports.ubuntu.com/ubuntu-ports" "$MIT_PORTS"
    rewrite_sources "$STOCK_PORTS" "$MIT_PORTS"
    echo 'Acquire::ForceIPv4 "true";' | "${SUDO[@]}" tee /etc/apt/apt.conf.d/99force-ipv4 >/dev/null
fi

current_mirror() {
    if [ "$(mirror_family)" = "ports" ]; then
        if sources_match "mirrors\.mit\.edu"; then echo mit; else echo stock; fi
    else
        if sources_match "azure\."; then echo azure; else echo stock; fi
    fi
}

# Move to whichever mirror we are NOT on, within this machine's family.
# Called once, after the first failure.
switch_mirror() {
    local to
    if [ "$(mirror_family)" = "ports" ]; then
        if [ "$(current_mirror)" = "mit" ]; then
            to="$STOCK_PORTS"; rewrite_sources "$MIT_PORTS" "$to"
        else
            to="$MIT_PORTS"; rewrite_sources "$STOCK_PORTS" "$to"
        fi
    else
        if [ "$(current_mirror)" = "azure" ]; then
            to="$STOCK_MIRROR"; rewrite_sources "$AZURE_MIRROR" "$to"
        else
            to="$AZURE_MIRROR"; rewrite_sources "$STOCK_MIRROR" "$to"
        fi
        # security.ubuntu.com is a separate host that stalls independently.
        rewrite_sources "http://security.ubuntu.com/ubuntu" "$to"
    fi
    echo "::notice::apt switching mirror to ${to}"
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
        # dpkg -s alone would not be proof either: it succeeds for a package
        # left in config-files state, which has no headers or binaries. Only
        # the installed status counts.
        missing=()
        for pkg in ${packages+"${packages[@]}"}; do
            [ "$(dpkg-query -W -f='${db:Status-Status}' "$pkg" 2>/dev/null)" = installed ] \
                || missing+=("$pkg")
        done
        if [ ${#missing[@]} -eq 0 ]; then
            exit 0
        fi
        echo "::warning::apt reported success but these are missing: ${missing[*]}"
    fi

    if [ "$i" -eq "$ATTEMPTS" ]; then
        echo "::error::apt failed after ${ATTEMPTS} attempts (mirror: $(current_mirror))"
        exit 1
    fi

    echo "::warning::apt attempt ${i} failed or timed out; retrying in ${RETRY_SLEEP}s"
    [ "$i" -eq 1 ] && switch_mirror
    sleep "$RETRY_SLEEP"
done
