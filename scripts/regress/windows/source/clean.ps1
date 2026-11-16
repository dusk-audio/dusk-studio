# Sent inline by scripts/regress/clean.sh with $root and inline-lib.ps1 in
# front. Removes what the regression runner owns in the guest and nothing else:
# the source tree, only with its marker, and the package runs' folders, only
# with theirs. Everything it leaves it names; a warn: line is something of the
# runner's that is still there.
$ErrorActionPreference = 'Continue'
$status = 0
$rgMarker = Join-Path $root '.dusk-regress-tree'

if (-not (Test-Path -LiteralPath $root)) {
  "no source tree at $root"
} elseif (Test-RegressReparse $root) {
  "error: $root is a link; not removing through it"
  $status = 1
} elseif (-not (Test-Path -LiteralPath $rgMarker -PathType Leaf)) {
  "error: $root has no $rgMarker marker, so it is not removed. A windows-source run adopts a tree an earlier runner made (one with stamps\ and regress\env.cmd); anything else is not the runner's"
  $status = 1
} else {
  $busy = @(Get-TreeProcesses)
  if ($busy.Count -gt 0) {
    "error: {0} process(es) are running in {1}; not removing it" -f $busy.Count, $root
    $busy | ForEach-Object { "  {0} {1}" -f $_.ProcessId, $_.CommandLine }
    $status = 1
  } else {
    $bytes = Get-RegressBytes $root
    if (Remove-RegressTree $root '.dusk-regress-tree') {
      "removed the source tree {0} ({1})" -f $root, (Format-RegressGb $bytes)
    } else {
      "error: could not remove all of $root; it keeps its marker, so the next clean takes the rest"
      $status = 1
    }
  }
}

# A package run's folder is in use while its app runs. One whose phase 4 never
# finished still holds Notepad's saved session, which nobody has put back.
$apps = @(Get-Process DuskStudio, dusk-studio-plugin-host -ErrorAction SilentlyContinue)
foreach ($run in @(Get-ChildItem -LiteralPath $env:LOCALAPPDATA -Directory -Filter 'dusk-regress-*' -ErrorAction SilentlyContinue)) {
  if (-not (Test-Path -LiteralPath (Join-Path $run.FullName '.dusk-regress-run'))) {
    "warn: left alone, no .dusk-regress-run marker: $($run.FullName)"
    continue
  }
  if (Test-RegressReparse $run.FullName) {
    "error: $($run.FullName) is a link; not removing through it"
    $status = 1
    continue
  }
  $activation = Join-Path $run.FullName 'activation'
  if ((Test-Path -LiteralPath "$activation\notepad-localstate") -and -not (Test-Path -LiteralPath "$activation\phase4.done")) {
    "error: $($run.FullName) holds Notepad's saved session from a phase 4 that never put it back; restore $activation\notepad-localstate by hand, then remove the folder"
    $status = 1
    continue
  }
  if ($apps.Count -gt 0) {
    "error: Dusk Studio is running in the guest (pid {0}), so a package run may still be using {1}; not removing it" -f (($apps | ForEach-Object Id) -join ', '), $run.FullName
    $status = 1
    continue
  }
  $bytes = Get-RegressBytes $run.FullName
  if (Remove-RegressTree $run.FullName '.dusk-regress-run') {
    "removed the package run folder {0} ({1})" -f $run.FullName, (Format-RegressGb $bytes)
  } else {
    "error: could not remove all of $($run.FullName); it keeps its marker"
    $status = 1
  }
}

# Phase 4 lends Explorer these for one launch, marked with the run folder that
# lent them; a run that died before taking them back leaves them pointing at a
# folder that is gone.
$owner = (Get-Item 'HKCU:\Environment').GetValue('DUSKSTUDIO_REGRESS_ENV_OWNER')
if ($null -ne $owner -and -not (Test-Path -LiteralPath $owner)) {
  foreach ($n in @('DUSKSTUDIO_CONFIG_DIR', 'DUSKSTUDIO_MUSIC_DIR', 'DUSKSTUDIO_REGRESS_ENV_OWNER')) {
    [Environment]::SetEnvironmentVariable($n, $null, 'User')
  }
  "cleared the user environment a phase 4 of $owner left behind"
}

# The account is shared, and temp_<hex> is JUCE's generic name, so these are
# counted, never removed.
$temp = [IO.Path]::GetTempPath()
$appTemp = @(Get-ChildItem -LiteralPath $temp -Force -ErrorAction SilentlyContinue |
  Where-Object { $_.Name -like 'temp_*' -or $_.Name -like 'dusk-*' })
if ($appTemp.Count -gt 0) {
  "left alone: {0} temp_* and dusk-* entries in {1}; the account is shared and they carry no runner marker" -f $appTemp.Count, $temp
}
foreach ($view in 'Registry64', 'Registry32') {
  $uninstall = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', $view).OpenSubKey(
    'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall')
  if ($null -eq $uninstall) { continue }
  foreach ($name in $uninstall.GetSubKeyNames()) {
    $entry = $uninstall.OpenSubKey($name)
    if ($null -eq $entry) { continue }
    if ("$($entry.GetValue('DisplayName'))" -like 'Dusk Studio*') {
      "left installed: {0} {1} in {2} (uninstalling needs a UAC prompt)" -f $entry.GetValue('DisplayName'), $entry.GetValue('DisplayVersion'), $entry.GetValue('InstallLocation')
    }
    $entry.Close()
  }
  $uninstall.Close()
}
"regress:free {0} {1}" -f (Get-RegressFree $root), [IO.Path]::GetPathRoot($root)
exit $status
