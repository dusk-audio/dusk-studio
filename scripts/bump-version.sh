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
