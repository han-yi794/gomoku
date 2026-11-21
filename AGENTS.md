# AGENTS.md — 五子棋 AI（Gomoku AI）

给在本仓库工作的编码代理的说明。项目文档与工程笔记见 `README.md`。

## 环境与基本事实

- **Windows + PowerShell**。任何会输出中文的命令前必须加：
  `chcp 65001 > $null; [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false);`
  （否则中文变乱码；`node` 自身的 stdout 不受影响，但 PowerShell 渲染的一切受影响。）
- **Emscripten**：构建脚本优先 `$env:EMSDK`，回退 `D:\emsdk`；未检测到时脚本打印提示。
- **PowerShell 版本**：仓库 .ps1 均为 UTF-8 无 BOM——PS 5.1 按 ANSI 读取会把中文串读炸（解析错误），本机用 `pwsh`（7+）跑 `build.ps1` / `package.ps1`。
- **新机装 emsdk 的坑（2026-11-17）**：某 Windows 环境里 `emsdk install` 的逐个 move 对同一文件确定性 `WinError 5`（解压本身没问题）——绕法：`zipfile` 手动解包（strip zip 里的 `install/` 前缀 → `D:\emsdk\upstream\`，再写 `.emsdk_version` 内容为工具名），然后 `emsdk activate latest` 即认账。
- **git 身份**已在仓库局部配置（han-yi794），直接 `git commit` 即可。
- **网络不稳**：命令失败先重试（TLS reset 常见）；探测用普通 `curl.exe -sS`（`-4`/部分 raw 主机可能因 schannel 报 TLS 错）。
- 本仓库**即发布仓库**：`han-yi794/gomoku`（public，Apache-2.0）。`main` = 源码树 + `docs/`（GitHub Pages 服务目录，Pages 配置 = `main:/docs`）。试玩 URL：根、`/v1030/`、`/v1018/`。
- 旧的 `D:\gomoku-pages` 已退役（内容已并入本仓库，**不要再推送它**）。
- `build/` 构建产物**随仓库提交**（克隆即开发）；重建后请把 `build/gomoku.js`、`build/gomoku.wasm` 一起提交。

## 常用命令

- 构建（改引擎后）：`./build.ps1`
- 打包便携版（单文件，`file://` 可玩）：`./package.ps1` → `dist/`
- 测试（先 build；全部 exit 0 才算绿）：
  - `node test_engine.js`
  - `node test_diagonal_defense.js`（19 项行为回归 = 发布底线；20s 档下约 10 分钟级）
  - `node test_loss_claims.js`（「全证败」抽查 T1/T2/T4 + 信息项；约 2 分钟）
  - `node test_session_state.js`（会话隔离）
  - CI：每次 push / PR 自动跑以上 4 套（`.github/workflows/ci.yml`，test-fast + test-regression 两个 job）

## 发布流程（GitHub Pages）

1. `./package.ps1`
2. 同步 `dist\{index.html, gomoku.js, fonts, .nojekyll}` → `docs/`
3. 若构建号变化，重生成子版本页（保持历史引擎不动）：
   - `docs/v1030/index.html`：把 `'<buildid>'` → `'<buildid>（引擎 10.30）'`
   - `docs/v1018/index.html`：把 `'<buildid>'` → `'<buildid>（引擎 10.18）'`
   - 两者同时把 `url('fonts/` → `url('../fonts/`
4. `git add -A && git commit && git push origin main`
5. 等 Pages：轮询 `gh api repos/han-yi794/gomoku/pages/builds/latest`（commit=刚推 sha 且 status=built）
6. 核对线上：逐字节对照（`curl.exe -sS -o NUL -w "%{http_code} %{size_download}"`）+ 真浏览器冒烟（build 号、字体、落子、控制台 0 错）
   - 注意：部署切换窗口内 CDN（Fastly）可能残留旧 404/503（响应头 `Age` 可辨）——**下一次部署或缓存过期自愈**，不要据此误判仓库内容（用 `git cat-file -s` 或 `raw.githubusercontent.com` 核对）。

## 构建号

