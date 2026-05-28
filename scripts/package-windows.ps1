# scripts/package-windows.ps1
# Build a Windows MSI via CPack WIX. Run from PowerShell on a Windows
# host with the WIX Toolset on PATH. The CMakeLists.txt CPack block
# configures the upgrade GUID + install dir; this script invokes
# cpack -G WIX and optionally codesigns the produced binaries with
# signtool.
#
# Usage:
#   .\scripts\package-windows.ps1                                       # MSI only, no signing
#   .\scripts\package-windows.ps1 -SigningCertThumbprint <hex>          # sign via cert in Windows store
#   .\scripts\package-windows.ps1 -SigningPfxPath <path> -SigningPfxPassword <pwd>
#                                                                         # sign via PFX file (CI path)
#
# Signing modes are mutually exclusive — PFX takes precedence when
# both sets are provided. PFX signing is the CI / GitHub Actions path
# (cert decoded from base64 secret into a temp file); thumbprint is
# the local dev path (cert installed in personal store).
#
# Prerequisites:
#   build\ already configured + built (Release).
#   WIX Toolset >= 3.11 on PATH (light.exe, candle.exe).
#   For codesign: either a PFX file on disk + its password, OR a
#   code-signing certificate installed in the user's personal store
#   identified by thumbprint.

[CmdletBinding()]
param (
    [string]$SigningCertThumbprint = "",
    [string]$SigningPfxPath        = "",
    [string]$SigningPfxPassword    = "",
    [string]$BuildDir              = "build",
    # H10: digicert is the canonical timestamp authority Microsoft
    # publishes in their Authenticode docs. Sectigo / GlobalSign are
    # acceptable secondary choices; the user can override via -TimestampUrl.
    [string]$TimestampUrl          = "http://timestamp.digicert.com"
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

# Resolve signing config once. PFX path takes precedence; falling back
# to thumbprint preserves the local-dev path so a developer with a cert
# in their personal store keeps signing the same way.
$SigningMode = "none"
if ($SigningPfxPath) {
    if (-not (Test-Path $SigningPfxPath)) {
        Write-Error "SigningPfxPath '$SigningPfxPath' does not exist."
    }
    if (-not $SigningPfxPassword) {
        Write-Error "SigningPfxPath supplied without SigningPfxPassword. Pass both or neither."
    }
    $SigningMode = "pfx"
} elseif ($SigningCertThumbprint) {
    $SigningMode = "thumbprint"
}

# H10: sign a single file via signtool. Wraps the two CLI shapes —
# /f <path> /p <pwd> for PFX, /sha1 <thumb> for store cert — behind one
# call site so the foreach below stays readable. Throws on non-zero
# signtool exit so the caller's $ErrorActionPreference catches it.
function Invoke-Signtool {
    param ([Parameter(Mandatory=$true)][string]$FilePath)

    if ($SigningMode -eq "none") {
        Write-Warning "No signing config supplied; '$FilePath' is unsigned (SmartScreen will warn)."
        return
    }
    if (-not (Get-Command "signtool.exe" -ErrorAction SilentlyContinue)) {
        Write-Error "signtool.exe not on PATH — install the Windows SDK signing tools."
    }

    Write-Host "Signing $FilePath ..."
    if ($SigningMode -eq "pfx") {
        & signtool sign /f $SigningPfxPath /p $SigningPfxPassword `
                          /tr $TimestampUrl /td sha256 /fd sha256 $FilePath
    } else {
        & signtool sign /sha1 $SigningCertThumbprint `
                          /tr $TimestampUrl /td sha256 /fd sha256 $FilePath
    }
    if ($LASTEXITCODE -ne 0) { throw "signtool failed signing '$FilePath' (exit $LASTEXITCODE)" }
}

# H10: sign every .exe the build emitted BEFORE cpack assembles the
# MSI. Order matters — cpack picks up the now-signed binaries and
# embeds them in the installer payload. Signing AFTER cpack would
# leave the EXE inside the MSI unsigned (SmartScreen warns on first
# launch even though the MSI itself is signed).
if ($SigningMode -ne "none") {
    $ArtefactDir = Join-Path $BuildDir "DuskStudio_artefacts\Release"
    if (Test-Path $ArtefactDir) {
        Get-ChildItem -Path $ArtefactDir -Filter "*.exe" | ForEach-Object {
            Invoke-Signtool -FilePath $_.FullName
        }
    } else {
        Write-Warning "Artefact dir '$ArtefactDir' not found; skipping pre-cpack EXE signing."
    }
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

    Invoke-Signtool -FilePath $LocalMsi

    $Hash = (Get-FileHash -Algorithm SHA256 $LocalMsi).Hash
    "$Hash  $($Msi.Name)" | Tee-Object -Append -FilePath "SHA256SUMS.windows" | Out-Host
}
