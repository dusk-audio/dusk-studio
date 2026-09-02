# Release Dusk Studio

Cut a Dusk Studio release: version bump, changelog, tag, CI assets, and the
acceptance pass that decides whether it can be announced.

## Usage

```
/release [version]
```

- `version` (optional): explicit `X.Y.Z`. Omit to auto-increment the patch from `VERSION`.

## Paths and facts this repo needs

- **Source of truth for the procedure**: [docs/MAINTAINER-GUIDE.md](../../docs/MAINTAINER-GUIDE.md) Part 10.
- **Binaries publish to a PRIVATE repo**: `dusk-audio/dusk-studio-releases`.
  `gh release list` against the public repo returns nothing. That is correct, not a fault.
- **One workflow fires on a `v*` tag**: `Dusk Studio release`. Its four build
  jobs fan in to one publisher, so a partial platform set is never published.
- **A complete release is exactly six assets**: two Linux tarballs, one macOS DMG,
  one Windows MSI, `MANUAL.pdf`, one `SHA256SUMS`.

## Instructions

Run these snippets with `bash`, not the macOS default `zsh`: several read
`PIPESTATUS`, which zsh spells `pipestatus` and indexes from 1, so a mount or
pipeline failure reads as success there.

Execute every step in order. Stop at the first failure and report it; do not
work around a failed guard. A step that edits a file must prove the edit landed,
because a silent no-op here ships the previous version's binaries under a new tag.

### Step 0: Preflight guards

```bash
CURRENT_BRANCH=$(git rev-parse --abbrev-ref HEAD)
[ "$CURRENT_BRANCH" = "main" ] || { echo "STOP: releases are cut from main, not $CURRENT_BRANCH"; exit 1; }
[ -z "$(git status --porcelain)" ] || { echo "STOP: working tree is dirty"; exit 1; }
git fetch origin main || { echo "STOP: cannot reach origin"; exit 1; }
[ "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)" ] || { echo "STOP: local main differs from origin/main"; exit 1; }
```

Also confirm no open PR is expected to land in this release. Ask the user if any
PR looks release-relevant; a fix merged five minutes after the tag is a fix that
did not ship.

### Step 1: Version

Read `VERSION`. Compute the new version (explicit argument, else patch bump).
Refuse to reuse an existing tag:

```bash
git rev-parse -q --verify "refs/tags/v$NEW_VERSION" >/dev/null \
  && { echo "STOP: tag v$NEW_VERSION exists locally"; exit 1; }
# A local checkout that never fetched the tag has no ref to find, so ask origin
# before the release commit is pushed. A failed lookup is a stop, not a pass.
REMOTE_TAG=$(git ls-remote --tags --refs origin "refs/tags/v$NEW_VERSION") \
  || { echo "STOP: cannot query origin for tag v$NEW_VERSION"; exit 1; }
[ -z "$REMOTE_TAG" ] || { echo "STOP: tag v$NEW_VERSION exists on origin"; exit 1; }
```

**Never move a published tag.** If a released version needs different binaries,
cut the next patch version instead. Moving a tag leaves the already-published
assets in place, so the release then carries binaries built from a commit the tag
no longer points at, and anyone who downloaded early has different bytes under
the same version.

### Step 2: Changelog

`CHANGELOG.md` must already carry a `## [X.Y.Z] - Unreleased` section with real
entries. If it is missing or the entries are generic, ask the user for specifics
with AskUserQuestion rather than inventing them.

### Step 3: Bump metadata

```bash
scripts/bump-version.sh "$NEW_VERSION"
```

It updates `VERSION`, the appdata XML, and the release notes, then prints its own
numbered checklist. Check each file on its own: a whole-tree `git diff` passes when
any one of the three changed, which is how a partial bump reaches a tag.

```bash
set -euo pipefail
grep -qx "$NEW_VERSION" VERSION \
  || { echo "STOP: VERSION not bumped"; exit 1; }
grep -q "<release version=\"$NEW_VERSION\"" packaging/DuskStudio.appdata.xml \
  || { echo "STOP: appdata has no release entry for $NEW_VERSION"; exit 1; }
# RELEASE-NOTES.md carries no version literal, so changed is all that can be
# checked here; its content is read by eye in Step 2.
git diff --quiet -- packaging/RELEASE-NOTES.md \
  && { echo "STOP: release notes unchanged"; exit 1; }
```

