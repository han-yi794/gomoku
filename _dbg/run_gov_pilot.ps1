# _dbg/run_gov_pilot.ps1 - B 档标签治理试点采集（L2 深验复核）
# 4 局 10s 教师 + --open 8 + --sanitize（L1 探针 + L2 对手侧 15s 深验）。
# 产出 native/nnue/dump_gov1.txt；日志 %TEMP%\gomoku_v7\gov_pilot.*.txt
$cli = Join-Path $PSScriptRoot '..\native\gomoku_cli.exe'
if (-not (Test-Path $cli)) { throw "native CLI not found - build it first (native\build_native.ps1)" }
$root = Split-Path -Parent $PSScriptRoot
$logBase = Join-Path $env:TEMP 'gomoku_v7\gov_pilot'
$outLog = "$logBase.out.txt"
$errLog = "$logBase.err.txt"

$p = Start-Process -FilePath $cli -ArgumentList @('dump','--games','4','--budget','10000','--open','8','--sanitize','--out','native/nnue/dump_gov1.txt') -WorkingDirectory $root -WindowStyle Hidden -RedirectStandardOutput $outLog -RedirectStandardError $errLog -PassThru
Write-Host ("gov pilot started pid=" + $p.Id + " log=" + $errLog) -ForegroundColor Green
Write-Host ("monitor: Get-Content " + $errLog + " -Tail 2") -ForegroundColor Cyan