# Phase 3: load a session, close the window twice, require one clean exit.
# WM_CLOSE on a fresh launch is swallowed while the startup picker is open
# (requestQuit returns with a modal up), so the session arrives through
# DUSKSTUDIO_LOAD_SESSION, which skips the picker. That leaves the phase with
# no screen coordinates and no dependency on the guest's Recent Sessions list.
# The second WM_CLOSE is queued behind the first. The first runs the shutdown
# sequence and then defers the quit itself across further message-loop ticks, so
# the second is dispatched while the latch is set and prints the re-entry line.
$ErrorActionPreference = 'Continue'
$rgIp = '@@HOSTIP@@'
$rgRoot = "$env:LOCALAPPDATA\@@ROOT@@"
$rgLog = ''
$rgResult = 'FAIL'

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

# The load and shutdown markers have to be read while the app is still running,
# and ReadToEndAsync only returns once the pipe closes. One bounded
# ReadLineAsync at a time instead, no delegates: a scriptblock callback throws
# under iex. The first wait paces the caller's poll loop; the rest drain
# whatever else is already buffered.
function Read-RegressStderr($state, $ms) {
    while ($null -ne $state.Task -and $state.Task.Wait($ms)) {
        $line = $state.Task.Result
        if ($null -eq $line) { $state.Task = $null; break }
        [void]$state.Text.AppendLine($line)
        $state.Task = $state.Reader.ReadLineAsync()
        $ms = 0
    }
    return $state.Text.ToString()
}

# A scriptblock delegate (EnumWindows and friends) throws under iex, so the
# P/Invoke surface stays limited to this direct call.
if (-not ('Regress.User32' -as [type])) {
    Add-Type -Namespace Regress -Name User32 -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
'@
}

try {
    Get-Process DuskStudio -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    $rgExe = (Get-ChildItem $rgRoot -Recurse -Filter DuskStudio.exe |
        Select-Object -First 1).FullName
    if (-not $rgExe) { throw "DuskStudio.exe not found under $rgRoot (run phase 1 first)" }
    $rgSession = "$rgRoot\regress-session\session.json"
    if (-not (Test-Path $rgSession)) { throw "payload session missing: $rgSession" }

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $rgExe
    $psi.WorkingDirectory = (Split-Path $rgExe)
    $psi.UseShellExecute = $false
    $psi.RedirectStandardError = $true
    $psi.RedirectStandardOutput = $true
    $psi.EnvironmentVariables['DUSKSTUDIO_LOAD_SESSION'] = $rgSession
    $rgApp = [System.Diagnostics.Process]::Start($psi)
    $rgOut = $rgApp.StandardOutput.ReadToEndAsync()
    $rgErrState = @{
        Reader = $rgApp.StandardError
        Text   = New-Object System.Text.StringBuilder
        Task   = $rgApp.StandardError.ReadLineAsync()
    }
    $rgStderr = ''

    $rgHwnd = [IntPtr]::Zero
    $rgUntil = (Get-Date).AddSeconds(40)
    while ((Get-Date) -lt $rgUntil -and -not $rgApp.HasExited) {
        $rgApp.Refresh()
        $rgHwnd = $rgApp.MainWindowHandle
        if ($rgHwnd -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 500
    }
    $rgLog += "pid=$($rgApp.Id) hwnd=$rgHwnd alive=$(-not $rgApp.HasExited)`n"
    if ($rgHwnd -eq [IntPtr]::Zero) { throw "no main window after 40 s" }

    # Closing before the session is in would be a different test, so the phase
    # waits for the load line rather than sleeping a guessed interval.
    $rgLoadSeen = $false
    $rgUntil = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $rgUntil) {
        $rgStderr = Read-RegressStderr $rgErrState 500
        if ($rgStderr -match 'Dusk Studio/Load\]') { $rgLoadSeen = $true; break }
        if ($rgApp.HasExited) { break }
    }
    $rgLog += "loadMarkerSeen=$rgLoadSeen`n"

    # Both posts before any sleep: the second one has to already be in the queue
    # when the first is dispatched.
    [Regress.User32]::PostMessage($rgHwnd, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    [Regress.User32]::PostMessage($rgHwnd, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $rgStart = Get-Date
    $rgUntil = $rgStart.AddSeconds(50)
    while (-not $rgApp.HasExited -and (Get-Date) -lt $rgUntil) {
        $rgStderr = Read-RegressStderr $rgErrState 500
    }
    $rgClosed = $rgApp.HasExited
    $rgElapsed = [int]((Get-Date) - $rgStart).TotalSeconds
    if ($rgClosed) {
        $rgLog += "exit=$($rgApp.ExitCode) after ${rgElapsed}s`n"
    } else {
        $rgLog += "still running 50 s after WM_CLOSE`n"
        try { $rgApp.Kill() } catch { }
        $rgApp.WaitForExit(10000) | Out-Null
    }
    $rgStderr = Read-RegressStderr $rgErrState 2000

    Stop-RegressChildren
    Set-Content -Path "$rgRoot\phase3.log" -Value ((Read-RegressTask $rgOut 10000) + "`n---stderr---`n" + $rgStderr)
    $rgMarkers = $rgStderr -split "`n" | Select-String -Pattern '\[Dusk Studio/(shutdown|Load)\]'
    $rgLog += (($rgMarkers | ForEach-Object { '  ' + $_.Line.Trim() }) -join "`n") + "`n"

    $rgLoaded = ($rgStderr -match 'Dusk Studio/Load\] session\.json')
    $rgShutdownPhases = ($rgMarkers | Where-Object { $_.Line -match 'shutdown\] phase' }).Count
    $rgReentry = ($rgStderr -match 're-entry ignored: shutdown already in progress')
    $rgLog += "loaded=$rgLoaded shutdownPhases=$rgShutdownPhases reentry=$rgReentry`n"
    if ($rgClosed -and $rgApp.ExitCode -eq 0 -and $rgLoaded -and $rgShutdownPhases -ge 8 -and $rgReentry) {
        $rgResult = 'PASS'
    }
} catch {
    $rgLog += "phase3 failed: $_`n"
}

$rgLog += "REGRESS-PHASE phase3 RESULT $rgResult`nREGRESS-PHASE phase3 END`n"
Invoke-RegressPost $rgLog

# iex runs the script in a child scope, so plain "exit" leaves the console
# open and repeat runs stack up windows in the guest.
[Environment]::Exit(0)
