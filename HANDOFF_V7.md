# HANDOFF V7 — NNUE v7 攻坚交接（2026-11-27）

> 用途：换一台电脑后按本文档原地续跑。**先读 AGENTS.md「NNUE 决策记录 / A1 三个阶段」**（决策与理由是唯一权威源），本文档只讲「现在在哪、下一步怎么点鼠标」。

## 0. 一句话现状

NNUE v7（30s 教师深标签）**探针已验证立论成功**：2s 档零败占优（0:3+7D）、5s 档 3:5+2D 占优、**m12 夹具（v4 以来挡默认启用的硬败着）已修复**。当前后台有 3 路 30s 续采（v7d/e/f）+ 1 个探针 20s 档 A/B 在跑；数据到量后训练正式 v7 → 完整判定链 → 协议发布。

## 1. 新电脑环境准备（照做，全绿后再动代码）

```powershell
git clone <repo-url> D:\gomoku; cd D:\gomoku
git checkout handoff/nnue-v7          # 本分支：含 v7 采集数据与探针产物（见 §3）
git config user.name han-yi794; git config user.email han-yi794@users.noreply.github.com
# 工具链（若已在本机装好则跳过）：D:\emsdk（7.0.0，解压 zip 版）、D:\mingw
pwsh -ExecutionPolicy Bypass -File native\build_native.ps1   # 原生 CLI（先于一切测量）
pwsh -ExecutionPolicy Bypass -File build.ps1                 # wasm（node 测试用）
node test_engine.js; node test_session_state.js; node test_loss_claims.js   # 各 <1min，须 exit 0
node test_diagonal_defense.js                                 # ~10min，19 项全 PASS
node _dbg/fixtures_replay.js 20000                            # 古典夹具，须 8/9（m8 (5,9) 为已知平局差异）
```

## 2. 代码状态（全部已提交、已推送）

- main = 7657b8b 之后的 0fc4bc9：优化后的累加器（快照恢复 + 威胁差分精确化）+ v6 内嵌权重 + 已验证 wasm。4 套古典测试全 exit 0、古典夹具 8/9 = b1-B 以来既定基线。
- **速度轴已闭合**：NNUE 吞吐达古典 97%（77k vs 74-80k nps，原 59%）；差分测试（`gomoku_cli.exe nnue-check --moves 150 --seed 7 --open 9`）零失配。
- 提交链要义：c9e8d31（v6 权重）→ afd1b8b（威胁差分精确化）→ 635e5a7（快照恢复，m12 达深度 10）→ ffcbcaa/eab376f（分层确诊 + A/B 重测）→ 7657b8b（v7 探针立论）。

## 3. 本分支强制入库的产物（平时 gitignored，仅供交接，**勿合并回 main**）

- `native/nnue/dump_v7.txt / dump_v7b.txt / dump_v7c.txt`：30s 消毒标签共 1286 行（服务重启中断幸存；后续局在 v7d/e/f 继续采，完成后再推）。
- `native/nnue/nnue_v7p.bin`：v7 探针权重（1283 行 30s 标签训练，val_mse 0.139）。
- `_dbg/mix_v7.js`：混合集生成器（读全部 dump_v7*，输出 `_dbg/mix_v7.txt`；mix_v7.txt 不入库，新机器上重跑一次生成）。`_dbg/native_fixtures_replay.js`、`_dbg/verify_drift.js`：原生夹具复现 + 协议核验（均支持 `--nnue` 任意权重路径）。

## 4. 后台任务（换机后按需重启）

```powershell
# 30s 续采（三路并行，4 核机）：
native\gomoku_cli.exe dump --games 12 --budget 30000 --open 8 --sanitize --out native/nnue/dump_v7d.txt
native\gomoku_cli.exe dump --games 12 --budget 30000 --open 8 --sanitize --out native/nnue/dump_v7e.txt
native\gomoku_cli.exe dump --games 12 --budget 30000 --open 8 --sanitize --out native/nnue/dump_v7f.txt
# 探针 20s 档 A/B（无人值守 ~3-4h，与采集并行会互相拖慢）：
native\gomoku_cli.exe match --games 10 --budget 20000 --nnue _dbg/nnue_v7p.bin
```
> dump 按局落盘，重启最多丢当前一局；数据文件 `native/nnue/*.txt` 是 gitignored，续采完记得补推本分支。

## 5. v7 正式训练与判定链（数据到 ~3000 行后执行）

