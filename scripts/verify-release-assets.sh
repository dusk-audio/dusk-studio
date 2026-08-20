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
# six assets, no more, no fewer, plus a populated release-summary slot. Exits
# nonzero if anything is missing, duplicated or unexpected.
#
# The six-asset shape is the target of issue #321. Until the four workflows are
# merged into one job that fans in, each job still uploads its own
# SHA256SUMS.<job> file: consolidate those five into a single SHA256SUMS before
# running this, as v0.13.0 was.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
REPO="${RELEASES_REPO:-dusk-audio/dusk-studio-releases}"
SUMMARY_START='<!-- summary-start -->'
SUMMARY_END='<!-- summary-end -->'

count_occurrences() {
    local remaining=$1
    local needle=$2
    local count=0
    while [[ "$remaining" == *"$needle"* ]]; do
        remaining=${remaining#*"$needle"}
        ((count += 1))
    done
    printf '%s' "$count"
}

strip_html_comments() {
    local remaining=$1
    local visible=''
    local comment_start='<!--'
    local comment_end='-->'
    while [[ "$remaining" == *"$comment_start"* ]]; do
        visible+=${remaining%%"$comment_start"*}
        remaining=${remaining#*"$comment_start"}
        if [[ "$remaining" != *"$comment_end"* ]]; then
            remaining=''
            break
        fi
        remaining=${remaining#*"$comment_end"}
    done
    visible+=$remaining
    printf '%s' "$visible"
}

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
# instead of passing; the checksum file carries no version. One SHA256SUMS
# covers every payload: a per-job split would report a partial release as
# complete, because each job would bring its own checksum file with it.
EXPECTED=(
    "Linux tarball x86_64|dusk-studio-${VER}-Linux-x86_64.tar.xz"
    "Linux tarball aarch64|dusk-studio-${VER}-Linux-aarch64.tar.xz"
    "macOS DMG|dusk-studio-${VER}-macOS-arm64.dmg"
    "Windows MSI|dusk-studio-${VER}-Windows-x64.msi"
    "User manual|MANUAL.pdf"
    "Checksums|SHA256SUMS"
)

GH_ERROR=$(mktemp "${TMPDIR:-/tmp}/duskstudio-release-verify.XXXXXX")
cleanup() {
    rm -f "$GH_ERROR"
}
trap cleanup EXIT

# Fetch asset names separately because summary validation needs the complete
# body text while asset matching needs one name per line.
if ! RECORDS=$(gh release view "$TAG" --repo "$REPO" --json assets \
        --jq '.assets[].name' 2>"$GH_ERROR"); then
    echo "error: cannot read release ${TAG} from ${REPO}:" >&2
    cat "$GH_ERROR" >&2
    exit 2
fi

: > "$GH_ERROR"
if ! BODY=$(gh release view "$TAG" --repo "$REPO" --json body --jq '.body // ""' \
        2>"$GH_ERROR"); then
    echo "error: cannot read release body for ${TAG} from ${REPO}:" >&2
    cat "$GH_ERROR" >&2
    exit 2
fi
BODY_BYTES=$(printf '%s' "$BODY" | wc -c | tr -d '[:space:]')

NAMES=()
while IFS= read -r name; do
    [[ -n "$name" ]] && NAMES+=("$name")
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

if [[ "$BODY_BYTES" -eq 0 ]]; then
    printf '%-8s %-24s %s\n' "EMPTY" "release body" \
        "no notes - the create step lost packaging/RELEASE-NOTES.md"
    status=1
else
    START_COUNT=$(count_occurrences "$BODY" "$SUMMARY_START")
    END_COUNT=$(count_occurrences "$BODY" "$SUMMARY_END")
    SUMMARY=${BODY#*"$SUMMARY_START"}
    if (( START_COUNT != 1 || END_COUNT != 1 )) \
        || [[ "$SUMMARY" != *"$SUMMARY_END"* ]]; then
        printf '%-8s %-24s %s\n' "INVALID" "release body" \
            "summary markers are missing, duplicated, or out of order"
        status=1
    else
        SUMMARY=${SUMMARY%%"$SUMMARY_END"*}
        SUMMARY_CONTENT=$(strip_html_comments "$SUMMARY" | tr -d '[:space:]')
        if [[ -z "$SUMMARY_CONTENT" ]]; then
            printf '%-8s %-24s %s\n' "PLACEHOLDER" "release body" \
                "summary slot is empty"
            status=1
        else
            printf '%-8s %-24s %s\n' "ok" "release body" \
                "${BODY_BYTES} bytes, summary populated"
        fi
    fi
fi

echo
echo "release ${TAG} in ${REPO}: ${#NAMES[@]} asset(s), expected ${#EXPECTED[@]}"
if [[ $status -ne 0 ]]; then
    echo "FAIL: incomplete release, do not announce it." >&2
else
    echo "PASS: all ${#EXPECTED[@]} assets present."
fi
exit $status
