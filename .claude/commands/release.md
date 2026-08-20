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
- **Four workflows fire on a `v*` tag**: Linux release (tarball, once per arch),
  macOS release (unsigned DMG), Windows build (MSI), Manual PDF.
- **A complete release is exactly six assets**: two Linux tarballs, one macOS DMG,
  one Windows MSI, `MANUAL.pdf`, one `SHA256SUMS`.

## Instructions

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
git rev-parse -q --verify "refs/tags/v$NEW_VERSION" >/dev/null && { echo "STOP: tag v$NEW_VERSION exists"; exit 1; }
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
numbered checklist. Verify each file actually changed before continuing:

```bash
grep -qx "$NEW_VERSION" VERSION || { echo "STOP: VERSION not bumped"; exit 1; }
git diff --quiet && { echo "STOP: bump-version.sh changed nothing"; exit 1; }
```

Date the changelog heading (`## [X.Y.Z] - Unreleased` -> `## [X.Y.Z] - YYYY-MM-DD`)
and confirm:

```bash
grep -q "^## \[$NEW_VERSION\] - [0-9]\{4\}-[0-9]\{2\}-[0-9]\{2\}$" CHANGELOG.md \
  || { echo "STOP: changelog heading not dated"; exit 1; }
```

### Step 4: Build and test locally before tagging

```bash
cmake --build build -j6
cmake --build build-tests --target dusk-studio-tests -j6 && ctest --test-dir build-tests --output-on-failure
```

On Linux also run `scripts/run-selftest-xvfb.sh`. On macOS run the self-test
directly: `DUSKSTUDIO_RUN_SELFTEST=1 <app>/Contents/MacOS/DuskStudio`.

### Step 5: Commit and prove it landed

```bash
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
git tag -a "v$NEW_VERSION" -m "Dusk Studio $NEW_VERSION" "$RELEASE_COMMIT"
git push origin "refs/tags/v$NEW_VERSION"
```

### Step 7: Wait for the four workflows

```bash
gh run list --branch "v$NEW_VERSION" --limit 10
```

All four must succeed. **A green run does not mean the artifact works**; see Step 10.

### Step 8: Consolidate the checksum files

Until issue #321 merges the workflows into one publish job, each uploads its own
`SHA256SUMS.<job>`. Replace those five with a single sorted `SHA256SUMS` covering
all five payloads, and delete the per-job files from the release.

### Step 9: Asset and body check

```bash
scripts/verify-release-assets.sh "v$NEW_VERSION"
```

Must report `PASS: all 6 assets present.` It checks names and the summary slot
only; it never opens a payload.

### Step 10: Acceptance - the artifacts must actually run

Download all six into a clean directory. This step exists because v0.13.0 shipped
a macOS DMG that passed every CI job and could not launch on any machine: the app
linked Homebrew dylibs by absolute path with none bundled, and CMake's bundle
install invalidated the signature after signing. The build and the test suite both
run against the build tree and the CI machine's library prefix, so they pass whether
or not the packaged artifact is self-contained.

```bash
shasum -a 256 --ignore-missing -c SHA256SUMS   # every downloaded payload must pass
```

Then, per platform:

- **macOS**: mount with `yes | hdiutil attach ...` (the DMG carries a license
  agreement and otherwise reports `attach canceled`; read `hdiutil`'s own status,
  not the pipeline's, or `yes` dying of SIGPIPE fails the check under `pipefail`).
  Confirm `codesign --verify --strict --deep` passes, that
  `otool -L <app>/Contents/MacOS/DuskStudio` shows no load command outside
  `/usr/lib`, `/System`, `@executable_path/../Frameworks/` or `@loader_path/`, and
  that `env -i HOME="$HOME" <app>/Contents/MacOS/DuskStudio --version` exits 0.
  The DMG must contain the `.app`, `LICENSE` and `LICENSES.txt` and nothing else;
  a `share/` tree is a Linux install rule leaking into the macOS package.
- **Linux**: extract both tarballs and smoke each binary only on its matching
  architecture, under a private Xvfb display with `WAYLAND_DISPLAY` unset. Check
  `DT_NEEDED` against what the tarball README tells users to install; a dependency
  the binary needs and the README does not name is a launch failure on a clean machine.
- **Windows**: install the MSI and launch it. If no Windows machine is available,
  say so explicitly in the report rather than recording it as passed.

Also confirm, on every platform: payloads are Dusk-only with no `JUCE-*` paths, each
bundled `LICENSES.txt` is the complete file, and `MANUAL.pdf` renders with its figures
and accidentals intact. Open the manual's verification section and confirm the
commands it gives users match the assets actually published.

### Step 11: Report

Print old version, new version, tag, the six asset names with their checksum
results, and the per-platform launch result. State plainly which platforms were
launch-tested and which were not. The release is announceable only when Steps 9
and 10 both pass on every platform.

## Error handling

- **Tag exists**: never overwrite. Cut the next patch version.
- **A workflow fails**: `gh run view <id> --log-failed`. Fix, then cut a new patch
  version; do not re-run against a moved tag.
- **`gh` unreachable**: `git push` can succeed while the API is down. Do the API
  steps from a working shell rather than skipping them.
- **Verifier reports EXTRA `SHA256SUMS.<job>`**: Step 8 was skipped.
- **Assets older than the tag push**: a previous run's artifacts are still attached.
  Compare each asset's upload time to the tag push before trusting it.
