# Phase 1: unpack the build under test and run the headless audio self-test.
$ErrorActionPreference = 'Continue'
# Invoke-WebRequest renders a progress bar per chunk in PowerShell 5.1, which
# costs minutes on a payload this size.
$ProgressPreference = 'SilentlyContinue'
$rgIp = '@@HOSTIP@@'
$rgRoot = "$env:LOCALAPPDATA\@@ROOT@@"
$rgLog = ''
$rgResult = 'FAIL'
$rgExe = ''

function Invoke-RegressPost($text) {
    Invoke-RestMethod -Uri "http://${rgIp}:9000/" -Method POST -Body $text | Out-Null
}

# Redirected stdio, not Start-Process: the self-test reports through stdout and
# stderr and there is no other way to read them back out of the guest.
function Invoke-RegressApp($name, $envs, $appArgs, $timeoutSec) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $rgExe
    $psi.Arguments = $appArgs
    $psi.WorkingDirectory = (Split-Path $rgExe)
    $psi.UseShellExecute = $false
    $psi.RedirectStandardError = $true
    $psi.RedirectStandardOutput = $true
    foreach ($k in $envs.Keys) { $psi.EnvironmentVariables[$k] = $envs[$k] }
    $p = [System.Diagnostics.Process]::Start($psi)
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $errTask = $p.StandardError.ReadToEndAsync()
    $finished = $p.WaitForExit($timeoutSec * 1000)
    if (-not $finished) {
        try { $p.Kill() } catch { }
        $code = 'TIMEOUT'
    } else {
        $code = $p.ExitCode
    }
    Set-Content -Path "$rgRoot\$name.log" `
        -Value ($outTask.Result + "`n---stderr---`n" + $errTask.Result)
    return $code
}

try {
    Get-Process DuskStudio -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    if (Test-Path $rgRoot) { Remove-Item -Recurse -Force $rgRoot }
    New-Item -ItemType Directory -Path $rgRoot | Out-Null
    Invoke-WebRequest -UseBasicParsing "http://${rgIp}:8000/@@ZIP@@" `
        -OutFile "$rgRoot\@@ZIP@@"
    Expand-Archive -Path "$rgRoot\@@ZIP@@" -DestinationPath $rgRoot -Force

    $rgExe = (Get-ChildItem $rgRoot -Recurse -Filter DuskStudio.exe |
        Select-Object -First 1).FullName
    $rgHostExe = (Get-ChildItem $rgRoot -Recurse -Filter dusk-studio-plugin-host.exe |
        Select-Object -First 1).FullName
    if (-not $rgExe) { throw "DuskStudio.exe not found under $rgRoot" }
    $rgLog += "exe=$rgExe`n"
    $rgLog += "plugin-host=$rgHostExe`n"
    $rgLog += "fileversion=$((Get-Item $rgExe).VersionInfo.FileVersion) size=$((Get-Item $rgExe).Length)`n"

    $rgCode = Invoke-RegressApp 'selftest' @{ DUSKSTUDIO_RUN_SELFTEST = '1' } '' 300
    $rgLines = Get-Content "$rgRoot\selftest.log" -ErrorAction SilentlyContinue |
        Select-String -Pattern '^\[(PASS|FAIL|SKIP)\]'
    $rgLog += "selftest exit=$rgCode`n"
    $rgLog += (($rgLines | ForEach-Object { '  ' + $_.Line }) -join "`n") + "`n"

    $rgPassCount = ($rgLines | Where-Object { $_.Line -like '`[PASS`]*' }).Count
    $rgFailCount = ($rgLines | Where-Object { $_.Line -like '`[FAIL`]*' }).Count
    $rgLog += "selftest pass=$rgPassCount fail=$rgFailCount`n"
    if ($rgCode -eq 0 -and $rgFailCount -eq 0 -and $rgPassCount -gt 0) {
        $rgResult = 'PASS'
    }
} catch {
    $rgLog += "phase1 failed: $_`n"
}

# DUSKSTUDIO_RUN_IPC_SELFTEST never returns on Windows (issue #504), so the
# out-of-process transport stays compile-and-contract verified there.
$rgLog += "ipc-selftest SKIPPED (#504: hangs on Windows)`n"
$rgLog += "REGRESS-PHASE phase1 RESULT $rgResult`nREGRESS-PHASE phase1 END`n"
Invoke-RegressPost $rgLog
exit
