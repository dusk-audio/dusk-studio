# Isolation audit, after every launch of the run: nothing in the real user's
# Documents, Music, Desktop or Dusk Studio config, nor at the top of the real
# home folder, may have appeared, changed or gone since the install phase took
# its snapshot, and every folder the app resolves inside a launch's private
# profile has to sit in that profile.
#
# The child resolves them the way the app does: home from USERPROFILE, the
# known folders from SHGetKnownFolderPath, temp from GetTempPath, config and
# Music from the environment. The Profile known folder comes from the account
# and never follows USERPROFILE; the app falls back to it only when
# USERPROFILE is unset or relative, so it is on record and nothing more.
$rgKnownFolders = @'
"Home=$env:USERPROFILE"
$ids = [ordered]@{ Profile = '5E6C858F-0E22-4760-9AFE-EA3317B67173'; RoamingAppData = '3EB685DB-65F9-4CF6-A03A-E3EF65729F3D'; LocalAppData = 'F1B32785-6FBA-4FCF-9D55-7B8E7F157091'; Documents = 'FDD39AD0-238F-46AF-ADB4-6C85480369C7'; Music = '4BD8D571-6D19-48D3-BE97-422220080E43' }
Add-Type -Namespace RegressKfChild -Name Api -MemberDefinition '[DllImport("shell32.dll")] public static extern int SHGetKnownFolderPath(ref Guid id, uint flags, IntPtr token, out IntPtr path);'
foreach ($k in $ids.Keys) {
    $g = [Guid]$ids[$k]; $p = [IntPtr]::Zero
    $hr = [RegressKfChild.Api]::SHGetKnownFolderPath([ref]$g, 0, [IntPtr]::Zero, [ref]$p)
    "$k=" + $(if ($hr -eq 0) { [Runtime.InteropServices.Marshal]::PtrToStringUni($p) } else { "error $hr" })
}
"Temp=" + [IO.Path]::GetTempPath()
"Config=$env:DUSKSTUDIO_CONFIG_DIR"
"MusicDir=$env:DUSKSTUDIO_MUSIC_DIR"
'@

function Test-RegressInside($path, $root) {
    if (-not $path -or -not [IO.Path]::IsPathRooted($path)) { return $false }
    $full = [IO.Path]::GetFullPath($path).TrimEnd('\') + '\'
    return $full.StartsWith([IO.Path]::GetFullPath($root).TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)
}

$rgEscaped = 0
try {
    Stop-RegressApps
    $rgBefore = "$rgRoot\real-before.txt"
    if (-not (Test-Path $rgBefore)) { throw "no ${rgBefore}: the msi-install phase did not complete" }
    $rgOld = @{}
    foreach ($l in Get-Content -Encoding UTF8 $rgBefore) { $rgOld[$l] = $true }
    $rgNew = @{}
    foreach ($l in Get-RegressSnapshot) { $rgNew[$l] = $true }
    $rgChanged = @($rgNew.Keys | Where-Object { -not $rgOld.ContainsKey($_) } | Sort-Object)
    $rgGone = @($rgOld.Keys | Where-Object { -not $rgNew.ContainsKey($_) } | Sort-Object)
    $rgLog += "watched: $((Get-RegressRealFolders) -join '; '); the entries of $([Environment]::GetFolderPath('UserProfile')); $env:TEMP\dusk-studio-*`n"
    if ($rgChanged.Count -gt 0 -or $rgGone.Count -gt 0) {
        $rgLog += "error: $($rgChanged.Count + $rgGone.Count) entries of the real profile appeared, changed or went away`n"
    }
    foreach ($l in $rgChanged) { $rgLog += "written: $l`n" }
    foreach ($l in $rgGone) { $rgLog += "before, now gone or changed: $l`n" }

    # The same private profile a launch gets, asked from a child process that
    # inherits it, which is how the app sees it.
    $rgEnv = New-RegressSandbox 'audit'
    $rgHome = $rgEnv['USERPROFILE']
    $rgLaunch = Split-Path $rgHome
    $rgWithin = [ordered]@{
        Home = $rgHome; RoamingAppData = $rgHome; LocalAppData = $rgHome; Documents = $rgHome; Music = $rgHome
        Temp = $rgLaunch; Config = $rgLaunch; MusicDir = $rgLaunch
    }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = 'powershell.exe'
    $psi.Arguments = '-NoProfile -NonInteractive -EncodedCommand ' +
        [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($rgKnownFolders))
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    foreach ($k in $rgEnv.Keys) { $psi.EnvironmentVariables[$k] = $rgEnv[$k] }
    $p = [System.Diagnostics.Process]::Start($psi)
    $rgOut = $p.StandardOutput.ReadToEndAsync()
    $null = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(60000)) { try { $p.Kill() } catch { } }
    $rgResolved = @{}
    foreach ($l in ((Read-RegressTask $rgOut 5000) -split "`r?`n" | Where-Object { $_ -match '=' })) {
        $rgPair = $l -split '=', 2
        $rgResolved[$rgPair[0]] = $rgPair[1]
    }
    $rgLog += "folders a launch resolves in its private profile ($rgLaunch):`n"
    foreach ($k in $rgWithin.Keys) {
        $value = $rgResolved[$k]
        if (Test-RegressInside $value $rgWithin[$k]) {
            $rgLog += "  $k=$value`n"
        } elseif (-not $value -or -not [IO.Path]::IsPathRooted($value)) {
            $rgLog += "error: $k did not resolve, so its isolation is unproven: '$value'`n"
            $rgEscaped++
        } else {
            $rgLog += "error: $k resolves outside $($rgWithin[$k]): $value`n"
            $rgEscaped++
        }
    }
    $rgLog += "Profile known folder, information only (the account's, not USERPROFILE's; the app does not use it here): $($rgResolved['Profile'])`n"
    if ($rgChanged.Count -eq 0 -and $rgGone.Count -eq 0 -and $rgEscaped -eq 0) { $rgResult = 'PASS' }
} catch {
    $rgLog += "isolation-audit failed: $_`n"
}

$rgLog += "REGRESS-PHASE isolation-audit RESULT $rgResult`nREGRESS-PHASE isolation-audit END`n"
Invoke-RegressPost $rgLog

# iex runs the script in a child scope, so plain "exit" leaves the console
# open and repeat runs stack up windows in the guest.
[Environment]::Exit(0)
