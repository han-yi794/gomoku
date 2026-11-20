# native/build_native.ps1 - build the native CLI (MinGW-w64 g++, no Emscripten)
# Portable MinGW at D:\mingw (winlibs). The engine is host-compiled: same source as
# the wasm build, single-threaded behavior must stay identical.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$gcc = 'D:\mingw\mingw64\bin\g++.exe'
if (-not (Test-Path $gcc)) { throw "g++ not found at $gcc (portable MinGW). See AGENTS.md." }
$env:Path = "D:\mingw\mingw64\bin;$env:Path"

& $gcc -std=c++17 -O2 -pthread -static -static-libgcc -static-libstdc++ -I "$root\src" "$root\src\engine.cpp" "$root\native\main_native.cpp" -o "$root\native\gomoku_cli.exe"
if ($LASTEXITCODE -ne 0) { throw 'g++ failed' }
$exe = Get-Item "$root\native\gomoku_cli.exe"
Write-Host ("native build ready: {0} ({1} KB)" -f $exe.FullName, [math]::Round($exe.Length/1KB,1)) -ForegroundColor Green