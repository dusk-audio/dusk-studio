# Phase 2: GUI launch plus single-instance handoff. The first instance opens the
# shipped session; a second launch carrying a different session path must hand
# it to the running instance and exit at once, leaving the first instance alive,
# in front, and loading what it was handed.
$ErrorActionPreference = 'Continue'
$rgIp = '@@HOSTIP@@'
$rgRoot = "$env:LOCALAPPDATA\@@ROOT@@"
$rgLog = ''
$rgResult = 'FAIL'
$rgExe = ''

function Invoke-RegressPost($text) {
    Invoke-RestMethod -Uri "http://${rgIp}:9000/" -Method POST -Body $text | Out-Null
}

# The plugin host inherits the app's redirected handles, so a pipe can stay
# open after the app itself has exited; a bounded wait keeps that from hanging
# the phase. Stop-RegressChildren closes the usual holder first.
function Stop-RegressChildren {
    Get-Process dusk-studio-plugin-host -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
}
function Read-RegressTask($task, $ms) {
    if ($task.Wait($ms)) { return $task.Result }
    return "<read timed out after ${ms} ms; output withheld by an open inherited handle>"
}

# A scriptblock delegate (EnumWindows and friends) throws under iex, so the
# P/Invoke surface stays limited to this direct call.
if (-not ('Regress.Foreground' -as [type])) {
    Add-Type -Namespace Regress -Name Foreground -MemberDefinition @'
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
'@
}

function Start-RegressApp($appArgs, $envs) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $rgExe
    $psi.Arguments = $appArgs
    $psi.WorkingDirectory = (Split-Path $rgExe)
    $psi.UseShellExecute = $false
    $psi.RedirectStandardError = $true
    $psi.RedirectStandardOutput = $true
    if ($envs) { foreach ($k in $envs.Keys) { $psi.EnvironmentVariables[$k] = $envs[$k] } }
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

    $rgSession = "$rgRoot\regress-session\session.json"
    $rgHandoff = "$rgRoot\regress-session\handoff.json"
    foreach ($f in @($rgSession, $rgHandoff)) {
        if (-not (Test-Path $f)) { throw "payload session missing: $f" }
    }

    # The session arrives through the environment so no picker is up: a handoff
    # into the picker would be a different test.
    $rgFirst = Start-RegressApp '' @{ DUSKSTUDIO_LOAD_SESSION = $rgSession }
    Start-Sleep -Seconds 20
    $rgHwnd = $rgFirst.MainWindowHandle
    $rgLog += "A pid=$($rgFirst.Id) alive=$(-not $rgFirst.HasExited) hwnd=$rgHwnd`n"
    if ($rgFirst.HasExited) { throw "first instance exited during startup" }

    $rgSecond = Start-RegressApp ('"' + $rgHandoff + '"')
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
    # The handoff is supposed to bring the running window forward, which is the
    # half of the check a bare "B exited 0" cannot see.
    $rgForeground = [Regress.Foreground]::GetForegroundWindow()
    $rgLog += "A after handoff: alive=$(-not $rgFirst.HasExited) foreground=$rgForeground wanted=$rgHwnd`n"
    $rgAlive = -not $rgFirst.HasExited

    # Clean shutdown is phase 3's job; here the instance is only torn down, and
    # its stderr can only be read in full once its pipe has closed.
    try { $rgFirst.Kill() } catch { }
    $rgFirst.WaitForExit(20000) | Out-Null
    Stop-RegressChildren
    $rgFirstErr = Read-RegressTask $rgFirst.RgErr 10000
    Set-Content -Path "$rgRoot\phase2-A.log" `
        -Value ((Read-RegressTask $rgFirst.RgOut 10000) + "`n---stderr---`n" + $rgFirstErr)
    $rgLog += (($rgFirstErr -split "`n" |
        Select-String -Pattern 'SingleInstance|handoff|anotherInstance|Load\]|Assert|exception|fault' |
        Select-Object -Last 8 | ForEach-Object { '  ' + $_.Line.Trim() }) -join "`n") + "`n"

    # B exiting 0 only proves it gave up its slot; the handed-off path has to
    # show up as a load in A.
    $rgHandoffLoaded = ($rgFirstErr -match 'Dusk Studio/Load\] handoff\.json')
    $rgLog += "A loaded the handed-off session: $rgHandoffLoaded`n"

    if ($rgHandedOff -and $rgSecond.ExitCode -eq 0 -and $rgAlive `
            -and $rgHwnd -ne 0 -and $rgForeground -eq $rgHwnd -and $rgHandoffLoaded) {
        $rgResult = 'PASS'
    }
} catch {
    $rgLog += "phase2 failed: $_`n"
}

$rgLog += "REGRESS-PHASE phase2 RESULT $rgResult`nREGRESS-PHASE phase2 END`n"
Invoke-RegressPost $rgLog

# iex runs the script in a child scope, so plain "exit" leaves the console
# open and repeat runs stack up windows in the guest.
[Environment]::Exit(0)
