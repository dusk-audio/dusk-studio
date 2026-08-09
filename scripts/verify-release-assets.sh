#!/usr/bin/env bash
# Verify a release in the PRIVATE releases repo carries its complete asset set
# before it is announced. Read-only: reads one release, changes nothing.
#
#   scripts/verify-release-assets.sh            # tag v$(cat VERSION)
#   scripts/verify-release-assets.sh v0.13.0
#
# Four workflows publish to the same tag (linux-release once per arch,
# macos-release, windows-build, manual-pdf). A platform that fails leaves a
# release that looks finished but is short its assets, so the set is the check:
# ten assets, no more, no fewer, plus a non-empty body. Exits nonzero if
# anything is missing, duplicated or unexpected.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
REPO="${RELEASES_REPO:-dusk-audio/dusk-studio-releases}"

TAG="${1:-}"
if [[ -z "$TAG" ]]; then
    if [[ ! -f "$ROOT/VERSION" ]]; then
        echo "error: no tag argument and no VERSION file at $ROOT/VERSION" >&2
        exit 2
    fi
    TAG="v$(tr -d '[:space:]' < "$ROOT/VERSION")"
fi
VER="${TAG#v}"

if ! command -v gh >/dev/null 2>&1; then
    echo "error: gh is not installed" >&2
    exit 2
fi

# label|glob. The glob is matched against the asset name. Versioned names carry
# the tag's version, so an asset built from a stale VERSION reads as MISSING
# instead of passing; the SHA256SUMS.* names carry no version.
EXPECTED=(
    "Linux tarball x86_64|dusk-studio-${VER}-Linux-x86_64.tar.xz"
    "Linux sums x86_64|SHA256SUMS.linux-x86_64"
    "Linux tarball aarch64|dusk-studio-${VER}-Linux-aarch64.tar.xz"
    "Linux sums aarch64|SHA256SUMS.linux-aarch64"
    "macOS DMG|dusk-studio-${VER}-macOS-*.dmg"
    "macOS sums|SHA256SUMS.macos"
    "Windows MSI|dusk-studio-${VER}-Windows-*.msi"
    "Windows sums|SHA256SUMS.windows"
    "User manual|MANUAL.pdf"
    "Manual sums|SHA256SUMS.manual"
)

# One API call, one record per line as "kind<TAB>value": the body is multi-line
# and would otherwise be indistinguishable from the asset names.
if ! RECORDS=$(gh release view "$TAG" --repo "$REPO" --json assets,body \
        --jq '"body\t\(.body | utf8bytelength)", (.assets[] | "asset\t\(.name)")' 2>&1); then
    echo "error: cannot read release ${TAG} from ${REPO}:" >&2
    echo "$RECORDS" >&2
    exit 2
fi

NAMES=()
BODY_BYTES=0
while IFS=$'\t' read -r kind value; do
    case "$kind" in
        asset) NAMES+=("$value") ;;
        body)  BODY_BYTES="$value" ;;
    esac
done <<< "$RECORDS"

MATCHED=()
for ((i = 0; i < ${#NAMES[@]}; i++)); do
    MATCHED[i]=0
done

status=0
printf '%-8s %-24s %s\n' "STATUS" "EXPECTED" "ASSET"
printf '%-8s %-24s %s\n' "------" "--------" "-----"

for entry in "${EXPECTED[@]}"; do
    label="${entry%%|*}"
    pattern="${entry#*|}"
    hits=()
    for ((i = 0; i < ${#NAMES[@]}; i++)); do
        # shellcheck disable=SC2053  # unquoted RHS: glob match is the point
        if [[ "${NAMES[i]}" == $pattern ]]; then
            hits+=("${NAMES[i]}")
            MATCHED[i]=1
        fi
    done
    case ${#hits[@]} in
        0)
            printf '%-8s %-24s %s\n' "MISSING" "$label" "nothing matches ${pattern}"
            status=1
            ;;
        1)
            printf '%-8s %-24s %s\n' "ok" "$label" "${hits[0]}"
            ;;
        *)
            printf '%-8s %-24s %s\n' "DUP" "$label" "${#hits[@]} match ${pattern}: ${hits[*]}"
            status=1
            ;;
    esac
done

for ((i = 0; i < ${#NAMES[@]}; i++)); do
    if [[ ${MATCHED[i]} -eq 0 ]]; then
        printf '%-8s %-24s %s\n' "EXTRA" "(unexpected)" "${NAMES[i]}"
        status=1
    fi
done

if [[ "$BODY_BYTES" -gt 0 ]]; then
    printf '%-8s %-24s %s\n' "ok" "release body" "${BODY_BYTES} bytes"
else
    printf '%-8s %-24s %s\n' "EMPTY" "release body" "no notes - the create step lost packaging/RELEASE-NOTES.md"
    status=1
fi

echo
echo "release ${TAG} in ${REPO}: ${#NAMES[@]} asset(s), expected ${#EXPECTED[@]}"
if [[ $status -ne 0 ]]; then
    echo "FAIL: incomplete release, do not announce it." >&2
else
    echo "PASS: all ${#EXPECTED[@]} assets present."
fi
exit $status
