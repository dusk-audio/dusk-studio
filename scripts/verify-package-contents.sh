#!/usr/bin/env bash
# Check one built package against packaging/contents.txt.
#
#   scripts/verify-package-contents.sh linux   <unpacked tarball dir>
#   scripts/verify-package-contents.sh macos   <mounted DMG>
#   scripts/verify-package-contents.sh windows <7z extraction of the MSI>
#
# Read-only. Exits non-zero listing every missing path, so one run reports the
# whole gap rather than the first hole in it. A Markdown file in the contract
# also fails the package when it links a relative path the package lacks.
#
# Windows is matched by file name rather than path: an MSI is a database and 7z
# flattens it on extraction, so the layout the contract records cannot be
# checked there. The other two are checked as literal paths.
#
# A real extraction of our MSI produces names like
#
#   CM_FP_bin.DuskStudio.exe
#   CM_FP_bin.dusk_studio_plugin_host.exe
#   CM_FP_LICENSES.txt
#
# so the component name is a suffix of the extracted name, WiX has replaced the
# hyphens with underscores, and case is not guaranteed. Matching therefore
# lowercases, folds '-' to '_', and requires the expected name to sit at a '.'
# or '_' boundary, which keeps LICENSE from matching LICENSES.txt.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONTRACT="${DUSKSTUDIO_CONTENTS_CONTRACT:-${REPO_ROOT}/packaging/contents.txt}"

usage() {
    echo "usage: $0 <linux|macos|windows> <package-root>" >&2
    exit 2
}

# Lowercase and fold the separator WiX rewrites, so a contract path and an
# extracted MSI name can be compared at all. tr rather than ${x,,}: macOS ships
# bash 3.2, where that expansion is a syntax error at expansion time, which is
# why it survived a macOS run that only exercised the literal-path branch.
normalise() {
    printf '%s' "$1" | tr '\-' '_' | tr '[:upper:]' '[:lower:]'
}

