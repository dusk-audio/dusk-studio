#!/usr/bin/env bash
# Bump the project version. Updates:
#   • VERSION  - top-level file CMake reads via file(READ)
#   • packaging/DuskStudio.appdata.xml - prepends a new <release> entry
#                                          dated today
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

echo "$NEW_VERSION" > VERSION

# Prepend a <release> block to the <releases> list in the AppStream
# metadata. Insertion point: the line immediately AFTER the opening
# <releases> tag. The xml comment "<!-- scripts/bump-version.sh prepends
# new <release> entries here. -->" pins the position deterministically.
APPDATA="packaging/DuskStudio.appdata.xml"
if [[ -f "$APPDATA" ]]; then
    if grep -q "scripts/bump-version.sh prepends new" "$APPDATA"; then
        # Compose the new <release> block in a temp file (real newlines)
        # and have awk splice it in after the anchor comment. awk's
        # -v assignment rejects literal newlines on BSD awk (macOS), so
        # the multi-line block has to come from a file via getline,
        # not from a string variable.
        RELEASE_FILE=$(mktemp -t duskstudio-release.XXXXXX)
        trap 'rm -f "$RELEASE_FILE" "$APPDATA.tmp"' EXIT
        printf '    <release version="%s" date="%s">\n      <description>\n        <p>%s</p>\n      </description>\n    </release>\n' \
            "$NEW_VERSION" "$TODAY" "$NOTES" > "$RELEASE_FILE"

        awk -v release_file="$RELEASE_FILE" '
            { print }
            /scripts\/bump-version.sh prepends new/ {
                while ((getline line < release_file) > 0) print line
                close (release_file)
            }
        ' "$APPDATA" > "$APPDATA.tmp" \
            || { echo "error: awk failed to update $APPDATA" >&2; exit 1; }
        mv "$APPDATA.tmp" "$APPDATA"
        rm -f "$RELEASE_FILE"
        trap - EXIT

        # Sanity check: the new version string MUST be present in the
        # file after the insert. Cheap, catches the case where awk
        # exited 0 but the anchor comment was missing/different.
        if ! grep -q "version=\"$NEW_VERSION\"" "$APPDATA"; then
            echo "error: $APPDATA was not updated with version $NEW_VERSION " \
                 "(the anchor comment may have moved)." >&2
            exit 1
        fi
    else
        echo "warning: $APPDATA missing the anchor comment; skipping <release> insert" >&2
    fi
fi

echo
echo "Bumped VERSION  -> $NEW_VERSION"
echo "Today's date     -> $TODAY"
echo "Updated files:"
git status --short VERSION "$APPDATA" 2>/dev/null || true
echo
echo "Next steps:"
echo "  1) Refresh patrons:   scripts/update-patrons.py   (commit in the plugins repo)"
echo "  2) Review the diff:   git diff VERSION $APPDATA"
echo "  3) Rebuild + smoke:   cmake --build build -j && build/.../DuskStudio --selftest"
echo "  4) Commit:            git commit -am \"Release v$NEW_VERSION\""
echo "  5) Tag:               git tag -a v$NEW_VERSION -m \"Dusk Studio $NEW_VERSION\""
echo "  6) Package:           scripts/package-{tarball,macos}.sh, scripts/package-windows.ps1"
