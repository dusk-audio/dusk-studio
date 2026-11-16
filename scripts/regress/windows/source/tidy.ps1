# Sent inline by windows-source.sh at the end of every run that got past its
# preflight, pass or fail: what the run brought or left in the tree goes, the
# warm tree stays, and its footprint is reported.
$ErrorActionPreference = 'Continue'
$status = 0
$busy = @(Get-TreeProcesses)
if ($busy.Count -gt 0) {
  "error: {0} process(es) still running in {1}; nothing removed" -f $busy.Count, $root
  $busy | ForEach-Object { "  {0} {1}" -f $_.ProcessId, $_.CommandLine }
  exit 1
}
foreach ($leftover in @((Join-Path $root 'regress\head.bundle'), (Join-Path $root 'tmp'))) {
  if (-not (Test-Path -LiteralPath $leftover)) { continue }
  if (Test-RegressReparse $leftover) {
    "error: $leftover is a link; not removing through it"
    $status = 1
    continue
  }
  $bytes = Get-RegressBytes $leftover
  if (Remove-RegressTree $leftover) {
    "removed {0} ({1})" -f $leftover, (Format-RegressGb $bytes)
  } else {
    "error: could not remove $leftover"
    $status = 1
  }
}
Write-RegressFootprint
exit $status