格式 `YYYYMMDD<字母>`（如 `20261114b`）。修改时同步：`index.html` 的 `window.__GOMOKU_BUILD__` 与 `<script src="build/gomoku.js?v=...">`；发布时再同步 `docs/` 与两个子版本页。

## 引擎行为契约（改引擎前必读；全部是实测教训）

1. **显示与选择分离**：选择可含战术加成，但对外上报（棋谱/胜率条/API）必须是**所选着法的原始分**——防加成粉饰败局。
2. **胜率条方向**：引擎分一律是「走子方视角」；显示前必须换算成黑方视角（白方走子时取反）。
3. **`MAX_TACTICAL_ADJUST = 10M` 是承重结构**：收紧到 2M 曾把 19 项回归打崩到 7 项。不要动它。
4. **探针计数器是累计的**：`g_vcfNodes/g_vctNodes` 以「超上限即判超时」语义使用。任何在搜索中途调用 vcf/vct 的地方（全证败抽查、b3）必须**先清零再调用**——否则搜索「零节点即空转」返回 false（曾把真败误判成「对手证不出杀」）。
5. **探针会置位 `g_timedOut`**：调用方要 save/restore（抽查、b3、post-scan 都是这么做的）。
6. **首层耗时**：迭代加深的「提前收手」必须用逐层真实耗时（`iterStartMs`）——把搜索前阶段算进首层曾把搜索误砍到深度 1。
7. **b3（选定手核杀保护）**：收尾时对最终选定的手做一次对手 VCF/VCT 核杀；被证杀且存在幸存候选 → 换成评分序最靠前的幸存者（幸存者的被杀深度在探针射程之外，方向保守）。预算 `< 8s` 不启用（隔离 500ms 级回归测试）。改动它要重跑电池夹具与第 13 局 m12 场景。
8. **已知盲区**：深度搜索对「对手威胁搜索一眼可见的杀」可能视而不见（评估悬崖；「铺垫手 + 冲四链」型尤其如此）。探针类保护（b3）与搜索深度/评估改进是两条互补路线。
9. **时间自适应**：同一局面在不同机器/负载下走法可能小幅不同；夹具比对容忍小抖动，但「已证杀」类结论必须稳定。
10. **b1-B 根节点 VCT 剔除 + 杀距比较（2026-11-18）**：(a) VCF 防守过滤后只对 VCF **幸存**候选补 VCT 核杀——节点上限 `VCT_FILTER_NODE_LIMIT`（30 万）、合计 ≤45% 预算、预算 <`VCT_FILTER_MIN_BUDGET`（4s）跳过；(b) 全部候选被证杀时**不删候选**（`banned>0 && safe.empty()` 不 swap），杀距记入 `killDist`，全证败分支按「未被证杀(0) > 杀距晚（mate 败带分更大）> 深搜 raw」挑拖延手；(c) 杀距是**下界排名度量**（活四/拆不掉类证明在手数上偏小），不是精确将杀手数；(d) b3 与 b1-B 的分界：b3 只在 `bestRawScore > LOSS_SCORE_CEIL`（非全证败）时启用，两者不重叠。改它要重跑 4 套 + 夹具 + game2/game12 取证。
11. **多线程架构（2026-11-19，仅原生 CLI）**：(a) 可变状态收进 `SearchContext`（TT 指针、威胁 TT、计数器、时限标志、节点上限），`ctx()` 经当前上下文指针访问——**wasm 路径必须是普通静态指针，不能 `thread_local`**（无 pthread 的 wasm 走模拟 TLS，热路径实测 ~2x 拖慢，曾把 game12-m13 从深度 10 压到 9——纯速度边界，非逻辑变化）；(b) 主 TT 是无锁条目（Xor 键校验 + 双原子字，借 Rapfi/Stockfish），威胁 TT 每线程私有；(c) 并行用**保守 SMP**：线程 0 = 参照线程（独立 `g_ttRefStorage`，结果与单线程逐位一致），线程 1..N-1 共享 `g_ttSmpStorage`，合并只接受严格更优——**并行结果构造性 ≥ 单线程**（曾实测全员共享 TT 的经典 lazy SMP：互渗把参照线程的同分 tie-break 带偏，game12-m13 翻成错手 (10,8)，故废弃）；(d) wasm 构建 `findBestMoveParallel` 下沉到单线程（Pages 无 COOP/COEP、便携 file:// 无 SAB，浏览器多线程留待自托管场景）；(e) 改它要重跑 4 套 + 夹具 + 原生 bench t1/tN 对照（t1 必须 9/9、tN 不低于 t1）。

