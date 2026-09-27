# Proves the console/HTTP channel is live before a phase is typed into it.
try {
    $rgLog = "host=$env:COMPUTERNAME user=$env:USERNAME`n"
    $rgLog += "ps=$($PSVersionTable.PSVersion) os=$([System.Environment]::OSVersion.VersionString)`n"
    $rgLog += "localappdata=$env:LOCALAPPDATA`n"
    $rgResult = 'PASS'
} catch {
    $rgLog += "probe failed: $_`n"
}

$rgLog += "REGRESS-PHASE console-probe RESULT $rgResult`nREGRESS-PHASE console-probe END`n"
Invoke-RegressPost $rgLog

# iex runs the script in a child scope, so plain "exit" leaves the console
# open and repeat runs stack up windows in the guest.
[Environment]::Exit(0)
