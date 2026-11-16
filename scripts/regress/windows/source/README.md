# Windows source leg, guest side

`scripts/regress/windows-source.sh` (and `scripts/regress.sh prepush`, which
runs it beside the Linux and macOS legs) builds the checkout's HEAD in the
libvirt `win11` guest over ssh and runs CI's Windows checks there. On every run
it replaces `<tree>\regress\` in the guest with this folder, `.cmd` files
converted to CRLF, so nothing here is ever edited in the guest.

The tree is `C:\Users\marc\dusk-audit-win` unless `DUSK_REGRESS_WIN_ROOT` says
otherwise. `env.cmd` takes it from the folder the scripts were copied into, so
the host side is the only place that names it.

| Script | Used for |
|---|---|
| `inline-lib.ps1` | sent in front of every inline script, and served in front of every package phase: the process lookup by tree, the sizes, the removal (entry by entry with `rd`, which takes a junction rather than its target and paths past MAX_PATH, marker last) and the footprint lines |
| `clock.ps1` | sent inline first: the guest's UTC clock, which the host compares with its own before anything in the tree moves |
| `preflight.ps1` | sent inline before the copy: fails when anything is already running in the tree, when the folder is not the runner's (no `.dusk-regress-tree` marker; one an older runner made, with `stamps\` and `regress\env.cmd`, is adopted); then removes `tmp\` and the last run's `regress\`, removes `src\build-tests` when a file in it is stamped more than 2 s after now or a folder in or above it cannot be listed (and stamps sources and dependencies in the future back to now), marks the tree not-content-indexed, and fails when its drive has less free space than a cold or a warm run needs |
| `update.cmd` | `<commit> <base>[+<base>...] [<bundle>]`: runs `setup.cmd`, fetches the bases by hash, applies the bundle of local commits, checks the commit out with its submodules, moves the donor to `DONOR_REV` and DAF to `DAF_REV`, and reinstalls Mesa and Python (`deps.ps1`) or the vcpkg packages (`vcpkg.cmd`) when the file pinning them changed |
| `built-check.ps1` | run by `build.cmd` after a build in which ninja did no work: fails (exit 87) when the commit differs from `stamps\built-commit` and a file the build reads changed between the two, by `build.ninja` (less its input-less `phony` edges, and the `RERUN_CMAKE` edge when CMake re-ran) and ninja's deps log, in absolute and build-relative form; records the commit on a pass, nothing on a warning, which a file it could not read also gives |
| `build.cmd` | CI's three targets at `-j4`; runs `configure.cmd` first when the tree has no cache or `configure.cmd` changed since it last ran; takes the commit, records it in `stamps\built-commit` when ninja did work, and runs `built-check.ps1` when it did none |
| `mesa.cmd`, `verify-mesa.ps1` | CI's "Verify packaged Mesa selects llvmpipe" step and the files its upload steps require |
| `ctest.cmd` | `list` prints the inventory the host checks CI's required test names against; otherwise the whole suite, serially, as CI runs it |
| `cancel.ps1` | sent inline on a timeout or interrupt: ends every process whose command line names the tree, since closing the ssh session leaves them running |
| `tidy.ps1` | sent inline at the end of every run past its preflight, pass or fail: removes `regress\head.bundle` and `tmp\`, then prints the tree's size by part and the drive's free space for the `footprint` row |
| `clean.ps1` | sent inline by `scripts/regress.sh clean windows`: removes the tree (only with its marker, only when nothing runs in it) and the package runs' `%LOCALAPPDATA%\dusk-regress-*` folders (only with their `.dusk-regress-run` marker), and names everything it leaves |
| `env.cmd` | the tree root and the guest's tool paths, each overridable through its `DUSK_WIN_*` variable; `toolchain` adds the MSVC environment and the private Python |

`configure.cmd` repeats the configure step of
`.github/workflows/windows-tests.yml`; change the two together.

## Recreating the guest tree

The guest needs, outside the tree: Visual Studio 2022 Build Tools with the C++
workload (`vcvars64.bat`, CMake and Ninja come with it), Git for Windows, 7-Zip,
a vcpkg clone at `C:\Users\marc\vcpkg` new enough for the `builtin-baseline` in
`vcpkg.json`, JUCE 8.0.4 at `C:\Users\marc\JUCE-8.0.4`, the Steinberg ASIO SDK
at `C:\Users\marc\SDKs\asiosdk`, and OpenSSH Server with this box's
`~/.ssh/dusk_winvm.pub` authorized for the account. None of it needs Program
Files beyond what those installers put there.

Then run `scripts/regress.sh windows-source` from the Linux box. The first run
creates the tree: `setup.cmd` clones `src` and initialises the donor and DAF
repositories, `update.cmd` fetches them and installs Mesa, Python and the vcpkg
packages, and `build.cmd` configures `build-tests`. That run is a full build;
later runs rebuild only what the commit changed.

## What a run leaves

The tree is a cache the runner owns, marked by `.dusk-regress-tree` at its root.
A run leaves the tree as it was built, and nothing else: the bundle goes at the
end of the run and `tmp\` (every step's `TEMP` and `TMP`) at the end and again
at the start of the next. The `footprint` row names the tree's size and the
drive's free space. To start over, or after the last run of a branch, run
`scripts/regress.sh clean windows` (or `clean all`, or the run with `--clean`)
from the Linux box; it refuses a tree without the marker. Deleting the tree by
hand works too.
