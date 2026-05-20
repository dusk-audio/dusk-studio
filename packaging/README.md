# Linux packaging

Dusk Studio is shipped to Patreon members as a self-contained AppImage. The
desktop entry, AppStream metadata, and MIME registration in this folder
are bundled inside the AppImage so installation registers Dusk Studio with
GNOME Software / KDE Discover and double-clicking `session.json`
launches it.

## Files

| File | Purpose |
|------|---------|
| `audio.dusk.studio.desktop` | Desktop Entry (XDG) — launcher icon, MIME association, WM class match. Filename matches `<id>` in the AppStream XML per spec (component-id + `.desktop`). |
| `DuskStudio.appdata.xml` | AppStream component — app-store metadata, summary, description |
| `DuskStudio.mime.xml` | MIME info — registers `application/x-dusk-studio-session` for `session.json` |

## Prerequisites

- `linuxdeploy` on `$PATH`. Not packaged by most distros — grab the
  AppImage release from
  <https://github.com/linuxdeploy/linuxdeploy/releases>, `chmod +x`,
  move into `/usr/local/bin/` or `~/.local/bin/`.
- A 256×256 PNG icon at `packaging/DuskStudio.png`. **Not committed to the
  repo** — provide your own (or symlink the brand asset) before
  running Step 2. The AppImage build fails loudly if the icon file is
  missing; AppStream metainfo also requires an icon to validate.

## Building an AppImage (one-shot manual procedure)

Done outside CMake because `linuxdeploy` and `appimagetool` are
external tooling that don't belong in the configure step. Run from a
clean Ubuntu 22.04 build:

```bash
# 1. Build Dusk Studio as usual.
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux -j

# 2. Stage AppDir.
mkdir -p AppDir/usr/bin AppDir/usr/share/applications \
         AppDir/usr/share/metainfo AppDir/usr/share/mime/packages \
         AppDir/usr/share/icons/hicolor/256x256/apps

cp build-linux/DuskStudio_artefacts/Release/DuskStudio           AppDir/usr/bin/
cp packaging/audio.dusk.studio.desktop                   AppDir/usr/share/applications/
cp packaging/DuskStudio.appdata.xml                          AppDir/usr/share/metainfo/
cp packaging/DuskStudio.mime.xml                             AppDir/usr/share/mime/packages/
cp packaging/DuskStudio.png                                  AppDir/usr/share/icons/hicolor/256x256/apps/

# 3. Pack with linuxdeploy.
linuxdeploy --appdir AppDir \
            --desktop-file packaging/audio.dusk.studio.desktop \
            --icon-file packaging/DuskStudio.png \
            --output appimage
```

Output: `DuskStudio-<version>-x86_64.AppImage` ready to upload to Patreon.

## Patreon delivery checklist

1. Tag the release: `git tag vX.Y.Z && git push --tags`.
2. Wait for `Linux build` workflow to upload `DuskStudio-linux-x86_64`.
3. Pull the artifact, repackage as AppImage per above.
4. SHA256 the file: `sha256sum Dusk Studio-vX.Y.Z-x86_64.AppImage > SHA256`.
5. Upload AppImage + SHA256 + `RELEASE_NOTES.md` to Patreon post.
6. Pinned support note: paste `Dusk Studio --version` output into any DM.
