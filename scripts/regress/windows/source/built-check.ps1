# Run by built.cmd after a successful build. The tree's objects are current
# only if ninja rebuilt what the commit change touched; when the clock has
# stepped back, everything built before the step looks newer than any source
# checked out after it, and ninja reports nothing to do on a stale build. So a
# build that did no work, at a commit other than the one the tree last built,
# fails here when any file the build reads changed between the two. "Reads" is
# ninja's own record: build.ninja for the sources and generated inputs, its
# deps log for the headers. A commit that changes only documents, or sources
# this platform does not compile, passes. The built commit is recorded only
# when the check passes, so a stale tree keeps failing until it is rebuilt.
param([string]$Root, [string]$Head, [string]$NoWork)
$ErrorActionPreference = 'Continue'
$stamp = Join-Path $Root 'stamps\built-commit'
$src = Join-Path $Root 'src'
$build = Join-Path $src 'build-tests'

function Save-Built { [IO.File]::WriteAllText($stamp, $Head) }
function ConvertTo-RegressKey([string]$path) { $path.Replace('$:', ':').Replace('\', '/').ToLowerInvariant() }
# A path counts only as a whole word of the text: foo.h is not in foo.hpp.
function Test-RegressMentions([string]$text, [string]$key) {
  if ($key.EndsWith('/')) { return $text.Contains($key) }
  $at = $text.IndexOf($key, [StringComparison]::Ordinal)
  while ($at -ge 0) {
    $next = $at + $key.Length
    if ($next -ge $text.Length -or " `r`n|:".Contains($text[$next])) { return $true }
    $at = $text.IndexOf($key, $next, [StringComparison]::Ordinal)
  }
  return $false
}

$prev = ''
if (Test-Path -LiteralPath $stamp) { $prev = ([IO.File]::ReadAllText($stamp)).Trim() }
if (-not $prev) { Save-Built; "built-commit: first build recorded, $Head"; exit 0 }
if ($prev -eq $Head) { "built-commit: the tree built $Head before"; exit 0 }
if ($NoWork -ne '1') { Save-Built; "built-commit: rebuilt from $prev to $Head"; exit 0 }

$changed = @(& git -C $src -c core.quotepath=off diff --name-only $prev $Head 2>$null)
if ($LASTEXITCODE -ne 0) {
  Save-Built
  "warn: ninja did no work for $Head, and what changed since $prev, the commit the tree last built, cannot be told (that commit is not in the guest's clone)"
  exit 0
}
$srcKey = (ConvertTo-RegressKey $src) + '/'
$files = @()
$folders = @()
$named = @{}
foreach ($rel in $changed) {
  if (-not $rel) { continue }
  $key = $srcKey + (ConvertTo-RegressKey $rel)
  $named[$key] = $rel
  $named[$key + '/'] = $rel
  if (Test-Path -LiteralPath (Join-Path $src $rel) -PathType Container) { $folders += $key + '/' } else { $files += $key }
}

$hits = @()
$ninjaFile = Join-Path $build 'build.ninja'
if (Test-Path -LiteralPath $ninjaFile) {
  $graph = ConvertTo-RegressKey ([IO.File]::ReadAllText($ninjaFile))
  $hits += @(($files + $folders) | Where-Object { Test-RegressMentions $graph $_ })
  $graph = $null
}
if ($hits.Count -eq 0) {
  $ninja = ''
  $cache = Join-Path $build 'CMakeCache.txt'
  if (Test-Path -LiteralPath $cache) {
    $line = Select-String -LiteralPath $cache -Pattern '^CMAKE_MAKE_PROGRAM:[A-Z]+=(.+)$' | Select-Object -First 1
    if ($line) { $ninja = $line.Matches[0].Groups[1].Value }
  }
  $depsFile = Join-Path $Root 'tmp\ninja-deps.txt'
  New-Item -ItemType Directory -Force -Path (Join-Path $Root 'tmp') | Out-Null
  if ($ninja) { & cmd.exe /d /c "`"$ninja`" -C `"$build`" -t deps > `"$depsFile`" 2>nul" }
  if (-not $ninja -or $LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $depsFile)) {
    Save-Built
    "warn: ninja did no work for $Head and $($changed.Count) files changed since $prev, but ninja's deps log could not be read to tell whether the build reads them"
    exit 0
  }
  $deps = ConvertTo-RegressKey ([IO.File]::ReadAllText($depsFile))
  Remove-Item -LiteralPath $depsFile -Force -ErrorAction SilentlyContinue
  $hits += @(($files + $folders) | Where-Object { Test-RegressMentions $deps $_ })
  $deps = $null
}

if ($hits.Count -gt 0) {
  "error: ninja did no work, but files the build reads changed since {2}, the commit the tree last built ({0} of the {1} changed; first {3}). The objects are stamped later than these sources, which is what a guest clock that stepped back leaves behind; remove {4} and run again." -f $hits.Count, $changed.Count, $prev, $named[$hits[0]], $build
  exit 1
}
Save-Built
"built-commit: no file the build reads changed since {1} ({0} changed)" -f $changed.Count, $prev
exit 0
