# Sent inline by windows-source.sh (-EncodedCommand) with $root in front.
# Closing the ssh session ends only the shell it started; the build or test
# processes under it run on. Everything whose command line names the tree is
# this run's, since the preflight found the tree idle.
function Get-TreeProcesses {
  @(Get-CimInstance Win32_Process | Where-Object {
    $_.ProcessId -ne $PID -and $_.CommandLine -and
    $_.CommandLine.IndexOf($root, [StringComparison]::OrdinalIgnoreCase) -ge 0
  })
}
for ($attempt = 0; $attempt -lt 30; $attempt++) {
  $left = Get-TreeProcesses
  if ($left.Count -eq 0) { "nothing left running in $root"; exit 0 }
  foreach ($process in $left) {
    & taskkill.exe /T /F /PID $process.ProcessId 2>&1 | Out-Null
  }
  Start-Sleep -Seconds 1
}
$left = Get-TreeProcesses
"error: still running in ${root}:"
$left | ForEach-Object { "  {0} {1}" -f $_.ProcessId, $_.CommandLine }
exit 75
