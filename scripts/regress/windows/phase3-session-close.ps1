# Phase 3: load a session, close the window twice, require one clean exit.
# WM_CLOSE on a fresh launch is swallowed while the startup picker is open
# (requestQuit returns with a modal up), so the session arrives through
# DUSKSTUDIO_LOAD_SESSION, which skips the picker. That leaves the phase with
# no screen coordinates and no dependency on the guest's Recent Sessions list.
# The second WM_CLOSE is queued behind the first. The first runs the shutdown
# sequence and then defers the quit itself across further message-loop ticks, so
# the second is dispatched while the latch is set and prints the re-entry line.

# The close goes to the window titled Dusk Studio. MainWindowHandle is the first
# visible unowned window .NET enumerates, and that can be one of the four
# drop-shadow windows around the main one, which drop WM_CLOSE: the app never
# hears the close and the phase reads it as a quit that did not happen.
# FindWindowEx walks the titled windows with no callback, which under iex would
# be a scriptblock delegate and throw. Its class argument is [NullString]::Value:
# PowerShell hands $null to a string parameter as "", which matches no class.
if (-not ('Regress.MainWindow' -as [type])) {
    Add-Type -Namespace Regress -Name MainWindow -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
[DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
'@
}

function Find-RegressMainWindow($procId) {
    $h = [IntPtr]::Zero
    while ($true) {
        $h = [Regress.MainWindow]::FindWindowEx([IntPtr]::Zero, $h, [NullString]::Value, 'Dusk Studio')
        if ($h -eq [IntPtr]::Zero) { return $h }
        $owner = 0
        [void][Regress.MainWindow]::GetWindowThreadProcessId($h, [ref]$owner)
        if ($owner -eq $procId -and [Regress.MainWindow]::IsWindowVisible($h)) { return $h }
    }
}

$rgApp = $null
try {
    Stop-RegressApps
    $rgExe = Get-RegressExe
    $rgSession = "$rgRoot\regress-session\session.json"
    if (-not (Test-Path $rgSession)) { throw "payload session missing: $rgSession" }

    $rgEnv = New-RegressSandbox 'session-close'
    $rgEnv['DUSKSTUDIO_LOAD_SESSION'] = $rgSession
    $rgApp = Start-RegressApp $rgEnv ''
    $rgStderr = ''

    # Closing before the session is in would be a different test, so the phase
    # waits for the load line rather than sleeping a guessed interval.
    $rgLoadSeen = $false
    $rgUntil = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $rgUntil) {
        $rgStderr = Read-RegressStderr $rgApp.RgErr 500
        if ($rgStderr -match 'Dusk Studio/Load\]') { $rgLoadSeen = $true; break }
        if ($rgApp.HasExited) { break }
    }
    $rgLog += "loadMarkerSeen=$rgLoadSeen`n"

    $rgHwnd = [IntPtr]::Zero
    $rgUntil = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $rgUntil -and -not $rgApp.HasExited) {
        $rgHwnd = Find-RegressMainWindow $rgApp.Id
        if ($rgHwnd -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 500
    }
    $rgApp.Refresh()
    $rgLog += "pid=$($rgApp.Id) hwnd=$rgHwnd mainWindowHandle=$($rgApp.MainWindowHandle) alive=$(-not $rgApp.HasExited)`n"
    if ($rgHwnd -eq [IntPtr]::Zero) { throw "no visible window titled Dusk Studio" }

    # Both posts before any sleep: the second one has to already be in the queue
    # when the first is dispatched.
    [Regress.MainWindow]::PostMessage($rgHwnd, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    [Regress.MainWindow]::PostMessage($rgHwnd, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $rgStart = Get-Date
    $rgUntil = $rgStart.AddSeconds(50)
    while (-not $rgApp.HasExited -and (Get-Date) -lt $rgUntil) {
        $rgStderr = Read-RegressStderr $rgApp.RgErr 500
    }
    $rgClosed = $rgApp.HasExited
    $rgElapsed = [int]((Get-Date) - $rgStart).TotalSeconds
    if ($rgClosed) {
        $rgApp.WaitForExit()
        $rgLog += "exit=$($rgApp.ExitCode) after ${rgElapsed}s`n"
    } else {
        $rgLog += "still running 50 s after WM_CLOSE`n"
    }
    $rgStderr = Read-RegressStderr $rgApp.RgErr 2000

    Set-Content -Path "$rgRoot\phase3.log" -Value ((Read-RegressTask $rgApp.RgOut 10000) + "`n---stderr---`n" + $rgStderr)
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
} finally {
    if ($null -ne $rgApp -and -not $rgApp.HasExited) {
        try { $rgApp.Kill() } catch { }
        $rgApp.WaitForExit(10000) | Out-Null
    }
    Stop-RegressChildren
}

$rgLog += "REGRESS-PHASE phase3-session-close RESULT $rgResult`nREGRESS-PHASE phase3-session-close END`n"
Invoke-RegressPost $rgLog

# iex runs the script in a child scope, so plain "exit" leaves the console
# open and repeat runs stack up windows in the guest.
[Environment]::Exit(0)
