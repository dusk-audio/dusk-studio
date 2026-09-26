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
        ForEach-Object { $rows += "$($_.FullName)|temp|$($_.LastWriteTimeUtc.Ticks)" }
    return $rows
}
