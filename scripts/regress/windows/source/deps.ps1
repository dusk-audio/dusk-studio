# Mesa through the checkout's own fetch script, as CI fetches it, and a
# private Python (the guest has no system one; CI's Python-backed ctest
# entries need it). Run by update.cmd, which has set the environment.
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$root = $env:DUSK_WIN_ROOT
if (-not $root) { throw "DUSK_WIN_ROOT is not set; run this through update.cmd" }
$env:PATH = "$env:DUSK_WIN_7ZIP;" + $env:PATH

$fetch = "$root\src\scripts\fetch-windows-software-opengl.ps1"
& $fetch -Destination "$root\deps\mesa"
Get-ChildItem "$root\deps\mesa" | ForEach-Object {
  "{0} {1}" -f $_.Name, (Get-FileHash -Algorithm SHA256 $_.FullName).Hash
}

if (-not (Test-Path "$root\deps\python\tools\python.exe")) {
  # The NuGet package is the full layout, unpacked without an installer.
  $pyZip = "$root\tmp\python.3.12.10.zip"
  Invoke-WebRequest -UseBasicParsing -Uri "https://api.nuget.org/v3-flatcontainer/python/3.12.10/python.3.12.10.nupkg" -OutFile $pyZip
  New-Item -ItemType Directory -Force -Path "$root\deps\python" | Out-Null
  & tar.exe -xf $pyZip -C "$root\deps\python"
  if ($LASTEXITCODE -ne 0) { throw "tar failed $LASTEXITCODE" }
  Remove-Item $pyZip
}
& "$root\deps\python\tools\python.exe" --version
if ($LASTEXITCODE -ne 0) { throw "python failed $LASTEXITCODE" }
# The stamp marks every dependency above as present, so it is written last.
Copy-Item -Force $fetch "$root\stamps\fetch-windows-software-opengl.ps1"
exit 0
