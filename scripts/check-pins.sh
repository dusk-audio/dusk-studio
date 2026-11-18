#!/usr/bin/env bash
# Says whether DONOR_REV and DAF_REV still hold what the two repositories have
# released.
#   - Dusk plugins: the newest release tag of every plugin Dusk Studio builds in
#     must be an ancestor of DONOR_REV. A tag with a suffix after its version
#     (-alpha1, -rc2) is not a release. Ancestry is all it proves: the pin's
#     tree may still hold a path back, as it does plugins/multi-comp.
#   - DAF has no release tags: DAF_REV must be on its main branch, and main
#     must hold nothing newer outside its tests, examples, CI and documents.
#
# Usage:   scripts/check-pins.sh [--cache-dir DIR] [--max-age-hours N]
#
# Exit 0: both pins are current. Exit 1: one is behind, and each line printed
# names what it lacks. Exit 3: could not tell (no network, no git) and nothing
# is known to be behind.
#
# History is kept as two commit-only clones under --cache-dir (default
# build/_deps/pin-check). With --max-age-hours the last answer for the same two
# pins is printed again without touching the network while it is younger than N
# hours, which is what lets every build ask. An answer that left a repository
# unchecked is kept for an hour at most.

set -uo pipefail

DONOR_URL=https://github.com/dusk-audio/dusk-audio-plugins.git
DAF_URL=https://github.com/dusk-audio/DAF.git
# One tag family per donor plugin compiled into the app: <family>-v<version>.
DONOR_FAMILIES=(4k-eq-2 duskverb-2 tapemachine-2 tape-echo-2 sunset-circuits multi-q)
# What a DAF commit can change without changing a Dusk Studio build.
DAF_IGNORED=(':(exclude).github' ':(exclude)tests' ':(exclude)examples' ':(exclude,glob)**/*.md')

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cache_dir="${root}/build/_deps/pin-check"
max_age_hours=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --cache-dir)     cache_dir="$2"; shift 2 ;;
        --max-age-hours) max_age_hours="$2"; shift 2 ;;
        *) echo "usage: $0 [--cache-dir DIR] [--max-age-hours N]" >&2; exit 2 ;;
    esac
done
[[ "${max_age_hours}" =~ ^[0-9]+$ ]] || { echo "error: --max-age-hours wants a whole number" >&2; exit 2; }

read_pin() {
    local rev
    rev="$(tr -d '[:space:]' < "${root}/$1" 2>/dev/null)"
    [[ "${rev}" =~ ^[0-9a-f]{40}$ ]] || { echo "error: $1 does not hold a 40-character commit" >&2; exit 2; }
    printf '%s' "${rev}"
}
donor_rev="$(read_pin DONOR_REV)" || exit 2
daf_rev="$(read_pin DAF_REV)" || exit 2

command -v git >/dev/null || { echo "pins: not checked, git is missing"; exit 3; }
[[ "$(printf '2\n10\n' | sort -V 2>/dev/null | tail -n 1)" == 10 ]] \
    || { echo "pins: not checked, this sort cannot order versions"; exit 3; }
mkdir -p "${cache_dir}" || { echo "pins: not checked, cannot write ${cache_dir}"; exit 3; }

# The stamp holds when it was written, the two pins, the exit code, how many
# seconds the answer may be reused for, and the lines printed. It is replaced by
# a rename and read in one piece, so a reader never sees half of two answers.
stamp="${cache_dir}/last-answer"
write_stamp() {
    local status="$1" keep="$2"; shift 2
    {
        date +%s
        echo "${donor_rev} ${daf_rev}"
        echo "${status}"
        echo "${keep}"
        printf '%s\n' "$@"
    } > "${stamp}.$$" && mv -f "${stamp}.$$" "${stamp}"
}

if [[ "${max_age_hours}" -gt 0 && -f "${stamp}" ]]; then
    answer="$(cat "${stamp}" 2>/dev/null)"
    stamp_time="$(sed -n 1p <<< "${answer}")"
    stamp_status="$(sed -n 3p <<< "${answer}")"
    stamp_keep="$(sed -n 4p <<< "${answer}")"
    if [[ "${stamp_time}" =~ ^[0-9]+$ && "${stamp_status}" =~ ^[013]$ && "${stamp_keep}" =~ ^[0-9]+$ ]] \
        && [[ "$(sed -n 2p <<< "${answer}")" == "${donor_rev} ${daf_rev}" ]]; then
        keep=$(( max_age_hours * 3600 ))
        [[ "${stamp_keep}" -ge "${keep}" ]] || keep="${stamp_keep}"
        if [[ $(( $(date +%s) - stamp_time )) -lt "${keep}" ]]; then
            sed -n '5,$p' <<< "${answer}"
            exit "${stamp_status}"
        fi
    fi
