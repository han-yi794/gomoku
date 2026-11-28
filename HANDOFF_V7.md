# HANDOFF V7 — NNUE v7 攻坚交接（2026-11-27）

> 用途：换一台电脑后按本文档原地续跑。**先读 AGENTS.md「NNUE 决策记录 / A1 三个阶段」**（决策与理由是唯一权威源），本文档只讲「现在在哪、下一步怎么点鼠标」。

## 0. 一句话现状

NNUE v7（30s 教师深标签）**探针已验证立论成功**：2s 档零败占优（0:3+7D）、5s 档 3:5+2D 占优、**m12 夹具（v4 以来挡默认启用的硬败着）已修复**。当前后台有 3 路 30s 续采（v7d/e/f）+ 1 个探针 20s 档 A/B 在跑；数据到量后训练正式 v7 → 完整判定链 → 协议发布。

## 1. 新电脑环境准备（照做，全绿后再动代码）

```powershell
git clone <repo-url> D:\gomoku; cd D:\gomoku
git checkout handoff/nnue-v7          # 本分支：含 v7 采集数据与探针产物（见 §3）
git config user.name han-yi794; git config user.email han-yi794@users.noreply.github.com
# 工具链（若已在本机装好则跳过；新机装法见 §6.7）：
#   - Emscripten：emsdk zip 解压到 D:\emsdk，emsdk_env.ps1 拷进 D:\emsdk\
#   - MinGW：winlibs 便携 zip，解压到 D:\mingw（见 §6.7 完整配方）
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
- `_dbg/fixtures_replay.js`：古典（wasm，NNUE 关）夹具复现——§1 引用的那个，此前漏提交（新机上手会 file not found），2026-11-27 补入库。`node _dbg/fixtures_replay.js 20000`：电池 n1/n13/n15/n17 + 第 11 局 m8/m10/m12 + 第 13 局 m12；本机实测 6/8 命中、m8 `(5,9)` 为 §7 已知平局差异（文档口径 8/9）。
- `_dbg/mix_v7.js`：2026-11-28 改为拼入 v7e/v7f，基础档缺失时跳过（配合降量重采）。
- **线上体验 NNUE（2026-11-28）**：Pages 已切到本分支；wasm 单文件已带 NNUE（`package.ps1` 修好 flag，默认关、古典路径实测逐位不变）。体验法：控制台 `Module._wasmNnueEnableEmbedded()` 返回 `1` 后点"新游戏"；关闭 `Module._wasmNnueSetEnabled(0)` 或刷新。当前内嵌权重 = v6。

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
7. **计时敏感测量的负载敏感性（2026-11-28 新发现，血泪）**：同一搜索（m12 后白 `(6,5)`、黑古典 20s）在两路采集并行时得 +253 万、单路时得 -148 万——±400 万摆动。后果：① **对手侧漂移核验（黑方分对比）必须在安静机器上跑**（采集全完之后），今天忙时跑出的非 mate 分数一律作废重测；② mate 证杀类结论鲁棒（`(7,5)` → +9999751 两次一致），可作数；③ 安静环境下每个漂移点复跑两次取稳定值再判定。
7. **新机工具链装法（2026-11-28 本机实测）**：**别再试 winlibs**——这台机器的网络把 GitHub release 资产 CDN 拉黑了（100 秒 0 字节，浏览器和 `gh release download` 同样卡死）。已验证可行路线是 MSYS2：
   - 下载 `msys2-base-x86_64-*.tar.xz`（repo.msys2.org/distrib/x86_64/，列表面页可看）：`curl.exe --ssl-no-revoke -C - --max-time 240 -sS -o D:\msys2-base.tar.xz <url>`——本网络每 ~35MB 断一次，`-C -` 续传循环约 15~20 次拿全（`.zst` 版同理，但见下一步）；
   - **Windows 自带 tar 解不了 zst/xz**（外挂 `zstd`/`xz` 程序不存在）——用 Python（本机有 3.12）：`python -c "import tarfile; tarfile.open(r'D:\msys2-base.tar.xz','r:xz').extractall(r'D:\msys2')"`；
   - 解压得到 `D:\msys2\msys64\`（msys2 的根，**别拍平**）；首跑 `& D:\msys2\msys64\usr\bin\bash.exe -lc "pacman -Sy --noconfirm mingw-w64-ucrt-x86_64-gcc"`（首跑自动初始化 keyring；包都小，断了重跑就行）；
   - `native\build_native.ps1` 已能自动找到 `D:\msys2\msys64\ucrt64\bin\g++.exe`；emsdk 仍走 zip 解压 `D:\emsdk`。
   - 两个坑：(a) g++ 的子进程 `cc1plus` 在 `lib/gcc/…` 下，依赖 DLL 靠 PATH 找——构建脚本已自动把该 bin 前置 PATH（手动编译时同样要加）；(b) `native\gomoku_cli.exe` 全静态链接、**不依赖 PATH**，可直接后台跑。

## 9. 过夜自主计划（2026-11-28 深夜 → 次日；用户睡觉中，质量优先）

**数据缺口（已确认）**：mix 的基础档 `native/nnue/dump_v2/4/6/v6deep.txt` 从未入库（gitignored，只 force-add 过 v7 探针产物），原机不可达，`mix_v7.js` 原样跑不了。适配：按 AGENTS.md 决策记录里各档的采集命令**降量重采**（`_dbg/regen_base_tiers.ps1`：v2 40 局 300ms、v4 12 局 800ms、v6 25 局 3s 消毒、v6deep 10 局 10s 消毒）——**结构不变**（浅/中/深三档齐全、深档 ×3 加权、消毒与随机开局标志与原采集一致），仅总量约为原设计的 1/2；判证质量由门禁保证，不由数据量保证。

**执行序**：① v7d/e/f 采集 + 探针 20s A/B 跑完（后台）→ ② `regen_base_tiers.ps1` 四路并行 → ③ `node _dbg/mix_v7.js` → `python native/nnue/train.py --iters 6000` → ④ `nnue-check`（mismatches 必须 0）→ nps 对比 → ⑤ 原生夹具（**m12 必须选 `(6,5)`**）→ ⑥ `verify_drift.js` 三处漂移点**对手侧古典真值**核验 → ⑦ 2s / 5s 三档 A/B（每档 10 局换先）。

**质量红线（用户指示：质量优先，逐条遵守）**：
1. 门禁一道不减：差分 0 失配、m12 夹具、漂移点逐项对手侧核验，缺一即判不过；
2. **边缘结果一律判不过**（打平/轻劣都算不过，不做"差不多"解释；每处漂移必须有黑方古典 20s 深搜分数对比）；
3. A/B 样本不缩水：每档 10 局换先；若 12h 窗口装不下 20s 档，就留到早上并如实记"20s 门未测"，**不拿 2-3 局充当门禁**；
4. 默认启用（`wasmNnueEnableEmbedded` 自动开）**绝不擅自决定**——协议第 6 步明属用户决策点；全过也只交方案；
5. 不回滚、不切回 Pages（用户正在体验本分支）、不干扰在跑的计时机作业。

**进度（2026-11-28 10:53）**：
- ✅ 采集完成：v7d/e/f = 1890/1145/1493 行 → **30s 标签总计 5814 行**（原目标 ~3000，超近 2 倍；mix 的 30s 权重因此高于原设计，结构不变）；
- ✅ 探针 20s A/B（10 局换先）：**古典 4 : 4 NNUE + 2 和**——按红线判「打平=不过」：**探针 20s 趋势门未过**（2s 档 0:3+7D 过、5s 档 3:5+2D 过；探针训练集仅 1283 行，正式 v7 用 ~5814 行重训后三档须全量重测，探针结论不作最终判定）；
- ⏭ 基础档重采（数据缺口适配）四路并行发车中。

**存档纪律（用户指示：及时存档）**：
- 每完成一段立即 `commit + push`；采集数据按现状 force-add 增量归档（本机服务曾重启中断过采集），跑满再补推完整版；
- `_dbg` 工具脚本随批次 force-add 进分支（既有惯例），本文档 §7 漂移表随手更新；
- 训练出的权重先落 `_dbg/nnue_v7.bin`，判定有结论再进 `native/nnue/` 并提交。

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

## 10. b4 落子深证（2026-11-28；用户选定主线「① 落子深证」）

**动机**：v7 判定链的结论——防「对手绝杀」的正路是经典搜索的深度/射程，不是 NNUE（NNUE 各配置在 m12 全错、古典 20s 全对）。第 11/12/13 局主败类 = b3 探针层（VCF6/VCT8）盖不住的**远距将杀**（m12 类杀手需对手 13.5s 全引擎深搜才证）。

**机制**（`src/engine.cpp`，接在 b3 块之后）：终局选定一手后，若**自评仍乐观（>-300 万）且对手盘面有实质威胁（`potentialThreatScore(对手) >= CLOSED_FOUR`）**，对选中着做一次**对手侧全引擎深搜**（预算 3/4、封顶 20s；`makeMove` 翻手番 → `findBestMove` → `undo`，按 post-scan 同法保存/恢复 `searchStartTime`/预算/节点/超时）；证出 mate（分 > WIN_THRESHOLD-1000）即拒选，走 b3 同形幸存者路径（按最近完整迭代原始分降序，取第一个 VCF/VCT 探不死的候选）。预算 <8s 跳过（探针开销占比过大，500ms 级回归天然隔离）。闸门哲学与 dump 侧标签治理 L2（`native/main_native.cpp`，ebee2c1）一致。

**实测（安静机器、古典路径、1 线程）**：

| 位置/档 | 无 b4 | 有 b4 | 判定 |
|---|---|---|---|
| game11-m12 @10s | (6,5) ms=11140 | (6,5) ms=18722 | 选点同；闸门触发 +7.6s |
| game11-m12 @20s | (6,5) ms=18105 | (6,5) ms=26871 | 选点同；闸门触发 +8.8s |
| game11-m10 @20s | (6,5) ms=32529（b3 既有成本） | (6,5) ms=48013 | 选点同；b3+b4 叠加 +15.5s |
| game12-m13 @20s | (6,4) ms=29466 | (6,4) ms=29031 | 深证跑过但无 mate，正确弃权 |
| game12-m12 @10s | (9,8) 自评 -370 万 ms=18849 | (9,8) ms=18635 | **闸门正确跳过**（自评已过 -300 万线），零开销 |
| battery-n15 @20s | — | 闸门 SKIP | 自评 -9999.8 万（引擎已见全败），正确跳过 |
| **m12verify**（白走败着 (7,5) 后黑方 15s = b4 深证预算） | — | **+9999751（mate）11.4s** | **谓词证明**：15s 深证恰好抓得住 m12 类杀手（与地面真值 +9999751 一致） |

**插桩实测（2026-11-28，诊断后已移除；方法留档 `_dbg/b4_trace.js`）**——闸门不是在"猜"，每个条件都当场看过：m12 PASS raw=-127 万 threat=460 万；m13 PASS raw=-3468 threat=1120 万；NNUE-m12 PASS raw=+60 万 threat=460 万；n15/深负视角正确 SKIP。

**⭐ 端到端拒选（NNUE v7 的 m12 盲点，7/7 复现）**：NNUE 在第 12 手自评 +60 万选**败着 (7,5)** → b4 深证 **score=10000000 KILLED** → 幸存者路径改选**正解 (6,5)**。这是 b4 立项要防的那类杀第一次被现场抓住（此前 NNUE 各配置在该点全错）。注意：NNUE 主搜极快（~4s，nodes=302117），"main+verify=18.6s"里已包含拒选——勿被平直的 ms 误读为未触发。

**成本模型（订正）**：仅「自评乐观 + 对手威胁 ≥ 冲四 + 预算 ≥8s」触发；平凡局面零成本。古典闸门位 = 主搜 + b3 探针 + 深证叠加，实测 27~48s（m10 最坏）；NNUE 闸门位 ~19s（主搜快）。困难 20s 档单步至多 ~48s，UI「关键局面更久」已声明。

**m13 刀锋**：分数显示随 ID 截断在 -3468/-9093332 间抖动（偶层视界），选点稳定 (6,4)；`bench`×3 全 9/9。

**修过的 bug（2026-11-28）**：幸存者窗口原用 b3 的「原始起点 +1.5 倍预算」，但深证后已耗时可达 2 倍预算（m10 实测 35s > 30s 窗口）→ 循环开局即过期、拒选静默失败（白烧 15s）。已改为**从当前已耗时再给半个预算**（`getElapsedMs() + g_timeBudgetMs/2`）。

**成本口径（发布承诺）**：仅「乐观+对手有威胁+预算≥8s」的局面触发；平凡局面零成本；实测触发器 +5~9s，最坏叠加 ~+15s（困难 20s 档单步至多 ~48s，UI「关键局面更久」已声明）。NNUE 关时古典路径 `nnue-check` 0 失配；`bench` 9/9（含全部败带夹具）。

**当前状态（2026-11-28 验证完毕，存档 `8a0e9b0`）**：双构建通过；`nnue-check` 0 失配；`bench`×3 全 9/9；4 套 node 测试全 exit 0；端到端拒选 7/7。插桩已移除、`engine.cpp` 与提交版逐位一致。发布：构建号升 `20261128a` → `package.ps1` → `docs/` 同步 → push → curl 核对线上。

## 11. 发布记录（2026-11-28）

- **Release v1.1.0**：https://github.com/han-yi794/gomoku/releases/tag/v1.1.0
  资产 `gomoku_portable_1.1.0.zip`（219,848 字节，SHA256 `8c1044c4…a95a69`；含 README + 字体 + .nojekyll）。
- **坑**：`gh release create` 默认把标签打在 **default 分支（main=7657b8b）**上，与产物源码不符；已删除 release+tag 后带 `--target 910ac94` 重建——发布标签必须指向产出该构建的那个提交。
- **线上 Pages**（本分支 docs）：https://han-yi794.github.io/gomoku/ 构建 `20261128a`，gomoku.js 94693 字节，与 dist 产物 SHA256 逐位一致。
- **待办**：用户体验完后把 Pages 切回 `main:/docs`（红线：切回前必须经用户确认）。