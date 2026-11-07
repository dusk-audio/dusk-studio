# The out-of-process transport round-trip against the installed plug-in host.
try {
    Stop-RegressApps
    $rgExe = Get-RegressExe
    $rgLog += "exe=$rgExe`n"

    $rgEnv = New-RegressSandbox 'ipc-selftest'
    $rgEnv['DUSKSTUDIO_RUN_IPC_SELFTEST'] = '1'
    $rgCode = Invoke-RegressApp 'ipc-selftest' $rgEnv '' 300
    $rgOut = Get-Content -Raw "$rgRoot\ipc-selftest.log" -ErrorAction SilentlyContinue
    if ($null -eq $rgOut) { $rgOut = '' }
    $rgLog += "ipc-selftest exit=$rgCode`n"
    $rgLog += (($rgOut -split "`r?`n" | Where-Object { $_ -match '^(host:|round-trips:|latency ns:|PASS$|FAIL)' } |
        ForEach-Object { '  ' + $_ }) -join "`n") + "`n"
    if ($rgCode -eq 0 -and $rgOut -match '(?m)^PASS\r?$' -and $rgOut -notmatch '(?m)^FAIL') {
        $rgResult = 'PASS'
        foreach ($rgLine in ($rgOut -split "`r?`n" | Where-Object { $_ -match '^(round-trips|latency ns):' })) {
            $rgLog += "REGRESS-NOTE $rgLine`n"
        }
    }
} catch {
    $rgLog += "ipc-selftest failed: $_`n"
}

$rgLog += "REGRESS-PHASE ipc-selftest RESULT $rgResult`nREGRESS-PHASE ipc-selftest END`n"
Invoke-RegressPost $rgLog

# iex runs the script in a child scope, so plain "exit" leaves the console
# open and repeat runs stack up windows in the guest.
[Environment]::Exit(0)
