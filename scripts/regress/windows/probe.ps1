# Proves the console/HTTP channel is live before a phase is typed into it.
# Every variable is assigned before use: iex runs in the console session scope,
# so leftovers from an earlier phase would otherwise be read as this run's.
$ErrorActionPreference = 'Continue'
$rgIp = '@@HOSTIP@@'
$rgLog = ''
$rgResult = 'FAIL'

try {
    $rgLog = "host=$env:COMPUTERNAME user=$env:USERNAME`n"
    $rgLog += "ps=$($PSVersionTable.PSVersion) os=$([System.Environment]::OSVersion.VersionString)`n"
    $rgLog += "localappdata=$env:LOCALAPPDATA`n"
    $rgResult = 'PASS'
} catch {
    $rgLog += "probe failed: $_`n"
}

$rgLog += "REGRESS-PHASE probe RESULT $rgResult`nREGRESS-PHASE probe END`n"
Invoke-RestMethod -Uri "http://${rgIp}:9000/" -Method POST -Body $rgLog | Out-Null

# iex runs the script in a child scope, so plain "exit" leaves the console
# open and repeat runs stack up windows in the guest.
[Environment]::Exit(0)
