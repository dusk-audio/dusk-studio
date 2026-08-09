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
# Does NOT git-commit, tag, or push - run those by hand once the
# diff looks right. Codesigning / notarization is handled per-OS by
# scripts/package-*.{sh,ps1}.

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "usage: $0 <semver> [release notes ...]" >&2
    exit 2
fi

NEW_VERSION="$1"
shift
NOTES="${*:-Patch release.}"

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
if [[ ! -f CHANGELOG.md ]]; then
    echo "error: CHANGELOG.md is missing - create it with a '## [$NEW_VERSION]' section first" >&2
    exit 1
elif ! grep -qF "## [$NEW_VERSION]" CHANGELOG.md; then
    echo "error: CHANGELOG.md has no '## [$NEW_VERSION]' section - add it first" >&2
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
NOTES_READY=0
APPDATA_READY=0
trap 'rm -f ${SUMMARY_FILE:+"$SUMMARY_FILE"} ${RELEASE_FILE:+"$RELEASE_FILE"} "$NOTES_FILE.tmp" "$APPDATA.tmp"' EXIT

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
        NOTES_READY=1
    else
        echo "warning: $NOTES_FILE has no usable summary slot (needs one" >&2
        echo "         $NOTES_START line before one $NOTES_END line);" >&2
        echo "         skipping the summary splice" >&2
    fi
else
    echo "warning: $NOTES_FILE not found; skipping the summary splice" >&2
fi

# Stage the AppStream <release> entry. Insertion point: the line immediately
# AFTER the opening <releases> tag, pinned by the anchor comment. Match the
# whole comment, not the bare phrase - release notes that quote the phrase
# would otherwise become anchors too and nest the next entry inside an old
# <description>.
if [[ -f "$APPDATA" ]]; then
    if grep -qF "$ANCHOR" "$APPDATA"; then
        # Compose the new <release> block in a temp file (real newlines)
        # and have awk splice it in after the anchor comment. awk's
        # -v assignment rejects literal newlines on BSD awk (macOS), so
        # the multi-line block has to come from a file via getline,
        # not from a string variable.
        RELEASE_FILE=$(mktemp -t duskstudio-release.XXXXXX)
        XML_NOTES=$(printf '%s' "$NOTES" \
            | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g')
        printf '    <release version="%s" date="%s">\n      <description>\n        <p>%s</p>\n      </description>\n    </release>\n' \
            "$NEW_VERSION" "$TODAY" "$XML_NOTES" > "$RELEASE_FILE"

        awk -v release_file="$RELEASE_FILE" -v anchor="$ANCHOR" '
            { print }
            !spliced && index($0, anchor) {
                while ((getline line < release_file) > 0) print line
                close (release_file)
                spliced = 1
            }
        ' "$APPDATA" > "$APPDATA.tmp" \
            || { echo "error: awk failed to update $APPDATA" >&2; exit 1; }

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

        # Sanity check on the staged file: the new version string MUST be
        # present after the insert. Cheap, catches the case where awk exited
        # 0 but the anchor comment was missing/different.
        if ! grep -q "version=\"$NEW_VERSION\"" "$APPDATA.tmp"; then
            echo "error: $APPDATA was not updated with version $NEW_VERSION " \
                 "(the anchor comment may have moved)." >&2
            exit 1
        fi
        APPDATA_READY=1
    else
        echo "warning: $APPDATA missing the anchor comment; skipping <release> insert" >&2
    fi
fi

# Everything above validated its staged copy; land the results together and
# write VERSION last, so any abort leaves the tree untouched.
if [[ "$NOTES_READY" == 1 ]]; then
    mv "$NOTES_FILE.tmp" "$NOTES_FILE"
fi
if [[ "$APPDATA_READY" == 1 ]]; then
    mv "$APPDATA.tmp" "$APPDATA"
fi
trap - EXIT
rm -f ${SUMMARY_FILE:+"$SUMMARY_FILE"} ${RELEASE_FILE:+"$RELEASE_FILE"}
echo "$NEW_VERSION" > VERSION

echo
echo "Bumped VERSION  -> $NEW_VERSION"
echo "Today's date     -> $TODAY"
echo "Updated files:"
git status --short VERSION "$APPDATA" "$NOTES_FILE" 2>/dev/null || true
echo
echo "Next steps:"
echo "  1) Date the changelog: CHANGELOG.md \"## [$NEW_VERSION] - Unreleased\" -> \"- $TODAY\""
echo "  2) Refresh patrons:   scripts/update-patrons.py   (commit in the plugins repo)"
echo "  3) Review the diff:   git diff VERSION $APPDATA $NOTES_FILE CHANGELOG.md"
echo "  4) Rebuild + smoke:   cmake --build build -j && DUSKSTUDIO_RUN_SELFTEST=1 build/.../DuskStudio"
echo "  5) Commit:            git commit -am \"Release v$NEW_VERSION\""
echo "  6) Tag:               git tag -a v$NEW_VERSION -m \"Dusk Studio $NEW_VERSION\""
echo "  7) Package:           scripts/package-{tarball,macos}.sh, scripts/package-windows.ps1"
echo "  8) Verify assets:     scripts/verify-release-assets.sh"
