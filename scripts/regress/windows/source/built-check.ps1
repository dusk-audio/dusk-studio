# Run by build.cmd after a build in which ninja did no work. The tree's objects
# are current only if ninja rebuilt what the commit change touched; when the
# clock has stepped back, everything built before the step looks newer than any
# source checked out after it, and ninja reports nothing to do on a stale build.
# So at a commit other than the one the tree last built, this fails when any
# file that changed between the two is one the build reads. "Reads" is ninja's
# own record: build.ninja for sources and generated inputs, its deps log for
# headers (which MSVC's deps keep relative to the build folder). When ninja
# re-ran CMake first, the configure inputs on the RERUN_CMAKE edge compared
# correctly and do not count; everything else still does. A change to documents,
# to CMake inputs alone, or to sources this platform does not compile passes.
# The commit is recorded only on a pass: a warning records nothing, so the doubt
# stays until a build does work.
param([string]$Root, [string]$Head, [string]$BuildLog)
$ErrorActionPreference = 'Continue'
$stamp = Join-Path $Root 'stamps\built-commit'
$src = Join-Path $Root 'src'
$build = Join-Path $src 'build-tests'

function Save-Built { [IO.File]::WriteAllText($stamp, $Head) }
function ConvertTo-RegressKey([string]$path) { $path.Replace('$:', ':').Replace('\', '/').ToLowerInvariant() }
# A path counts only as a whole word of the text: foo.h is not in foo.hpp, and
# ../src/a.h is not in x/../src/a.h.
function Test-RegressMentions([string]$text, [string]$key) {
  $at = $text.IndexOf($key, [StringComparison]::Ordinal)
  while ($at -ge 0) {
    $next = $at + $key.Length
    $before = $at -eq 0 -or " `t`r`n|".Contains([string]$text[$at - 1])
    $after = $key.EndsWith('/') -or $next -ge $text.Length -or " `t`r`n|:".Contains([string]$text[$next])
    if ($before -and $after) { return $true }
    $at = $text.IndexOf($key, $next, [StringComparison]::Ordinal)
  }
  return $false
}

$prev = ''
if (Test-Path -LiteralPath $stamp) { $prev = ([IO.File]::ReadAllText($stamp)).Trim() }
if ($prev -eq $Head) { "built-commit: the tree built $Head before"; exit 0 }
if (-not $prev) {
  "warn: ninja did no work, and there is no record of the commit this tree last built, so whether it holds $Head cannot be told; nothing is recorded until a build does work"
  exit 0
}

$changed = @(& git -C $src -c core.quotepath=off diff --name-only $prev $Head 2>$null)
if ($LASTEXITCODE -ne 0) {
  "warn: ninja did no work for $Head, and what changed since $prev, the commit the tree last built, cannot be told (that commit is not in the guest's clone); nothing is recorded"
  exit 0
}
# A submodule shows as one path; what changed inside it is asked of the
# submodule's own clone, which has both commits only if both were checked out.
$paths = @()
$unknown = @()
foreach ($rel in $changed) {
  if (-not $rel) { continue }
  if (-not (Test-Path -LiteralPath (Join-Path $src $rel) -PathType Container)) { $paths += $rel; continue }
  $old = "$(& git -C $src ls-tree $prev -- $rel 2>$null)" -replace '^\S+\s+commit\s+(\S+)\s.*$', '$1'
  $new = "$(& git -C $src ls-tree $Head -- $rel 2>$null)" -replace '^\S+\s+commit\s+(\S+)\s.*$', '$1'
  $inner = @()
  if ($old -match '^[0-9a-f]{40}$' -and $new -match '^[0-9a-f]{40}$') {
    $inner = @(& git -C (Join-Path $src $rel) -c core.quotepath=off diff --name-only $old $new 2>$null)
  }
  if ($old -notmatch '^[0-9a-f]{40}$' -or $new -notmatch '^[0-9a-f]{40}$' -or $LASTEXITCODE -ne 0) { $unknown += $rel; continue }
  foreach ($f in $inner) { if ($f) { $paths += "$rel/$f" } }
}
if ($unknown.Count -gt 0) {
  "warn: ninja did no work for $Head, and what changed inside $($unknown -join ', ') since $prev cannot be told; nothing is recorded"
  exit 0
}

# Each path in the two forms ninja writes: absolute, and relative to the build
# folder, which sits directly in the checkout.
$srcKey = (ConvertTo-RegressKey $src) + '/'
$forms = @{}
foreach ($rel in $paths) {
  $key = ConvertTo-RegressKey $rel
  $forms[$srcKey + $key] = $rel
  $forms['../' + $key] = $rel
}

$regenerated = (Test-Path -LiteralPath $BuildLog) -and
  [bool](Select-String -LiteralPath $BuildLog -SimpleMatch -Pattern 'Re-running CMake' -Quiet)
$hits = @()
$ninjaFile = Join-Path $build 'build.ninja'
if (Test-Path -LiteralPath $ninjaFile) {
  # Comment lines name each CMakeLists.txt a part of the file comes from.
  $graph = [regex]::Replace((ConvertTo-RegressKey ([IO.File]::ReadAllText($ninjaFile))), '(?m)^#[^\n]*', '')
  # An edge whose rule is phony with no inputs only says its paths may be
  # missing; nothing reads them through it. CMake lists its configure inputs
  # on one such edge, beside RERUN_CMAKE.
  foreach ($ending in @(": phony`r`n", ": phony`n")) {
    $at = $graph.IndexOf($ending, [StringComparison]::Ordinal)
    while ($at -ge 0) {
      $from = $graph.LastIndexOf("`nbuild ", $at, [StringComparison]::Ordinal) + 1
      $graph = $graph.Remove($from, $at + $ending.Length - $from)
      $at = $graph.IndexOf($ending, $from, [StringComparison]::Ordinal)
    }
  }
  if ($regenerated) {
    $rerun = $graph.IndexOf(': rerun_cmake', [StringComparison]::Ordinal)
    if ($rerun -ge 0) {
      $from = $graph.LastIndexOf("`n", $rerun) + 1
      $to = $graph.Length
      foreach ($blank in @("`n`n", "`n`r`n")) {
        $end = $graph.IndexOf($blank, $rerun, [StringComparison]::Ordinal)
        if ($end -ge 0 -and $end -lt $to) { $to = $end }
      }
      $graph = $graph.Remove($from, $to - $from)
    }
  }
  $hits += @($forms.Keys | Where-Object { Test-RegressMentions $graph $_ })
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
    "warn: ninja did no work for $Head and $($paths.Count) files changed since $prev, but ninja's deps log could not be read to tell whether the build reads them; nothing is recorded"
    exit 0
  }
  $deps = ConvertTo-RegressKey ([IO.File]::ReadAllText($depsFile))
  Remove-Item -LiteralPath $depsFile -Force -ErrorAction SilentlyContinue
  $hits += @($forms.Keys | Where-Object { Test-RegressMentions $deps $_ })
  $deps = $null
}

if ($hits.Count -gt 0) {
  $named = @($hits | ForEach-Object { $forms[$_] } | Sort-Object -Unique)
  "error: ninja did no work, but files the build reads changed since {0}, the commit the tree last built ({1} of the {2} changed; first {3}). The objects are stamped later than these sources, which is what a guest clock that stepped back leaves behind; remove {4} and run again." -f $prev, $named.Count, $paths.Count, $named[0], $build
  exit 87
}
Save-Built
"built-commit: no file the build reads changed since {0} ({1} changed{2})" -f $prev, $paths.Count, $(if ($regenerated) { '; CMake re-ran for its own inputs' } else { '' })
exit 0
