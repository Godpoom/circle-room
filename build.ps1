# Builds CircleRoom.exe into .\bin
# Ninja/MinGW cannot handle non-ASCII (Thai) folder names, so the build goes
# through an ASCII-only junction in %TEMP% that points back at this folder.
$ErrorActionPreference = 'Stop'
$src = $PSScriptRoot
$link = Join-Path $env:TEMP 'darkroom_src'
$build = Join-Path $env:TEMP 'darkroom_build'

if (Test-Path $link) {
  $target = (Get-Item $link).Target
  if ($target -is [array]) { $target = $target[0] }
  if ($target -ne $src) { cmd /c rmdir "$link" | Out-Null }  # removes only the junction itself
}
if (-not (Test-Path $link)) { New-Item -ItemType Junction -Path $link -Target $src | Out-Null }

cmake -S $link -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
cmake --build $build -j
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }

$bin = Join-Path $src 'bin'
New-Item -ItemType Directory -Force $bin | Out-Null
Copy-Item (Join-Path $build 'CircleRoom.exe') $bin -Force
# AI runtime + models (only copied when newer: ~120 MB)
$aiSrc = Join-Path $src 'ai'
if (Test-Path $aiSrc) {
  robocopy $aiSrc (Join-Path $bin 'ai') /XO /NJH /NJS /NFL /NDL | Out-Null
  if ($LASTEXITCODE -ge 8) { throw 'Copying ai\ failed' }
  $global:LASTEXITCODE = 0
}
Write-Host "OK -> $bin\CircleRoom.exe"
