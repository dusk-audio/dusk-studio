$rgPhase = 'gui-plugin-picker'
$rgStart = Get-Date
try {
    Stop-RegressApps
    $rgExe = Get-RegressExe
    $rgNames = @('gui.plugin_picker_filter_and_load', 'gui.pick_opens_native_editor')
    $rgFailed = 0
    foreach ($rgAttempt in 1..3) {
        $rgName = $rgPhase + '-' + $rgAttempt
        try {
            $rgEnv = New-RegressSandbox $rgName
            Initialize-RegressPickerCache $rgEnv['DUSKSTUDIO_CONFIG_DIR'] @($rgEnv['DUSKSTUDIO_FIXTURE_DIR'] -split ';' | Where-Object { $_ })
            $rgEnv['DUSKSTUDIO_RUN_SCENARIOS'] = 'gui:' + ($rgNames -join ',')
            $rgCode = Invoke-RegressApp $rgName $rgEnv '' 60
            $rgText = Get-Content -Raw -Encoding UTF8 (Join-Path $rgRoot ($rgName + '.log'))
            $rgLog += "$rgName native exit $rgCode`n"
            Assert-RegressScenarioResults $rgText $rgCode $rgNames ([ordered]@{})
        } catch {
            $rgFailed++
            $rgLog += "$rgName failed: $_`n"
        } finally {
            $rgPath = Join-Path $rgRoot ($rgName + '.log')
            if (Test-Path -LiteralPath $rgPath) {
                Invoke-RegressPost (Get-Content -LiteralPath $rgPath -Raw -Encoding UTF8) ($rgName + '.log')
            }
        }
    }
    if ($rgFailed -ne 0) { throw "$rgFailed picker attempts failed" }
    $rgResult = 'PASS'
} catch {
    $rgLog += "$rgPhase failed: $_`n"
}
$rgSeconds = [int]((Get-Date) - $rgStart).TotalSeconds
$rgLog += "REGRESS-SECONDS $rgSeconds`nREGRESS-PHASE $rgPhase RESULT $rgResult`nREGRESS-PHASE $rgPhase END`n"
Invoke-RegressPost $rgLog
[Environment]::Exit(0)
