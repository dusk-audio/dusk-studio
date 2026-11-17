$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../scripts/regress/windows/source/inline-lib.ps1')
$cleanupSource = [IO.File]::ReadAllText((Join-Path $PSScriptRoot '../scripts/regress/windows/cleanup.ps1'))
$begin = $cleanupSource.IndexOf('    $rgEvidence = @()')
$end = $cleanupSource.IndexOf('    $rgBytes =', $begin)
if ($begin -lt 0 -or $end -le $begin) { throw 'Cannot locate the cleanup evidence pass' }
$evidencePass = [ScriptBlock]::Create($cleanupSource.Substring($begin, $end - $begin))
$evidenceRoot = Join-Path ([IO.Path]::GetFullPath($env:TEMP)) ('dusk-evidence-' + [Guid]::NewGuid().ToString('N'))
$deep = $evidenceRoot + '\launch\owned\' + ('nested-' * 30) + '\config'
$posts = New-Object 'Collections.Generic.List[object]'

function Invoke-RegressPost($text, $path) { $posts.Add(@{ Name = $path; Text = $text }) }
function Assert-Evidence($condition, $message) { if (-not $condition) { throw $message } }

try {
    [void](New-Object IO.DirectoryInfo ('\\?\' + $deep)).Create()
    [void][IO.Directory]::CreateDirectory($evidenceRoot + '\activation')
    $files = @{
        ($evidenceRoot + '\install.log') = 'install'
        ($evidenceRoot + '\activation\launch.log') = 'activation'
        ($deep + '\DuskStudio.log') = 'long app log'
        ($deep + '\crash-fixture.txt') = 'long crash report'
        ($deep + '\not-evidence.bin') = 'ignore'
    }
    foreach ($path in $files.Keys) { [IO.File]::WriteAllText(('\\?\' + $path), $files[$path]) }

    & {
        $rgRoot = $evidenceRoot
        $rgLeft = @()
        $rgLog = ''
        $rgEvidenceCap = 64MB
        . $evidencePass
        Assert-Evidence (-not $rgKeep -and $rgLeft.Count -eq 0) ($rgLeft -join '; ')
        Assert-Evidence ($posts.Count -eq 4) 'Cleanup missed long-path logs or collected unrelated files'
        Assert-Evidence (@($posts | Where-Object { $_.Text -ceq 'long crash report' }).Count -eq 1) 'The long-path crash report was not sent'
        Assert-Evidence (@($posts | Where-Object { $_.Name -notlike 'guest-*' -or $_.Name -match '[\\?]' }).Count -eq 0) 'Evidence names contain a long-path prefix'
    }
    & {
        $rgRoot = $evidenceRoot
        $rgLeft = @()
        $rgLog = ''
        $rgEvidenceCap = 64MB
        function Get-RegressFutureFiles { [pscustomobject]@{ Files = @(); Unlisted = @($deep) } }
        . $evidencePass
        Assert-Evidence ($rgKeep -and ($rgLeft -join '; ') -like '*could not list*') 'Cleanup discarded an unlisted evidence folder'
    }
    & {
        $rgRoot = $evidenceRoot
        $rgLeft = @()
        $rgLog = ''
        $rgEvidenceCap = 64MB
        $locked = [IO.File]::Open(('\\?\' + $deep + '\DuskStudio.log'), 'Open', 'ReadWrite', 'None')
        try {
            . $evidencePass
            Assert-Evidence ($rgKeep -and ($rgLeft -join '; ') -like '*could not read or send*') 'Cleanup discarded unreadable evidence'
        } finally { $locked.Dispose() }
    }
    & {
        $rgRoot = $evidenceRoot
        $rgLeft = @()
        $rgLog = ''
        $rgEvidenceCap = 64MB
        function Invoke-RegressPost { throw 'fixture transfer failure' }
        . $evidencePass
        Assert-Evidence ($rgKeep -and ($rgLeft -join '; ') -like '*fixture transfer failure*') 'Cleanup discarded evidence after a failed transfer'
    }
    Write-Output 'Windows cleanup evidence: long paths, unlisted folders, refused reads and failed transfers passed'
} finally {
    & cmd.exe /d /c rd /s /q ('\\?\' + $evidenceRoot)
    if (Test-Path -LiteralPath $evidenceRoot) { throw "Could not remove the evidence fixture $evidenceRoot" }
}