# Print the first extracted file whose name ends in $1 at a '.' or '_' boundary.
findWindowsFile() {
    local name candidate base
    name="$(normalise "$1")"
    while IFS= read -r candidate; do
        base="$(normalise "${candidate##*/}")"
        if [[ "$base" == "$name" || "$base" == *".${name}" || "$base" == *"_${name}" ]]; then
            printf '%s\n' "$candidate"
            return
        fi
    done < <(find "$ROOT" -type f)
}

# Print the physical path of an existing $1, every symlink resolved including
# the last component, or nothing when it does not exist. readlink -f and realpath
# are not on every macOS the release runs on, so the chain is walked by hand.
physicalPath() {
    local path="$1" link dir hops=0
    while [[ -L "$path" ]]; do
        hops=$((hops + 1))
        [[ $hops -le 40 ]] || return 0
        link="$(readlink "$path")"
        [[ "$link" == /* ]] || link="$(dirname "$path")/$link"
        path="$link"
    done
    if [[ -d "$path" ]]; then
        (cd "$path" 2>/dev/null && pwd -P) || true
    elif [[ -e "$path" ]]; then
        dir="$(cd "$(dirname "$path")" 2>/dev/null && pwd -P)" || return 0
        printf '%s/%s\n' "$dir" "${path##*/}"
    fi
}

[[ $# -eq 2 ]] || usage
PLATFORM="$1"
ROOT="$2"

case "$PLATFORM" in
    linux|macos|windows) ;;
    *) usage ;;
esac

[[ -d "$ROOT" ]] || { echo "error: package root is not a directory: $ROOT" >&2; exit 2; }
[[ -f "$CONTRACT" ]] || { echo "error: contract not found: $CONTRACT" >&2; exit 2; }
ROOT_PHYSICAL="$(cd "$ROOT" && pwd -P)"

expected=()
lineNo=0
while IFS= read -r line || [[ -n "$line" ]]; do
    lineNo=$((lineNo + 1))
    # Git for Windows checks the contract out with CRLF by default; a trailing
    # CR would otherwise become part of every expected name.
    line="${line%$'\r'}"
    [[ -z "${line//[[:space:]]/}" ]] && continue
    [[ "$line" == \#* ]] && continue
    # Fail closed: a record that is not exactly <platform><TAB><path> means the
    # contract was edited into a shape this cannot read, and silently skipping
    # it would drop whatever it required.
    if [[ "$line" != *$'\t'* ]] || [[ "$(awk -F'\t' '{print NF}' <<< "$line")" != "2" ]]; then
        echo "error: ${CONTRACT}:${lineNo}: expected <platform><TAB><path>, got: $line" >&2
        exit 2
    fi
    recordPlatform="${line%%$'\t'*}"
    recordPath="${line#*$'\t'}"
    # An empty path would pass vacuously: "$ROOT/" is a directory and so always
    # exists, which turns a mistyped record into a check that cannot fail.
    if [[ -z "${recordPath//[[:space:]]/}" ]]; then
        echo "error: ${CONTRACT}:${lineNo}: record has an empty path" >&2
        exit 2
    fi
    case "$recordPlatform" in
        linux|macos|windows) ;;
        *) echo "error: ${CONTRACT}:${lineNo}: unknown platform '$recordPlatform'" >&2; exit 2 ;;
    esac
    [[ "$recordPlatform" == "$PLATFORM" ]] && expected+=("$recordPath")
done < "$CONTRACT"

if [[ ${#expected[@]} -eq 0 ]]; then
    echo "error: contract lists nothing for platform '$PLATFORM'" >&2
    exit 2
fi

missing=()
for path in "${expected[@]}"; do
    if [[ "$PLATFORM" == "windows" ]]; then
        [[ -n "$(findWindowsFile "${path##*/}")" ]] || missing+=("$path")
    elif [[ "$PLATFORM" == "macos" && "$path" == "Applications" ]]; then
        if [[ ! -L "$ROOT/$path" || "$(readlink "$ROOT/$path")" != "/Applications" ]]; then
            missing+=("$path")
        fi
    elif [[ "$PLATFORM" == "macos" && "$path" == */Contents/Resources/QUICKSTART.md ]]; then
        if [[ ! -f "$ROOT/$path" || -L "$ROOT/$path" ]]; then
            missing+=("$path")
        fi
    elif [[ ! -e "$ROOT/$path" ]]; then
        missing+=("$path")
    fi
done

# A link that resolves only in the source tree is dead once packaged, inline or
# as a reference definition. URLs and in-page anchors are skipped; a relative
# target must be in the package, beside the document on Linux and macOS, and by
# file name in a flattened MSI. A target that climbs out of the package root is
# dead even when the packaging host has a file there.
deadLinks=()
for path in "${expected[@]}"; do
    [[ "$path" == *.md ]] || continue
    if [[ "$PLATFORM" == "windows" ]]; then
        doc="$(findWindowsFile "${path##*/}")"
    else
        doc="$ROOT/$path"
    fi
    [[ -n "$doc" && -f "$doc" ]] || continue
    while IFS= read -r target; do
        target="${target%% *}"
        target="${target#<}"
        target="${target%>}"
        target="${target%%#*}"
        [[ -z "$target" ]] && continue
        [[ "$target" =~ ^[A-Za-z][A-Za-z0-9+.-]*: || "$target" == //* ]] && continue
        if [[ "$PLATFORM" == "windows" ]]; then
            [[ -n "$(findWindowsFile "${target##*/}")" ]] || deadLinks+=("$path -> $target")
        else
            resolved="$(physicalPath "$(dirname "$doc")/$target")"
            [[ -n "$resolved" && ( "$resolved" == "$ROOT_PHYSICAL" \
                  || "$resolved" == "$ROOT_PHYSICAL"/* ) ]] \
                || deadLinks+=("$path -> $target")
        fi
    done < <({ grep -oE '\]\([^)]+\)' "$doc" || true; } | sed 's/^](//; s/)$//'
             { grep -E '^ {0,3}\[[^]]+\]:[[:space:]]*[^[:space:]]' "$doc" || true; } \
                 | sed -E 's/^ {0,3}\[[^]]+\]:[[:space:]]*//')
done

if [[ ${#missing[@]} -gt 0 || ${#deadLinks[@]} -gt 0 ]]; then
    if [[ ${#missing[@]} -gt 0 ]]; then
        echo "error: ${PLATFORM} package is missing ${#missing[@]} required path(s):" >&2
        for path in "${missing[@]}"; do
            echo "  $path" >&2
        done
    fi
    if [[ ${#deadLinks[@]} -gt 0 ]]; then
        echo "error: ${PLATFORM} package has ${#deadLinks[@]} link(s) to paths it does not carry:" >&2
        for link in "${deadLinks[@]}"; do
            echo "  $link" >&2
        done
    fi
    if [[ "$PLATFORM" == "windows" ]]; then
        echo "extracted names under $ROOT:" >&2
        find "$ROOT" -type f | head -40 | sed "s|^$ROOT/||; s/^/  /" >&2
    fi
    echo "see packaging/contents.txt" >&2
    exit 1
fi

echo "${PLATFORM} package contents OK (${#expected[@]} paths checked)"
