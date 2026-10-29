param([string]$LibraryPath = (Join-Path $PSScriptRoot '../scripts/regress/windows/lib.ps1'),
      [string]$ScenarioPath = (Join-Path $PSScriptRoot '../scripts/regress/windows/scenarios.ps1'))

. $LibraryPath
$ErrorActionPreference = 'Stop'
$snapshotRoot = Join-Path ([IO.Path]::GetFullPath((Get-Item -LiteralPath $env:TEMP).FullName)) ('dusk-regress-snapshot-' + [Guid]::NewGuid().ToString('N'))
$originalTemp = $env:TEMP
$watched = Join-Path $snapshotRoot 'watched'
$privateTemp = Join-Path $snapshotRoot 'temp'
$child = Join-Path $watched 'child'
$tempChild = Join-Path $privateTemp 'dusk-studio-owned'

function Get-RegressRealFolders { return $watched }

function Get-OwnedSnapshot {
    return @(Get-RegressSnapshot | Where-Object {
        $_.StartsWith($snapshotRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)
    } | Sort-Object)
}

function Assert-Snapshot($condition, $message) {
    if (-not $condition) { throw $message }
}

try {
    [void][IO.Directory]::CreateDirectory($child)
    [void][IO.Directory]::CreateDirectory($tempChild)
    $env:TEMP = $privateTemp
    foreach ($dir in $child, $tempChild) {
        $null = Get-ChildItem -LiteralPath (Split-Path $dir) -Force
        $file = Join-Path $dir 'temporary'
        [IO.File]::WriteAllText($file, 'temporary')
        [IO.File]::Delete($file)
    }
    $first = Get-OwnedSnapshot
    $second = Get-OwnedSnapshot
    Assert-Snapshot ($first.Count -ge 2) 'The snapshot missed owned fixture entries'
    Assert-Snapshot (($first -join "`n") -eq ($second -join "`n")) `
        'An unchanged filesystem produced different snapshots after cold directory enumeration'

    $file = Join-Path $child 'changed'
    [IO.File]::WriteAllText($file, 'a')
    $created = Get-OwnedSnapshot
    Assert-Snapshot (@($created | Where-Object { $_.StartsWith($file + '|1|') }).Count -eq 1) `
        'The snapshot missed a created file'
    Assert-Snapshot (($created -join "`n") -ne ($second -join "`n")) `
        'The snapshot missed a real filesystem change'

    [IO.File]::WriteAllText($file, ('b' * 42))
    $modified = Get-OwnedSnapshot
    Assert-Snapshot (@($modified | Where-Object { $_.StartsWith($file + '|42|') }).Count -eq 1) `
        'The snapshot missed changed file contents'

    [IO.File]::Delete($file)
    $deleted = Get-OwnedSnapshot
    Assert-Snapshot (@($deleted | Where-Object { $_.StartsWith($file + '|') }).Count -eq 0) `
        'The snapshot retained a deleted file'

    $beforeTemp = @($deleted | Where-Object { $_.StartsWith($tempChild + '|temp|') })
    [IO.File]::WriteAllText((Join-Path $tempChild 'changed'), 'changed')
    $afterTemp = @(Get-OwnedSnapshot | Where-Object { $_.StartsWith($tempChild + '|temp|') })
    Assert-Snapshot (($beforeTemp -join "`n") -ne ($afterTemp -join "`n")) `
        'The snapshot missed a real change in a watched temporary directory'
    $allowed = Get-RegressWindowsGuiSkips
    Assert-Snapshot ($allowed.Count -eq 17) 'The Windows skip policy must name exactly 17 unsupported paths'
    $fullNames = @(0..288 | ForEach-Object { 'gui.supported_' + $_.ToString('000') }) + @($allowed.Keys)
    $fullRows = @($fullNames | ForEach-Object {
        if ($allowed.Contains($_)) { '[SKIP] ' + $_ + ': ' + $allowed[$_] }
        else { '[PASS] ' + $_ + ' (1 ms)' }
    })
    $footer = '=== scenarios: 289 pass, 0 fail, 17 skip ==='
    $fullText = ($fullRows + $footer) -join "`n"
    Assert-RegressScenarioResults $fullText 0 $fullNames $allowed
    & {
        $rgRoot = Join-Path $snapshotRoot 'scenario-launch'
        [void][IO.Directory]::CreateDirectory($rgRoot)
        $launches = New-Object 'Collections.Generic.List[object]'
        function Stop-RegressApps {}
        function Get-RegressExe { return 'owned-fixture.exe' }
        function New-RegressSandbox($name) { return @{ DUSKSTUDIO_CONFIG_DIR = $rgRoot; DUSKSTUDIO_FIXTURE_DIR = $rgRoot } }
        function Initialize-RegressPickerCache($config, $roots) {}
        function Invoke-RegressPost($text, $path) {}
        function Invoke-RegressApp($name, $envs, $arguments, $timeout) {
            $launches.Add($envs.Clone())
            $text = if ($envs['DUSKSTUDIO_RUN_SCENARIOS'] -ceq 'list') {
                (@($fullNames | ForEach-Object { $_ + ' [gui]' }) -join "`n")
            } else { $fullText }
            [IO.File]::WriteAllText((Join-Path $rgRoot ($name + '.log')), $text)
            return 0
        }
        $phase = [IO.File]::ReadAllText($ScenarioPath).Replace('@@PHASE@@', 'fixture-gui').Replace('@@SPEC@@', 'gui').Replace('@@TIMEOUT@@', '60')
        $phase = $phase.Replace('[Environment]::Exit(0)', '')
        Invoke-Expression $phase
        Assert-Snapshot ($rgResult -ceq 'PASS') 'The GUI launch template failed its supported verdicts'
        Assert-Snapshot ($launches.Count -eq 2 -and $launches[0]['DUSKSTUDIO_RUN_SCENARIOS'] -ceq 'list' -and $launches[1]['DUSKSTUDIO_RUN_SCENARIOS'] -ceq 'gui') 'The GUI launch did not inventory then run the suite'
        Assert-Snapshot ($launches[1]['DUSKSTUDIO_SETTINGS_SCAN_TEST'] -ceq '1') 'The isolated GUI launch omitted the supported plugin scan'
        Assert-Snapshot (-not $launches[0].ContainsKey('DUSKSTUDIO_SETTINGS_SCAN_TEST')) 'The registry launch enabled an interactive plugin scan'
    }
    $focused = @('gui.plugin_picker_filter_and_load', 'gui.pick_opens_native_editor')
    $focusedRows = @($focused | ForEach-Object { '[PASS] ' + $_ + ' (1 ms)' })
    $focusedText = ($focusedRows + '=== scenarios: 2 pass, 0 fail, 0 skip ===') -join "`n"
    Assert-RegressScenarioResults $focusedText 0 $focused ([ordered]@{})

    $script:guiRejections = 0
    function Assert-GuiRejected([scriptblock]$action, [string]$name) {
        $rejected = $false
        try { & $action | Out-Null } catch { $rejected = $true; Write-Output ("Expected rejection ${name}: " + $_.Exception.Message) }
        Assert-Snapshot $rejected ("Accepted broken GUI evidence: " + $name)
        $script:guiRejections++
    }
    Assert-GuiRejected { Assert-RegressScenarioResults $fullText 1 $fullNames $allowed } 'native failure'
    Assert-GuiRejected { Assert-RegressScenarioResults $fullText 'TIMEOUT' $fullNames $allowed } 'native timeout'
    Assert-GuiRejected { Assert-RegressScenarioResults $fullText $null $fullNames $allowed } 'missing native exit'
    Assert-GuiRejected { Assert-RegressScenarioResults (($fullRows[1..305] + $footer) -join "`n") 0 $fullNames $allowed } 'missing identity'
    Assert-GuiRejected { Assert-RegressScenarioResults (($fullRows + $fullRows[0] + $footer) -join "`n") 0 $fullNames $allowed } 'duplicate identity'
    $reordered = @($fullRows[1], $fullRows[0]) + $fullRows[2..305]
    Assert-GuiRejected { Assert-RegressScenarioResults (($reordered + $footer) -join "`n") 0 $fullNames $allowed } 'reordered identities'
    $foreign = @($fullRows); $foreign[0] = '[PASS] gui.foreign (1 ms)'
    Assert-GuiRejected { Assert-RegressScenarioResults (($foreign + $footer) -join "`n") 0 $fullNames $allowed } 'foreign identity'
    Assert-GuiRejected { Assert-RegressScenarioResults (($fullRows + '[PASS] gui.foreign (1 ms)' + $footer) -join "`n") 0 $fullNames $allowed } 'extra foreign identity'
    $badPass = @($fullRows); $badPass[0] = '[PASS] ' + $fullNames[0] + ' no elapsed time'
    Assert-GuiRejected { Assert-RegressScenarioResults (($badPass + $footer) -join "`n") 0 $fullNames $allowed } 'malformed pass detail'
    $supportedSkip = @($fullRows); $supportedSkip[0] = '[SKIP] ' + $fullNames[0] + ': missing fixture: required.clap'
    Assert-GuiRejected { Assert-RegressScenarioResults (($supportedSkip + '=== scenarios: 288 pass, 0 fail, 18 skip ===') -join "`n") 0 $fullNames $allowed } 'supported missing-fixture skip'
    $wrongReason = @($fullRows); $wrongReason[289] = '[SKIP] ' + $fullNames[289] + ': another reason'
    Assert-GuiRejected { Assert-RegressScenarioResults (($wrongReason + $footer) -join "`n") 0 $fullNames $allowed } 'changed unsupported reason'
    $unsupportedPass = @($fullRows); $unsupportedPass[289] = '[PASS] ' + $fullNames[289] + ' (1 ms)'
    Assert-GuiRejected { Assert-RegressScenarioResults (($unsupportedPass + '=== scenarios: 290 pass, 0 fail, 16 skip ===') -join "`n") 0 $fullNames $allowed } 'unsupported path reported pass'
    Assert-GuiRejected { Assert-RegressScenarioResults ($fullRows -join "`n") 0 $fullNames $allowed } 'missing footer'
    Assert-GuiRejected { Assert-RegressScenarioResults (($fullRows + $footer + $footer) -join "`n") 0 $fullNames $allowed } 'duplicate footer'
    Assert-GuiRejected { Assert-RegressScenarioResults (($fullRows + '=== scenarios: 290 pass, 0 fail, 17 skip ===') -join "`n") 0 $fullNames $allowed } 'wrong footer count'
    Assert-GuiRejected { Assert-RegressScenarioResults (($fullRows + '=== scenarios: 289 pass, 1 fail, 17 skip ===') -join "`n") 0 $fullNames $allowed } 'footer failure'
    Assert-GuiRejected { Assert-RegressScenarioResults ("[DIRTY] cleanup`n" + $fullText) 0 $fullNames $allowed } 'dirty cleanup'
    Assert-GuiRejected { Assert-RegressScenarioResults ("REGRESS-TIMEOUT app`n" + $fullText) 0 $fullNames $allowed } 'timeout marker'
    $failed = @($fullRows); $failed[0] = '[FAIL] ' + $fullNames[0] + ': regression (1 ms)'
    Assert-GuiRejected { Assert-RegressScenarioResults (($failed + '=== scenarios: 288 pass, 1 fail, 17 skip ===') -join "`n") 0 $fullNames $allowed } 'failed result'
    Assert-GuiRejected { Assert-RegressScenarioResults '=== scenarios: 0 pass, 0 fail, 0 skip ===' 0 @() ([ordered]@{}) } 'empty inventory'
    Assert-GuiRejected { Assert-RegressScenarioResults $focusedText 0 @($focused[0], $focused[0]) ([ordered]@{}) } 'duplicate expected identity'
    $malformed = @($fullRows); $malformed[0] = '[PASS] broken result'
    Assert-GuiRejected { Assert-RegressScenarioResults (($malformed + $footer) -join "`n") 0 $fullNames $allowed } 'malformed result'
    Assert-GuiRejected { Assert-RegressScenarioResults (($fullRows + '=== scenarios: nonsense ===') -join "`n") 0 $fullNames $allowed } 'malformed footer'

    $catalogText = ((@($fullNames | ForEach-Object { $_ + ' [gui]' }) + @('gui.helper [gui,helper]', 'gui.helpful [gui,helperful]')) -join "`n")
    $catalog = @(Get-RegressGuiCatalog $catalogText 0)
    Assert-Snapshot ($catalog.Count -eq 307 -and $catalog[-1] -ceq 'gui.helpful' -and $catalog -cnotcontains 'gui.helper') 'Catalog did not exclude precisely the helper tag'
    Assert-GuiRejected { Get-RegressGuiCatalog $catalogText 1 } 'inventory native failure'
    Assert-GuiRejected { Get-RegressGuiCatalog ($catalogText + "`ngui.helpful [gui]") 0 } 'duplicate catalog identity'
    Assert-GuiRejected { Get-RegressGuiCatalog 'gui.case [gui] trailing' 0 } 'malformed catalog'
    Assert-GuiRejected { Get-RegressGuiCatalog 'headless.case [engine]' 0 } 'empty GUI catalog'
    Assert-GuiRejected { Get-RegressGuiCatalog ($catalogText + "`ngui.case [gui] trailing") 0 } 'malformed row among valid catalog entries'

    $fixtures = Join-Path $snapshotRoot 'fixture roots with spaces and apostrophe''s'
    [void][IO.Directory]::CreateDirectory((Join-Path $fixtures 'Release'))
    $fixtureNames = @('dusk-studio-multi-bus-clap-fixture.clap', 'dusk-studio-param-touch-clap-fixture.clap', 'dusk-studio-no-window-clap-fixture.clap')
    $fixturePaths = @((Join-Path (Join-Path $fixtures 'Release') $fixtureNames[0]), (Join-Path $fixtures $fixtureNames[1]), (Join-Path $fixtures $fixtureNames[2]))
    foreach ($path in $fixturePaths) { [IO.File]::WriteAllText($path, 'fixture') }
    $cacheDir = Join-Path $snapshotRoot 'picker-cache'
    Initialize-RegressPickerCache $cacheDir @($fixtures)
    $cachePath = Join-Path $cacheDir 'clap-cache.json'
    $cacheBytes = [IO.File]::ReadAllBytes($cachePath)
    Assert-Snapshot ($cacheBytes.Length -gt 3 -and -not ($cacheBytes[0] -eq 239 -and $cacheBytes[1] -eq 187 -and $cacheBytes[2] -eq 191)) 'Picker cache has a UTF-8 BOM or is empty'
    $cache = [IO.File]::ReadAllText($cachePath) | ConvertFrom-Json
    Assert-Snapshot ($cache.version -eq 1 -and $cache.descriptors.Count -eq 3) 'Picker cache inventory changed'
    $expectedIds = @('studio.dusk.test.multi-bus', 'studio.dusk.test.param-touch', 'studio.dusk.test.no-window')
    $expectedLabels = @('Picker Alpha', 'Picker Beta', 'Window Pick')
    $expectedMakers = @('Scenario Maker A', 'Scenario Maker B', 'Scenario Maker C')
    $expectedCategories = @('Fx|EQ', 'Fx|Delay', 'Fx|Utility')
    for ($index = 0; $index -lt 3; $index++) {
        $entry = $cache.descriptors[$index]
        Assert-Snapshot ($entry.plugin_id -ceq $expectedIds[$index] -and $entry.location -ceq $fixturePaths[$index]) 'Picker cache lost a distinct fixture identity'
        Assert-Snapshot ($entry.name -ceq $expectedLabels[$index] -and $entry.manufacturer -ceq $expectedMakers[$index] -and $entry.category -ceq $expectedCategories[$index]) 'Picker labels no longer match filter controls'
        Assert-Snapshot ($entry.backend -ceq 'native' -and $entry.format_name -ceq 'CLAP' -and $entry.version -eq 1 -and $entry.num_input_channels -eq 2 -and $entry.num_output_channels -eq 2 -and -not $entry.is_instrument) 'Picker cache is not the stereo native CLAP fixture set'
    }
    Assert-Snapshot (-not (Test-Path -LiteralPath (Join-Path $cacheDir 'lv2-native-cache.json'))) 'The Windows picker seeder wrote an LV2 cache'
    for ($index = 0; $index -lt 3; $index++) {
        $missingPath = $fixturePaths[$index]
        $missingCache = Join-Path $snapshotRoot ('missing-picker-' + $index)
        [IO.File]::Delete($missingPath)
        try {
            Assert-GuiRejected { Initialize-RegressPickerCache $missingCache @($fixtures) } ('missing fixture ' + $fixtureNames[$index])
            Assert-Snapshot (-not [IO.Directory]::Exists($missingCache)) 'Missing fixture left a partial picker cache'
        } finally { [IO.File]::WriteAllText($missingPath, 'fixture') }
    }
    Write-Output "Windows GUI validation contracts: full 289/17 and focused 2/0 accepted, $script:guiRejections bad inputs rejected"

    function New-ForegroundEvidence([long]$foreignOffset = 1200) {
        [long]$origin = 9007199254745001
        $line = '[SCENARIO_END_QPC] freq=3579545 start=' + ($origin + 1000) + ' end=' + ($origin + 1100) + ' pid=42 frequency_ok=1 start_ok=1 end_ok=1 write=42 flush=0'
        return @{
            app_pid = 42; app_session = 1; launcher_session = 1; activation = @{}
            qpc_frequency = 3579545; qpc_is_high_resolution = $true; qpc_sample_scope = 'foreground-and-session-v1'
            footer_observed_qpc = ($origin + 2000); footer_qpc_lines = @(@{line = $line; observed_qpc = ($origin + 2100)})
            foreground_samples = @(
                @{qpc_start = ($origin + 100); qpc_end = ($origin + 110); in_suite = $true; suite_started = $true; owned = $true; desktop = 'Default'; active_console = 1},
                @{qpc_start = ($origin + $foreignOffset); qpc_end = ($origin + $foreignOffset + 10); in_suite = $true; suite_started = $true; owned = $false; desktop = 'Default'; active_console = 1})
        }
    }
    foreach ($offset in @(800, 1050, 1099, 1100, 1101, 1102, 1200)) {
        $evidence = New-ForegroundEvidence $offset
        Complete-RegressForegroundSamples $evidence
        Assert-Snapshot ($evidence.strict_focus_pass -eq ($offset -gt 1101)) ('Wrong foreground completion boundary at offset ' + $offset)
        Assert-Snapshot ($evidence.foreground_samples[1].in_suite -eq ($offset -le 1101)) 'A foreign sample was lost across the completion boundary'
        Assert-Snapshot ($evidence.completion_qpc.end -eq 9007199254746101) 'Completion ticks lost 64-bit integer precision'
    }
    foreach ($offset in @(1050, 1060, 1099, 1100, 1101, 1102, 1200)) {
        $evidence = New-ForegroundEvidence $offset
        $evidence.footer_observed_qpc = 9007199254745001 + $offset - 5
        $evidence.foreground_samples[1].in_suite = $false
        Complete-RegressForegroundSamples $evidence
        Assert-Snapshot ($evidence.strict_focus_pass -eq ($offset -gt 1101)) ('Ignored a previously observed footer at offset ' + $offset)
    }
    foreach ($change in @('desktop', 'session')) {
        $evidence = New-ForegroundEvidence
        $sample = $evidence.foreground_samples[0]
        if ($change -ceq 'desktop') { $sample.desktop = 'Winlogon' } else { $sample.active_console = 2 }
        Complete-RegressForegroundSamples $evidence
        Assert-Snapshot (-not $evidence.strict_focus_pass) ('Accepted a changed in-suite ' + $change)
    }
    $script:foregroundRejections = 0
    function Assert-ForegroundRejected($evidence, [string]$name) {
        $rejected = $false
        try { Complete-RegressForegroundSamples $evidence } catch { $rejected = $true; Write-Output ("Expected foreground rejection ${name}: " + $_.Exception.Message) }
        Assert-Snapshot $rejected ('Accepted invalid foreground timing: ' + $name)
        $script:foregroundRejections++
    }
    foreach ($field in @('frequency_ok', 'start_ok', 'end_ok', 'flush')) {
        $evidence = New-ForegroundEvidence
        $from = if ($field -ceq 'flush') { 'flush=0' } else { $field + '=1' }
        $to = if ($field -ceq 'flush') { 'flush=-1' } else { $field + '=0' }
        $evidence.footer_qpc_lines[0].line = $evidence.footer_qpc_lines[0].line.Replace($from, $to)
        Assert-ForegroundRejected $evidence $field
    }
    foreach ($change in @('missing-marker', 'duplicate-marker', 'malformed-marker', 'frequency', 'pid', 'write', 'backward-footer', 'overflow-footer', 'observed-marker', 'observed-footer', 'missing-footer', 'non-qpc-clock', 'zero-frequency', 'sample-scope', 'no-activation', 'app-session', 'zero-sample', 'backward-sample', 'decreasing-samples', 'missing-start-state', 'no-during-sample')) {
        $evidence = New-ForegroundEvidence
        switch ($change) {
            'missing-marker' { $evidence.footer_qpc_lines = @() }
            'duplicate-marker' { $evidence.footer_qpc_lines += $evidence.footer_qpc_lines[0] }
            'malformed-marker' { $evidence.footer_qpc_lines[0].line += ' extra' }
            'frequency' { $evidence.qpc_frequency++ }
            'pid' { $evidence.app_pid++ }
            'write' { $evidence.footer_qpc_lines[0].line = $evidence.footer_qpc_lines[0].line.Replace('write=42', 'write=-1') }
            'backward-footer' { $evidence.footer_qpc_lines[0].line = $evidence.footer_qpc_lines[0].line.Replace('end=9007199254746101', 'end=9007199254746000') }
            'overflow-footer' { $evidence.footer_qpc_lines[0].line = $evidence.footer_qpc_lines[0].line.Replace('end=9007199254746101', 'end=9223372036854775807'); $evidence.footer_qpc_lines[0].observed_qpc = [long]::MaxValue; $evidence.footer_observed_qpc = [long]::MaxValue }
            'observed-marker' { $evidence.footer_qpc_lines[0].observed_qpc = 1 }
            'observed-footer' { $evidence.footer_observed_qpc = 1 }
            'missing-footer' { $evidence.footer_observed_qpc = $null }
            'non-qpc-clock' { $evidence.qpc_is_high_resolution = $false }
            'zero-frequency' { $evidence.qpc_frequency = 0 }
            'sample-scope' { $evidence.qpc_sample_scope = 'foreground-only' }
            'no-activation' { $evidence.activation = $null }
            'app-session' { $evidence.app_session = 2 }
            'zero-sample' { $evidence.foreground_samples[0].qpc_start = 0 }
            'backward-sample' { $evidence.foreground_samples[0].qpc_end = 1 }
            'decreasing-samples' { $evidence.foreground_samples[1].qpc_start = $evidence.foreground_samples[0].qpc_start }
            'missing-start-state' { $evidence.foreground_samples[1].Remove('suite_started') }
            'no-during-sample' { $evidence.foreground_samples[0].suite_started = $false; $evidence.foreground_samples[0].in_suite = $false }
        }
        Assert-ForegroundRejected $evidence $change
    }
    Write-Output "Windows foreground timing: 14 integer boundary controls, 2 session controls, $script:foregroundRejections invalid inputs rejected"

    Write-Output 'Windows regression snapshots: 7 checks passed'
} finally {
    $env:TEMP = $originalTemp
    if (Test-Path -LiteralPath $snapshotRoot) { Remove-Item -LiteralPath $snapshotRoot -Recurse -Force }
}
