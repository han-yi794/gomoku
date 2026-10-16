# Gomoku WASM Build Script
# Usage: 
#   PowerShell: .\build.ps1
#   Command Prompt: build.bat

# Set Emscripten environment
$env:EMSDK = 'D:\emsdk'
$env:Path = "D:\emsdk;D:\emsdk\upstream\emscripten;$env:Path"

# Clean build directory
if (Test-Path 'build') {
    Remove-Item 'build\*' -Force -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Path 'build' -Force | Out-Null

Write-Host "Building Gomoku WASM..." -ForegroundColor Cyan

# Compile
em++ src/engine.cpp src/main.cpp -o build/gomoku.js -O2 -I src

if ($LASTEXITCODE -eq 0) {
    Write-Host "Build successful!" -ForegroundColor Green
    Write-Host "Output: build/gomoku.js + build/gomoku.wasm" -ForegroundColor Yellow
    
    # Show file sizes
    $wasm = Get-Item 'build\gomoku.wasm'
    $js = Get-Item 'build\gomoku.js'
    Write-Host ("WASM size: {0} bytes" -f $wasm.Length)
    Write-Host ("JS size:   {0} bytes" -f $js.Length)
    
    Write-Host ""
    Write-Host "To test: node test_engine.js" -ForegroundColor Cyan
    Write-Host "To play: Open index.html in a browser" -ForegroundColor Cyan
} else {
    Write-Host "Build failed!" -ForegroundColor Red
    exit 1
}
