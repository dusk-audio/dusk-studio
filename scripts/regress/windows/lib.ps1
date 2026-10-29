# Shared by every phase script: the host serves each phase with this file in
# front of it, so the helpers exist in the console before the phase body runs.
# iex runs in the console's session scope, so every variable a phase reads is
# assigned here or in the phase itself, never left over from an earlier one.
$ErrorActionPreference = 'Continue'
# Invoke-WebRequest renders a progress bar per chunk in PowerShell 5.1, which
# costs minutes on a payload the size of the installer.
$ProgressPreference = 'SilentlyContinue'
$rgIp = '@@HOSTIP@@'
$rgRoot = "$env:LOCALAPPDATA\@@ROOT@@"
$rgLog = ''
$rgResult = 'FAIL'
$rgExe = ''

# PowerShell 5.1 sends a string body as ISO-8859-1; the app's output is UTF-8.
function Invoke-RegressPost($text, $path) {
    if (-not $path) { $path = '' }
    $bytes = [System.Text.Encoding]::UTF8.GetBytes([string]$text)
    Invoke-RestMethod -Uri "http://${rgIp}:9000/$path" -Method POST -Body $bytes `
        -ContentType 'text/plain; charset=utf-8' | Out-Null
}

function Get-RegressFile($name, $dest) {
    Invoke-WebRequest -UseBasicParsing "http://${rgIp}:8000/$name" -OutFile $dest
}

# The msi-install phase records the executable it installed or unpacked; every
# later phase starts that one and no other.
function Get-RegressExe {
    $record = "$rgRoot\installed-exe.txt"
    if (-not (Test-Path $record)) { throw "no ${record}: the msi-install phase did not complete" }
    $exe = (Get-Content $record -TotalCount 1).Trim()
    if (-not (Test-Path $exe)) { throw "recorded executable is missing: $exe" }
    return $exe
}

# Everything that could still hold the single-instance slot, which is keyed on
# the user and not on the profile, so a leftover instance would take the
# next launch's handoff.
function Stop-RegressApps {
    Get-Process DuskStudio, dusk-studio-plugin-host -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
}

# The plugin host inherits the app's redirected handles, so a pipe can stay
# open after the app itself has exited; a bounded wait keeps that from hanging
# the phase. Stopping the host first closes the usual holder.
function Stop-RegressChildren {
    Get-Process dusk-studio-plugin-host -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
}

function Read-RegressTask($task, $ms) {
    if ($task.Wait($ms)) { return $task.Result }
    return "<read timed out after ${ms} ms; output withheld by an open inherited handle>"
}

# A marker has to be seen while the app is still running, and ReadToEndAsync
# only returns once the pipe closes. One bounded ReadLineAsync at a time
# instead, no delegates: a scriptblock callback throws under iex. The first
# wait paces the caller's poll loop; the rest drain what is already buffered.
function Read-RegressStderr($state, $ms) {
    while ($null -ne $state.Task -and $state.Task.Wait($ms)) {
        $line = $state.Task.Result
        if ($null -eq $line) { $state.Task = $null; break }
        [void]$state.Text.AppendLine($line)
        $state.Task = $state.Reader.ReadLineAsync()
        $ms = 100
    }
    return $state.Text.ToString()
}

# A private profile for one launch, under the run folder. SHGetKnownFolderPath
# expands the AppData, Documents and Music folders from USERPROFILE, so they
# follow it as long as they exist; the Profile folder itself does not, so the
# app takes its home from USERPROFILE. DUSKSTUDIO_CONFIG_DIR and
# DUSKSTUDIO_MUSIC_DIR name the app's own folders outright, beside the home
# rather than inside it. Every release build carries the MP3 encoder, so
# DUSKSTUDIO_EXPECT_MP3 turns the MP3 scenarios' skip into a failure.
function New-RegressSandbox($name) {
    $base = "$rgRoot\launch\$name"
    if (Test-Path $base) { Remove-Item -Recurse -Force $base }
    $home_ = "$base\home"
    foreach ($d in @("$home_\AppData\Roaming", "$home_\AppData\Local", "$home_\Documents",
            "$home_\Music", "$home_\Desktop", "$base\temp", "$base\config", "$base\music")) {
        New-Item -ItemType Directory -Force -Path $d | Out-Null
    }
    return @{
        USERPROFILE           = $home_
        HOME                  = $home_
        HOMEDRIVE             = $home_.Substring(0, 2)
        HOMEPATH              = $home_.Substring(2)
        APPDATA               = "$home_\AppData\Roaming"
        LOCALAPPDATA          = "$home_\AppData\Local"
        TEMP                  = "$base\temp"
        TMP                   = "$base\temp"
        DUSKSTUDIO_CONFIG_DIR = "$base\config\Dusk Studio"
        DUSKSTUDIO_MUSIC_DIR  = "$base\music"
        DUSKSTUDIO_FIXTURE_DIR = "$rgRoot\fixtures\build-tests;$rgRoot\fixtures\tests-fixtures"
        DUSKSTUDIO_EXPECT_MP3  = "1"
    }
}

# Redirected stdio, not Start-Process: the app reports through stdout and
# stderr and there is no other way to read them back out of the guest.
function Start-RegressApp($envs, $appArgs) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $rgExe
    $psi.Arguments = $appArgs
    $psi.WorkingDirectory = (Split-Path $rgExe)
    $psi.UseShellExecute = $false
    $psi.RedirectStandardError = $true
    $psi.RedirectStandardOutput = $true
    $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [System.Text.Encoding]::UTF8
    if ($envs) { foreach ($k in $envs.Keys) { $psi.EnvironmentVariables[$k] = $envs[$k] } }
    $p = [System.Diagnostics.Process]::Start($psi)
    $p | Add-Member -NotePropertyName RgOut -NotePropertyValue $p.StandardOutput.ReadToEndAsync()
    $p | Add-Member -NotePropertyName RgErr -NotePropertyValue @{
        Reader = $p.StandardError
        Text   = New-Object System.Text.StringBuilder
        Task   = $p.StandardError.ReadLineAsync()
    }
    return $p
}

# Runs the app to completion or to its deadline and writes stdout, then
# stderr, to $rgRoot\<name>.log. Returns the exit code, or TIMEOUT. A timeout
# is announced to the host first, so it can screenshot the guest before the
# process is killed.
function Invoke-RegressApp($name, $envs, $appArgs, $timeoutSec) {
    $p = Start-RegressApp $envs $appArgs
    $until = (Get-Date).AddSeconds($timeoutSec)
    $stderr = ''
    while (-not $p.HasExited -and (Get-Date) -lt $until) {
        $stderr = Read-RegressStderr $p.RgErr 1000
    }
    if (-not $p.HasExited) {
        Invoke-RegressPost "REGRESS-TIMEOUT $name after ${timeoutSec}s`n"
        Start-Sleep -Seconds 8
        try { $p.Kill() } catch { }
        $p.WaitForExit(10000) | Out-Null
        $code = 'TIMEOUT'
    } else {
        $p.WaitForExit()
        $code = $p.ExitCode
    }
    Stop-RegressChildren
    $stderr = Read-RegressStderr $p.RgErr 2000
    Set-Content -Encoding UTF8 -Path "$rgRoot\$name.log" `
        -Value ((Read-RegressTask $p.RgOut 10000) + "`n---stderr---`n" + $stderr)
    return $code
}

function Complete-RegressForegroundSamples($result) {
    if (-not $result.qpc_is_high_resolution -or $result.qpc_frequency -le 0 -or
        $result.qpc_sample_scope -cne 'foreground-and-session-v1') { throw 'Foreground clock is unavailable' }
    if (@($result.footer_qpc_lines).Count -ne 1 -or $null -eq $result.footer_observed_qpc) { throw 'Missing or duplicate scenario completion clock' }
    $entry = $result.footer_qpc_lines[0]
    $marker = [regex]::Match($entry.line, '^\[SCENARIO_END_QPC\] freq=([1-9][0-9]*) start=([1-9][0-9]*) end=([1-9][0-9]*) pid=([1-9][0-9]*) frequency_ok=1 start_ok=1 end_ok=1 write=([1-9][0-9]*) flush=0$')
    if (-not $marker.Success) { throw 'Invalid scenario completion clock' }
    [long]$frequency = $marker.Groups[1].Value
    [long]$start = $marker.Groups[2].Value
    [long]$end = $marker.Groups[3].Value
    [long]$owner = $marker.Groups[4].Value
    if ($frequency -ne $result.qpc_frequency -or $owner -ne $result.app_pid -or
        $end -lt $start -or $end -ge [long]::MaxValue -or
        [long]$entry.observed_qpc -lt $end -or [long]$result.footer_observed_qpc -lt $start) { throw 'Inconsistent scenario completion clock' }
    if ($null -eq $result.activation -or $result.app_session -ne $result.launcher_session) { throw 'Owned foreground session was unavailable' }
    [long]$previous = 0
    $during = 0
    $after = 0
    $pass = $true
    foreach ($sample in $result.foreground_samples) {
        [long]$sampleStart = $sample.qpc_start
        [long]$sampleEnd = $sample.qpc_end
        if ($sampleStart -le 0 -or $sampleStart -lt $previous -or $sampleEnd -lt $sampleStart -or
            $sample.suite_started -isnot [bool]) { throw 'Invalid foreground sample clock' }
        $previous = $sampleEnd
        $sample.legacy_in_suite = [bool]$sample.in_suite
        $sample.after_suite = $sampleStart -gt ($end + 1)
        $sample.in_suite = $sample.suite_started -and -not $sample.after_suite
        if ($sample.after_suite) { $after++ }
        if ($sample.in_suite) {
            $during++
            if (-not $sample.owned -or $sample.desktop -cne 'Default' -or
                $sample.active_console -ne $result.launcher_session) { $pass = $false }
        }
    }
    if ($during -eq 0) { throw 'No foreground samples during the scenario suite' }
    $result.completion_qpc = @{ frequency = $frequency; start = $start; end = $end; during_samples = $during; after_samples = $after }
    $result.strict_focus_pass = $pass
}

# The real folders a launch must leave alone. The snapshot is taken after the
# install and compared once every launch is done.
function Get-RegressRealFolders {
    $real = @(
        [Environment]::GetFolderPath('MyDocuments'),
        [Environment]::GetFolderPath('MyMusic'),
        [Environment]::GetFolderPath('Desktop'),
        "$env:APPDATA\Dusk Studio",
        "$env:LOCALAPPDATA\Dusk Studio"
    )
    return $real | Where-Object { $_ }
}

function Get-RegressSnapshot {
    $rows = @()
    foreach ($dir in Get-RegressRealFolders) {
        if (-not (Test-Path $dir)) { $rows += "$dir|absent|"; continue }
        Get-ChildItem -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue |
            ForEach-Object {
                $_.Refresh()
                $len = if ($_.PSIsContainer) { 'dir' } else { $_.Length }
                $rows += "$($_.FullName)|$len|$($_.LastWriteTimeUtc.Ticks)"
            }
    }
    # The home folder itself by entry name only: Windows rewrites its registry
    # hive files there all the time, but a launch that took its home from the
    # real profile would leave a new entry beside them.
    $real = [Environment]::GetFolderPath('UserProfile')
    Get-ChildItem -LiteralPath $real -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notlike 'ntuser*' } |
        ForEach-Object { $rows += "$($_.FullName)|home-entry|" }
    # The harness config and music folders land in the temp dir when the
    # environment does not name them.
    Get-ChildItem -LiteralPath $env:TEMP -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like 'dusk-studio-*' } |
        ForEach-Object {
            $_.Refresh()
            $rows += "$($_.FullName)|temp|$($_.LastWriteTimeUtc.Ticks)"
        }
    return $rows
}

function Get-RegressGuiCatalog([string]$text, $exitCode) {
    if ([string]$exitCode -cne '0') { throw "GUI inventory exited with $exitCode" }
    $names = New-Object 'Collections.Generic.List[string]'
    $seen = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    foreach ($line in ($text -split "`r?`n")) {
        if (-not $line.StartsWith('gui.', [StringComparison]::Ordinal)) { continue }
        $match = [regex]::Match($line, '^gui\.[A-Za-z0-9_]+(?: \[([^\]]*)\])?$')
        if (-not $match.Success) { throw "Malformed GUI inventory row: $line" }
        $name = ($line -split ' ', 2)[0]
        if (-not $seen.Add($name)) { throw "Duplicate GUI inventory row: $name" }
        $tags = @($match.Groups[1].Value -split ',' | ForEach-Object { $_.Trim() })
        if ($tags -ccontains 'helper') { continue }
        [void]$names.Add($name)
    }
    if ($names.Count -eq 0) { throw 'The GUI inventory is empty' }
    return $names.ToArray()
}

function Get-RegressWindowsGuiSkips {
    return [ordered]@{
        'gui.clap_no_window_message' = 'only the Linux host looks for a plug-in window in the container'
        'gui.dp_stereo_finalization_failure' = 'requires the native finalization fault fixture'
        'gui.dsp_readout_realtime_count' = 'the realtime CPU-time guard is Linux-only'
        'gui.fresh_sandbox_crash_saves_clean' = 'needs sandboxed plug-ins and a child that can be killed by pid'
        'gui.lv2_editor_reflects_state' = 'missing fixture: file_state.lv2'
        'gui.lv2_midi_learn' = 'missing fixture: file_state.lv2'
        'gui.midi_io_selectors' = 'virtual MIDI destination fixture requires ALSA sequencer'
        'gui.oop_editor_closes_before_child' = 'Windows tracks the child by handle, not by pid'
        'gui.oop_editor_failure_no_strand' = 'the sandbox stub children are driven on POSIX only'
        'gui.piano_live_recording_preview' = 'requires the native virtual keyboard and X server readback'
        'gui.piano_roll_follows_take' = 'reads the window back through the X server'
        'gui.piano_roll_keeps_its_region' = 'reads the window back through the X server'
        'gui.piano_roll_shows_each_take' = 'reads the window back through the X server'
        'gui.plugin_editor_tab_left_alone' = 'needs the Linux CLAP fixture'
        'gui.plugin_edits_prompt_lv2' = 'missing fixture: state_probe.lv2'
        'gui.settings_rescan_devices' = 'requires native settings and ALSA sequencer'
        'gui.slot_health_labels' = 'the sandbox stub children are driven on POSIX only'
    }
}

function Assert-RegressScenarioResults([string]$text, $exitCode, [string[]]$expectedNames, $allowedSkips) {
    if ([string]$exitCode -cne '0') { throw "Scenarios exited with $exitCode" }
    if ($text -match '\[DIRTY\]|REGRESS-TIMEOUT') { throw 'Scenario cleanup or timeout failed' }
    $expected = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    foreach ($name in $expectedNames) {
        if (-not $expected.Add($name)) { throw "Duplicate expected scenario: $name" }
    }
    if ($expected.Count -eq 0) { throw 'No expected scenarios were supplied' }
    $seen = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    $records = New-Object 'Collections.Generic.List[string]'
    $passed = 0
    $skipped = 0
    $footer = $null
    foreach ($line in ($text -split "`r?`n")) {
        if ($line -match '^\[(PASS|FAIL|SKIP)\]') {
            $match = [regex]::Match($line, '^\[(PASS|FAIL|SKIP)\] ([A-Za-z0-9_.]+)(.*)$')
            if (-not $match.Success) { throw "Malformed scenario result: $line" }
            $status = $match.Groups[1].Value
            $name = $match.Groups[2].Value
            $detail = $match.Groups[3].Value
            if (-not $expected.Contains($name)) { throw "Unexpected scenario result: $name" }
            if (-not $seen.Add($name)) { throw "Duplicate scenario result: $name" }
            if ($status -ceq 'FAIL') { throw "Scenario failed: $line" }
            if ($status -ceq 'SKIP') {
                if (-not $allowedSkips.Contains($name)) { throw "Supported scenario skipped: $name" }
                if ($detail -cne (': ' + $allowedSkips[$name])) { throw "Unexpected skip reason: $line" }
                $skipped++
            } else {
                if ($allowedSkips.Contains($name)) { throw "Unsupported scenario reported a pass: $name" }
                if ($detail -notmatch '^ \([0-9]+ ms\)$') { throw "Malformed pass result: $line" }
                $passed++
            }
            [void]$records.Add($name)
        } elseif ($line.StartsWith('=== scenarios:', [StringComparison]::Ordinal)) {
            if ($null -ne $footer) { throw 'Duplicate scenario summary' }
            $footer = [regex]::Match($line, '^=== scenarios: ([0-9]+) pass, ([0-9]+) fail, ([0-9]+) skip ===$')
            if (-not $footer.Success) { throw "Malformed scenario summary: $line" }
        }
    }
    if ($records.Count -ne $expectedNames.Count) { throw 'Scenario inventory is incomplete' }
    for ($index = 0; $index -lt $expectedNames.Count; $index++) {
        if ($records[$index] -cne $expectedNames[$index]) { throw 'Scenario result order differs from the selected inventory' }
    }
    if ($null -eq $footer) { throw 'Missing scenario summary' }
    if ([int]$footer.Groups[1].Value -ne $passed -or [int]$footer.Groups[2].Value -ne 0 -or [int]$footer.Groups[3].Value -ne $skipped) {
        throw 'Scenario summary disagrees with individual results'
    }
}

function Initialize-RegressPickerCache([string]$configDir, [string[]]$fixtureRoots) {
    $specs = @(
        @('dusk-studio-multi-bus-clap-fixture.clap', 'studio.dusk.test.multi-bus', 'Picker Alpha', 'Scenario Maker A', 'Fx|EQ'),
        @('dusk-studio-param-touch-clap-fixture.clap', 'studio.dusk.test.param-touch', 'Picker Beta', 'Scenario Maker B', 'Fx|Delay'),
        @('dusk-studio-no-window-clap-fixture.clap', 'studio.dusk.test.no-window', 'Window Pick', 'Scenario Maker C', 'Fx|Utility')
    )
    $descriptors = @()
    foreach ($spec in $specs) {
        $location = $null
        foreach ($root in $fixtureRoots) {
            foreach ($relative in @($spec[0], (Join-Path 'Release' $spec[0]))) {
                $candidate = [IO.Path]::GetFullPath((Join-Path $root $relative))
                if ([IO.File]::Exists($candidate)) { $location = $candidate; break }
            }
            if ($null -ne $location) { break }
        }
        if ($null -eq $location) { throw ('Missing picker fixture: ' + $spec[0]) }
        $descriptors += [ordered]@{
            version = 1; backend = 'native'; name = $spec[2]; manufacturer = $spec[3]; category = $spec[4]
            format_name = 'CLAP'; location = $location; plugin_id = $spec[1]
            num_input_channels = 2; num_output_channels = 2; is_instrument = $false
        }
    }
    [void][IO.Directory]::CreateDirectory($configDir)
    $json = [ordered]@{version = 1; descriptors = $descriptors} | ConvertTo-Json -Depth 8
    [IO.File]::WriteAllText((Join-Path $configDir 'clap-cache.json'), $json, (New-Object Text.UTF8Encoding($false)))
}
