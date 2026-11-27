# native/build_native.ps1 - build the native CLI (MinGW-w64 g++, no Emscripten)
# The engine is host-compiled: same source as the wasm build, single-threaded
# behavior must stay identical.
# Compiler discovery (first hit wins): $env:GXX, then the documented portable
# toolchains - winlibs at D:\mingw, MSYS2 UCRT at D:\msys2. New-machine recipes:
# see HANDOFF_V7.md section 6.7.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$candidates = @(@($env:GXX,
                'D:\mingw\mingw64\bin\g++.exe',
                'D:\msys2\msys64\ucrt64\bin\g++.exe',
                'D:\msys2\msys64\mingw64\bin\g++.exe',
                'D:\msys2\ucrt64\bin\g++.exe',
                'D:\msys2\mingw64\bin\g++.exe') | Where-Object { $_ -and (Test-Path $_) })
if (-not $candidates) {
    throw "g++ not found. Set `$env:GXX, or install a portable MinGW (winlibs / MSYS2). See HANDOFF_V7.md section 6.7."
}
$gcc = $candidates[0]
$gccDir = Split-Path $gcc -Parent
$env:Path = "$gccDir;$env:Path"   # g++ driver finds cc1plus & friends next to itself
Write-Host ("using g++: {0}" -f $gcc) -ForegroundColor DarkGray

& $gcc -std=c++17 -O2 -pthread -static -static-libgcc -static-libstdc++ -DGOMOKU_WITH_NNUE -I "$root\src" "$root\src\engine.cpp" "$root\src\nnue.cpp" "$root\native\main_native.cpp" -o "$root\native\gomoku_cli.exe"
if ($LASTEXITCODE -ne 0) { throw 'g++ failed' }
$exe = Get-Item "$root\native\gomoku_cli.exe"
Write-Host ("native build ready: {0} ({1} KB)" -f $exe.FullName, [math]::Round($exe.Length/1KB,1)) -ForegroundColor Green