# Install phase: puts the installer under test into Program Files the way a
# user does, through msiexec and its UAC prompt, then proves what landed: the
# product the Windows Installer now lists, its version, that it is installed
# per machine, every file of the package byte for byte at the path its tables
# name, the all-users shortcuts, the HKLM uninstall entry, and the installed
# executable's own --version.
#
# UAC is never answered from here. When the prompt is up this phase posts a
# REGRESS-UAC line and waits, bounded, for a person at the VM console.
#
# Mode @@INSTALLMODE@@: install reuses an install of this very package
# (same ProductCode) and otherwise installs over whatever is there; reinstall
# removes this package and any per-user install first; extract unpacks it with
# an administrative install into the run folder instead, which needs no
# elevation.
$rgMode = '@@INSTALLMODE@@'
$rgVersion = '@@VERSION@@'
$rgUacWait = @@UACWAIT@@
$rgNotes = @()

function Get-MsiRows($db, $query) {
    $rows = @()
    $view = $db.GetType().InvokeMember('OpenView', 'InvokeMethod', $null, $db, @($query))
    [void]$view.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $view, $null)
    while ($true) {
        $rec = $view.GetType().InvokeMember('Fetch', 'InvokeMethod', $null, $view, $null)
        if ($null -eq $rec) { break }
        $n = $rec.GetType().InvokeMember('FieldCount', 'GetProperty', $null, $rec, $null)
        $fields = @()
        for ($i = 1; $i -le $n; $i++) {
            $fields += $rec.GetType().InvokeMember('StringData', 'GetProperty', $null, $rec, @($i))
        }
        $rows += , $fields
    }
    [void]$view.GetType().InvokeMember('Close', 'InvokeMethod', $null, $view, $null)
    return , $rows
}

function Get-ProductInfo($code, $prop) {
    try { return $rgInstaller.GetType().InvokeMember('ProductInfo', 'GetProperty', $null, $rgInstaller, @($code, $prop)) }
    catch { return '' }
}

function Get-RelatedProducts($upgradeCode) {
    $codes = @()
    $related = $rgInstaller.GetType().InvokeMember('RelatedProducts', 'GetProperty', $null, $rgInstaller, @($upgradeCode))
    foreach ($c in $related) { $codes += [string]$c }
    return , $codes
}

# A DefaultDir is "[target:]source" and each side "short|long".
function Get-MsiLongName($defaultDir) {
    $name = ($defaultDir -split ':')[0]
    if ($name -match '\|') { $name = ($name -split '\|')[1] }
    return $name
}

function Resolve-MsiDir($id) {
    if ($rgAnchors.ContainsKey($id)) { return $rgAnchors[$id] }
    $row = $rgDirs[$id]
    if (-not $row) { throw "the Directory table has no $id" }
    $parent = Resolve-MsiDir $row[1]
    $name = Get-MsiLongName $row[2]
    if ($name -eq '.') { return $parent }
    return (Join-Path $parent $name)
}

