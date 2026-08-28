# Handoff: finish the Dusk Studio 0.13.1 release

You are picking up a release that is code-complete. Every defect that blocked it is
fixed and merged or sitting in one open PR. What remains is a merge, a tag, and an
acceptance pass on the artifacts the tag produces.

Read [.claude/commands/release.md](../.claude/commands/release.md) first. It is the
procedure. This document only records where the work stopped and what the previous
session learned the hard way.

## Ground truth to re-verify before trusting anything here

State moves. Check these rather than believing the summary below:

```bash
git -C . fetch origin --prune
git log --oneline -1 origin/main
gh pr view 339 --json state,mergeable,commits
gh pr checks 339
gh issue list --milestone "0.13.1" --state open
git ls-remote --tags --refs origin refs/tags/v0.13.1
```

At handoff time: `origin/main` was `ccd21ef`, PR #339 was open and fully green on
branch `fix/notepad-first-frame-guard`, no `v0.13.1` tag existed anywhere, and the
only open issue in the milestone was #335.

## Where things stand

**Closed and merged** (PR #337, squashed as `ccd21ef`): the three original blockers.
#332 the Mesa D3D12 renderer ending the application, #333 the temporary stage
tracing, #334 the macOS disk image. All three were verified against packaged
artifacts, not CI status. That distinction is the whole reason 0.13.1 exists.

**Open, PR #339**: a first-frame guard plus a configuration switch, pulled into
0.13.1 deliberately. Refusing a renderer by name only protects against drivers
somebody has already lost a session to, and the D3D12 failure exits zero through a
graceful shutdown, so no exception handler sees it. The guard writes a marker naming
the renderer before the first frame is pumped and removes it once one completes; a
run that never comes back leaves the marker and the next launch refuses the notepad
and names the renderer. `notepad_enabled` in `app-config.properties` is the separate
explicit switch. The notepad owns the marker, the configuration owns the switch, and
neither writes the other's state.

**Not blocking**: #335 (notepad unverified on real Windows GPU hardware, cannot be
closed without hardware, and #339 makes it survivable), #336 (a cosmetic DGL
assertion printed on the refusal path 0.13.1 ships, already triaged to 0.14.0),
#338 (the notepad keeps its scale factor when the display scale changes, pre-existing,
deferred out of #337).

## Step 1: land PR #339

Every check passed at handoff, `Catch2 tests (MSVC x64 Release, Windows)` included.
That one was the last unknown: it is the first time `tests/appconfig_store.cpp` is
compiled by MSVC, and that file uses `_putenv_s`,
`std::filesystem::recursive_directory_iterator` and the `APPDATA` branch of the store
path. Its passing also proves the redirect works rather than merely compiling, since
`ScopedStore::file()` only searches inside the scratch directory and every case
requires the store to be found there.

Two things to weigh before merging:

- CodeRabbit returned "rate limited" on two of the three pushes to #339. It reports
  `pass`, but that is a default, not a review. The six commits touch notepad startup
  and the configuration writer. Give them a human read.
- The last green Windows *build* was from `cf3774e`, two commits stale, and it
  predates `tests/appconfig_store.cpp`, so it never compiled it. It proves nothing
  about what ships. Do not treat it as Windows validation.

## Step 2: tag

Only from `main`, clean tree, in sync with origin. `scripts/bump-version.sh` has
already run: `VERSION` is `0.13.1`, the appdata carries a `0.13.1` release entry, and
`packaging/RELEASE-NOTES.md` is rewritten for this release.

**The changelog heading is dated `2026-08-23`.** If you tag on a different day, fix
the date in `CHANGELOG.md` *and* the `date=` attribute on the `0.13.1` release entry
in `packaging/DuskStudio.appdata.xml`. The release-mechanics test checks the
changelog format but not that the two agree with each other or with reality.

Never move a published tag. If the binaries are wrong, cut 0.13.2.

## Step 3: acceptance, which is the actual remaining risk

Four workflows fire on the tag. Green CI is not acceptance: 0.13.0 passed every
workflow and every asset check and shipped a disk image that would not launch.

Consolidate the five per-job `SHA256SUMS.*` into one sorted `SHA256SUMS` (Step 8),
then `scripts/verify-release-assets.sh v0.13.1` must print `PASS: all 6 assets
present.` (Step 9). Then run Step 10 against the downloaded artifacts.

### Linux: never run once this cycle

This is the gap. No Linux acceptance has happened on any 0.13.1 build.

- **amd64**: `nuc` (see below) is openSUSE Leap 16.0, x86_64, and has Xvfb.
- **arm64**: no machine exists here. Report it untested rather than passed.

