# Handoff: finish the remaining 0.13.0 milestone work

Paste everything below as the opening prompt of the session that will do the work.

---

You are the orchestrator for Dusk Studio (`/home/marc/projects/DuskStudio`). Read
`CLAUDE.md` first; its audio-thread, de-JUCE, and verification requirements are
absolute. Of the 0.13.0 milestone, finish only issues #178 and #271. Issues
#266, #272, and #274 are closed; do not reopen or reimplement them. Verify the
current GitHub state and each finding against the current source before acting.

The 0.13.0 metadata landed on `origin/main` as `e4ac7d6` through #284, and
`origin/main:VERSION` is 0.13.0. Do not continue on the merged
`release/0.13.0-metadata` branch. Start any still-valid #178 change from an
up-to-date `origin/main`, and retire the merged branch only under #271 after
Marc authorizes cleanup.

The former uncommitted bounded-build edits are preserved in #288. Do not
duplicate or fold that separate reliability change into the #178 branch.

For issue #178, create a separate branch from an up-to-date `origin/main`.
Issue #271 is Marc-gated repository maintenance, not a code branch.

## Working model

Review every diff before committing. Run one adversarial review pass that hunts
for defects with code evidence, fix verified findings, and re-review the final
diff.

Hard rules:

- Never push, open a PR, or merge without Marc's explicit word. Commit locally,
  report, and wait.
- Zero attribution: no AI/tool mentions or co-author trailers.
- New code adds zero `juce::` tokens; `tools/juce-gate.sh` must pass.
- Keep changes scoped. File a follow-up issue instead of widening a fix.
- After Marc authorizes both a squash merge and its cleanup, retire the branch
  and worktree; never commit again on the merged branch.
- Before committing, remove change-narration comments, dead code, debug
  leftovers, and unintended artifacts from the diff.
- Audit `MANUAL.md` for user-visible source changes and update it in the same
  commit when behavior or controls changed.

## Verification

Use dedicated release-verification checkouts outside `/tmp`, with names that do
not trigger sibling auto-detection. This setup pins every source input used by
the native notepad build and safely refuses to overwrite a dirty checkout:

```bash
(
  set -euo pipefail
  JUCE=/home/marc/projects/dusk-juce-013
  DONOR=/home/marc/projects/dusk-plugins-013
  DAF=/home/marc/projects/dusk-daf-013
  DAF_WIDGETS=/home/marc/projects/dusk-daf-widgets-013
  JUCE_REV=4d85afa175a45e0b5da11f9211de3ba88705588e
  DONOR_REV=0a1b17f8e9dbecd26bf78dd45704c6c149e4b2ea
  DAF_REV=dfc50729f7a7d31dc0e0740c863bf88dee71c7c2
  DAF_WIDGETS_REV=1c09e1ef29f92ae7feb200bac8febdf814cf5e4a

  fetch_pin() {
    local checkout=$1
    local repository=$2
    local revision=$3
    local fetch_ref=${4:-$revision}
    if [[ -e "$checkout" ]]; then
      if [[ ! -e "$checkout/.git" ]]; then
        echo "ERROR: refusing non-repository path: $checkout" >&2
        return 1
      fi
      local current_repository
      current_repository=$(git -C "$checkout" remote get-url origin 2>/dev/null || true)
      if [[ "$current_repository" != "$repository" ]]; then
        echo "ERROR: unexpected origin for $checkout: $current_repository" >&2
        return 1
      fi
      if [[ -n "$(git -C "$checkout" status --porcelain)" ]]; then
        echo "ERROR: dirty checkout: $checkout" >&2
        return 1
      fi
    else
      git init -q "$checkout"
      git -C "$checkout" remote add origin "$repository"
    fi
    git -C "$checkout" fetch --depth 1 origin "$fetch_ref"
    git -C "$checkout" checkout -q --detach FETCH_HEAD
    test "$(git -C "$checkout" rev-parse HEAD)" = "$revision"
  }

  fetch_pin "$JUCE" https://github.com/dusk-audio/JUCE-wayland.git \
    "$JUCE_REV" refs/tags/dusk-wayland-v2
  fetch_pin "$DONOR" https://github.com/dusk-audio/dusk-audio-plugins.git "$DONOR_REV"
  fetch_pin "$DAF" https://github.com/dusk-audio/DAF.git "$DAF_REV"
  git -C "$DAF" submodule update --init --depth 1
  fetch_pin "$DAF_WIDGETS" https://github.com/dusk-audio/DAF-Widgets.git \
    "$DAF_WIDGETS_REV"
)

JUCE=/home/marc/projects/dusk-juce-013
DONOR=/home/marc/projects/dusk-plugins-013
DAF=/home/marc/projects/dusk-daf-013
DAF_WIDGETS=/home/marc/projects/dusk-daf-widgets-013
```