# Returns the msiexec exit code, or TIMEOUT when nobody answered in time.
# Windows withdraws an unanswered UAC prompt after about two minutes, and
# msiexec reports that as 1602, the same code a No gets. A prompt that stayed
# up that long expired rather than being refused, so the install is asked for
# again until the overall wait runs out; one closed sooner was answered.
function Invoke-RegressMsiexec($label, $arguments) {
    $until = (Get-Date).AddSeconds($rgUacWait)
    $attempt = 0
    while ($true) {
        $attempt++
        $p = Start-Process msiexec.exe -ArgumentList $arguments -PassThru
        # Start-Process only keeps the exit code if the handle is opened early.
        $null = $p.Handle
        $shownAt = $null
        $shownFor = 0
        while (-not $p.HasExited -and (Get-Date) -lt $until) {
            $consent = Get-Process consent -ErrorAction SilentlyContinue
            if ($consent -and -not $shownAt) {
                $shownAt = Get-Date
                Invoke-RegressPost "REGRESS-UAC $label prompt-up $attempt`n"
            } elseif ($shownAt -and -not $consent -and $shownFor -eq 0) {
                $shownFor = [int]((Get-Date) - $shownAt).TotalSeconds
                Invoke-RegressPost "REGRESS-UAC $label closed $attempt after ${shownFor}s`n"
            }
            Start-Sleep -Seconds 1
        }
        # Withdrawing our own request is not answering the prompt, and an
        # install nobody is watching must not complete later on its own.
        if (-not $p.HasExited) {
            Invoke-RegressPost "REGRESS-UAC $label timed-out after ${rgUacWait}s`n"
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
            $script:rgLog += "${label}: msiexec still running after ${rgUacWait}s, attempt $attempt; stopped it`n"
            return 'TIMEOUT'
        }
        $p.WaitForExit()
        $code = $p.ExitCode
        if ($shownAt -and $shownFor -eq 0) { $shownFor = [int]((Get-Date) - $shownAt).TotalSeconds }
        $script:rgLog += "${label}: attempt $attempt msiexec exit=$code uacShown=$([bool]$shownAt) for ${shownFor}s`n"
        if ($code -eq 1602 -and $shownFor -ge 100 -and (Get-Date) -lt $until) {
            Invoke-RegressPost "REGRESS-UAC $label expired $attempt`n"
            continue
        }
        return $code
    }
}

function Test-MsiexecSucceeded($label, $code, $logPath) {
    if ($code -eq 0 -or $code -eq 3010) { return $true }
    $why = switch ($code) {
        1602 { 'cancelled at the UAC prompt or the installer UI' }
        1603 { 'fatal error during installation' }
        1618 { 'another installation is already in progress' }
        1625 { 'blocked by system policy' }
        1638 { 'another version of this product is already installed' }
        default { 'see the log' }
    }
    $script:rgLog += "${label} failed: $code ($why)`n"
    if (Test-Path $logPath) {
        $tail = Get-Content $logPath -ErrorAction SilentlyContinue |
            Select-String -Pattern 'Return value 3|Error \d{4}|error code' | Select-Object -First 10
        $script:rgLog += (($tail | ForEach-Object { '  ' + $_.Line.Trim() }) -join "`n") + "`n"
    }
    return $false
}

