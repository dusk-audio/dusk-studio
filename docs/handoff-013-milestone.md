# Handoff: finish the remaining 0.13.0 milestone work

Paste everything below as the opening prompt of the session that will do the work.

---

You are the orchestrator for Dusk Studio (`/home/marc/projects/DuskStudio`). Read
`CLAUDE.md` first; its audio-thread, de-JUCE, and verification requirements are
absolute. Finish only the still-open 0.13.0 milestone issues: #178, #266, #271,
#272, and #274. Verify their current GitHub state and each finding against the
current source before acting. Do not reopen or reimplement closed issues.

The release metadata is already prepared on `release/0.13.0-metadata`; commit
`b4daede` contains the 0.13.0 bump. Until Marc merges that branch,
`origin/main:VERSION` remains 0.12.2. Continue from the release branch instead
of recreating the bump on `main`.

The working tree also contains unrelated, user-owned edits in
`BUILDING-LINUX.md`, `CLAUDE.md`, `docs/capture-screenshots.sh`, and
`docs/lv2-dsp-integration-handoff.md`. Do not stage, rewrite, or discard them.

Keep the metadata branch limited to #178 and this handoff. Create separate
issue branches from an up-to-date `origin/main` for #266, #272, and any code
resulting from #274; each gets its own review and PR. #271 is Marc-gated
repository maintenance, not a code branch.

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
  DPF=/home/marc/projects/dusk-dpf-013
  DPF_WIDGETS=/home/marc/projects/dusk-dpf-widgets-013
  JUCE_REV=4d85afa175a45e0b5da11f9211de3ba88705588e
  DONOR_REV=69f04318e3b7063e382c80ac1cde2388170e668b
  DPF_REV=f9fbc62af6fa7ce638a6f1e1482896c385a4955e
  DPF_WIDGETS_REV=668de17f06abdeb98d5a4b62594bd634f8d1ac2e

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
  fetch_pin "$DPF" https://github.com/dusk-audio/DPF.git "$DPF_REV"
  git -C "$DPF" submodule update --init --depth 1
  fetch_pin "$DPF_WIDGETS" https://github.com/dusk-audio/DPF-Widgets.git \
    "$DPF_WIDGETS_REV"
)

JUCE=/home/marc/projects/dusk-juce-013
DONOR=/home/marc/projects/dusk-plugins-013
DPF=/home/marc/projects/dusk-dpf-013
DPF_WIDGETS=/home/marc/projects/dusk-dpf-widgets-013
```

Before using the revisions above, compare them with every file under
`.github/workflows/` that defines `JUCE_REV` or `DONOR_REV`, plus
`.github/actions/clone-dpf-stack/action.yml`, `BUILDING-LINUX.md`,
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
  -DDPF_PATH="$DPF" -DDPF_WIDGETS_PATH="$DPF_WIDGETS"
cmake --fresh -S . -B build-tests-w -DCMAKE_BUILD_TYPE=Release \
  -DDUSKSTUDIO_BUILD_TESTS=ON \
  -DDUSKSTUDIO_REQUIRE_NATIVE_LV2=ON \
  -DDUSKSTUDIO_REQUIRE_PIPEWIRE=ON \
  -DJUCE_PATH="$JUCE" -DDUSK_PLUGINS_PATH="$DONOR" \
  -DDPF_PATH="$DPF" -DDPF_WIDGETS_PATH="$DPF_WIDGETS"
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
#7. List the registered tests, identify the exact name, and rerun that name with
failure output enabled:

```bash
ctest --test-dir build-tests-w -N | grep -E 'ALSA seq.*subscription'
ctest --test-dir build-tests-w --output-on-failure \
  -R '^ALSA seq backend does not report its own subscriptions as a change$'
```

## Release gate and remaining work

The #262-#270 tag gate consisted of #262-#265, the documentation part of #266,
and #267-#270. Those parts are complete. In particular, #266's documentation
half had to land before tagging and is independently complete. Its remaining
CMake half is separately closable and may land after the v0.13.0 tag.

- #178, release mechanics: verify the metadata branch and current repository
  against the issue's still-valid acceptance criteria and prepare it for Marc
  to land on `origin/main`. Leave merging and tagging to Marc. Do not redo
  release chores already completed. The untagged 0.12.7 wave is folded into
  the 0.13.0 notes; do not create a 0.12.7 tag unless Marc reverses that
  decision. Ask Marc for the real package-contact address that must replace
  `support@duskaudio.example`; never invent one. Do not run
  `scripts/update-patrons.py` locally: the tag workflow refreshes patrons in its
  isolated donor checkout and refuses to ship stale data. The local patrons step
  printed at `scripts/bump-version.sh:342` at this handoff revision is superseded
  and must be corrected under #178 before tagging. Follow the tracked release
  procedures in `packaging/README.md` and `docs/MAINTAINER-GUIDE.md`.
- #266, CMake half: add a dependency-aware opt-out for the unconditional
  libsodium dependency, keeping missing libsodium fatal when catalog support is
  explicitly requested. Gate the SFZ catalog sources and tests, verify deb/rpm
  dependency emission, and make `sodium_init()` ordering race-free. Before
  closing it, build both option states: the enabled build must retain the
  libsodium link and catalog tests; the disabled build must omit that link and
  those tests. Configure and package separate ON and OFF build directories,
  inspect each binary with `readelf`, list each test set with `ctest -N`, build
  artifacts with `cpack -G DEB` and `cpack -G RPM`, then compare
  `dpkg-deb -f <deb> Depends` and `rpm -qpR <rpm>` output. On non-Debian hosts,
  distinguish a missing `dpkg-shlibdeps` database from a product failure. The
  enabled build must satisfy either the dynamic or static linkage check above.
  The disabled build must have neither a libsodium `NEEDED` entry nor a sodium
  library cache entry; any match is a failure.
- #271, repository maintenance: each push, discard, branch deletion, and
  worktree removal needs Marc's explicit authorization.
- #272, release workflows: add token preflights where missing and make release
  body verification detect an unfilled summary slot. Marc must decide before
  tagging: merge #272 first so it protects v0.13.0, or explicitly defer it to
  v0.14. Landing it after the v0.13.0 tag cannot protect that tag.
- #274, product decision: ask Marc whether to rename `vca_overeasy`. If yes,
  scope the change to the session serializer's write and read sites
  (`SessionSerializer.cpp:629` and `:1285` at this handoff revision): write the
  new session JSON key, read both session keys, and reset to the model default
  when neither key exists so a previous session's value cannot leak through.
  Implement the reset at this call site or through an opt-in default argument;
  do not change `loadB`'s behavior for its sibling keys. File their analogous
  absent-key behavior as a follow-up rather than widening #274. Document the
  migration and add tests for new, legacy, and absent keys. Do not rename the
  donor multi-compressor's APVTS parameter ID or its tests; that identifier is
  part of shipped preset compatibility and needs a separate migration decision.

Tag mechanics remain Marc's: assist, never absorb. The order is metadata bump,
land on `origin/main`, tag, then verify all release assets before announcing.

Report each issue's changes, rationale, verification output, and any review
finding fixed. Start by listing the open 0.13.0 milestone issues and reading
only those issue bodies.