Date the changelog heading (`## [X.Y.Z] - Unreleased` -> `## [X.Y.Z] - YYYY-MM-DD`)
and confirm:

```bash
grep -q "^## \[$NEW_VERSION\] - [0-9]\{4\}-[0-9]\{2\}-[0-9]\{2\}$" CHANGELOG.md \
  || { echo "STOP: changelog heading not dated"; exit 1; }
```

### Step 4: Build and test locally before tagging

```bash
set -euo pipefail
cmake --build build -j6
cmake --build build-tests --target dusk-studio-tests -j6
ctest --test-dir build-tests --output-on-failure
```

On Linux also run `scripts/run-selftest-xvfb.sh`. On macOS run the self-test
directly: `DUSKSTUDIO_RUN_SELFTEST=1 <app>/Contents/MacOS/DuskStudio`.

### Step 5: Commit and prove it landed

```bash
set -euo pipefail
git commit -am "Release v$NEW_VERSION"
RELEASE_COMMIT=$(git rev-parse HEAD)
git push origin main
git fetch origin main
git merge-base --is-ancestor "$RELEASE_COMMIT" origin/main \
  || { echo "STOP: release commit is not on origin/main"; exit 1; }
```

If the commit landed via a squashed PR, re-record `RELEASE_COMMIT` as the squashed
commit and re-run the ancestor check. Tagging a commit that is not on `origin/main`
produces a tag nobody else can reach.

### Step 6: Tag and push

```bash
set -euo pipefail
git tag -a "v$NEW_VERSION" -m "Dusk Studio $NEW_VERSION" "$RELEASE_COMMIT"
git push origin "refs/tags/v$NEW_VERSION"
```

### Step 7: Wait for the release workflow

Query by commit, not by eye. The workflow is not successful until both Linux
architectures, macOS, Windows, the manual, and the fan-in publisher pass.

A run that has not registered yet, or is queued or in progress, is pending, not
failed - a single snapshot taken right after the tag push reads every workflow as
a failure. Poll until each one is terminal, and stop on the timeout instead of
waiting forever.

```bash
set -euo pipefail
WORKFLOWS=("Dusk Studio release")
DEADLINE=$(( $(date +%s) + 3600 ))
while :; do
  RUNS=$(gh run list --commit "$RELEASE_COMMIT" --limit 50 \
         --json name,status,conclusion,headSha)
  PENDING=0
  for wf in "${WORKFLOWS[@]}"; do
    STATE=$(echo "$RUNS" | jq -r --arg wf "$wf" --arg sha "$RELEASE_COMMIT" '
      [.[] | select(.name == $wf and .headSha == $sha)] as $r
      | if ($r | length) == 0 then "pending"
        elif ($r | any(.status != "completed")) then "pending"
        elif ($r | all(.conclusion == "success")) then "success"
        else "failed" end')
    case "$STATE" in
      success) ;;
      pending) PENDING=1 ;;
      *) echo "STOP: $wf did not fully succeed on $RELEASE_COMMIT"; exit 1 ;;
    esac
  done
  [ "$PENDING" -eq 1 ] || break
  [ "$(date +%s)" -lt "$DEADLINE" ] \
    || { echo "STOP: workflows still pending on $RELEASE_COMMIT after 60 min"; exit 1; }
  sleep 30
done
```

**A green run does not mean the artifact works**; see Step 9.

### Step 8: Asset and body check

```bash
scripts/verify-release-assets.sh "v$NEW_VERSION"
```

Must report `PASS: all 6 assets present.` It checks names and the summary slot
only; it never opens a payload.

### Step 9: Acceptance - the artifacts must actually run

Download all six into a clean directory. This step exists because v0.13.0 shipped
a macOS DMG that passed every CI job and could not launch on any machine: the app
linked Homebrew dylibs by absolute path with none bundled, and CMake's bundle
install invalidated the signature after signing. The build and the test suite both
run against the build tree and the CI machine's library prefix, so they pass whether
or not the packaged artifact is self-contained.

All six are present, so every entry must verify. `--ignore-missing` belongs in the
advice given to users, who download one payload; here it would pass a release with
an asset missing.

`shasum -c` says nothing about a payload the file never listed, so a short or
duplicated `SHA256SUMS` verifies clean. Prove the entry set is exactly the five
payloads first, then verify.