$rgInstaller = $null
try {
    Stop-RegressApps

    # Earlier runs' folders, and only those: each carries the marker file.
    Get-ChildItem $env:LOCALAPPDATA -Directory -Filter 'dusk-regress-*' -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -ne $rgRoot -and (Test-Path (Join-Path $_.FullName '.dusk-regress-run')) } |
        ForEach-Object { Remove-Item -Recurse -Force $_.FullName -ErrorAction SilentlyContinue }
    if (Test-Path $rgRoot) { Remove-Item -Recurse -Force $rgRoot }
    New-Item -ItemType Directory -Path "$rgRoot\regress-session" | Out-Null
    Set-Content -Path "$rgRoot\.dusk-regress-run" -Value 'dusk-regress run folder; removed by the next run'

    $rgMsi = "$rgRoot\@@MSI@@"
    Get-RegressFile '@@MSI@@' $rgMsi
    Get-RegressFile 'manifest.tsv' "$rgRoot\manifest.tsv"
    Get-RegressFile 'session.json' "$rgRoot\regress-session\session.json"
    Get-RegressFile 'handoff.json' "$rgRoot\regress-session\handoff.json"
    Get-RegressFile 'fixtures.zip' "$rgRoot\fixtures.zip"
    Expand-Archive -Path "$rgRoot\fixtures.zip" -DestinationPath "$rgRoot\fixtures" -Force
    foreach ($d in 'build-tests', 'tests-fixtures') {
        New-Item -ItemType Directory -Force -Path "$rgRoot\fixtures\$d" | Out-Null
    }
    $rgLog += "run folder: $rgRoot`n"

    $rgInstaller = New-Object -ComObject WindowsInstaller.Installer
    $db = $rgInstaller.GetType().InvokeMember('OpenDatabase', 'InvokeMethod', $null, $rgInstaller, @($rgMsi, 0))
    $props = @{}
    foreach ($r in (Get-MsiRows $db 'SELECT Property,Value FROM Property')) { $props[$r[0]] = $r[1] }
    $rgDirs = @{}
    foreach ($r in (Get-MsiRows $db 'SELECT Directory,Directory_Parent,DefaultDir FROM Directory')) { $rgDirs[$r[0]] = $r }
    $components = @{}
    foreach ($r in (Get-MsiRows $db 'SELECT Component,Directory_ FROM Component')) { $components[$r[0]] = $r[1] }
    $files = Get-MsiRows $db 'SELECT File,FileName,Component_ FROM File'
    $shortcuts = Get-MsiRows $db 'SELECT Shortcut,Directory_,Name,Target FROM Shortcut'
    [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($db)
    $db = $null

    $code = $props['ProductCode']
    $upgrade = $props['UpgradeCode']
    $rgLog += "package: $($props['ProductName']) $($props['ProductVersion']) product=$code upgrade=$upgrade ALLUSERS=$($props['ALLUSERS'])`n"
    $ok = $true
    if ($props['ProductVersion'] -ne $rgVersion) {
        $rgLog += "error: the package says ProductVersion $($props['ProductVersion']), its file name says $rgVersion`n"
        $ok = $false
    }

    $before = Get-RelatedProducts $upgrade
    $rgPerUser = @()
    foreach ($c in $before) {
        $a = Get-ProductInfo $c 'AssignmentType'
        if ($a -eq '0') { $rgPerUser += $c }
        $rgLog += "installed before: $c $(Get-ProductInfo $c 'VersionString') at $(Get-ProductInfo $c 'InstallLocation') assignment=$a`n"
    }
    if ($before.Count -eq 0) { $rgLog += "installed before: nothing`n" }

    if ($rgMode -eq 'extract') {
        $target = "$rgRoot\extract"
        $log = "$rgRoot\msi-extract.log"
        $rc = Invoke-RegressMsiexec 'extract' @('/a', "`"$rgMsi`"", '/qn', "TARGETDIR=`"$target`"", '/l*v', "`"$log`"")
        if (-not (Test-MsiexecSucceeded 'extract' $rc $log)) { throw 'administrative install failed' }
        $rgAnchors = @{ TARGETDIR = $target }
        $rgNotes += "extracted with msiexec /a, not installed"
    } else {
        $installed = $before -contains $code
        if (($rgMode -eq 'reinstall' -or -not $installed) -and $props['ALLUSERS'] -ne '1') {
            throw "the package is not per machine (ALLUSERS='$($props['ALLUSERS'])'); the gate needs a per-machine MSI"
        }
        $rgRemoved = @()
        if ($rgMode -eq 'reinstall' -and $installed) {
            $log = "$rgRoot\msi-uninstall.log"
            $rc = Invoke-RegressMsiexec 'uninstall' @('/x', $code, '/passive', '/norestart', '/l*v', "`"$log`"")
            if (-not (Test-MsiexecSucceeded 'uninstall' $rc $log)) { throw 'uninstall before the reinstall failed' }
            $installed = $false
            $rgRemoved += $code
        }
        # Windows Installer never upgrades a per-user product to a per-machine
        # one, and the package refuses to install beside it.
        foreach ($c in $rgPerUser) {
            if ($rgRemoved -contains $c -or $c -eq $code) { continue }
            $v = Get-ProductInfo $c 'VersionString'
            if ($rgMode -ne 'reinstall') {
                $rgNotes += "pass --reinstall to uninstall the per-user $v first; that is one more UAC prompt"
                throw "a per-user install of $v ($c) is present, which this per-machine package cannot upgrade"
            }
            $log = "$rgRoot\msi-uninstall-per-user-$v.log"
            $rc = Invoke-RegressMsiexec "uninstall-per-user-$v" @('/x', $c, '/passive', '/norestart', '/l*v', "`"$log`"")
            if (-not (Test-MsiexecSucceeded "uninstall of the per-user $v" $rc $log)) { throw "could not remove the per-user $v" }
            $rgRemoved += $c
        }
        # The package's major upgrade replaces any older or same-version build,
        # but refuses a newer one: that has to go first.
        foreach ($c in $before) {
            if ($c -eq $code -or $rgRemoved -contains $c) { continue }
            $v = Get-ProductInfo $c 'VersionString'
            if ([version]$v -gt [version]$rgVersion) {
                $log = "$rgRoot\msi-uninstall-$v.log"
                $rc = Invoke-RegressMsiexec "uninstall-$v" @('/x', $c, '/passive', '/norestart', '/l*v', "`"$log`"")
                if (-not (Test-MsiexecSucceeded "uninstall of $v" $rc $log)) { throw "could not remove the newer $v" }
            }
        }
        if ($installed) {
            $rgNotes += "this package was already installed, so it was reused, not reinstalled"
        } else {
            $log = "$rgRoot\msi-install.log"
            $rc = Invoke-RegressMsiexec 'install' @('/i', "`"$rgMsi`"", '/passive', '/norestart', '/l*v', "`"$log`"")
            if (-not (Test-MsiexecSucceeded 'install' $rc $log)) { throw 'msiexec did not install the package' }
            if ($rc -eq 3010) { $rgNotes += 'installed; Windows asks for a restart (3010)' }
            else { $rgNotes += 'installed by msiexec' }
        }

        $after = Get-RelatedProducts $upgrade
        $rgLog += "installed after: $($after -join ', ')`n"
        if ($after.Count -ne 1 -or $after[0] -ne $code) {
            $rgLog += "error: expected exactly $code installed for this upgrade code`n"
            $ok = $false
        }
        $installedVersion = Get-ProductInfo $code 'VersionString'
        $location = Get-ProductInfo $code 'InstallLocation'
        $assignment = Get-ProductInfo $code 'AssignmentType'
        $rgLog += "installed version=$installedVersion location=$location assignment=$assignment (0 per-user, 1 per-machine)`n"
        if ($assignment -ne '1') {
            $rgLog += "error: the product is installed per user (AssignmentType '$assignment'), not per machine`n"
            $ok = $false
            if ($installed) { $rgNotes += "the reused install is per user; pass --reinstall with a per-machine MSI" }
        }
        $rgArp = $null
        foreach ($regView in 'Registry64', 'Registry32') {
            $rgArp = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', $regView).OpenSubKey(
                "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\$code")
            if ($null -ne $rgArp) { break }
        }
        if ($null -eq $rgArp) {
            $rgLog += "error: no uninstall entry for $code under HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall`n"
            $ok = $false
        } else {
            $rgLog += "uninstall entry: $($rgArp.Name) '$($rgArp.GetValue('DisplayName'))' $($rgArp.GetValue('DisplayVersion'))`n"
            $rgArp.Close()
        }
        if ($installedVersion -ne $rgVersion) {
            $rgLog += "error: the Windows Installer lists version '$installedVersion', expected $rgVersion`n"
            $ok = $false
        }
        if (-not $location) { throw 'the installed product has no InstallLocation' }
        $rgAnchors = @{ INSTALL_ROOT = $location.TrimEnd('\') }
        if (-not $location.StartsWith($env:ProgramFiles, [StringComparison]::OrdinalIgnoreCase)) {
            $rgLog += "error: installed to $location, not under $env:ProgramFiles`n"
            $ok = $false
        }
    }

    $manifest = @{}
    foreach ($line in Get-Content "$rgRoot\manifest.tsv") {
        $parts = $line -split "`t"
        if ($parts.Count -eq 2) { $manifest[$parts[0]] = $parts[1].ToUpperInvariant() }
    }
    $expected = @{}
    foreach ($f in $files) {
        $path = Join-Path (Resolve-MsiDir $components[$f[2]]) (Get-MsiLongName $f[1])
        $expected[$path.ToLowerInvariant()] = $true
        if (-not (Test-Path -LiteralPath $path)) {
            $rgLog += "error: missing $path`n"
            $ok = $false
            continue
        }
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash
        if (-not $manifest.ContainsKey($f[0])) {
            $rgLog += "error: $($f[0]) is in the package but not in the host's manifest`n"
            $ok = $false
        } elseif ($manifest[$f[0]] -ne $hash) {
            $rgLog += "error: $path differs from the package's $($f[0])`n"
            $ok = $false
        } else {
            $rgLog += "file ok: $path`n"
        }
        if ((Get-MsiLongName $f[1]) -eq 'DuskStudio.exe') { $rgExe = $path }
    }
    $root = Resolve-MsiDir 'INSTALL_ROOT'
    Get-ChildItem -LiteralPath $root -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object {
        if (-not $expected.ContainsKey($_.FullName.ToLowerInvariant())) {
            $rgLog += "error: $($_.FullName) is not part of the package`n"
            $ok = $false
        }
    }
    if (-not $rgExe) { throw 'the package installs no DuskStudio.exe' }
    $hostExe = Join-Path (Split-Path $rgExe) 'dusk-studio-plugin-host.exe'
    if (-not (Test-Path -LiteralPath $hostExe)) {
        $rgLog += "error: no dusk-studio-plugin-host.exe beside DuskStudio.exe`n"
        $ok = $false
    }

    if ($rgMode -ne 'extract') {
        $shell = New-Object -ComObject WScript.Shell
        foreach ($s in $shortcuts) {
            $name = (Get-MsiLongName $s[2]) + '.lnk'
            if ($s[1] -eq 'DesktopFolder') {
                $found = Join-Path ([Environment]::GetFolderPath('CommonDesktopDirectory')) $name
                $perUserLnk = Join-Path ([Environment]::GetFolderPath('Desktop')) $name
            } else {
                $sub = Get-MsiLongName $rgDirs[$s[1]][2]
                $found = Join-Path (Join-Path ([Environment]::GetFolderPath('CommonPrograms')) $sub) $name
                $perUserLnk = Join-Path (Join-Path ([Environment]::GetFolderPath('Programs')) $sub) $name
            }
            if (-not (Test-Path -LiteralPath $found)) {
                $rgLog += "error: no all-users shortcut $found"
                if (Test-Path -LiteralPath $perUserLnk) { $rgLog += "; there is a per-user one at $perUserLnk" }
                $rgLog += "`n"
                $ok = $false
                continue
            }
            $target = $shell.CreateShortcut($found).TargetPath
            if ($s[3] -like '`[#*' -and $target -ne $rgExe) {
                $rgLog += "error: $found points at '$target', not $rgExe`n"
                $ok = $false
            } else {
                $rgLog += "shortcut ok: $found -> $target`n"
            }
        }
    }

    $fileVersion = (Get-Item -LiteralPath $rgExe).VersionInfo.FileVersion
    $rgLog += "DuskStudio.exe FileVersion=$fileVersion`n"
    if (-not "$fileVersion".StartsWith($rgVersion)) {
        $rgLog += "error: FileVersion '$fileVersion' is not $rgVersion`n"
        $ok = $false
    }
    $rgCode = Invoke-RegressApp 'version' (New-RegressSandbox 'version') '--version' 60
    $rgReported = Get-Content "$rgRoot\version.log" -Raw
    $rgLog += "--version exit=${rgCode}: $(($rgReported -split "`n")[0].Trim())`n"
    if ($rgCode -ne 0 -or $rgReported -notmatch [regex]::Escape($rgVersion)) {
        $rgLog += "error: --version did not report $rgVersion`n"
        $ok = $false
    }

    Set-Content -Path "$rgRoot\installed-exe.txt" -Value $rgExe
    Set-Content -Encoding UTF8 -Path "$rgRoot\real-before.txt" -Value (Get-RegressSnapshot)
    $rgLog += "executable under test: $rgExe`n"
    if ($ok) { $rgResult = 'PASS' }
} catch {
    $rgLog += "msi-install failed: $_`n"
} finally {
    if ($null -ne $rgInstaller) { [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($rgInstaller) }
}

foreach ($n in $rgNotes) { $rgLog += "REGRESS-NOTE $n`n" }
$rgLog += "REGRESS-PHASE msi-install RESULT $rgResult`nREGRESS-PHASE msi-install END`n"
Invoke-RegressPost $rgLog

# iex runs the script in a child scope, so plain "exit" leaves the console
# open and repeat runs stack up windows in the guest.
[Environment]::Exit(0)
