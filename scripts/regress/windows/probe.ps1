# Proves the console/HTTP channel is live before a phase is typed into it, and
# that the guest has the free space a package run needs before one starts.
$rgFreeGb = @@FREEGB@@
try {
    $rgLog = "host=$env:COMPUTERNAME user=$env:USERNAME`n"
    $rgLog += "ps=$($PSVersionTable.PSVersion) os=$([System.Environment]::OSVersion.VersionString)`n"
    $rgLog += "localappdata=$env:LOCALAPPDATA`n"
    $rgDrive = [IO.Path]::GetPathRoot($env:LOCALAPPDATA)
    $rgFree = (New-Object IO.DriveInfo $rgDrive).AvailableFreeSpace
    $rgFreeText = '{0} has {1:N1} GB free; a package run needs {2} GB' -f $rgDrive, ($rgFree / 1GB), $rgFreeGb
    if ($rgFree -lt [long]$rgFreeGb * 1GB) {
        $rgLog += "error: $rgFreeText`n"
    } else {
        $rgLog += "REGRESS-NOTE $rgFreeText`n"
        $rgResult = 'PASS'
    }
} catch {
    $rgLog += "probe failed: $_`n"
}

$rgLog += "REGRESS-PHASE console-probe RESULT $rgResult`nREGRESS-PHASE console-probe END`n"
Invoke-RegressPost $rgLog

# iex runs the script in a child scope, so plain "exit" leaves the console
# open and repeat runs stack up windows in the guest.
[Environment]::Exit(0)
