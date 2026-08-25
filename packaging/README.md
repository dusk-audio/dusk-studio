# Linux packaging

Dusk Studio is shipped to Patreon members as a portable tarball: a
self-contained `DuskStudio/` program directory you run in place
(`./DuskStudio/DuskStudio`) plus an `install.sh` that does optional desktop
integration. The desktop entry, AppStream metadata, and MIME registration in
this folder are bundled into the tarball; `install.sh` copies them into place so
Dusk Studio registers with GNOME Software / KDE Discover and double-clicking
`session.json` launches it.

## Files

| File | Purpose |
|------|---------|
| `audio.dusk.studio.desktop` | Desktop Entry (XDG) — launcher icon, MIME association, WM class match. Ships a relative `Exec=DuskStudio`; `install.sh` rewrites it to the installed absolute path. Filename matches `<id>` in the AppStream XML per spec (component-id + `.desktop`). |
| `DuskStudio.appdata.xml` | AppStream component — app-store metadata, summary, description |
| `DuskStudio.mime.xml` | MIME info — registers `application/x-dusk-studio-session` for `session.json` |
| `DuskStudio.png` | 256×256 hicolor icon the `Icon=DuskStudio` key resolves. Committed rather than derived at package time so a packager needs no image tooling and every build ships the same bytes. |
| `README-linux.txt` | End-user run/install notes bundled at the tarball top level |

## Prerequisites

- A Release build in `build-linux/` with both the `DuskStudio` and
  `dusk-studio-plugin-host` artefacts present.
- The DPF stack discoverable at configure time — sibling `../DPF` and
  `../DPF-Widgets` checkouts, or `-DDPF_PATH=` / `-DDPF_WIDGETS_PATH=`. Miss
  either and the native notepad is compiled out of the binary you are about to
  ship, announced by nothing louder than a
  `Native notepad: DPF / DPF-Widgets not found - disabled` line in the
  configure log. Clone instructions and the pinned revisions are in
  `BUILDING-LINUX.md` under "The native notepad".
- The committed 256×256 icon at `packaging/DuskStudio.png` — the size the
  desktop entry's `Icon=DuskStudio` key resolves. Edit the master
  `assets/ds-icon.png` and you must regenerate it by hand with
  `magick assets/ds-icon.png -resize 256x256 -strip packaging/DuskStudio.png`
  (ImageMagick 6 spells the command `convert`);
  nothing checks that the two stay in sync.

## Building the tarball

Done outside CMake by `scripts/package-tarball.sh`, which stages the program
directory, copies the integration assets, and packs everything with `tar`. Run
from a clean Ubuntu 22.04 build.

Configure into a fresh `build-linux/`. `DUSKSTUDIO_ENABLE_NATIVE_UI` is a
cached option: a directory first configured without the DPF checkouts keeps the
notepad off after you clone them, and drops its STATUS line too, so a release
built in a reused directory can ship without any of the native views - notepad,
startup dialog, compressor editor, virtual keyboard - and without saying so.

```bash
# 1. Build Dusk Studio as usual.
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux -j6

# 2. Pack the tarball.
scripts/package-tarball.sh
```

Output: `dusk-studio-<version>-Linux-<arch>.tar.xz` in the repo root, ready to
upload to Patreon. Its structure:

```text
dusk-studio-<version>-Linux-<arch>/
  DuskStudio/                 portable program dir — run ./DuskStudio/DuskStudio in place
    DuskStudio
    dusk-studio-plugin-host
    share/                    .desktop, AppStream, MIME, icon (installed by install.sh)
  install.sh                  optional desktop/PATH/MIME integration
  README-linux.txt
```

`install.sh` (from `scripts/install-linux.sh`) handles integration: a user
install to `~/.local` by default, `sudo ./install.sh --system` system-wide
(`/opt` + `/usr/local` + `/usr/share`), and `./install.sh --uninstall` to remove
a previous install of the same scope. It copies the program dir into place, adds
a `DuskStudio` launcher on `$PATH`, installs the desktop / MIME / icon assets,
and refreshes the desktop databases. None of it is required — the program runs
straight from the extracted tarball.

## Patreon delivery checklist

1. Prepare the release metadata before tagging. Add the `CHANGELOG.md` section,
   then run `scripts/bump-version.sh X.Y.Z "Release summary"` to update
   `VERSION`, `packaging/DuskStudio.appdata.xml`, and
   `packaging/RELEASE-NOTES.md`.
2. Date the changelog, then complete the Patreon freshness procedure in
   [Maintainer Guide Part 10](../docs/MAINTAINER-GUIDE.md#part-10---release).
   Run `env -u DUSK_PLUGINS_PATH scripts/update-patrons.py --dry-run` from the
   primary checkout, compare the active tiers with the workflow-pinned donor,
   and update any rotated Actions secrets. Do not tag with missing Patreon
   secrets if the pinned supporter list is stale.
3. Review the complete metadata diff and commit it. Land
   that commit on `origin/main` and confirm the remote branch contains it before
   creating the tag.
4. Tag that exact commit and push the tag:
   `git tag -a vX.Y.Z -m "Dusk Studio X.Y.Z"` followed by
   `git push origin vX.Y.Z`.
5. Wait for the `Linux release (tarball)`, `macOS release (unsigned DMG)`,
   `Windows build`, and `Manual PDF` workflows. A complete release carries
   exactly six assets on the private `dusk-audio/dusk-studio-releases` release:
   two Linux tarballs, one macOS DMG, one Windows MSI, `MANUAL.pdf`, and one
   `SHA256SUMS` covering all five payloads. The workflows cannot see each
   other's outputs, so each still uploads its own `SHA256SUMS.<job>` file;
   replace those five with the single sorted `SHA256SUMS` and delete them.
   `docs/MAINTAINER-GUIDE.md` Part 10 has the commands; issue #321 removes the
   step.
6. Before announcing the release, run
   `scripts/verify-release-assets.sh vX.Y.Z`. Do not announce unless it reports
   all six assets and a populated release-summary slot.
7. Pinned support note: paste `DuskStudio --version` output into any DM.
