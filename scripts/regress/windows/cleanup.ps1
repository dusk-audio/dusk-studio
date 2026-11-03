# Last step of an --installed run, which leaves the guest as it found it: no
# instance running, nothing of the run on disk, and the user environment free
# of the private folders phase 4 lends Explorer for one launch.
try {
    # A phase 4 the host stopped waiting for may still be running, with Notepad's
    # saved session backed up in the run folder until it has put it back.
    $rgWaDir = "$rgRoot\activation"
    $rgUntil = (Get-Date).AddSeconds(90)
    while ((Test-Path $rgWaDir) -and -not (Test-Path "$rgWaDir\phase4.done") -and (Get-Date) -lt $rgUntil) {
        Start-Sleep -Seconds 2
    }
    $rgPhase4Done = (-not (Test-Path $rgWaDir)) -or (Test-Path "$rgWaDir\phase4.done")
    Stop-RegressApps
    Start-Sleep -Seconds 2
    $rgLeft = @()
    $rgLeft += @(Get-Process DuskStudio, dusk-studio-plugin-host -ErrorAction SilentlyContinue |
        ForEach-Object { "$($_.ProcessName) pid $($_.Id) still running" })
    $rgLeft += @(Get-RegressUserEnvLeftovers)
    # Still a failure, but the guest is not left pointing Explorer at a folder
    # this step removes. Only the names phase 4 lends, and only under its marker;
    # SetEnvironmentVariable also tells Explorer the environment changed.
    if ($null -ne (Get-Item 'HKCU:\Environment').GetValue('DUSKSTUDIO_REGRESS_ENV_OWNER')) {
        foreach ($n in @('DUSKSTUDIO_CONFIG_DIR', 'DUSKSTUDIO_MUSIC_DIR', 'DUSKSTUDIO_REGRESS_ENV_OWNER')) {
            [Environment]::SetEnvironmentVariable($n, $null, 'User')
        }
        $rgLog += "cleared the user environment phase 4 left behind`n"
    }
    if (-not $rgPhase4Done) {
        $rgLeft += "phase 4 has not finished; $rgRoot is left in place, Notepad's backup included"
    } elseif (Test-Path $rgRoot) {
        if (-not (Test-Path "$rgRoot\.dusk-regress-run")) { throw "$rgRoot has no run marker; not removing it" }
        Remove-Item -Recurse -Force $rgRoot -ErrorAction SilentlyContinue
        if (Test-Path $rgRoot) { $rgLeft += "could not remove $rgRoot" } else { $rgLog += "removed $rgRoot`n" }
    }
    foreach ($l in $rgLeft) { $rgLog += "error: $l`n" }
    if ($rgLeft.Count -eq 0) { $rgResult = 'PASS' }
} catch {
    $rgLog += "run-cleanup failed: $_`n"
}

$rgLog += "REGRESS-PHASE run-cleanup RESULT $rgResult`nREGRESS-PHASE run-cleanup END`n"
Invoke-RegressPost $rgLog

# iex runs the script in a child scope, so plain "exit" leaves the console
# open and repeat runs stack up windows in the guest.
[Environment]::Exit(0)
