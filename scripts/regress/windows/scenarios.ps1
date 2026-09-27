# One run of the in-app scenario suite (DUSKSTUDIO_RUN_SCENARIOS=@@SPEC@@)
# against the executable under test, in a private profile. The whole report
# goes back to the host, which judges it the way the Linux legs do; this phase
# only says whether it got one.
$rgPhase = '@@PHASE@@'
try {
    Stop-RegressApps
    $rgExe = Get-RegressExe
    $rgEnv = New-RegressSandbox $rgPhase
    $rgEnv['DUSKSTUDIO_RUN_SCENARIOS'] = '@@SPEC@@'
    $rgStart = Get-Date
    $rgCode = Invoke-RegressApp $rgPhase $rgEnv '' @@TIMEOUT@@
    $rgSeconds = [int]((Get-Date) - $rgStart).TotalSeconds
    Invoke-RegressPost (Get-Content -Raw -Encoding UTF8 "$rgRoot\$rgPhase.log") "$rgPhase.log"
    $rgLog += "REGRESS-EXIT $rgCode`nREGRESS-SECONDS $rgSeconds`n"
    $rgResult = 'PASS'
} catch {
    $rgLog += "$rgPhase failed: $_`n"
}

$rgLog += "REGRESS-PHASE $rgPhase RESULT $rgResult`nREGRESS-PHASE $rgPhase END`n"
Invoke-RegressPost $rgLog

# iex runs the script in a child scope, so plain "exit" leaves the console
# open and repeat runs stack up windows in the guest.
[Environment]::Exit(0)
