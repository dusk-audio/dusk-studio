#!/usr/bin/env bash
# Bump the project version. Updates:
#   - VERSION  - top-level file CMake reads via file(READ)
#   - packaging/DuskStudio.appdata.xml - prepends a new <release> entry
#     dated today
#   - packaging/RELEASE-NOTES.md - rewrites the summary slot of the canonical
#     release body every tag workflow publishes
# Then prints what to do next (git commit + tag).
#
# Usage:   scripts/bump-version.sh 1.0.0
#          scripts/bump-version.sh 1.0.0 "Release notes line one"
#
# Refuses to overwrite if the requested version is already in VERSION.
# Does NOT git-commit, tag, or push. Tag-triggered CI workflows build, package,
# and publish the release after the reviewed metadata commit lands on main.

set -euo pipefail

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

if [[ $# -lt 1 ]]; then
    echo "usage: $0 <semver> [release notes ...]" >&2
    exit 2
fi

NEW_VERSION="$1"
shift
if [[ $# -eq 0 ]]; then
    NOTES='Patch release.'
else
    NOTES="$*"
fi

VISIBLE_NOTES=$(strip_html_comments "$NOTES")
while [[ "$VISIBLE_NOTES" == [[:space:]]* ]]; do
    VISIBLE_NOTES=${VISIBLE_NOTES#?}
done
while [[ "$VISIBLE_NOTES" == *[[:space:]] ]]; do
    VISIBLE_NOTES=${VISIBLE_NOTES%?}
done
if [[ -z "${VISIBLE_NOTES//[[:space:]]/}" ]]; then
    echo "error: release notes must contain visible text" >&2
    exit 2
fi
NOTES="$VISIBLE_NOTES"

if [[ ! "$NEW_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "error: '$NEW_VERSION' is not a semver triple (MAJOR.MINOR.PATCH)" >&2
    exit 2
fi

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_DIR"

CURRENT="$(tr -d '[:space:]' < VERSION || true)"
if [[ "$CURRENT" == "$NEW_VERSION" ]]; then
    echo "error: VERSION is already $NEW_VERSION - nothing to bump" >&2
    exit 1
fi

# A release must have its CHANGELOG section written before we stamp the
# version, so the published notes never lag the tag.
EXPECTED_CHANGELOG_HEADING="## [$NEW_VERSION] - Unreleased"
if [[ ! -f CHANGELOG.md ]]; then
    echo "error: CHANGELOG.md is missing - add '$EXPECTED_CHANGELOG_HEADING' first" >&2
    exit 1
elif ! grep -qFx "$EXPECTED_CHANGELOG_HEADING" CHANGELOG.md; then
    echo "error: CHANGELOG.md has no exact '$EXPECTED_CHANGELOG_HEADING' heading" >&2
    exit 1
fi

# Anti-drift gate: sibling JUCE-wayland dev fork vs the release snapshot.
# linux-release.yml pins the Linux build to a Dusk-owned mirror snapshot
# (JUCE_REV under an immutable dusk-wayland-vN tag). If a maintainer keeps a
# sibling ../JUCE-wayland dev checkout, make sure a release can't be cut from a
# fork state that no snapshot captures:
#   (a) HARD FAIL if that checkout is dirty - uncommitted fork changes are in no
#       snapshot and would silently not ship.
#   (b) WARN if the fork HEAD tree differs from the snapshot the release pins -
#       the dev fork has moved on and a new dusk-wayland-vN tag is due.
JUCE_FORK_DIR="$REPO_DIR/../JUCE-wayland"
RELEASE_WORKFLOW=".github/workflows/linux-release.yml"
if [[ -e "$JUCE_FORK_DIR/.git" ]]; then
    if [[ -n "$(git -C "$JUCE_FORK_DIR" status --porcelain 2>/dev/null)" ]]; then
        echo "error: sibling JUCE fork at $JUCE_FORK_DIR has uncommitted changes." >&2
        echo "       Commit + snapshot to the mirror before cutting a release -" >&2
        echo "       a dirty working tree is in no dusk-wayland-vN snapshot." >&2
        exit 1
    fi

    JUCE_REV="$(sed -n 's/^[[:space:]]*JUCE_REV:[[:space:]]*//p' "$RELEASE_WORKFLOW" | head -n1)"
    JUCE_TAG="$(sed -n 's/^[[:space:]]*JUCE_TAG:[[:space:]]*//p' "$RELEASE_WORKFLOW" | head -n1)"
    JUCE_MIRROR="$(sed -n 's/^[[:space:]]*JUCE_MIRROR:[[:space:]]*//p' "$RELEASE_WORKFLOW" | head -n1)"

    if [[ -z "$JUCE_REV" ]]; then
        echo "warning: could not read JUCE_REV from $RELEASE_WORKFLOW; skipping the fork-drift check." >&2
    elif ! git -C "$JUCE_FORK_DIR" cat-file -e "$JUCE_REV" 2>/dev/null; then
        # The snapshot commit may exist only on the mirror, not in this checkout.
        echo "note: release snapshot $JUCE_REV is not present in $JUCE_FORK_DIR; skipping the fork-drift check." >&2
        echo "      To enable it, fetch the snapshot first:" >&2
        echo "        git -C $JUCE_FORK_DIR fetch $JUCE_MIRROR tag $JUCE_TAG" >&2
    else
        FORK_TREE="$(git -C "$JUCE_FORK_DIR" rev-parse 'HEAD^{tree}')"
        SNAP_TREE="$(git -C "$JUCE_FORK_DIR" rev-parse "${JUCE_REV}^{tree}")"
        if [[ "$FORK_TREE" != "$SNAP_TREE" ]]; then
            echo "warning: sibling JUCE fork HEAD tree ($FORK_TREE)" >&2
            echo "         != release snapshot ${JUCE_TAG} tree ($SNAP_TREE)." >&2
            echo "         The dev fork has moved past the pinned snapshot - push a new" >&2
            echo "         dusk-wayland-vN tag to the mirror and bump JUCE_TAG / JUCE_REV" >&2
            echo "         in $RELEASE_WORKFLOW before releasing." >&2
        fi
    fi
fi

TODAY="$(date -u +%Y-%m-%d)"

# Both splices are staged to temp files and validated before either lands,
# then everything is moved into place together: an abort in either stage
# leaves the whole tree untouched, and nothing non-idempotent (the appdata
# prepend) can land ahead of a later failure.
NOTES_FILE="packaging/RELEASE-NOTES.md"
NOTES_START='<!-- summary-start -->'
NOTES_END='<!-- summary-end -->'
APPDATA="packaging/DuskStudio.appdata.xml"
ANCHOR='<!-- scripts/bump-version.sh prepends new <release> entries here. -->'
VERSION_FILE="VERSION"
VERSION_TMP="${VERSION_FILE}.tmp"
NOTES_BACKUP=""
APPDATA_BACKUP=""
VERSION_BACKUP=""

cleanup_temps() {
    rm -f ${SUMMARY_FILE:+"$SUMMARY_FILE"} ${RELEASE_FILE:+"$RELEASE_FILE"} \
        "$NOTES_FILE.tmp" "$APPDATA.tmp" "$VERSION_TMP" \
        ${NOTES_BACKUP:+"$NOTES_BACKUP"} \
        ${APPDATA_BACKUP:+"$APPDATA_BACKUP"} \
        ${VERSION_BACKUP:+"$VERSION_BACKUP"}
}

restore_metadata() {
    local rollback_failed=0
    cp -p "$NOTES_BACKUP" "$NOTES_FILE" || rollback_failed=1
    cp -p "$APPDATA_BACKUP" "$APPDATA" || rollback_failed=1
    cp -p "$VERSION_BACKUP" "$VERSION_FILE" || rollback_failed=1
    if [[ "$rollback_failed" == 1 ]]; then
        echo "fatal: release metadata rollback was incomplete" >&2
        echo "       recovery copies: $NOTES_BACKUP $APPDATA_BACKUP $VERSION_BACKUP" >&2
        trap - EXIT
        return 1
    fi
}

handle_apply_signal() {
    local signal="$1"
    local exit_code=1
    trap - HUP INT TERM
    echo "error: release metadata application interrupted by $signal; restoring originals" >&2
    restore_metadata || true
    case "$signal" in
        HUP)  exit_code=129 ;;
        INT)  exit_code=130 ;;
        TERM) exit_code=143 ;;
    esac
    exit "$exit_code"
}

trap cleanup_temps EXIT

# Stage the summary slot of the canonical release body. Every tag-triggered
# workflow publishes this file verbatim, so the summary is script-managed:
# nothing hand-written there can lag behind the tag.
if [[ -f "$NOTES_FILE" ]]; then
    # The splice replaces whole lines, so each marker has to sit alone on its
    # own line, exactly once, start before end. Anything else - including both
    # markers sharing a line - would swallow the rest of the file.
    SLOT_LAYOUT=$(awk -v start="$NOTES_START" -v end="$NOTES_END" '
        { trimmed = $0; sub(/^[ \t]+/, "", trimmed); sub(/[ \t]+$/, "", trimmed) }
        trimmed == start { starts++; if (startAt == 0) startAt = NR }
        trimmed == end   { ends++;   if (endAt == 0)   endAt = NR }
        END { print (starts == 1 && ends == 1 && startAt < endAt) ? "ok" : "bad" }
    ' "$NOTES_FILE")

    if [[ "$SLOT_LAYOUT" == "ok" ]]; then
        # awk reads the summary from a file: BSD awk rejects literal newlines
        # in a -v assignment.
        SUMMARY_FILE=$(mktemp -t duskstudio-summary.XXXXXX)
        printf '%s\n' "$NOTES" > "$SUMMARY_FILE"

        awk -v summary_file="$SUMMARY_FILE" -v start="$NOTES_START" -v end="$NOTES_END" '
            { trimmed = $0; sub(/^[ \t]+/, "", trimmed); sub(/[ \t]+$/, "", trimmed) }
            trimmed == start {
                print
                while ((getline line < summary_file) > 0) print line
                close (summary_file)
                inside = 1
                next
            }
            trimmed == end { inside = 0 }
            !inside { print }
        ' "$NOTES_FILE" > "$NOTES_FILE.tmp" \
            || { echo "error: awk failed to update $NOTES_FILE" >&2; exit 1; }

        # Validate before the file lands: the slot must now hold exactly the
        # release notes and nothing else.
        SPLICED=$(awk -v start="$NOTES_START" -v end="$NOTES_END" '
            { trimmed = $0; sub(/^[ \t]+/, "", trimmed); sub(/[ \t]+$/, "", trimmed) }
            trimmed == start { inside = 1; next }
            trimmed == end   { inside = 0; next }
            inside { print }
        ' "$NOTES_FILE.tmp")
        if [[ "$SPLICED" != "$NOTES" ]]; then
            echo "error: the summary slot in $NOTES_FILE did not take the release" >&2
            echo "       notes; $NOTES_FILE left unchanged." >&2
            exit 1
        fi
    else
        echo "error: $NOTES_FILE has no usable summary slot (needs one" >&2
        echo "       $NOTES_START line before one $NOTES_END line)." >&2
        exit 1
    fi
else
    echo "error: $NOTES_FILE not found" >&2
    exit 1
fi

# Stage the AppStream <release> entry. Insertion point: the line immediately
# AFTER the opening <releases> tag, pinned by its direct-child anchor comment.
# Match the complete comment line only at element depth zero inside <releases>;
# a release description that quotes the anchor must not become an insertion
# point.
if [[ -f "$APPDATA" ]]; then
    # Compose the new <release> block in a temp file (real newlines) and have
    # awk splice it after the one valid anchor. awk reads the multi-line block
    # from a file because BSD awk rejects literal newlines in -v assignments.
    RELEASE_FILE=$(mktemp -t duskstudio-release.XXXXXX)
    XML_NOTES=$(printf '%s' "$NOTES" \
        | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g')
    printf '    <release version="%s" date="%s">\n      <description>\n        <p>%s</p>\n      </description>\n    </release>\n' \
        "$NEW_VERSION" "$TODAY" "$XML_NOTES" > "$RELEASE_FILE"

    AWK_STATUS=0
    awk -v release_file="$RELEASE_FILE" -v anchor="$ANCHOR" '
        function updateDepth(text, token) {
            while (match(text, /<[^>]+>/)) {
                token = substr(text, RSTART, RLENGTH)
                text = substr(text, RSTART + RLENGTH)
                if (token ~ /^<\//) depth--
                else if (token !~ /^<[!?]/ && token !~ /\/>$/) depth++
            }
        }
        {
            trimmed = $0
            sub(/^[ \t]+/, "", trimmed)
            sub(/[ \t]+$/, "", trimmed)

            if (!inReleases && trimmed ~ /^<releases([ \t][^>]*)?>$/) {
                inReleases = 1
                depth = 0
                print
                next
            }
            if (inReleases && depth == 0 && trimmed == "</releases>") {
                inReleases = 0
                print
                next
            }

            print
            if (inReleases && depth == 0 && trimmed == anchor) {
                anchors++
                if (anchors == 1) {
                    while ((getline line < release_file) > 0) print line
                    close (release_file)
                }
            }
            if (inReleases) updateDepth(trimmed)
        }
        END { if (anchors != 1 || inReleases) exit 42 }
    ' "$APPDATA" > "$APPDATA.tmp" || AWK_STATUS=$?
    if [[ "$AWK_STATUS" == 42 ]]; then
        echo "error: $APPDATA must contain exactly one direct release anchor" >&2
        exit 1
    elif [[ "$AWK_STATUS" != 0 ]]; then
        echo "error: awk failed to update $APPDATA" >&2
        exit 1
    fi

    # XML-illegal control characters (vertical tab, form feed, ESC) pass
    # straight through the entity escaping above, so validate the result
    # before it lands in the tree.
    if command -v xmllint >/dev/null 2>&1; then
        if ! xmllint --noout "$APPDATA.tmp"; then
            echo "error: the updated $APPDATA is not well-formed XML;" >&2
            echo "       $APPDATA left unchanged. Check the release notes" >&2
            echo "       for characters XML rejects." >&2
            exit 1
        fi
    else
        echo "warning: xmllint not found - skipping XML validation of $APPDATA" >&2
    fi

    # Sanity check on the staged file: the new version string MUST be present.
    if ! grep -q "version=\"$NEW_VERSION\"" "$APPDATA.tmp"; then
        echo "error: $APPDATA was not updated with version $NEW_VERSION" >&2
        exit 1
    fi
else
    echo "error: $APPDATA not found" >&2
    exit 1
fi

# Everything above validated its staged copy or exited. Stage VERSION before
# replacing any original, then keep rollback copies until all three moves
# succeed. A failed application restores every metadata file to its exact
# pre-application contents and mode.
if ! cp -p "$VERSION_FILE" "$VERSION_TMP" \
    || ! printf '%s\n' "$NEW_VERSION" > "$VERSION_TMP"; then
    echo "error: could not stage $VERSION_TMP" >&2
    exit 1
fi

NOTES_BACKUP=$(mktemp "${NOTES_FILE}.rollback.XXXXXX")
APPDATA_BACKUP=$(mktemp "${APPDATA}.rollback.XXXXXX")
VERSION_BACKUP=$(mktemp "${VERSION_FILE}.rollback.XXXXXX")
cp -p "$NOTES_FILE" "$NOTES_BACKUP"
cp -p "$APPDATA" "$APPDATA_BACKUP"
cp -p "$VERSION_FILE" "$VERSION_BACKUP"

trap 'handle_apply_signal HUP' HUP
trap 'handle_apply_signal INT' INT
trap 'handle_apply_signal TERM' TERM
APPLY_FAILED=0
if ! mv "$NOTES_FILE.tmp" "$NOTES_FILE"; then
    APPLY_FAILED=1
elif ! mv "$APPDATA.tmp" "$APPDATA"; then
    APPLY_FAILED=1
elif ! mv "$VERSION_TMP" "$VERSION_FILE"; then
    APPLY_FAILED=1
fi

if [[ "$APPLY_FAILED" == 1 ]]; then
    trap - HUP INT TERM
    echo "error: release metadata application failed; restoring originals" >&2
    restore_metadata || true
    exit 1
fi

trap - HUP INT TERM
rm -f "$NOTES_BACKUP" "$APPDATA_BACKUP" "$VERSION_BACKUP" \
    ${SUMMARY_FILE:+"$SUMMARY_FILE"} ${RELEASE_FILE:+"$RELEASE_FILE"} || true
trap - EXIT

echo
echo "Bumped VERSION  -> $NEW_VERSION"
echo "Today's date     -> $TODAY"
echo "Updated files:"
git status --short VERSION "$APPDATA" "$NOTES_FILE" 2>/dev/null || true
echo
echo "Next steps:"
echo "  1) Date the changelog: CHANGELOG.md \"## [$NEW_VERSION] - Unreleased\" -> \"## [$NEW_VERSION] - $TODAY\""
echo "  2) Refresh patrons:   scripts/update-patrons.py   (commit in the plugins repo)"
echo "  3) Review the diff:   git diff VERSION $APPDATA $NOTES_FILE CHANGELOG.md"
echo "  4) Rebuild + smoke:   cmake --build build -j && scripts/run-selftest-xvfb.sh"
echo "  5) Commit metadata:   git commit -am \"Release v$NEW_VERSION\" && RELEASE_COMMIT=\$(git rev-parse HEAD)"
echo "  6) Fetch main:        git fetch origin main || { echo \"STOP: git fetch origin main failed\" >&2; false; }"
echo "     Prove landing:     git merge-base --is-ancestor \"\${RELEASE_COMMIT:?record RELEASE_COMMIT after committing metadata}\" origin/main && echo \"landed on origin/main\" || { echo \"STOP: release commit is not on origin/main\" >&2; false; }"
echo "     PR squash:         re-record RELEASE_COMMIT as the landed commit, then rerun step 6 (see MAINTAINER-GUIDE Part 10)"
echo "  7) Tag landed commit: git tag -a v$NEW_VERSION -m \"Dusk Studio $NEW_VERSION\" \"\${RELEASE_COMMIT:?record RELEASE_COMMIT after committing metadata}\""
echo "  8) Push tag:          git push origin \"refs/tags/v$NEW_VERSION\""
echo "  9) Wait for CI assets: Linux release (tarball), macOS release (unsigned DMG), Windows build, Manual PDF (all 10 assets)"
echo " 10) Verify assets:     scripts/verify-release-assets.sh v$NEW_VERSION"