Extract each tarball, smoke the binary on its matching architecture under a private
Xvfb display with `WAYLAND_DISPLAY` unset, and check every non-system `DT_NEEDED`
entry is named in the tarball README:

```bash
readelf -d DuskStudio/DuskStudio | awk -F'[][]' '/NEEDED/ {print $2}'
```

### macOS

Run the script in issue #334 verbatim against the released DMG. It checks every
Mach-O in the bundle rather than just the main executable, verifies the signature on
the *installed* copy, and requires the image root to hold only `DuskStudio.app`,
`LICENSE` and `LICENSES.txt`. `hdiutil` needs `yes |` and its own `PIPESTATUS` read
because the image carries a license agreement.

If the build box is missing `../DAF` or `../DAF-Widgets`, configure fails hard rather
than quietly dropping the notepad. Clone them at the pins in
[.github/actions/clone-daf-stack/action.yml](../.github/actions/clone-daf-stack/action.yml).

### Windows

Install the released MSI on the VM and re-run the guard cases below.

## The Windows test rig, and three traps that fake a pass

```
Mac --ssh--> marc@192.168.1.230 (nuc)
    --ssh -i ~/.ssh/dusk_winvm--> marc@192.168.122.85 (win11, DESKTOP-GPG3I47)
```

The `dusk_winvm` key lives on `nuc`, not on the Mac, so the second hop runs from
`nuc`. VMs are under `qemu:///system`. `nuc` sleeps; if it is unreachable, that is
why.

1. **An SSH session on Windows is session 0 and has no desktop.** A GUI process
   launched from it gets a different OpenGL renderer than a real user, which is the
   variable under test. Launch through a scheduled task with an INTERACTIVE principal
   (`New-ScheduledTaskPrincipal -GroupId "S-1-5-4"`) to land in session 1. No stored
   password needed.
2. **The VM's execution policy blocks `.ps1` files** and helper scripts then fail
   silently with no output. Prepend
   `Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force`.
3. **Task Scheduler drops `&&` and redirection** from argument strings. Write a
   `.cmd` wrapper and point the task at that.

Quoting PowerShell through two SSH hops mangles it. Encode the script UTF-16LE, then
base64, and pass it to `powershell -EncodedCommand`.

The VM has no OpenGL past 1.1 by default: no ICD registered, the QXL adapter exposes
no `OpenGLDriverName`, no Mesa. `winget install 9NQPSL29BFFF` adds the Compatibility
Pack, which is the only way to get the Mesa D3D12 renderer #332 is about. That pack
ships `OpenGLOn12.dll` only, so `GALLIUM_DRIVER=llvmpipe` does **not** work: any Mesa
override makes it fall back to GDI Generic. Uninstall the pack afterwards if you
installed it.

`DUSKSTUDIO_OPEN_NOTEPAD=1` opens the notepad about four seconds after launch, which
is how these runs avoid clicking a toolbar button by coordinate.

### The four Windows cases, and what each proved last time

Against a dev MSI, all four passed:

| Case | Expected |
|---|---|
| Default renderer, Compatibility Pack installed | `renderer=D3D12 (Microsoft Basic Render Driver)`, refused by name, application alive. This configuration ended the process 3 of 3 runs before the fix. |
| No marker, GDI Generic | `display provides no OpenGL 3 context`, refused, no marker left behind |
| Planted marker | refused naming the renderer and the full `%APPDATA%` path, marker preserved |
| `notepad_enabled=0` | no notepad output at all; short-circuits before the driver is touched |

The marker lives at `%APPDATA%\Dusk Studio\notepad-first-frame`.

**Do not append to `app-config.properties` with `Add-Content` or `echo >>` without
checking the file ends with a newline.** The previous session did and produced
`ui_scale=0.74notepad_enabled=0`, which reads as a corrupted `ui_scale` and no
`notepad_enabled` at all. The writer was fixed to terminate its last line (commit
`0f2bc8f`), so files it writes are safe, but a file written by an older build is not.

## Housekeeping once the release is out

- Move #335 to 0.14.0 or accept that the 0.13.1 milestone closes with it open.
- Delete the local `backup/wip-0131-snapshot` branch; its work is on main.
- #338 stays out of 0.13.1.

## Standing rules that bite on this work

- **No new JUCE.** `tools/juce-gate.sh` is a ratchet and the allowlist only goes
  down. The tree is at 9206 occurrences. If a change needs a `juce::` reference in a
  file that has none to spare, use a `src/foundation/` seam or take the type out of
  the signature. Never write the literal `juce::` token in a comment; the gate greps
  text.
- Verification means the packaged artifact ran, not that CI was green.
- Do not add a Claude or Anthropic attribution trailer to commits or PR bodies.
- Do not `git push` without being asked, and never force-push `main`.