## 回归夹具（改搜索/评估必跑；2026-11-14 的 20s 档基线）

跑法：`_playerMove` 依次播前 N 手（双方手顺都固定），然后 `_aiMove()`，检查白方落点与评分。

- **电池线**（白方应手必须为）：
  `[[7,7],[7,6],[6,6],[8,8],[6,7],[8,7],[6,8],[6,9],[6,5],[6,4],[8,6],[9,5],[5,9],[4,10],[5,7],[4,6],[4,8]]`
  - n1（前 1 手后）→ `(7,8)`
  - n13（前 13 手后）→ `(4,10)`
  - n15（前 15 手后）→ `(5,10)`（**b3 保护生效点：不得选 `(4,6)`**——它自评 +366 万、实为黑方 1.7s 证杀）
  - n17（前 17 手后）→ `(3,9)`（全证败，评分须落败带 `-9999xxx`）
- **第 11 局线**：`[[7,7],[8,7],[8,6],[7,6],[6,6],[8,8],[6,8],[9,5],[6,7],[6,9],[5,7],[7,9],[7,5],[8,4]]`
  - m8（前 7 手后）→ `(9,5)` 或 `(5,9)`（2026-11-17 新机取证修正：探针扫描 22/24 候选被黑 VCF 21ply 证杀，仅此两手幸存；定深 11 真值 `(9,5)=-3688825` / `(5,9)=-3685025`，(5,9) 微优 3800，在 20s 档噪声内——两手皆可，15/20s 档会随 tie-break 翻转）；m10（前 9 手后）→ `(6,5)`；m12（前 11 手后）→ `(6,5)`（旧选 `(9,8)` 被黑方 6.1s 证杀）
- **第 12 局关键点**：前 13 手 `[[7,7],[7,8],[8,6],[6,8],[8,8],[9,9],[9,7],[8,7],[9,6],[10,6],[9,5],[9,4],[7,5]]` 之后，20s 必须选 `(6,4)`（10s/15s 会错选 `(10,8)`）；同局 P15（前 15 手后）需 25s+ 才落败带。注意（2026-11-19 MT 重构后）：着法仍稳定 `(6,4)`，但 20s 档上报深度从 10 掉到 9（评分从 -9093332 变 -3468）——该点处在加深边界上，同期旧 wasm 产物运行间节点数本身就有 ±3% 抖动；本局本就不要求 20s 如实败报（P15 需 25s+），故夹具判据（着法）不受影响。
- **第 13 局关键点**：前 11 手 `[[7,7],[7,8],[6,8],[5,9],[8,6],[5,10],[9,5],[10,4],[9,7],[10,8],[10,6]]` 之后不得选 `(8,8)`（黑方 43ms 证杀；b3 会换到 `(5,11)` 等幸存手）。
- 核查方法：用下方的 `_dbg` 调试构建做「候选核杀扫描 + 黑方 15-20s 全引擎深搜」地面真值（见第 12 局/第 13 局的取证模式）。

## 取证工具（`_dbg`，本机研究目录，gitignored）

