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
| `README-linux.txt` | End-user run/install notes bundled at the tarball top level |

## Prerequisites

- A Release build in `build-linux/` with both the `DuskStudio` and
  `dusk-studio-plugin-host` artefacts present.
- ImageMagick (`magick` or `convert`) on `$PATH` — used to scale the brand
  icon to the 256×256 PNG the desktop entry references.
- The brand icon at `assets/ds-icon.png`.

## Building the tarball

Done outside CMake by `scripts/package-tarball.sh`, which stages the program
directory, copies the integration assets, generates the icon, and packs
everything with `tar`. Run from a clean Ubuntu 22.04 build:

```bash
# 1. Build Dusk Studio as usual.
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux -j

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

1. Tag the release: `git tag vX.Y.Z && git push --tags`.
2. Wait for the `Linux build` workflow to upload the build artefacts.
3. Pull the artefacts, run `scripts/package-tarball.sh` to produce the `.tar.xz`.
4. SHA256 the file: `sha256sum dusk-studio-*-Linux-*.tar.xz > SHA256`.
5. Upload the tarball + SHA256 + `RELEASE_NOTES.md` to the Patreon post.
6. Pinned support note: paste `DuskStudio --version` output into any DM.
