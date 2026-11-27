# _dbg/regen_base_tiers.ps1 - 重建 mix_v7 的基础档训练数据（2026-11-28 过夜计划）
# 背景：v6 时代的 dump_v2/v4/v6/v6deep.txt 从未入库（gitignored，只 force-add 过 v7 探针
# 产物），原机不可达 -> mix_v7.js 缺基础档。按 AGENTS.md 决策记录里各档的采集命令降量
# 重采（结构/权重不变，量为 ~1/3，适配 12h 窗口）；采集命令与原始记录一致：
#   v2   : 300ms 自对弈（浅标签；--open 是 v3 才加的，浅档保持原样 plain）
#   v4   : 800ms + --open 8（随机开局，修 v3 分布偏移）
#   v6   : 3s + --open 8 + --sanitize（A1 标签消毒）
#   v6deep: 10s + --open 8 + --sanitize（A1 深标签）
$cli = Join-Path $PSScriptRoot '..\native\gomoku_cli.exe'
if (-not (Test-Path $cli)) { throw "native CLI not found - build it first (native\build_native.ps1)" }
$root = Split-Path -Parent $PSScriptRoot
$log = Join-Path $env:TEMP 'gomoku_v7'
New-Item -ItemType Directory -Force $log | Out-Null

function Start-JobProc($name, $arglist) {
  $o = Join-Path $log "regen_$name.out.txt"
  $e = Join-Path $log "regen_$name.err.txt"
  $p = Start-Process -FilePath $cli -ArgumentList $arglist -WorkingDirectory $root -WindowStyle Hidden -RedirectStandardOutput $o -RedirectStandardError $e -PassThru
  Write-Host ("started {0} pid={1} log={2}" -f $name, $p.Id, $o) -ForegroundColor Green
}

Start-JobProc v2    @('dump','--games','40','--budget','300',  '--out','native/nnue/dump_v2.txt')
Start-JobProc v4    @('dump','--games','12','--budget','800',  '--open','8','--out','native/nnue/dump_v4.txt')
Start-JobProc v6    @('dump','--games','25','--budget','3000', '--open','8','--sanitize','--out','native/nnue/dump_v6.txt')
Start-JobProc v6deep @('dump','--games','10','--budget','10000','--open','8','--sanitize','--out','native/nnue/dump_v6deep.txt')

Write-Host ""
Write-Host "monitor: Get-Process gomoku_cli | Select Id,CPU" -ForegroundColor Cyan
Write-Host "yield:   (Get-ChildItem native/nnue/dump_v*.txt | ForEach-Object { `$_.Name + ' ' + (Get-Content `$_.FullName).Count })" -ForegroundColor Cyan
Write-Host "regen ~35/20/145/165 min (parallel); done when all four exit." -ForegroundColor DarkGray