fi

# A caller that gives up on a fetch that hangs leaves this answer behind, so the
# next build does not wait on the same fetch.
write_stamp 3 3600 "pins: not checked, the last check did not finish"

export GIT_TERMINAL_PROMPT=0
fetch() {
    local repo="$1" filter="$2" url="$3"; shift 3
    [[ -d "${repo}" ]] || git init -q --bare "${repo}" || return 1
    git -C "${repo}" -c http.lowSpeedLimit=1000 -c http.lowSpeedTime=15 \
        fetch -q --filter="${filter}" --no-tags --force --prune "${url}" "$@" 2>/dev/null
}

lines=()
behind=0
unknown=0

donor="${cache_dir}/donor.git"
if fetch "${donor}" tree:0 "${DONOR_URL}" '+refs/tags/*:refs/tags/*' "${donor_rev}" \
    && donor_tags="$(git -C "${donor}" tag -l)"; then
    for family in "${DONOR_FAMILIES[@]}"; do
        newest="$(grep -E "^${family}-v[0-9]+(\.[0-9]+)*$" <<< "${donor_tags}" | sort -V | tail -n 1)"
        [[ -n "${newest}" ]] || continue
        git -C "${donor}" merge-base --is-ancestor "${newest}^{commit}" "${donor_rev}" 2>/dev/null
        case $? in
            0) ;;
            1)  day="$(git -C "${donor}" log -1 --format=%cd --date=short "${newest}^{commit}")"
                lines+=("pins: DONOR_REV ${donor_rev:0:8} lacks ${newest} (${day})")
                behind=1 ;;
            *)  lines+=("pins: ${newest} not checked, git could not compare it with DONOR_REV")
                unknown=1 ;;
        esac
    done
else
    lines+=("pins: Dusk plugins not checked, could not fetch ${DONOR_URL}")
    unknown=1
fi

# Trees, not only commits: the comparison below is of paths.
daf="${cache_dir}/daf.git"
if fetch "${daf}" blob:none "${DAF_URL}" '+refs/heads/main:refs/heads/main' "${daf_rev}" \
    && daf_head="$(git -C "${daf}" rev-parse --verify -q refs/heads/main)"; then
    git -C "${daf}" merge-base --is-ancestor "${daf_rev}" "${daf_head}" 2>/dev/null
    case $? in
        0)  git -C "${daf}" diff --quiet "${daf_rev}" "${daf_head}" -- . "${DAF_IGNORED[@]}" 2>/dev/null
            case $? in
                0) ;;
                1)  count="$(git -C "${daf}" rev-list --count "${daf_rev}..${daf_head}")"
                    lines+=("pins: DAF_REV ${daf_rev:0:8} is ${count} commits behind DAF main ${daf_head:0:8}")
                    behind=1 ;;
                *)  lines+=("pins: DAF not checked, git could not compare DAF_REV with main")
                    unknown=1 ;;
            esac ;;
        1)  lines+=("pins: DAF_REV ${daf_rev:0:8} is not on DAF main (${daf_head:0:8})")
            behind=1 ;;
        *)  lines+=("pins: DAF not checked, git could not compare DAF_REV with main")
            unknown=1 ;;
    esac
else
    lines+=("pins: DAF not checked, could not fetch ${DAF_URL}")
    unknown=1
fi

status=0
[[ "${unknown}" -eq 0 ]] || status=3
[[ "${behind}" -eq 0 ]] || status=1
[[ "${status}" -ne 0 ]] || lines+=("pins: DONOR_REV ${donor_rev:0:8} and DAF_REV ${daf_rev:0:8} are current")

keep=$(( 365 * 24 * 3600 ))
[[ "${unknown}" -eq 0 ]] || keep=3600
write_stamp "${status}" "${keep}" "${lines[@]}"
printf '%s\n' "${lines[@]}"
exit "${status}"
