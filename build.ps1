# Gomoku WASM Build Script
# Usage: 
#   PowerShell: .\build.ps1
#   Command Prompt: build.bat

# Emscripten 环境：优先用已设置的 EMSDK，否则回退到常见安装路径 D:\emsdk
if (-not $env:EMSDK -and (Test-Path 'D:\emsdk\upstream\emscripten')) { $env:EMSDK = 'D:\emsdk' }
if ($env:EMSDK) { $env:Path = "$env:EMSDK;$env:EMSDK\upstream\emscripten;$env:Path" }
if (-not $env:EMSDK) {
    Write-Host "提示：未检测到 Emscripten（EMSDK）。请安装 emsdk 后设置 `$env:EMSDK = '你的路径'；" -ForegroundColor Yellow
    Write-Host "      若只是做前端/测试开发，本仓库已附带构建产物，无需重建引擎。" -ForegroundColor Yellow
}

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
