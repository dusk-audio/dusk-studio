#!/usr/bin/env bash
# Check one built package against packaging/contents.txt.
#
#   scripts/verify-package-contents.sh linux   <unpacked tarball dir>
#   scripts/verify-package-contents.sh macos   <mounted DMG>
#   scripts/verify-package-contents.sh windows <7z extraction of the MSI>
#
# Read-only. Exits non-zero listing every missing path, so one run reports the
# whole gap rather than the first hole in it.
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

[[ $# -eq 2 ]] || usage
PLATFORM="$1"
ROOT="$2"

case "$PLATFORM" in
    linux|macos|windows) ;;
    *) usage ;;
esac

[[ -d "$ROOT" ]] || { echo "error: package root is not a directory: $ROOT" >&2; exit 2; }
[[ -f "$CONTRACT" ]] || { echo "error: contract not found: $CONTRACT" >&2; exit 2; }

expected=()
lineNo=0
while IFS= read -r line || [[ -n "$line" ]]; do
    lineNo=$((lineNo + 1))
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
        name="$(normalise "${path##*/}")"
        found=0
        while IFS= read -r candidate; do
            candidate="$(normalise "${candidate##*/}")"
            if [[ "$candidate" == "$name" || "$candidate" == *".${name}" \
                  || "$candidate" == *"_${name}" ]]; then
                found=1
                break
            fi
        done < <(find "$ROOT" -type f)
        [[ $found -eq 1 ]] || missing+=("$path")
    elif [[ ! -e "$ROOT/$path" ]]; then
        missing+=("$path")
    fi
done

if [[ ${#missing[@]} -gt 0 ]]; then
    echo "error: ${PLATFORM} package is missing ${#missing[@]} required path(s):" >&2
    for path in "${missing[@]}"; do
        echo "  $path" >&2
    done
    echo "see packaging/contents.txt" >&2
    exit 1
fi

echo "${PLATFORM} package contents OK (${#expected[@]} paths checked)"
