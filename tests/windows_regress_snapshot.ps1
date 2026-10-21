param([string]$LibraryPath = (Join-Path $PSScriptRoot '../scripts/regress/windows/lib.ps1'))

. $LibraryPath
$ErrorActionPreference = 'Stop'
$snapshotRoot = Join-Path ([IO.Path]::GetFullPath((Get-Item -LiteralPath $env:TEMP).FullName)) ('dusk-regress-snapshot-' + [Guid]::NewGuid().ToString('N'))
$originalTemp = $env:TEMP
$watched = Join-Path $snapshotRoot 'watched'
$privateTemp = Join-Path $snapshotRoot 'temp'
$child = Join-Path $watched 'child'
$tempChild = Join-Path $privateTemp 'dusk-studio-owned'

function Get-RegressRealFolders { return $watched }

function Get-OwnedSnapshot {
    return @(Get-RegressSnapshot | Where-Object {
        $_.StartsWith($snapshotRoot + '\', [StringComparison]::OrdinalIgnoreCase)
    } | Sort-Object)
}

function Assert-Snapshot($condition, $message) {
    if (-not $condition) { throw $message }
}

try {
    [void][IO.Directory]::CreateDirectory($child)
    [void][IO.Directory]::CreateDirectory($tempChild)
    $env:TEMP = $privateTemp
    foreach ($dir in $child, $tempChild) {
        $null = Get-ChildItem -LiteralPath (Split-Path $dir) -Force
        $file = Join-Path $dir 'temporary'
        [IO.File]::WriteAllText($file, 'temporary')
        [IO.File]::Delete($file)
    }
    $first = Get-OwnedSnapshot
    $second = Get-OwnedSnapshot
    Assert-Snapshot ($first.Count -ge 2) 'The snapshot missed owned fixture entries'
    Assert-Snapshot (($first -join "`n") -eq ($second -join "`n")) `
        'An unchanged filesystem produced different snapshots after cold directory enumeration'

    $file = Join-Path $child 'changed'
    [IO.File]::WriteAllText($file, 'a')
    $created = Get-OwnedSnapshot
    Assert-Snapshot (@($created | Where-Object { $_.StartsWith($file + '|1|') }).Count -eq 1) `
        'The snapshot missed a created file'
    Assert-Snapshot (($created -join "`n") -ne ($second -join "`n")) `
        'The snapshot missed a real filesystem change'

    [IO.File]::WriteAllText($file, ('b' * 42))
    $modified = Get-OwnedSnapshot
    Assert-Snapshot (@($modified | Where-Object { $_.StartsWith($file + '|42|') }).Count -eq 1) `
        'The snapshot missed changed file contents'

    [IO.File]::Delete($file)
    $deleted = Get-OwnedSnapshot
    Assert-Snapshot (@($deleted | Where-Object { $_.StartsWith($file + '|') }).Count -eq 0) `
        'The snapshot retained a deleted file'

    $beforeTemp = @($deleted | Where-Object { $_.StartsWith($tempChild + '|temp|') })
    [IO.File]::WriteAllText((Join-Path $tempChild 'changed'), 'changed')
    $afterTemp = @(Get-OwnedSnapshot | Where-Object { $_.StartsWith($tempChild + '|temp|') })
    Assert-Snapshot (($beforeTemp -join "`n") -ne ($afterTemp -join "`n")) `
        'The snapshot missed a real change in a watched temporary directory'
    Write-Output 'Windows regression snapshots: 7 checks passed'
} finally {
    $env:TEMP = $originalTemp
    if (Test-Path -LiteralPath $snapshotRoot) { Remove-Item -LiteralPath $snapshotRoot -Recurse -Force }
}
