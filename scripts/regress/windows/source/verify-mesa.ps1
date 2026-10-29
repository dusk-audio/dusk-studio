# Body of the "Verify packaged Mesa selects llvmpipe" step of
# .github/workflows/windows-tests.yml, plus the files its two upload steps
# require (if-no-files-found: error). Run through mesa.cmd.
$ErrorActionPreference = 'Stop'
$root = $env:DUSK_WIN_ROOT
if (-not $root) { throw "DUSK_WIN_ROOT is not set; run this through mesa.cmd" }
$env:MESA_SOFTGL_PATH = "$root\deps\mesa"
Set-Location "$root\src"
foreach ($name in @('opengl32.dll', 'libgallium_wgl.dll')) {
  $expectedFile = Join-Path $env:MESA_SOFTGL_PATH $name
  $expected = (Get-FileHash -Algorithm SHA256 -LiteralPath $expectedFile).Hash
  foreach ($folder in @('build-tests', 'build-tests/DuskStudio_artefacts/Release')) {
    $actualFile = Join-Path $folder $name
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $actualFile).Hash
    if ($actual -ne $expected) { throw "Mesa runtime differs: $folder/$name" }
  }
}
"Mesa DLL hashes match in build-tests and DuskStudio_artefacts/Release"
& .\build-tests\dusk-windows-software-opengl-probe.exe
if ($LASTEXITCODE -ne 0) {
  throw "Windows software OpenGL probe failed (exit $LASTEXITCODE)"
}
"probe exit 0"
$fixtures = @(Get-ChildItem build-tests -Filter 'dusk-studio-*-fixture.clap')
if ($fixtures.Count -eq 0) { throw "no build-tests/dusk-studio-*-fixture.clap for the fixtures upload" }
if (-not (Test-Path build-tests/VST3)) { throw "no build-tests/VST3 for the fixtures upload" }
"fixture plug-ins: " + (($fixtures | ForEach-Object Name) -join ', ') + ", VST3/"
if (-not (Get-ChildItem build-tests/DuskStudio_artefacts/Release -ErrorAction SilentlyContinue)) {
  throw "build-tests/DuskStudio_artefacts/Release is empty for the app upload"
}
exit 0
