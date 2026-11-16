# Put in front of every script the host sends inline (-EncodedCommand), after
# the variables it sets ($root first), and served in front of every package
# phase with lib.ps1, so none of them depends on a copy of the scripts in the
# guest.

# Closing the ssh session ends only the shell it started, so a run's build and
# test processes are found by the tree on their command line. CMake, ctest and
# Python write it with forward slashes, and a longer name that starts the same
# (dusk-audit-win2) is another tree.
function Get-TreeProcesses {
  $parts = @($root.TrimEnd('\') -split '\\' | ForEach-Object { [regex]::Escape($_) })
  $pattern = '(?i)' + ($parts -join '[\\/]') + '(?=[\\/"'' ]|$)'
  @(Get-CimInstance Win32_Process | Where-Object {
    $_.ProcessId -ne $PID -and $_.CommandLine -and $_.CommandLine -match $pattern
  })
}

function Format-RegressGb([double]$bytes) {
  '{0:N1} GB' -f ($bytes / 1GB)
}

function Get-RegressFree([string]$path) {
  (New-Object IO.DriveInfo ([IO.Path]::GetPathRoot($path))).AvailableFreeSpace
}

function Test-RegressReparse([string]$path) {
  $item = Get-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
  return ($null -ne $item) -and (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
}

# robocopy, because Get-ChildItem in PowerShell 5.1 stops at paths past
# MAX_PATH, and the build tree has them. /L lists without copying; /XJ keeps it
# out of junctions.
function Get-RegressBytes([string]$path) {
  if (-not (Test-Path -LiteralPath $path)) { return [long]0 }
  if (Test-Path -LiteralPath $path -PathType Leaf) { return [long](Get-Item -LiteralPath $path -Force).Length }
  $nowhere = Join-Path ([IO.Path]::GetPathRoot($path)) 'dusk-regress-size-listing'
  $out = & robocopy.exe $path $nowhere /L /S /XJ /BYTES /NJH /NFL /NDL /NC /NP /R:0 /W:0
  $global:LASTEXITCODE = 0
  foreach ($line in $out) {
    if ($line -match '^\s*Bytes\s*:\s*(\d+)') { return [long]$Matches[1] }
  }
  return [long]0
}

# Remove-Item -Recurse and a robocopy /MIR purge both follow a junction into
# its target on PowerShell 5.1. rd removes the link itself, and with the \\?\
# prefix it takes paths past MAX_PATH.
function Remove-RegressEntry([string]$path) {
  $item = Get-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
  if ($null -eq $item) { return $true }
  if ($item.PSIsContainer) {
    & cmd.exe /d /c rd /s /q "\\?\$path" 2>&1 | Out-Null
  } else {
    try {
      $item.Attributes = [IO.FileAttributes]::Normal
      [IO.File]::Delete($path)
    } catch { }
  }
  $global:LASTEXITCODE = 0
  return -not (Test-Path -LiteralPath $path)
}

# Entries first and the marker last, so a removal that stops part way leaves a
# folder that still carries its marker and that the next clean takes again. The
# root itself can still refuse to go once it is empty (some process has it as
# its current directory), so the marker is put back when it does. A root that is
# itself a link is refused.
function Remove-RegressTree([string]$path, [string]$marker = '') {
  if (-not (Test-Path -LiteralPath $path)) { return $true }
  if (Test-RegressReparse $path) { return $false }
  if (Test-Path -LiteralPath $path -PathType Leaf) { return (Remove-RegressEntry $path) }
  $ok = $true
  foreach ($entry in @(Get-ChildItem -LiteralPath $path -Force -ErrorAction SilentlyContinue)) {
    if ($marker -and $entry.Name -eq $marker) { continue }
    if (-not (Remove-RegressEntry $entry.FullName)) { $ok = $false }
  }
  if (-not $ok) { return $false }
  if (-not $marker) { return (Remove-RegressEntry $path) }
  $markerPath = Join-Path $path $marker
  $markerText = Get-Content -LiteralPath $markerPath -Raw -ErrorAction SilentlyContinue
  if (-not (Remove-RegressEntry $markerPath)) { return $false }
  if (Remove-RegressEntry $path) { return $true }
  if (Test-Path -LiteralPath $path -PathType Container) {
    try {
      [IO.File]::WriteAllText($markerPath, $(if ($markerText) { $markerText } else { 'dusk-regress marker' }))
    } catch { }
  }
  return $false
}

# The tree's size by top-level entry, then the free space on its drive, as
# lines the host reads: regress:part <name> <bytes>, regress:footprint <tree
# bytes> <free bytes> <drive>.
function Write-RegressFootprint {
  $total = [long]0
  foreach ($entry in @(Get-ChildItem -LiteralPath $root -Force -ErrorAction SilentlyContinue)) {
    $bytes = Get-RegressBytes $entry.FullName
    $total += $bytes
    if ($entry.PSIsContainer) { "regress:part {0} {1}" -f $entry.Name, $bytes }
  }
  $build = Join-Path $root 'src\build-tests'
  if (Test-Path -LiteralPath $build) { "regress:part src\build-tests {0}" -f (Get-RegressBytes $build) }
  "regress:footprint {0} {1} {2}" -f $total, (Get-RegressFree $root), [IO.Path]::GetPathRoot($root)
}
