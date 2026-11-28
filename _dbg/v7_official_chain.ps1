# _dbg/v7_official_chain.ps1 - 正式 v7 判定链的机械段（mix -> train -> 差分 -> nps -> 干净夹具）
# 前置：v6deep 采集已结束（安静机器！trap 6/7），native CLI 已构建。
# 输出全部写入 %TEMP%\gomoku_v7\official_chain.log；判读由人/代理按 red line 做：
#   nnue-check mismatches 必须 0；nps 记对比值；夹具 9 项里 **m12 必须 (6,5) 或经黑方 20s 深搜不劣于**。
$cli = Join-Path $PSScriptRoot '..\native\gomoku_cli.exe'
$root = Split-Path -Parent $PSScriptRoot
$log = Join-Path $env:TEMP 'gomoku_v7\official_chain.log'
$bin = 'D:/gomoku/_dbg/nnue_v7.bin'

Set-Content $log ("=== official v7 chain " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + " ===") -Encoding utf8

'--- preflight: no collection running? ---' | Out-File $log -Append -Encoding utf8
$running = @(Get-Process gomoku_cli -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    Add-Content $log ("WARNING: " + $running.Count + " gomoku_cli still running - results will be load-distorted (trap 7)")
    Add-Content $log (($running | ForEach-Object { $_.Id }) -join ',')
} else { Add-Content $log 'quiet machine: OK' }

'--- 1) mix ---' | Out-File $log -Append -Encoding utf8
node (Join-Path $PSScriptRoot 'mix_v7.js') 2>&1 | Out-File $log -Append -Encoding utf8

'--- 2) train v7 (6000 iters) ---' | Out-File $log -Append -Encoding utf8
python (Join-Path $root 'native\nnue\train.py') --data (Join-Path $PSScriptRoot 'mix_v7.txt') --out $bin --iters 6000 2>&1 | Select-Object -Last 8 | Out-File $log -Append -Encoding utf8

'--- 3) nnue-check (mismatches must be 0) ---' | Out-File $log -Append -Encoding utf8
& $cli nnue-check --moves 150 --seed 7 --open 8 2>&1 | Out-File $log -Append -Encoding utf8

'--- 4) nps compare ---' | Out-File $log -Append -Encoding utf8
& $cli nps 1 10000 2>&1 | Out-File $log -Append -Encoding utf8
& $cli nps 1 10000 --nnue $bin 2>&1 | Out-File $log -Append -Encoding utf8

'--- 5) clean fixtures (m12 must be (6,5) or verified-not-worse) ---' | Out-File $log -Append -Encoding utf8
$env:NNUE_BIN = $bin
node (Join-Path $PSScriptRoot 'native_fixtures_replay.js') 20000 2>&1 | Out-File $log -Append -Encoding utf8

Add-Content $log ("=== chain done " + (Get-Date -Format 'HH:mm:ss') + " ===")
Write-Host ("done - see " + $log) -ForegroundColor Green