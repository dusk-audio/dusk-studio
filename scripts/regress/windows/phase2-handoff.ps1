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

# The first instance's load has to be seen while it is still running, and
# ReadToEndAsync only returns once the pipe closes. One bounded ReadLineAsync at
# a time instead, no delegates: a scriptblock callback throws under iex. The
# first wait paces the caller's poll loop; the rest drain whatever else is
# already buffered.
function Read-RegressStderr($state, $ms) {
    while ($null -ne $state.Task -and $state.Task.Wait($ms)) {
        $line = $state.Task.Result
        if ($null -eq $line) { $state.Task = $null; break }
        [void]$state.Text.AppendLine($line)
        $state.Task = $state.Reader.ReadLineAsync()
        $ms = 100
    }
    return $state.Text.ToString()
}

# A scriptblock delegate (EnumWindows and friends) throws under iex, so the
# P/Invoke surface stays limited to this direct call.
# A type added under a name the guest console already holds from an earlier run
# keeps that run's members, so a changed surface needs a new name.
if (-not ('Regress.ForegroundOwner' -as [type])) {
    Add-Type -Namespace Regress -Name ForegroundOwner -MemberDefinition @'
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
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
    $p | Add-Member -NotePropertyName RgErr -NotePropertyValue @{
        Reader = $p.StandardError
        Text   = New-Object System.Text.StringBuilder
        Task   = $p.StandardError.ReadLineAsync()
    }
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
    # into the picker would be a different test. B is not launched until A has
    # logged that load, so the handoff lands on a window that is showing it.
    $rgFirst = Start-RegressApp '' @{ DUSKSTUDIO_LOAD_SESSION = $rgSession }
    $rgSessionLoaded = $false
    $rgHwnd = [IntPtr]::Zero
    $rgFirstErr = ''
    $rgUntil = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $rgUntil -and -not $rgFirst.HasExited) {
        $rgFirstErr = Read-RegressStderr $rgFirst.RgErr 500
        if (-not $rgSessionLoaded -and $rgFirstErr -match 'Dusk Studio/Load\] session\.json') {
            $rgSessionLoaded = $true
        }
        if ($rgHwnd -eq [IntPtr]::Zero) {
            $rgFirst.Refresh()
            $rgHwnd = $rgFirst.MainWindowHandle
        }
        if ($rgSessionLoaded -and $rgHwnd -ne [IntPtr]::Zero) { break }
    }
    $rgLog += "A pid=$($rgFirst.Id) alive=$(-not $rgFirst.HasExited) hwnd=$rgHwnd sessionLoaded=$rgSessionLoaded`n"
    if ($rgFirst.HasExited) { throw "first instance exited during startup" }
    # A short settle so the load's own window work is done before the handoff.
    Start-Sleep -Seconds 3

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
    # half of the check a bare "B exited 0" cannot see. Checked by owning
    # process: the handle A reported at startup is not the one it holds by now.
    $rgForeground = [Regress.ForegroundOwner]::GetForegroundWindow()
    $rgForegroundPid = 0
    [void][Regress.ForegroundOwner]::GetWindowThreadProcessId($rgForeground, [ref]$rgForegroundPid)
    $rgForegroundTitle = New-Object System.Text.StringBuilder 256
    [void][Regress.ForegroundOwner]::GetWindowText($rgForeground, $rgForegroundTitle, 256)
    $rgForegroundName = try { (Get-Process -Id $rgForegroundPid).ProcessName } catch { '?' }
    $rgLog += "A after handoff: alive=$(-not $rgFirst.HasExited) foreground=$rgForegroundName($rgForegroundPid) '$($rgForegroundTitle.ToString())' A=$($rgFirst.Id)`n"
    $rgAlive = -not $rgFirst.HasExited

    # Clean shutdown is phase 3's job; here the instance is only torn down.
    try { $rgFirst.Kill() } catch { }
    $rgFirst.WaitForExit(20000) | Out-Null
    Stop-RegressChildren
    $rgFirstErr = Read-RegressStderr $rgFirst.RgErr 2000
    $rgSecondErr = Read-RegressStderr $rgSecond.RgErr 2000
    Set-Content -Path "$rgRoot\phase2-A.log" `
        -Value ((Read-RegressTask $rgFirst.RgOut 10000) + "`n---stderr---`n" + $rgFirstErr)
    Set-Content -Path "$rgRoot\phase2-B.log" `
        -Value ((Read-RegressTask $rgSecond.RgOut 10000) + "`n---stderr---`n" + $rgSecondErr)
    $rgLog += (($rgFirstErr -split "`n" |
        Select-String -Pattern 'SingleInstance|handoff|anotherInstance|Load\]|Assert|exception|fault' |
        Select-Object -Last 8 | ForEach-Object { '  ' + $_.Line.Trim() }) -join "`n") + "`n"

    # B exiting 0 only proves it gave up its slot; the handed-off path has to
    # show up as a load in A, after the load A was started with.
    $rgSessionAt = $rgFirstErr.IndexOf('Dusk Studio/Load] session.json')
    $rgHandoffAt = $rgFirstErr.IndexOf('Dusk Studio/Load] handoff.json')
    $rgHandoffLoaded = ($rgSessionAt -ge 0 -and $rgHandoffAt -gt $rgSessionAt)
    $rgLog += "A load markers: session.json at $rgSessionAt, handoff.json at $rgHandoffAt, ordered=$rgHandoffLoaded`n"

    if ($rgSessionLoaded -and $rgHandedOff -and $rgSecond.ExitCode -eq 0 -and $rgAlive `
            -and $rgHwnd -ne [IntPtr]::Zero -and $rgForegroundPid -eq $rgFirst.Id -and $rgHandoffLoaded) {
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
