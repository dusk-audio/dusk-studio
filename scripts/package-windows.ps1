# scripts/package-windows.ps1
# Build a Windows MSI via CPack WIX. Run from PowerShell on a Windows
# host with the WIX Toolset on PATH. The CMakeLists.txt CPack block
# configures the upgrade GUID + install dir; this script invokes
# cpack -G WIX. The MSI ships UNSIGNED by design (no Authenticode
# certificate) — Windows SmartScreen warns on first launch.
#
# Usage:
#   .\scripts\package-windows.ps1                  # MSI moved to repo root
#   .\scripts\package-windows.ps1 -BuildDir build  # explicit build dir
#
# Prerequisites:
#   build\ already configured + built (Release).
#   WIX Toolset >= 3.11 on PATH (light.exe, candle.exe).

[CmdletBinding()]
param (
    [string]$BuildDir = "build"
)

$ErrorActionPreference = "Stop"
$RepoDir = (Resolve-Path "$PSScriptRoot\..").Path
Set-Location $RepoDir

if (-not (Test-Path "$BuildDir\CMakeCache.txt")) {
    Write-Error "$BuildDir missing - run: cmake -S . -B `"$BuildDir`" -DCMAKE_BUILD_TYPE=Release; if (`$LASTEXITCODE -eq 0) { cmake --build `"$BuildDir`" -j6 }"
}
if (-not (Get-Command "candle.exe" -ErrorAction SilentlyContinue)) {
    Write-Error "WIX Toolset not on PATH — install from https://wixtoolset.org/"
}

# CPack consumes CMake's install rules. Validate that layout before building
# the MSI so a compiled-but-uninstalled helper cannot silently ship again.
$InstallCheckDir = Join-Path ([System.IO.Path]::GetTempPath()) `
    ("dusk-studio-install-check-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $InstallCheckDir | Out-Null
try {
    & cmake --install $BuildDir --config Release --prefix $InstallCheckDir
    if ($LASTEXITCODE -ne 0) {
        throw "cmake --install validation failed with exit $LASTEXITCODE"
    }

    foreach ($RequiredExe in @("DuskStudio.exe", "dusk-studio-plugin-host.exe")) {
        $InstalledExe = Join-Path (Join-Path $InstallCheckDir "bin") $RequiredExe
        if (-not (Test-Path -PathType Leaf $InstalledExe)) {
            throw "install layout missing required executable: $InstalledExe"
        }
    }

    # GPL section 4: the MSI must lay the license texts down on disk. The EULA
    # page cpack builds from CPACK_RESOURCE_FILE_LICENSE leaves no file behind.
    foreach ($RequiredDoc in @("LICENSE", "LICENSES.txt")) {
        $InstalledDoc = Join-Path $InstallCheckDir $RequiredDoc
        if (-not (Test-Path -PathType Leaf $InstalledDoc)) {
            throw "install layout missing required license text: $InstalledDoc"
        }
    }

    $ForbiddenDirs = @(
        (Join-Path $InstallCheckDir "include"),
        (Join-Path (Join-Path $InstallCheckDir "lib") "cmake")
    )
    foreach ($ForbiddenDir in $ForbiddenDirs) {
        if (Test-Path -PathType Container $ForbiddenDir) {
            throw "install layout contains forbidden directory: $ForbiddenDir"
        }
    }
    Write-Host "Validated install layout: app + plugin scan host + license texts; no development files"
} finally {
    Remove-Item -Recurse -Force $InstallCheckDir -ErrorAction SilentlyContinue
}

Push-Location $BuildDir
try {
    & cpack -G WIX -C Release
    if ($LASTEXITCODE -ne 0) { throw "cpack failed with exit $LASTEXITCODE" }
} finally {
    Pop-Location
}

$Msis = Get-ChildItem "$BuildDir\*.msi" -ErrorAction SilentlyContinue
if (-not $Msis) {
    Write-Error "No .msi produced — check cpack output above"
}

foreach ($Msi in $Msis) {
    Move-Item $Msi.FullName . -Force
    $LocalMsi = Join-Path (Get-Location) $Msi.Name
    Write-Host "Built (unsigned): $($Msi.Name)"

    $Hash = (Get-FileHash -Algorithm SHA256 $LocalMsi).Hash
    "$Hash  $($Msi.Name)" | Tee-Object -Append -FilePath "SHA256SUMS.windows" | Out-Host
}