- 重建：复制 `src\{engine.cpp,engine.h,main.cpp}` → `_dbg\`；把 `_dbg\debug_block.txt` 追加到 `_dbg\engine.cpp`；`em++ _dbg/engine.cpp _dbg/main.cpp -o _dbg/gomoku.js -O2 -I _dbg`。
- 导出：`_debugReset / _debugMove / _debugDumpRoot / _debugProbeAll(预算ms) / _debugProbeKill(x,y,预算ms) / _debugSearchCand(定深) / _debugSearchCandFull(x,y,预算ms)`。
- **必须 `_setDifficultyLevel(2)`**：否则中等档 8 层深度上限会截断深搜结果。
- `_forensic/`、`_b1030/` 为早期/对照构建（旧代引擎，已被 `_dbg` 取代）；`_parked/` 为历史停放补丁（已被仓库内的抽查与 b3 取代，仅作考古）。

## 风格与约定

- 注释与界面文案用**中文**；代码用英文。用户以中文交流。
- 修改引擎的硬流程：先测量（夹具/取证）→ 改动 → 全绿（4 套测试 + 电池 + 相关棋局夹具）→ 才发布。「**不劣于旧**」是硬门槛：夹具变化要逐项做对手侧核验（黑方深搜对比），必要时更新夹具并在提交信息里说明理由。
- 回滚/大改前先总结现状、等用户确认（除非用户明确说"做"）。
- 提交信息保持本仓库风格：**详述动机与实测数据**（面向以后复盘）。

## NNUE 预研（2026-11-20 开局，GitHub 参考研究结论）

- **参考**：D:\refs\rapfi（dotb1ue/rapfi，GPL-3.0）生产级 mix10 NNUE（64 维形状特征 + 9 点深度卷积 + 策略/价值双头、int16/int8 量化、增量累加器、与古典评估 blend + 分段线性读出）；训练器独立子项目 dhbloo/pytorch-nnue-trainer（PyTorch/accelerate、EMA、katago-numpy/packed-binary 数据集、导出带标准头 magic/arch_hash/rule_mask/boardsize_mask 的 lz4 二进制）。Stockfish NNUE 集成模式（特征变换器 + 增量累加器 + 叶端评估替换 + 搜索分蒸馏）为通用范式；zhongjn/gomokuer 是 MCTS+CNN 的另一路线（不采用）。
- **本仓库 Spike（逐级可证伪，默认关闭）**：
  1. 数据：native CLI 加 dump 命令——selfplay 每步输出（局面特征, 引擎搜评分），Stockfish 式蒸馏标签；
  2. 特征 v0：复用现有 pattern 检测——双方各 pattern 类型 × 三区（中心/中腹/边），约 42-48 维 + 手数/先手；
  3. 网络：FC 48→32(ReLU)→16(ReLU)→1，int8 量化推理，权重带标准头二进制落 native/nnue/；
  4. 训练：Python 3.13；网络极小先用纯 numpy SGD（不引 PyTorch），跑通再迁 PyTorch CPU；
  5. 集成：evaluatePosition 加 NNUE 分支（flag 控制），古典默认；native CLI 匹配赛（同时限古典 vs NNUE）判定；
  6. 纪律：换评估=重标定全部夹具（硬规矩）；发布前必须「不劣于旧」；夹具漂移逐项对手侧核验。

## NNUE spike 实测（2026-11-21，stage 1-3 已落地，stage 4 判定进行中）

- 基建已入库：`src/nnue.{h,cpp}`（单一特征规格 + int8 加载/反量化前向）、native CLI `dump`/`match` 命令、`native/nnue/train.py`（纯 numpy）、`native/nnue/nnue_v1.bin`（50->32->16->1）。
- 数据：40 局 200ms 自对弈 = 2771 个标注局面（root search score 作标签，tanh(score/1e6) 映射）。
- 训练：4000 迭代 val_mse 0.235（基线 0.66）——小样本已学到信号。
- 集成：`--nnue <file>` 加载后 `evaluatePosition` 整条替换为网络输出；**默认关**，wasm 构建完全不涉及（`GOMOKU_WITH_NNUE` 仅原生定义；wasm 重建产物逐位不变已验证）。
- 首场 A/B（4 局 2s）：NNUE 2 : 1 古典 + 1 和——方向积极但样本不足；20 局同时限匹配赛进行中，以它的结果定 stage 4（投入 or 回滚特征版本）。
- 复现：`pwsh native\build_native.ps1` → `gomoku_cli.exe dump --games 40 --budget 200 --out d.txt` → `python native/nnue/train.py --data d.txt --out nnue_v1.bin` → `gomoku_cli.exe match --games 20 --budget 2000 --nnue nnue_v1.bin`。
