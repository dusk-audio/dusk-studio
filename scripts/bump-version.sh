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

TODAY="$(date -u +%Y-%m-%d)"

echo "$NEW_VERSION" > VERSION

# Prepend a <release> block to the <releases> list in the AppStream
# metadata. Insertion point: the line immediately AFTER the opening
# <releases> tag. The xml comment "<!-- scripts/bump-version.sh prepends
# new <release> entries here. -->" pins the position deterministically.
APPDATA="packaging/DuskStudio.appdata.xml"
if [[ -f "$APPDATA" ]]; then
    RELEASE_BLOCK="    <release version=\"$NEW_VERSION\" date=\"$TODAY\">\n      <description>\n        <p>$NOTES</p>\n      </description>\n    </release>"
    # The trailing colon after the address keeps the comment line in
    # place; the insert happens AFTER it (a-command in sed). awk would
    # be cleaner but sed is universally available.
    if grep -q "scripts/bump-version.sh prepends new" "$APPDATA"; then
        # macOS sed needs a backup-suffix arg; GNU sed is fine with -i ''.
        # `set -e` already aborts on a non-zero sed exit, but the explicit
        # `|| { ... }` blocks emit a clearer message before bailing so the
        # operator knows which side of the GNU/BSD branch tripped.
        if sed --version >/dev/null 2>&1; then
            sed -i "/scripts\\/bump-version.sh prepends new/a\\
$RELEASE_BLOCK" "$APPDATA" \
                || { echo "error: GNU sed failed to update $APPDATA" >&2; exit 1; }
        else
            sed -i '' "/scripts\\/bump-version.sh prepends new/a\\
$RELEASE_BLOCK
" "$APPDATA" \
                || { echo "error: BSD sed failed to update $APPDATA" >&2; exit 1; }
        fi
        # Sanity check: the new version string MUST be present in the
        # file after the insert. Cheap, catches the case where sed
        # exited 0 but the address pattern didn't match (the `a\`
        # command is silent in that case).
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
echo "  1) Review the diff:   git diff VERSION $APPDATA"
echo "  2) Rebuild + smoke:   cmake --build build -j && build/.../DuskStudio --selftest"
echo "  3) Commit:            git commit -am \"Release v$NEW_VERSION\""
echo "  4) Tag:               git tag -a v$NEW_VERSION -m \"Dusk Studio $NEW_VERSION\""
echo "  5) Package:           scripts/package-{appimage,linux,windows,macos}.{sh,ps1}"