```bash
set -euo pipefail
LISTED=$(awk '{sub(/^\*/, "", $2); print $2}' SHA256SUMS | sort)
PRESENT=$(ls -1 | grep -vx SHA256SUMS | sort)
[ "$(printf '%s\n' "$LISTED" | wc -l)" -eq 5 ] \
  || { echo "STOP: SHA256SUMS does not carry exactly 5 entries"; exit 1; }
[ "$(printf '%s\n' "$LISTED" | sort -u | wc -l)" -eq 5 ] \
  || { echo "STOP: SHA256SUMS names a payload more than once"; exit 1; }
diff <(printf '%s\n' "$LISTED") <(printf '%s\n' "$PRESENT") \
  || { echo "STOP: SHA256SUMS entries do not match the downloaded payloads"; exit 1; }
shasum -a 256 --strict -c SHA256SUMS
```

**macOS.** The DMG carries a license agreement, so `hdiutil` blocks and reports
`attach canceled` without `yes |`; read `hdiutil`'s own status rather than the
pipeline's, or `yes` dying of SIGPIPE fails the step under `pipefail`.

```bash
set -euo pipefail
MNT=$(mktemp -d)
set +o pipefail
yes | hdiutil attach -nobrowse -readonly -noverify -noautoopen -mountpoint "$MNT" "$DMG" >/dev/null
mount_rc=${PIPESTATUS[1]}
set -o pipefail
[ "$mount_rc" -eq 0 ] || { echo "STOP: cannot mount $DMG"; exit 1; }
APP="$MNT/DuskStudio.app"

codesign --verify --strict --deep --verbose=2 "$APP"

# Every Mach-O, not just the main executable: the bundle also carries the
# relocated dylibs and, on a sandbox build, the plugin-host helper.
while IFS= read -r macho; do
  file -b "$macho" | grep -q 'Mach-O' || continue
  if otool -L "$macho" | tail -n +2 | awk '{print $1}' \
     | grep -vqE '^/usr/lib/|^/System/|^@executable_path/\.\./Frameworks/|^@loader_path/'; then
    echo "STOP: $macho links outside the bundle"; exit 1
  fi
done < <(find "$APP" -type f)

env -i HOME="$HOME" "$APP/Contents/MacOS/DuskStudio" --version
```

The DMG must contain the `.app`, `LICENSE` and `LICENSES.txt` and nothing else; a
`share/` tree is a Linux install rule leaking into the macOS package.

**Linux.** Extract both tarballs and smoke each binary only on its matching
architecture, under a private Xvfb display with `WAYLAND_DISPLAY` unset. Every
`DT_NEEDED` entry that is not a core system library must be named in the tarball
README, or the binary dies at the loader on a clean machine:

```bash
readelf -d DuskStudio/DuskStudio | awk -F'[][]' '/NEEDED/ {print $2}'
```

**Windows.** Install the MSI and launch it, and open the session notepad: its
embedded window is created the same way as the macOS one, which was blank until
0.13.1. If no Windows machine is available, report it as untested rather than passed.

**Every platform**, deterministically:

```bash
set -euo pipefail
find "$PAYLOAD" -path '*JUCE-*' -print -quit | grep -q . \
  && { echo "STOP: JUCE-* path in payload"; exit 1; }
cmp -s "$PAYLOAD/LICENSES.txt" LICENSES.txt \
  || { echo "STOP: bundled LICENSES.txt is not the repo's current file"; exit 1; }
pdftotext MANUAL.pdf - | grep -qE 'SHA256SUMS\.(linux|macos|windows|manual)' \
  && { echo "STOP: manual documents checksum files this release does not publish"; exit 1; }
```

`MANUAL.pdf` also needs a human: confirm the figures render and the sharp and flat
accidentals display. Report that as checked or not checked; there is no command for it.

### Step 10: Report

Print old version, new version, tag, the six asset names with their checksum
results, and the per-platform launch result. State plainly which platforms were
launch-tested and which were not. The release is announceable only when Steps 8
and 9 both pass on every platform.

## Error handling

- **Tag exists**: never overwrite. Cut the next patch version.
- **A workflow fails**: `gh run view <id> --log-failed`. Fix, then cut a new patch
  version; do not re-run against a moved tag.
- **`gh` unreachable**: `git push` can succeed while the API is down. Do the API
  steps from a working shell rather than skipping them.
- **Verifier reports an extra checksum fragment**: the release predates the
  fan-in publisher or retained a stale asset; do not announce it.
- **Assets older than the tag push**: a previous run's artifacts are still attached.
  Compare each asset's upload time to the tag push before trusting it.
