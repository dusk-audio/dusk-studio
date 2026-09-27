# Phase 1: the headless audio self-test against the executable under test.
try {
    Stop-RegressApps
    $rgExe = Get-RegressExe
    $rgLog += "exe=$rgExe`n"
    $rgLog += "fileversion=$((Get-Item $rgExe).VersionInfo.FileVersion) size=$((Get-Item $rgExe).Length)`n"

    $rgEnv = New-RegressSandbox 'selftest'
    $rgEnv['DUSKSTUDIO_RUN_SELFTEST'] = '1'
    $rgCode = Invoke-RegressApp 'selftest' $rgEnv '' 300
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

$rgLog += "REGRESS-PHASE phase1-selftest RESULT $rgResult`nREGRESS-PHASE phase1-selftest END`n"
Invoke-RegressPost $rgLog

# iex runs the script in a child scope, so plain "exit" leaves the console
# open and repeat runs stack up windows in the guest.
[Environment]::Exit(0)
