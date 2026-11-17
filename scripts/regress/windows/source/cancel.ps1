# Sent inline by windows-source.sh with $root and inline-lib.ps1 in front.
# Everything whose command line names the tree is this run's, since the
# preflight found the tree idle.
for ($attempt = 0; $attempt -lt 30; $attempt++) {
  $left = @(Get-TreeProcesses)
  if ($left.Count -eq 0) { "nothing left running in $root"; exit 0 }
  foreach ($process in $left) {
    & taskkill.exe /T /F /PID $process.ProcessId 2>&1 | Out-Null
  }
  Start-Sleep -Seconds 1
}
$left = @(Get-TreeProcesses)
"error: still running in ${root}:"
$left | ForEach-Object { "  {0} {1}" -f $_.ProcessId, $_.CommandLine }
exit 75
