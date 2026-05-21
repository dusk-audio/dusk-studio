# scripts/package-windows.ps1
# Build a Windows MSI via CPack WIX. Run from PowerShell on a Windows
# host with the WIX Toolset on PATH. The CMakeLists.txt CPack block
# configures the upgrade GUID + install dir; this script invokes
# cpack -G WIX and optionally codesigns with signtool.
#
# Usage:
#   .\scripts\package-windows.ps1                                # MSI only
#   .\scripts\package-windows.ps1 -SigningCertThumbprint <hex>   # MSI + signtool
#
# Prerequisites:
#   build\ already configured + built (Release).
#   WIX Toolset >= 3.11 on PATH (light.exe, candle.exe).
#   For codesign: a code-signing certificate installed in the user's
#   personal store, identified by thumbprint.

[CmdletBinding()]
param (
    [string]$SigningCertThumbprint = "",
    [string]$BuildDir              = "build",
    [string]$TimestampUrl          = "http://timestamp.sectigo.com"
)

$ErrorActionPreference = "Stop"
$RepoDir = (Resolve-Path "$PSScriptRoot\..").Path
Set-Location $RepoDir

if (-not (Test-Path "$BuildDir\CMakeCache.txt")) {
    Write-Error "build/ missing — run: cmake -S . -B $BuildDir -DCMAKE_BUILD_TYPE=Release && cmake --build $BuildDir -j"
}
if (-not (Get-Command "candle.exe" -ErrorAction SilentlyContinue)) {
    Write-Error "WIX Toolset not on PATH — install from https://wixtoolset.org/"
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
    Write-Host "Built: $($Msi.Name)"

    if ($SigningCertThumbprint) {
        Write-Host "Signing $($Msi.Name) ..."
        & signtool sign /sha1 $SigningCertThumbprint /tr $TimestampUrl /td sha256 /fd sha256 $LocalMsi
        if ($LASTEXITCODE -ne 0) { throw "signtool failed" }
    } else {
        Write-Warning "No -SigningCertThumbprint supplied; MSI is unsigned (SmartScreen will warn)."
    }

    $Hash = (Get-FileHash -Algorithm SHA256 $LocalMsi).Hash
    "$Hash  $($Msi.Name)" | Tee-Object -Append -FilePath "SHA256SUMS.windows" | Out-Host
}
