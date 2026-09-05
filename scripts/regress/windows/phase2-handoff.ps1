# Phase 2: GUI launch plus single-instance handoff. A second launch carrying a
# session path must hand the path to the running instance and exit at once,
# leaving the first instance alive.
$ErrorActionPreference = 'Continue'
$rgIp = '@@HOSTIP@@'
$rgRoot = "$env:LOCALAPPDATA\@@ROOT@@"
$rgLog = ''
$rgResult = 'FAIL'
$rgExe = ''

function Invoke-RegressPost($text) {
    Invoke-RestMethod -Uri "http://${rgIp}:9000/" -Method POST -Body $text | Out-Null
}

function Start-RegressApp($appArgs) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $rgExe
    $psi.Arguments = $appArgs
    $psi.WorkingDirectory = (Split-Path $rgExe)
    $psi.UseShellExecute = $false
    $psi.RedirectStandardError = $true
    $psi.RedirectStandardOutput = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $p | Add-Member -NotePropertyName RgOut -NotePropertyValue $p.StandardOutput.ReadToEndAsync()
    $p | Add-Member -NotePropertyName RgErr -NotePropertyValue $p.StandardError.ReadToEndAsync()
    return $p
}

try {
    Get-Process DuskStudio -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    $rgExe = (Get-ChildItem $rgRoot -Recurse -Filter DuskStudio.exe |
        Select-Object -First 1).FullName
    if (-not $rgExe) { throw "DuskStudio.exe not found under $rgRoot (run phase 1 first)" }

    $rgFirst = Start-RegressApp ''
    Start-Sleep -Seconds 20
    $rgHwnd = $rgFirst.MainWindowHandle
    $rgLog += "A pid=$($rgFirst.Id) alive=$(-not $rgFirst.HasExited) hwnd=$rgHwnd`n"
    if ($rgFirst.HasExited) { throw "first instance exited during startup" }

    $rgSecond = Start-RegressApp ('"' + "$rgRoot\handoff-probe\session.json" + '"')
    $rgStart = Get-Date
    $rgHandedOff = $rgSecond.WaitForExit(30000)
    $rgElapsed = [int]((Get-Date) - $rgStart).TotalSeconds
    if (-not $rgHandedOff) {
        try { $rgSecond.Kill() } catch { }
        $rgLog += "B: did not exit within 30 s`n"
    } else {
        $rgLog += "B: exit=$($rgSecond.ExitCode) after ${rgElapsed}s`n"
    }
    Start-Sleep -Seconds 3
    $rgLog += "A after handoff: alive=$(-not $rgFirst.HasExited)`n"

    if ($rgHandedOff -and $rgSecond.ExitCode -eq 0 -and -not $rgFirst.HasExited -and $rgHwnd -ne 0) {
        $rgResult = 'PASS'
    }

    # The startup session picker swallows WM_CLOSE, so clean shutdown is
    # phase 3's job; here the instance is only torn down.
    try { $rgFirst.Kill() } catch { }
    $rgFirst.WaitForExit(20000) | Out-Null
    Set-Content -Path "$rgRoot\phase2-A.log" `
        -Value ($rgFirst.RgOut.Result + "`n---stderr---`n" + $rgFirst.RgErr.Result)
    $rgLog += (($rgFirst.RgErr.Result -split "`n" |
        Select-String -Pattern 'SingleInstance|handoff|anotherInstance|Assert|exception|fault' |
        Select-Object -Last 8 | ForEach-Object { '  ' + $_.Line.Trim() }) -join "`n") + "`n"
} catch {
    $rgLog += "phase2 failed: $_`n"
}

$rgLog += "REGRESS-PHASE phase2 RESULT $rgResult`nREGRESS-PHASE phase2 END`n"
Invoke-RegressPost $rgLog

# iex runs the script in a child scope, so plain "exit" leaves the console
# open and repeat runs stack up windows in the guest.
[Environment]::Exit(0)
