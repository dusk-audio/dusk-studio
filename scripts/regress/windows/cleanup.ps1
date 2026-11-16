# Last step of every package run that reached the guest, pass or fail: no
# instance running, the run's evidence sent to the host, nothing of the run on
# disk, and the user environment free of the private folders phase 4 lends
# Explorer for one launch. A REGRESS-LEFT line names what stays in the guest;
# the host's trailer lists them.
$rgEvidenceCap = 64MB
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

    # The evidence no phase posted: the logs at the top of the run folder
    # (msiexec's, each launch's output), phase 4's under activation\ (each
    # case's primary instance), and, from every launch's private profile, the
    # app's own log and crash reports. All of it reaches the host, under a name
    # that says where it came from, before the folder goes.
    $rgEvidence = @()
    $rgKeep = $false
    if (Test-Path -LiteralPath $rgRoot) {
        $rgWalk = Get-RegressFutureFiles $rgRoot ([DateTime]::MinValue)
        foreach ($dir in $rgWalk.Unlisted) {
            $rgLeft += "could not list evidence in $dir"
            $rgKeep = $true
        }
        foreach ($f in $rgWalk.Files) {
            $plain = Get-RegressPlainPath $f.FullName
            $rel = $plain.Substring($rgRoot.TrimEnd('\').Length + 1)
            if (($rel -notmatch '\\' -and $f.Name -like '*.log') -or
                $rel -like 'activation\*.log' -or
                ($rel -like 'launch\*' -and (($f.Name -like '*.log' -and $plain -like '*\config\*') -or $f.Name -like 'crash-*.txt'))) {
                $rgEvidence += @{ File = $f; Name = "guest-$rel" -replace '[^A-Za-z0-9._-]', '_' }
            }
        }
    }
    $rgEvidenceBytes = [long]0
    foreach ($e in $rgEvidence) { $rgEvidenceBytes += $e.File.Length }
    if ($rgEvidenceBytes -gt $rgEvidenceCap) {
        $rgLeft += "the run's logs and crash reports come to {0:N1} MB, over the {1:N0} MB this step sends" -f ($rgEvidenceBytes / 1MB), ($rgEvidenceCap / 1MB)
        $rgKeep = $true
    } else {
        # The phases run with errors set to continue, so a read that is
        # refused has to be made to throw, or an empty body would be sent and
        # the log it never read removed with the folder.
        foreach ($e in $rgEvidence) {
            try {
                $rgBody = Get-Content -LiteralPath $e.File.FullName -Raw -ErrorAction Stop
                Invoke-RegressPost $rgBody $e.Name
            } catch {
                $rgLeft += "could not read or send $($e.File.FullName): $_"
                $rgKeep = $true
            }
        }
        $rgLog += "sent $($rgEvidence.Count) logs and crash reports to the host`n"
    }

    $rgBytes = (Get-ChildItem -LiteralPath $rgRoot -Recurse -Force -File -ErrorAction SilentlyContinue |
        Measure-Object -Property Length -Sum).Sum
    if (-not $rgPhase4Done) {
        $rgLeft += "phase 4 has not finished, so Notepad's backup is still in it"
        $rgKeep = $true
    } elseif (Test-Path -LiteralPath "$rgWaDir\notepad-localstate") {
        $rgLeft += "phase 4 did not put Notepad's saved session back; restore $rgWaDir\notepad-localstate to Notepad's LocalState by hand"
        $rgKeep = $true
    }
    if (Test-Path $rgRoot) {
        if ($rgKeep) {
            $rgLog += "REGRESS-LEFT the run folder $rgRoot, kept because this step failed with what it holds; restore any saved Notepad session and preserve needed evidence before scripts/regress.sh clean windows`n"
        } elseif (-not (Test-Path "$rgRoot\.dusk-regress-run")) {
            throw "$rgRoot has no run marker; not removing it"
        } elseif (Remove-RegressTree $rgRoot '.dusk-regress-run') {
            $rgLog += "removed $rgRoot`n"
            $rgLog += "REGRESS-NOTE run folder {0:N1} GB removed`n" -f ($rgBytes / 1GB)
        } else {
            $rgLeft += "could not remove all of $rgRoot"
            $rgLog += "REGRESS-LEFT what is left of the run folder $rgRoot; it keeps its marker, so scripts/regress.sh clean windows takes the rest`n"
        }
    }
    $rgDrive = [IO.Path]::GetPathRoot($env:LOCALAPPDATA)
    $rgLog += "REGRESS-NOTE {0} {1:N1} GB free`n" -f $rgDrive, ((New-Object IO.DriveInfo $rgDrive).AvailableFreeSpace / 1GB)
    foreach ($l in $rgLeft) { $rgLog += "error: $l`n" }
    if ($rgLeft.Count -eq 0) { $rgResult = 'PASS' }
} catch {
    $rgLog += "run-cleanup failed: $_`n"
    if (Test-Path $rgRoot) { $rgLog += "REGRESS-LEFT the run folder $rgRoot`n" }
}

$rgLog += "REGRESS-PHASE run-cleanup RESULT $rgResult`nREGRESS-PHASE run-cleanup END`n"
Invoke-RegressPost $rgLog

# iex runs the script in a child scope, so plain "exit" leaves the console
# open and repeat runs stack up windows in the guest.
[Environment]::Exit(0)