```powershell
cd D:\gomoku
node _dbg/mix_v7.js                                   # 生成 _dbg/mix_v7.txt
python native/nnue/train.py --data _dbg/mix_v7.txt --out _dbg/nnue_v7.bin --iters 6000
# 快速体检（5 分钟）：
native\gomoku_cli.exe nnue-check --moves 150 --seed 7 --open 9        # mismatches 必须 0
native\gomoku_cli.exe nps 1 10000; native\gomoku_cli.exe nps 1 10000 --nnue _dbg/nnue_v7.bin
$env:NNUE_BIN="D:/gomoku/_dbg/nnue_v7.bin"; node _dbg/native_fixtures_replay.js 20000
# → 看 m12 是否仍选 (6,5)、n15  drift、m10/m11 已知轻漂移；更新 _dbg/verify_drift.js 的 CASES 新选点后跑协议核验
node _dbg/verify_drift.js
```
**通过标准（沿用协议）**：m12 必须选 (6,5) 或其新选点经黑方 20s 深搜不劣于 (6,5)；每个新漂移点都要有对手侧真值；n17 仍落败带。

然后换嵌权重并重跑 A/B 三档（2s ~20min、5s ~2h、20s ~3h，机器空闲时串行）：
```powershell
python native/nnue/export_weights_header.py --bin _dbg/nnue_v7.bin --out src/nnue_weights.h
pwsh -ExecutionPolicy Bypass -File build.ps1; pwsh -ExecutionPolicy Bypass -File native\build_native.ps1
native\gomoku_cli.exe match --games 10 --budget 2000  --nnue _dbg/nnue_v7.bin
native\gomoku_cli.exe match --games 10 --budget 5000  --nnue _dbg/nnue_v7.bin
native\gomoku_cli.exe match --games 10 --budget 20000 --nnue _dbg/nnue_v7.bin
```
全过 → 按 AGENTS.md「发布流程」升构建号 + CI + Pages；把 nnue_v7.bin 提交到 `native/nnue/`（上一版权重都在那里）。

## 6. 陷阱清单（都是血泪，逐条照做）

1. **PowerShell 变量不拆 argv**：`$H="7,7 8,7 ..."` 后 `cmd $H` 会整串当一个参数——CLI 只解析到第一对坐标，测量静默作废。含棋谱的测量**只准**字面量或 node 脚本（execSync 整串交 cmd.exe 正确分词）。
2. **`_dbg/` 与 `native/nnue/*.txt` 是 gitignored**：探针权重/数据在 `_dbg/`，正式权重才进 `native/nnue/*.bin`。
3. **wasm / native 双轨**：改了 `src/` 必须两个都重建（`build.ps1` + `native\build_native.ps1`）；夹具复现走 wasm（`nnue_fixtures_replay.js` 启用内嵌权重），原生选点用 `native_fixtures_replay.js`（`--nnue` 指定 bin）——**两者选点可能因时序/nps 差异而不同，以原生为准核验，wasm 用于发布前全链**。
4. **原生 eval 命令**：`gomoku_cli.exe eval --nnue <bin> x,y ...`（静态对照；evaluatePosition 是统一入口，NNUE 开启时读未 reset 的默认累加器出垃圾值——CLI 已处理，勿再「修」）。
5. **别边采集边跑计时敏感的夹具复现**：CPU 争抢会改选点。
6. AGENTS.md「引擎行为契约」「回归夹具」两节是硬规矩——改引擎/换评估必跑。

## 7. 已知漂移点现状（v7 探针，20s 原生）

| 夹具 | v7 探针选点 | 期望 | 真值（黑方 20s 古典深搜，低分=白方更好） | 结论 |
|---|---|---|---|---|
| n1 | (7,8) | (7,8) | — | MATCH |
| n13 | (4,10) | (4,10) | — | MATCH |
| n15 | (9,6) | (5,10) | NEW +128.7 万 vs OLD +126.4 万 | 近似平局，非硬败；新漂移点待正式 v7 复核 |
| n17 | (3,9) | 败带 | 评分 -9999783 | MATCH |
| m8 | (5,9) | (9,5) | 同分平局（与古典基线同） | 已知差异 |
| m10 | (6,9) | (6,5) | NEW -11.9 万 vs OLD -127.7 万 | 旧略好，已知轻漂移 |
| m12 | **(6,5)** | (6,5) | 选点即期望 | **已修复** |
| m13 | (6,4) | (6,4) | — | MATCH |
| m11 | (5,8) | (5,11) | NEW +90.9 万 vs OLD -75.1 万 | 旧略好，已知轻漂移 |

**趋势门（探针，10 局换先）**：2s 0:3+7D 占优 / 5s 3:5+2D 占优 / 20s 测量中。v6+快照构建对照：2s 3:3+4 和、5s 4:3+3D。

## 8. v7 之后的路线图（v7 正式版判定通过后）

- 若趋势门三档全占优 + 单点门通过 → 默认启用协议（AGENTS.md 检查单 6 步）+ 发布。
- 若仍差一点：下一杠杆是**特征工程**（当前 62 维是「我方/对方 8 棋型 ×3 区 + 行棋方 + 手数」，可加「对手潜在威胁」类——古典根级 `potentialThreatScore` 正是它缺的信号）或换更宽的网络；**再堆标签深度的边际收益已递减一次**（10s→30s 修好了关键点但 m10/m11 未动），先测再加。