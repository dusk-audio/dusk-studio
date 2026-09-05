# Phase 3: open a session from the startup picker, then close the window and
# require a clean exit. WM_CLOSE sent while the picker is still up is swallowed
# (requestQuit returns with a modal open), so the session has to be opened
# first. That needs at least one entry in the guest's Recent Sessions list.
$ErrorActionPreference = 'Continue'
$rgIp = '@@HOSTIP@@'
$rgRoot = "$env:LOCALAPPDATA\@@ROOT@@"
$rgLog = ''
$rgResult = 'FAIL'

function Invoke-RegressPost($text) {
    Invoke-RestMethod -Uri "http://${rgIp}:9000/" -Method POST -Body $text | Out-Null
}

# A scriptblock delegate (EnumWindows and friends) throws under iex, so the
# P/Invoke surface stays limited to these direct calls.
if (-not ('Regress.User32' -as [type])) {
    Add-Type -Namespace Regress -Name User32 -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int hh, bool r);
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
'@
}

function Invoke-RegressClick($x, $y) {
    [Regress.User32]::SetCursorPos($x, $y) | Out-Null
    Start-Sleep -Milliseconds 150
    [Regress.User32]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 80
    [Regress.User32]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
}

try {
    Get-Process DuskStudio -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    $rgExe = (Get-ChildItem $rgRoot -Recurse -Filter DuskStudio.exe |
        Select-Object -First 1).FullName
    if (-not $rgExe) { throw "DuskStudio.exe not found under $rgRoot (run phase 1 first)" }

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $rgExe
    $psi.WorkingDirectory = (Split-Path $rgExe)
    $psi.UseShellExecute = $false
    $psi.RedirectStandardError = $true
    $psi.RedirectStandardOutput = $true
    $rgApp = [System.Diagnostics.Process]::Start($psi)
    $rgOut = $rgApp.StandardOutput.ReadToEndAsync()
    $rgErr = $rgApp.StandardError.ReadToEndAsync()
    Start-Sleep -Seconds 20

    $rgHwnd = $rgApp.MainWindowHandle
    $rgLog += "pid=$($rgApp.Id) hwnd=$rgHwnd alive=$(-not $rgApp.HasExited)`n"
    if ($rgHwnd -eq 0) { throw "no main window after 20 s" }

    # Pinned geometry so the picker's Open button is at a fixed coordinate.
    [Regress.User32]::SetForegroundWindow($rgHwnd) | Out-Null
    [Regress.User32]::MoveWindow($rgHwnd, 0, 0, 1068, 660, $true) | Out-Null
    Start-Sleep -Seconds 2
    Invoke-RegressClick 755 496
    Start-Sleep -Seconds 14

    [Regress.User32]::PostMessage($rgHwnd, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $rgStart = Get-Date
    $rgClosed = $rgApp.WaitForExit(50000)
    $rgElapsed = [int]((Get-Date) - $rgStart).TotalSeconds
    if ($rgClosed) {
        $rgLog += "exit=$($rgApp.ExitCode) after ${rgElapsed}s`n"
    } else {
        $rgLog += "still running 50 s after WM_CLOSE`n"
        try { $rgApp.Kill() } catch { }
    }

    $rgStderr = $rgErr.Result
    Set-Content -Path "$rgRoot\phase3.log" -Value ($rgOut.Result + "`n---stderr---`n" + $rgStderr)
    $rgMarkers = $rgStderr -split "`n" | Select-String -Pattern '\[Dusk Studio/(shutdown|Load)\]'
    $rgLog += (($rgMarkers | ForEach-Object { '  ' + $_.Line.Trim() }) -join "`n") + "`n"

    $rgLoaded = ($rgStderr -match 'Dusk Studio/Load\] session\.json')
    if (-not $rgLoaded) {
        $rgLog += "no session load marker: the guest's Recent Sessions list may be empty`n"
    }
    $rgShutdownPhases = ($rgMarkers | Where-Object { $_.Line -match 'shutdown\] phase' }).Count
    $rgLog += "loaded=$rgLoaded shutdownPhases=$rgShutdownPhases`n"
    if ($rgClosed -and $rgApp.ExitCode -eq 0 -and $rgLoaded -and $rgShutdownPhases -ge 8) {
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