Before using the revisions above, compare them with every file under
`.github/workflows/` that defines `JUCE_REV` or `DONOR_REV`, plus
`.github/actions/clone-daf-stack/action.yml`, `BUILDING-LINUX.md`,
`BUILDING-WINDOWS.md`, `docs/MAINTAINER-GUIDE.md`, and `LICENSES.txt`; all
tracked copies must move together when a pin changes. Initialize this
repository's submodules as well:

```bash
git submodule update --init --recursive
```

Use the same paths for both configure commands. The `-w` directories are
deliberate clean release-verification builds, separate from the canonical
everyday `build/` and `build-tests/` directories documented in `CLAUDE.md`:
this procedure requires CMake 3.24 or newer for `--fresh`. It deliberately
leaves `DUSKSTUDIO_SKIP_FORK_CHECK` disabled, making local verification stricter
than CI by checking the pinned JUCE fork's XEmbed patch.

```bash
cmake --fresh -S . -B build-w -DCMAKE_BUILD_TYPE=Release \
  -DDUSKSTUDIO_REQUIRE_NATIVE_LV2=ON \
  -DDUSKSTUDIO_REQUIRE_PIPEWIRE=ON \
  -DJUCE_PATH="$JUCE" -DDUSK_PLUGINS_PATH="$DONOR" \
  -DDAF_PATH="$DAF" -DDAF_WIDGETS_PATH="$DAF_WIDGETS"
cmake --fresh -S . -B build-tests-w -DCMAKE_BUILD_TYPE=Release \
  -DDUSKSTUDIO_BUILD_TESTS=ON \
  -DDUSKSTUDIO_REQUIRE_NATIVE_LV2=ON \
  -DDUSKSTUDIO_REQUIRE_PIPEWIRE=ON \
  -DJUCE_PATH="$JUCE" -DDUSK_PLUGINS_PATH="$DONOR" \
  -DDAF_PATH="$DAF" -DDAF_WIDGETS_PATH="$DAF_WIDGETS"
```

Run these commands sequentially with the dev-box concurrency limit, then run
CTest and the JUCE gate:

```bash
(
  set -e
  cmake --build build-w --parallel 6
  if readelf -d build-w/DuskStudio_artefacts/Release/DuskStudio \
      | grep -E '\(NEEDED\).*libsodium\.so\.' >/dev/null; then
    echo "libsodium: dynamic linkage verified"
  elif grep -E \
      '^DUSKSTUDIO_SODIUM_(LIBRARY|STATIC_LIBRARY):FILEPATH=.*libsodium\.a$' \
      build-w/CMakeCache.txt >/dev/null; then
    echo "libsodium: static linkage selected by CMake"
  else
    echo "ERROR: release configuration did not resolve libsodium" >&2
    exit 1
  fi
  cmake --build build-tests-w --target dusk-studio-tests --parallel 6
  ctest --test-dir build-tests-w --output-on-failure
  bash tools/juce-gate.sh
)
```

DSP, engine, and session changes require tests.

Keep the four pinned checkouts through verification, then report them for
authorized cleanup under #271 instead of deleting them silently.

For changes touching app startup or runtime services, isolate both the display
and `XDG_RUNTIME_DIR` so no default Wayland socket under the live runtime
directory is reachable. Preserve the real PipeWire runtime path explicitly so
backend tests can still reach PipeWire without exposing Wayland:

