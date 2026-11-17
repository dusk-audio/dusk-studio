# Sent inline by windows-source.sh with $root, $coldGb and $warmGb in front,
# then inline-lib.ps1, so it runs before the scripts folder is replaced.
$ErrorActionPreference = 'Stop'
$rgMarker = Join-Path $root '.dusk-regress-tree'
$os = Get-CimInstance Win32_OperatingSystem
"{0} {1}, {2} MB free memory" -f $os.Caption, $os.Version, [int]($os.FreePhysicalMemory / 1024)
# Anything already running out of the tree is another run, and two in one
# tree overwrite each other's checkout and build.
$busy = @(Get-TreeProcesses)
if ($busy.Count -gt 0) {
  "error: {0} process(es) are already running in {1}:" -f $busy.Count, $root
  $busy | ForEach-Object { "  {0} {1}" -f $_.ProcessId, $_.CommandLine }
  exit 1
}
# The marker is what `scripts/regress.sh clean windows` removes a tree by. A
# tree an older runner made has none, but it holds stamps\ and the copied
# regress\env.cmd, which nothing else makes; anything else at the root is not
# the runner's to build in.
$markerText = 'dusk-regress source tree, a cache the regression runner owns; scripts/regress.sh clean windows removes it'
if (-not (Test-Path -LiteralPath $root -PathType Container)) {
  New-Item -ItemType Directory -Path $root | Out-Null
  Set-Content -LiteralPath $rgMarker -Value $markerText
  "created $root"
} elseif (Test-RegressReparse $root) {
  "error: $root is a link, not a folder the runner made"
  exit 1
} elseif (-not (Test-Path -LiteralPath $rgMarker -PathType Leaf)) {
  $entries = @(Get-ChildItem -LiteralPath $root -Force)
  $made = (Test-Path -LiteralPath (Join-Path $root 'stamps') -PathType Container) -and
    (Test-Path -LiteralPath (Join-Path $root 'regress\env.cmd') -PathType Leaf)
  if ($entries.Count -gt 0 -and -not $made) {
    "error: $root holds {0} entries and is not a tree the runner made (no $rgMarker, no stamps\ and regress\env.cmd); move it or point DUSK_REGRESS_WIN_ROOT elsewhere" -f $entries.Count
    exit 1
  }
  Set-Content -LiteralPath $rgMarker -Value $markerText
  if ($entries.Count -gt 0) { "adopted $root, made by an earlier runner, as the runner's tree" } else { "took the empty folder $root as the runner's tree" }
}
# Every TEMP and TMP of a run points at tmp\, and regress\ holds the last run's
# bundle, so both go before the free space is measured; env.cmd makes tmp\
# again.
foreach ($leftover in @((Join-Path $root 'tmp'), (Join-Path $root 'regress'))) {
  if (-not (Test-Path -LiteralPath $leftover)) { continue }
  if (Test-RegressReparse $leftover) { "error: $leftover is a link; not removing through it"; exit 1 }
  $bytes = Get-RegressBytes $leftover
  if (-not (Remove-RegressTree $leftover)) { "error: could not remove $leftover"; exit 1 }
  if ($bytes -gt 0) { "removed {0} ({1})" -f $leftover, (Format-RegressGb $bytes) }
}
# A guest whose clock ran ahead (the host compared the two before this) leaves
# build outputs stamped later than any source checked out after the clock steps
# back, and ninja then takes the stale build for current: the build goes and
# this run builds from nothing. A few seconds ahead do that too, as the
# checkout comes seconds after this, so the only allowance is FAT's 2 s write
# time granularity, the coarsest Windows keeps; a file written by now is never
# stamped later than that. A folder in or above the build that could not be
# listed may hold such files, so it costs the build too. Sources and
# dependencies stamped in the future are only stamped back to now, which costs
# their objects one rebuild instead of one on every run until that time has
# passed, so the same allowance does for them.
$now = [DateTime]::UtcNow
$future = Get-RegressTree $root $now.AddSeconds(2)
$build = Join-Path $root 'src\build-tests'
$buildPrefix = $build + '\'
$blind = @($future.Unlisted | Where-Object {
  ($_ + '\').StartsWith($buildPrefix, [StringComparison]::OrdinalIgnoreCase) -or $buildPrefix.StartsWith($_ + '\', [StringComparison]::OrdinalIgnoreCase)
})
if ($future.Unlisted.Count -gt $blind.Count) {
  "note: {0} folder(s) outside {1} could not be listed, so files there were not checked for stamps in the future" -f ($future.Unlisted.Count - $blind.Count), $build
}
if ($future.Files.Count -gt 0 -or $blind.Count -gt 0) {
  $inBuild = @($future.Files | Where-Object { (Get-RegressPlainPath $_.FullName).StartsWith($buildPrefix, [StringComparison]::OrdinalIgnoreCase) })
  $latest = ($future.Files | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1).LastWriteTimeUtc
  if ($inBuild.Count -gt 0) {
    "warn: {1} holds files stamped as late as {2:yyyy-MM-dd HH:mm:ss} UTC ({0} of them), after the guest's now, {3:yyyy-MM-dd HH:mm:ss} UTC: ninja would take them for newer than anything checked out today and build nothing. REMOVING {1}; this run builds from nothing." -f $inBuild.Count, $build, $latest, $now
  } elseif ($blind.Count -gt 0) {
    "warn: {0} folder(s) in or above {1} could not be listed, first {2}, so whether it holds files stamped after the guest's now cannot be told. REMOVING {1}; this run builds from nothing." -f $blind.Count, $build, $blind[0]
  }
  if ($inBuild.Count -gt 0 -or $blind.Count -gt 0) {
    if (-not (Remove-RegressTree $build)) { "error: could not remove $build"; exit 1 }
  }
  $stale = @($future.Files | Where-Object {
    -not (Get-RegressPlainPath $_.FullName).StartsWith($buildPrefix, [StringComparison]::OrdinalIgnoreCase) -and $_.FullName -notmatch '\\\.git(\\|$)'
  })
  $reset = 0
  foreach ($file in $stale) {
    try { $file.LastWriteTimeUtc = $now; $reset++ } catch { }
  }
  if ($stale.Count -gt 0) {
    "warn: sources and dependencies were stamped as late as {1:yyyy-MM-dd HH:mm:ss} UTC ({0} in all, {2} of them stamped back to now), so their objects rebuild once rather than on every run until then" -f $stale.Count, $latest, $reset
  }
}
# Windows Search indexes everything under the profile, the tree too; what is
# created under a folder carrying this attribute inherits it.
try {
  $rootItem = Get-Item -LiteralPath $root -Force
  $rootItem.Attributes = $rootItem.Attributes -bor [IO.FileAttributes]::NotContentIndexed
} catch {
  "note: could not keep Windows Search out of ${root}: $_"
}
$cold = -not (Test-Path -LiteralPath (Join-Path $root 'src\build-tests\CMakeCache.txt'))
$kind = $(if ($cold) { 'cold' } else { 'warm' })
$wantGb = $(if ($cold) { $coldGb } else { $warmGb })
$free = Get-RegressFree $root
$drive = [IO.Path]::GetPathRoot($root)
$line = "{0} has {1} free; a {2} run needs {3} GB" -f $drive, (Format-RegressGb $free), $kind, $wantGb
if ($free -lt [long]$wantGb * 1GB) { "error: $line"; exit 1 }
$line
New-Item -ItemType Directory -Path (Join-Path $root 'regress') | Out-Null
"tree $root ready"
exit 0
