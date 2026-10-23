# One private launch of the selected suite, with GUI identities and skips checked
# against the same executable's registry before the phase can pass.
$rgPhase = '@@PHASE@@'
$rgSpec = '@@SPEC@@'
$rgPostedLogs = @()
$rgStart = Get-Date
try {
    Stop-RegressApps
    $rgExe = Get-RegressExe
    $rgCatalog = @()
    if ($rgSpec -ceq 'gui') {
        $rgListPhase = $rgPhase + '-catalog'
        $rgPostedLogs += $rgListPhase
        $rgListEnv = New-RegressSandbox $rgListPhase
        $rgListEnv['DUSKSTUDIO_RUN_SCENARIOS'] = 'list'
        $rgListCode = Invoke-RegressApp $rgListPhase $rgListEnv '' 60
        $rgCatalog = @(Get-RegressGuiCatalog (Get-Content -Raw -Encoding UTF8 (Join-Path $rgRoot ($rgListPhase + '.log'))) $rgListCode)
        if ($rgCatalog.Count -ne 274) { throw "Expected 274 ordinary GUI scenarios, found $($rgCatalog.Count)" }
        $rgAllowed = Get-RegressWindowsGuiSkips
        foreach ($rgName in $rgAllowed.Keys) {
            if ($rgCatalog -cnotcontains $rgName) { throw "Unsupported scenario is absent from registry: $rgName" }
        }
    }
    $rgEnv = New-RegressSandbox $rgPhase
    $rgEnv['DUSKSTUDIO_RUN_SCENARIOS'] = $rgSpec
    if ($rgSpec -ceq 'gui') {
        $rgEnv['DUSKSTUDIO_SETTINGS_SCAN_TEST'] = '1'
        Initialize-RegressPickerCache $rgEnv['DUSKSTUDIO_CONFIG_DIR'] @($rgEnv['DUSKSTUDIO_FIXTURE_DIR'] -split ';' | Where-Object { $_ })
    }
    $rgPostedLogs += $rgPhase
    $rgCode = Invoke-RegressApp $rgPhase $rgEnv '' @@TIMEOUT@@
    $rgText = Get-Content -Raw -Encoding UTF8 (Join-Path $rgRoot ($rgPhase + '.log'))
    $rgLog += "REGRESS-EXIT $rgCode`n"
    if ($rgSpec -ceq 'gui') {
        Assert-RegressScenarioResults $rgText $rgCode $rgCatalog $rgAllowed
    } elseif ([string]$rgCode -cne '0') {
        throw "Scenarios exited with $rgCode"
    }
    $rgResult = 'PASS'
} catch {
    $rgLog += "$rgPhase failed: $_`n"
} finally {
    foreach ($rgName in $rgPostedLogs) {
        $rgPath = Join-Path $rgRoot ($rgName + '.log')
        if (Test-Path -LiteralPath $rgPath) {
            Invoke-RegressPost (Get-Content -LiteralPath $rgPath -Raw -Encoding UTF8) ($rgName + '.log')
        }
    }
}
$rgSeconds = [int]((Get-Date) - $rgStart).TotalSeconds
$rgLog += "REGRESS-SECONDS $rgSeconds`nREGRESS-PHASE $rgPhase RESULT $rgResult`nREGRESS-PHASE $rgPhase END`n"
Invoke-RegressPost $rgLog
[Environment]::Exit(0)
