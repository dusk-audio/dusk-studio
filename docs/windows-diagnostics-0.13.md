# Windows 0.13 diagnostics session: issues #116 and #237

One hardware session, two checklists - the issue #116 device and SFZ matrix,
and the hosted-editor matrix owed since #237. Run both in the same session so
a single log covers everything.

This temporary build adds support logging only when
`DUSKSTUDIO_SUPPORT_DIAGNOSTICS` is enabled. Normal launches are unchanged.
ASIO, SFZ loading, plugin hosting, and hosted plugin editors are all included.

The session notepad is absent from this build by design: that unrelated
DPF-Widgets surface does not currently compile under MSVC and nothing under
test depends on it. Its absence is not a finding.

## Launch

Download `DuskStudio-Windows-diagnostics.zip` from the private
`dusk-audio/dusk-studio-releases` prerelease named `diagnostics-116`, then
extract the complete bundle. In PowerShell, from the extracted directory:

```powershell
$ErrorActionPreference = "Stop"
Get-Content .\SHA256SUMS | ForEach-Object {
  $hash, $relative = $_ -split '\s+', 2
  $actual = (Get-FileHash -Algorithm SHA256 $relative).Hash.ToLowerInvariant()
  if ($actual -ne $hash) { throw "checksum mismatch: $relative" }
}
$env:DUSKSTUDIO_SUPPORT_DIAGNOSTICS = "1"
& ".\bin\DuskStudio.exe"
```

The build is unsigned, so SmartScreen will warn on first launch. `SHA256SUMS`
covers every other file in the bundle and is what confirms the extract is
intact.

Relaunch Dusk Studio when a row calls for a device state before launch. Use
**Rescan devices** after changing hardware state within a row. When finished,
quit Dusk Studio and disable the flag:

```powershell
Remove-Item Env:DUSKSTUDIO_SUPPORT_DIAGNOSTICS
```

The resulting log is in `%APPDATA%\Dusk Studio\log`. Keep the unredacted
`dusk-studio-YYYYMMDD.log` with the private hardware report. The log contains
full local file and device names, so post only the relevant lines to issue
#116 after redacting usernames and other private path components.

## Issue #116 matrix

For every row, record the exact device state, selected backend/device, action,
on-screen result, and relevant diagnostic lines.

### ASIO

- Interface powered and idle before launch; select it and verify audio I/O.
- Saved ASIO device powered off or unplugged before launch.
- Interface occupied by another audio application before launch.
- Reconnect or release the interface, click **Rescan devices**, then select it.

### SFZ

Use the same known-good instrument and samples where possible.

- ASCII-only path on a local drive.
- Path containing non-ASCII characters on a local drive.
- ASCII-only path on a mapped drive or UNC network share.
- Path containing non-ASCII characters on a mapped drive or UNC share.

For network tests, record whether the `sample=` filename casing exactly matches
the real files. Capture the full on-screen error text for every failure.

## Editor matrix owed since #237

Exercise AU, CLAP, and VST3 where that format is supported:

- Open, close, and reopen an editor on a track and on an aux.
- Save and reload the session with hosted plugins.
- Replace and evict hosted plugins.
- Undo and redo the relevant plugin action.
- Quit with an editor open.
- Enter and leave fullscreen with an editor open.
