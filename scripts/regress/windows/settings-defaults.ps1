# tests/gui_settings_defaults.sh for the guest: gui.settings_defaults needs an
# app-config.properties seeded before launch, which the suite run does not
# have. Both values, three launches each, every one must pass.
try {
    Stop-RegressApps
    $rgExe = Get-RegressExe
    $rgOk = $true
    foreach ($rgEnabled in 0, 1) {
        foreach ($rgAttempt in 1, 2, 3) {
            $rgName = "settings-defaults-$rgEnabled-$rgAttempt"
            $rgEnv = New-RegressSandbox $rgName
            New-Item -ItemType Directory -Force -Path $rgEnv['DUSKSTUDIO_CONFIG_DIR'] | Out-Null
            [System.IO.File]::WriteAllText((Join-Path $rgEnv['DUSKSTUDIO_CONFIG_DIR'] 'app-config.properties'),
                "tape_strip_expanded_default=$rgEnabled`nfollow_playhead_default=$rgEnabled`n")
            $rgEnv['DUSKSTUDIO_RUN_SCENARIOS'] = 'gui:gui.settings_defaults'
            $rgCode = Invoke-RegressApp $rgName $rgEnv '' 120
            $rgLine = Get-Content "$rgRoot\$rgName.log" -Encoding UTF8 |
                Select-String -Pattern '^\[(PASS|FAIL|SKIP)\] gui\.settings_defaults' | Select-Object -First 1
            $rgLog += "enabled=$rgEnabled attempt=$rgAttempt exit=${rgCode}: $($rgLine.Line)`n"
            if ($rgCode -ne 0 -or -not $rgLine -or $rgLine.Line -notlike '`[PASS`]*') {
                $rgOk = $false
                $rgLog += ((Get-Content "$rgRoot\$rgName.log" -Encoding UTF8 |
                    Select-String -Pattern '^\[|^       ' | Select-Object -First 20 |
                    ForEach-Object { '  ' + $_.Line }) -join "`n") + "`n"
            }
        }
    }
    if ($rgOk) { $rgResult = 'PASS' }
} catch {
    $rgLog += "gui-settings-defaults failed: $_`n"
}

$rgLog += "REGRESS-PHASE gui-settings-defaults RESULT $rgResult`nREGRESS-PHASE gui-settings-defaults END`n"
Invoke-RegressPost $rgLog

# iex runs the script in a child scope, so plain "exit" leaves the console
# open and repeat runs stack up windows in the guest.
[Environment]::Exit(0)
