# Stands in for the msi-install phase when a run tests the package the guest
# already has (--installed): installs nothing. Finds the one Dusk Studio product
# installed for all users, requires the version the run names, and records its
# executable and the snapshot of the real profile that the isolation audit
# compares against, in a run folder of its own.
$rgVersion = '@@VERSION@@'
try {
    Stop-RegressApps
    if (Test-Path $rgRoot) { Remove-Item -Recurse -Force $rgRoot }
    New-Item -ItemType Directory -Path $rgRoot | Out-Null
    Set-Content -Path "$rgRoot\.dusk-regress-run" -Value 'dusk-regress run folder; removed by the next run'

    $found = @{}
    foreach ($view in 'Registry64', 'Registry32') {
        $uninstall = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', $view).OpenSubKey(
            'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall')
        foreach ($name in $uninstall.GetSubKeyNames()) {
            $entry = $uninstall.OpenSubKey($name)
            $location = "$($entry.GetValue('InstallLocation'))"
            # Other products' entries hold locations on drives that are gone,
            # or in quotes, which Join-Path and Combine both refuse.
            $exe = try { [IO.Path]::Combine($location, 'bin\DuskStudio.exe') } catch { '' }
            if ($location -and $exe -and [IO.File]::Exists($exe)) {
                $found[$name] = @{ Name = "$($entry.GetValue('DisplayName'))"; Version = "$($entry.GetValue('DisplayVersion'))"; Location = $location }
            }
            $entry.Close()
        }
        $uninstall.Close()
    }
    foreach ($k in $found.Keys) { $rgLog += "installed: $k '$($found[$k].Name)' $($found[$k].Version) at $($found[$k].Location)`n" }
    if ($found.Count -ne 1) { throw "expected one installed Dusk Studio, found $($found.Count)" }
    $product = @($found.Values)[0]
    $ok = $true
    if ($product.Version -ne $rgVersion) {
        $rgLog += "error: the installed version is $($product.Version), not $rgVersion`n"
        $ok = $false
    }
    if (-not $product.Location.StartsWith($env:ProgramFiles, [StringComparison]::OrdinalIgnoreCase)) {
        $rgLog += "error: installed to $($product.Location), not under $env:ProgramFiles`n"
        $ok = $false
    }
    $rgExe = [IO.Path]::Combine($product.Location, 'bin\DuskStudio.exe')
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
    $rgLog += "installed-check failed: $_`n"
}

$rgLog += "REGRESS-PHASE installed-check RESULT $rgResult`nREGRESS-PHASE installed-check END`n"
Invoke-RegressPost $rgLog

# iex runs the script in a child scope, so plain "exit" leaves the console
# open and repeat runs stack up windows in the guest.
[Environment]::Exit(0)
