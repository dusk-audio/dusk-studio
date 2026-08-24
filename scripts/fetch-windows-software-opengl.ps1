# Fetch the exact Mesa for Windows binaries used by the MSI's deterministic
# llvmpipe fallback. The release workflow passes the resulting directory to
# CMake as DUSKSTUDIO_WINDOWS_SOFTWARE_OPENGL_DIR.

[CmdletBinding()]
param (
    [Parameter(Mandatory = $true)]
    [string]$Destination
)

$ErrorActionPreference = "Stop"
$MesaVersion = "26.2.0"
$ArchiveName = "mesa3d-$MesaVersion-release-msvc.7z"
$ArchiveUrl = "https://github.com/pal1000/mesa-dist-win/releases/download/$MesaVersion/$ArchiveName"
$ArchiveSha256 = "dcb2719ef346dab5b609fcb193a5f13cfc4b0502e3f4de1ad43d349477402f47"
$RequiredFiles = @{
    "opengl32.dll" = "33b217ed7947b48684baa987914475898a2b4d7d64cce96b078216c67a633582"
    "libgallium_wgl.dll" = "1a2e49cd5fdb1a857d98117ab04240d723b57da5dffe6d07f5386f42014557c1"
}

$SevenZip = Get-Command "7z.exe" -ErrorAction SilentlyContinue
if (-not $SevenZip) {
    $SevenZip = Get-Command "7z" -ErrorAction SilentlyContinue
}
if (-not $SevenZip) {
    throw "7-Zip is required to extract $ArchiveName"
}

$Scratch = Join-Path ([System.IO.Path]::GetTempPath()) `
    ("dusk-studio-mesa-" + [guid]::NewGuid().ToString("N"))
$Archive = Join-Path $Scratch $ArchiveName
$Extracted = Join-Path $Scratch "extracted"
New-Item -ItemType Directory -Path $Scratch | Out-Null

try {
    Write-Host "Downloading pinned Mesa $MesaVersion software renderer..."
    for ($Attempt = 1; $Attempt -le 3; $Attempt++) {
        try {
            Invoke-WebRequest -UseBasicParsing -Uri $ArchiveUrl -OutFile $Archive
            break
        } catch {
            if ($Attempt -eq 3) {
                throw
            }
            Write-Warning "Mesa download attempt $Attempt failed; retrying in 5 seconds"
            Start-Sleep -Seconds 5
        }
    }

    $ActualArchiveHash = (Get-FileHash -Algorithm SHA256 $Archive).Hash.ToLowerInvariant()
    if ($ActualArchiveHash -ne $ArchiveSha256) {
        throw "Mesa archive SHA256 mismatch: expected $ArchiveSha256, got $ActualArchiveHash"
    }

    & $SevenZip.Source x -y "-o$Extracted" $Archive `
        "x64\opengl32.dll" "x64\libgallium_wgl.dll" | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "7-Zip failed to extract $ArchiveName (exit $LASTEXITCODE)"
    }

    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    foreach ($Entry in $RequiredFiles.GetEnumerator()) {
        $Source = Join-Path (Join-Path $Extracted "x64") $Entry.Key
        if (-not (Test-Path -PathType Leaf $Source)) {
            throw "Mesa archive is missing x64\$($Entry.Key)"
        }
        $ActualFileHash = (Get-FileHash -Algorithm SHA256 $Source).Hash.ToLowerInvariant()
        if ($ActualFileHash -ne $Entry.Value) {
            throw "$($Entry.Key) SHA256 mismatch: expected $($Entry.Value), got $ActualFileHash"
        }
        Copy-Item $Source (Join-Path $Destination $Entry.Key) -Force
    }

    Write-Host "Mesa $MesaVersion llvmpipe payload ready: $Destination"
} finally {
    Remove-Item -Recurse -Force $Scratch -ErrorAction SilentlyContinue
}
