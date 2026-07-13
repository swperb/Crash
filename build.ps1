# Configure + build Crash (Phase 0 spike) with the VS 2022 bundled CMake/Ninja.
$ErrorActionPreference = "Stop"
$vs = "C:\Program Files\Microsoft Visual Studio\2022\Community"
$cmake = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

# Import the MSVC x64 environment (cl, link, headers, libs) into this session.
$vcvars = "$vs\VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" && set" | ForEach-Object {
    if ($_ -match "^(.*?)=(.*)$") { Set-Item -Path "Env:$($matches[1])" -Value $matches[2] }
}

$root = $PSScriptRoot
$build = Join-Path $root "build"

& $cmake -S $root -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw "configure failed" }

& $cmake --build $build --config Release
if ($LASTEXITCODE -ne 0) { throw "build failed" }

Write-Host "`nBuilt: $build\crash.exe" -ForegroundColor Green
