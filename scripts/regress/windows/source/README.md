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
| `preflight.ps1` | sent inline before the copy: fails when anything is already running in the tree, then empties `regress\` |
| `update.cmd` | `<commit> <base>[+<base>...] [<bundle>]`: runs `setup.cmd`, fetches the bases by hash, applies the bundle of local commits, checks the commit out with its submodules, moves the donor to `DONOR_REV` and DAF to the tip of main, and reinstalls Mesa and Python (`deps.ps1`) or the vcpkg packages (`vcpkg.cmd`) when the file pinning them changed |
| `build.cmd` | CI's three targets at `-j4`; runs `configure.cmd` first when the tree has no cache or `configure.cmd` changed since it last ran |
| `mesa.cmd`, `verify-mesa.ps1` | CI's "Verify packaged Mesa selects llvmpipe" step and the files its upload steps require |
| `ctest.cmd` | `list` prints the inventory the host checks CI's required test names against; otherwise the whole suite, serially, as CI runs it |
| `cancel.ps1` | sent inline on a timeout or interrupt: ends every process whose command line names the tree, since closing the ssh session leaves them running |
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

To start over, delete the tree in the guest and run the leg again.
