<#
.SYNOPSIS
    Prove a published Windows artifact runs on a machine that did not build it.

.DESCRIPTION
    Extracts the MSI with 7z, checks it against packaging/contents.txt, and runs
    the app's --version. Read-only with respect to the artifact: everything goes
    into a scratch directory that is removed on exit, and nothing is installed.

    The MSI is extracted rather than installed on purpose: installing needs
    elevation, and this has to be runnable on a VM without it. That means the
    extracted layout is flat and mangled, which is why the contents check
    matches by file name on this platform.

    Coverage is narrower than the Unix script by design. The headless self-test
    leg is not run here: DUSKSTUDIO_RUN_IPC_SELFTEST hangs on Windows (#504),
    and until that is fixed a self-test leg would be a hang rather than a check.

.PARAMETER Artifact
    Path to dusk-studio-X.Y.Z-Windows-x64.msi.

.PARAMETER ExpectedVersion
    Defaults to the version in the artifact's file name, so a mismatch between
    the name and what the binary reports is itself a failure.

.EXAMPLE
    pwsh -NoProfile -File scripts/release-smoke-test.ps1 dusk-studio-0.13.3-Windows-x64.msi
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $Artifact,
    [string] $ExpectedVersion
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Continue'

$script:Failures = 0
function Write-Pass { param([string] $What) Write-Host "PASS  $What" }
function Write-Fail {
    param([string] $What)
    Write-Host "FAIL  $What"
    $script:Failures++
}

if (-not (Test-Path -LiteralPath $Artifact -PathType Leaf)) {
    Write-Error "no such artifact: $Artifact"
    exit 2
}

if (-not $ExpectedVersion) {
    # dusk-studio-0.13.3-Windows-x64.msi -> 0.13.3
    $name = [System.IO.Path]::GetFileName($Artifact)
    if ($name -match '^dusk-studio-([^-]+)-') { $ExpectedVersion = $Matches[1] }
}
if (-not $ExpectedVersion) {
    Write-Error 'could not derive a version from the artifact name'
    exit 2
}

# Without this the extract below raises CommandNotFoundException, StrictMode
# turns the $LASTEXITCODE read that follows into a terminating error, and every
# check is skipped on the way to the success message.
if (-not (Get-Command 7z -ErrorAction SilentlyContinue)) {
    Write-Error '7z is required to extract the MSI: install 7-Zip and put it on PATH'
    exit 2
}

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$contentsCheck = Join-Path $repoRoot 'scripts/verify-package-contents.sh'
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("dusk-smoke-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work -Force | Out-Null

try {
    # --- Extract ---
    $extract = Join-Path $work 'msi'
    New-Item -ItemType Directory -Path $extract -Force | Out-Null
    & 7z x -y "-o$extract" $Artifact | Out-Null
    if ($LASTEXITCODE -eq 0) {
        Write-Pass "extract: $([System.IO.Path]::GetFileName($Artifact))"
    } else {
        Write-Fail "extract: 7z returned $LASTEXITCODE"
    }

    # --- Package contents ---
    # The checker is bash; the Windows runners have it through Git for Windows.
    $bash = Get-Command bash -ErrorAction SilentlyContinue
    if ($bash -and (Test-Path -LiteralPath $contentsCheck)) {
        $out = & bash $contentsCheck windows $extract 2>&1
        if ($LASTEXITCODE -eq 0) {
            Write-Pass 'package contents'
        } else {
            Write-Fail "package contents: $($out -join ' ')"
        }
    } else {
        Write-Fail 'package contents: bash or verify-package-contents.sh is unavailable'
    }

    # --- Version ---
    # 7z flattens the MSI, so the executable is found by name rather than path.
    $exe = Get-ChildItem -Path $extract -Recurse -File |
           Where-Object { $_.Name -like '*DuskStudio.exe' } |
           Select-Object -First 1
    if ($null -eq $exe) {
        Write-Fail 'no DuskStudio.exe in the extracted MSI'
    } else {
        $reported = (& $exe.FullName --version 2>&1 | Out-String).Trim()
        if ($LASTEXITCODE -ne 0) {
            Write-Fail "--version exited $LASTEXITCODE"
        } elseif ($reported -like "*$ExpectedVersion*") {
            Write-Pass "--version reports $ExpectedVersion"
        } else {
            Write-Fail "--version reported `"$reported`", expected $ExpectedVersion"
        }
    }
}
catch {
    # A terminating error inside the try would otherwise skip every check and
    # fall through to the all-passed message with exit 0.
    Write-Fail "aborted: $($_.Exception.Message)"
}
finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ''
if ($script:Failures -eq 0) {
    Write-Host "release smoke test: all checks passed (windows, $ExpectedVersion)"
    exit 0
}
Write-Host "release smoke test: $script:Failures check(s) failed (windows, $ExpectedVersion)"
exit 1
