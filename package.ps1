# Gomoku Portable Package Script
# Usage:
#   PowerShell: .\package.ps1
#
# Produces dist\ containing:
#   index.html          - self-contained page (no server needed)
#   gomoku.js           - engine with WASM embedded as base64
#
# The recipient only needs a browser: double-click dist\index.html.

$ErrorActionPreference = 'Stop'

$env:EMSDK = 'D:\emsdk'
$env:Path = "D:\emsdk;D:\emsdk\upstream\emscripten;$env:Path"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$dist = Join-Path $root 'dist'

if (-not (Test-Path (Join-Path $root 'build\gomoku.wasm'))) {
    throw 'build\gomoku.wasm not found. Run .\build.ps1 first.'
}

Write-Host "Packaging portable Gomoku..." -ForegroundColor Cyan

if (Test-Path $dist) {
    Remove-Item $dist -Recurse -Force
}
New-Item -ItemType Directory -Path $dist -Force | Out-Null

# Build a single-file engine: the WASM binary is embedded, so no fetch and no
# local web server is required (works from file://).
Push-Location $root
try {
    em++ src/engine.cpp src/main.cpp -o dist/gomoku.js -O2 -I src -sSINGLE_FILE=1
    if ($LASTEXITCODE -ne 0) { throw 'em++ failed' }
} finally {
    Pop-Location
}

# Copy the page, repointing the script tag at the portable engine and dropping
# the cache-busting query (there is no cache to bust for a local file).
# Only the dev-only locateFile override is stripped -- it points at build/ and
# would 404 for the portable engine. The engine-ready bridge right below it must
# stay: it is what makes initialization independent of script load ordering.
$html = Get-Content (Join-Path $root 'index.html') -Raw -Encoding UTF8
$html = $html -replace '(?m)^.*Keep browser-loaded engine artifacts.*\r?\n', ''
$html = $html -replace '(?s)\r?\n\s*window\.Module\.locateFile = function\(path\) \{.*?\r?\n\s*\};', ''
$html = $html -replace '<script src="build/gomoku\.js\?[^"]*"></script>', '<script src="gomoku.js"></script>'
if ($html -match 'locateFile|build/gomoku') {
    throw 'index.html rewrite incomplete: dev-only engine references remain'
}
if ($html -notmatch '__engineReadyHook') {
    throw 'index.html rewrite incomplete: engine-ready bridge was dropped'
}
Set-Content -Path (Join-Path $dist 'index.html') -Value $html -Encoding UTF8 -NoNewline

# 自托管字体随包分发（页面里用相对路径 fonts/ 引用）。
if (Test-Path (Join-Path $root 'fonts')) {
    Copy-Item (Join-Path $root 'fonts') -Destination (Join-Path $dist 'fonts') -Recurse -Force
}

# Pages runs Jekyll by default; skip it so raw files are served as-is.
Set-Content -Path (Join-Path $dist '.nojekyll') -Value '' -Encoding UTF8 -NoNewline

$engine = Get-Item (Join-Path $dist 'gomoku.js')
Write-Host ""
Write-Host "Package ready: $dist" -ForegroundColor Green
Write-Host ("  index.html  {0} bytes" -f (Get-Item (Join-Path $dist 'index.html')).Length)
Write-Host ("  gomoku.js   {0} KB (WASM embedded)" -f [math]::Round($engine.Length / 1KB, 1))
Write-Host ""
Write-Host "Share the whole dist\ folder as a zip. Recipient double-clicks index.html." -ForegroundColor Yellow