# Sent inline by windows-source.sh (-EncodedCommand), which puts the $root
# assignment in front, so it runs before the scripts folder is replaced.
$ErrorActionPreference = 'Stop'
$os = Get-CimInstance Win32_OperatingSystem
"{0} {1}, {2} MB free memory" -f $os.Caption, $os.Version, [int]($os.FreePhysicalMemory / 1024)
if (-not (Test-Path -LiteralPath $root -PathType Container)) {
  New-Item -ItemType Directory -Path $root | Out-Null
}
# Anything already running out of the tree is another run, and two in one
# tree overwrite each other's checkout and build.
$busy = @(Get-CimInstance Win32_Process | Where-Object {
  $_.ProcessId -ne $PID -and $_.CommandLine -and
  $_.CommandLine.IndexOf($root, [StringComparison]::OrdinalIgnoreCase) -ge 0
})
if ($busy.Count -gt 0) {
  "error: {0} process(es) are already running in {1}:" -f $busy.Count, $root
  $busy | ForEach-Object { "  {0} {1}" -f $_.ProcessId, $_.CommandLine }
  exit 1
}
$scripts = Join-Path $root 'regress'
if (Test-Path -LiteralPath $scripts) { Remove-Item -LiteralPath $scripts -Recurse -Force }
New-Item -ItemType Directory -Path $scripts | Out-Null
"tree $root free"
exit 0