```bash
(
  set -e
  LIVE_PIPEWIRE_RUNTIME=${PIPEWIRE_RUNTIME_DIR:-${XDG_RUNTIME_DIR:-}}
  test -n "$LIVE_PIPEWIRE_RUNTIME"
  SELFTEST_RUNTIME=$(mktemp -d)
  trap 'rm -rf -- "$SELFTEST_RUNTIME"' EXIT
  chmod 0700 "$SELFTEST_RUNTIME"
  env -u WAYLAND_DISPLAY \
    XDG_RUNTIME_DIR="$SELFTEST_RUNTIME" \
    PIPEWIRE_RUNTIME_DIR="$LIVE_PIPEWIRE_RUNTIME" \
    scripts/run-selftest-xvfb.sh build-w/DuskStudio_artefacts/Release/DuskStudio
)
```

This selftest does not construct the main window, but its backend cycle can open
real ALSA or PipeWire devices. Nothing else may hold the audio device while it
runs; report device-busy failures separately from product regressions. The
isolated runtime directory can intentionally hide PulseAudio and session-bus
sockets, so do not treat those unavailable routes as equivalent to a normal
desktop-session failure.

The ALSA sequence-subscription test can be flaky under parallel load. CTest's
`-R` option takes a test-name regular expression, not an ordinal such as test
number 7. List the registered tests, identify the exact name, and rerun that
name with failure output enabled:

```bash
ctest --test-dir build-tests-w -N | grep -E 'ALSA seq.*subscription'
ctest --test-dir build-tests-w --output-on-failure \
  -R '^ALSA seq backend does not report its own subscriptions as a change$'
```

## Release gate and remaining work

The #262-#270 tag gate consisted of #262-#265, the documentation part of #266,
and #267-#270. Those parts are complete. Issue #266 is closed; its non-gating
CMake enhancement moved to #286 in the 0.14.0 milestone.

- #178, release mechanics: verify the metadata on `origin/main` and the current
  repository against the issue's still-valid acceptance criteria. Leave tagging
  to Marc and do not redo release chores already completed. The untagged 0.12.7
  wave is folded into the 0.13.0 notes; do not create a 0.12.7 tag unless Marc
  reverses that decision. Ask Marc for the real package-contact address that
  must replace `support@duskaudio.example`; never invent one. The binary release
  workflows refresh patrons in isolated donor checkouts when all four Patreon
  secrets are available, but warn and ship the committed supporters list when
  any secret is missing. With all four secrets configured, a refresh failure
  fails the release build. Before tagging, configure local Patreon credentials
  and run the freshness check from the root checkout, not a
  `.codex/worktrees/*` issue worktree. The script resolves sibling donor paths
  from its own location, so an issue-worktree copy finds no donor header:

  ```bash
  (cd /home/marc/projects/DuskStudio && \
    env -u DUSK_PLUGINS_PATH scripts/update-patrons.py --dry-run)
  ```

  Reconcile any out-of-sync sibling headers and rerun the check. Follow
  Maintainer Guide Part 10 to print the header from the workflow-pinned donor
  revision, then compare its active tiers separately with the dry-run output.
  The dry run does not rewrite headers but may rotate local tokens; update the
  matching Actions secrets if it does. If the tiers differ, require all four
  repository secrets before tagging so the workflows inject the live list. If
  the secrets are unavailable, stop and ask Marc whether to update the donor
  list and pin; never advance `DONOR_REV` without that separate decision.
  Follow the tracked release procedures in
  `packaging/README.md` and `docs/MAINTAINER-GUIDE.md`.
- #286, optional SFZ catalog build: this is a 0.14.0 enhancement and does not
  gate the 0.13.0 tag. Do not implement it during this release session.
- #271, repository maintenance: each push, discard, branch deletion, and
  worktree removal needs Marc's explicit authorization.
- #274, persisted VCA key: closed with the decision not to rename
  `vca_overeasy` for 0.13.0. Do not add a session migration during this release.

Tag mechanics remain Marc's: assist, never absorb. The order is metadata bump,
land on `origin/main`, tag, then verify all release assets before announcing.

Report each issue's changes, rationale, verification output, and any review
finding fixed. Start by listing the open 0.13.0 milestone issues and reading
only those issue bodies.
