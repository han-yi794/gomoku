# 交接文档 — 五子棋 AI（Gomoku AI）

> 日期：2026-11-17 · 面向：接手本项目的新代理 / 新会话
> 本文件同时保存于：系统临时目录（handoff 技能惯例）与本仓库根目录 `HANDOFF.md`。

## 当前状态（一句话）

**新开发机迁移完成 + b1-B 引擎迭代已落地**：本机工作克隆 = `D:\gomoku`；emsdk 7.0.0 在 `D:\emsdk`；**build 20261118a（b1 完整形态）已提交推送，4 套测试 + 20s 夹具基线全绿**（详见下）；Release v1.0.0 已发布；GitHub Actions CI 已上线。b1-B 尚未发布到 Pages 线上（待用户确认）。

## 本会话交付摘要（细节见提交信息）

- **迁移**：克隆 D:\gomoku；git 局部身份（沿用 han-yi794 noreply 邮箱）；基线重跑 4 套全绿（diagonal 19 PASS 两次复核一致）
- **CI（交接待办 ③）**：`.github/workflows/ci.yml` 两 job（test-fast：engine/session_state/loss_claims，15 分钟上限；test-regression：diagonal 发布底线，30 分钟上限）；README 加 CI 徽标；顺带修了 README API 表遗漏 `wasm` 前缀的文档 bug、清理 test_diagonal_defense 的重复别名函数
- **Release v1.0.0**（交接待办 ③）：`package.ps1`（用 pwsh 7 跑，原因见 AGENTS.md）→ dist → zip；node 层冒烟（便携引擎评分与仓库版逐位一致 -25257）+ 静态核对（build 20261114b / 引擎内嵌 / 字体相对路径 / 桥接保留）；`gh release create v1.0.0` 已上线
- **取证工具重建**（旧机 `_dbg` 随 gitignore 丢失）：按 AGENTS.md 规格重写 `_dbg/debug_block.txt` + `_dbg/rebuild.ps1`（含 main.cpp 去 static 的补丁步骤）、夹具复现 `_dbg/fixtures_replay.js`、预算扫描 `_dbg/sweep.js`、m8 取证 `_dbg/forensic_m8.js`
- **m8 夹具取证**（20s 档唯一 DIFF，game11 前 7 手后白方）：探针扫描 **22/24 根候选被黑 VCF 21ply 证杀**（vcf=4 手冲四），仅 `(9,5)`/`(5,9)` 是探针幸存者；定深 11 真值 `(9,5)=-3688825`（黑应 (9,8)、108s/6.4M 节点）vs `(5,9)=-3685025`（黑应 (6,5)、97s/5.4M 节点）——**两手真值差 3800，在 20s 档噪声内，两手皆可**（15/20s 档随 tie-break 翻转是噪声不是回归）。AGENTS.md 夹具记录已按「对手侧核验」流程正名
- AGENTS.md 增补环境坑：仓库 .ps1 是 UTF-8 无 BOM（PS 5.1 解析必炸，用 pwsh 7+）；本机 emsdk 安装器 per-file move 确定性 WinError 5 的手动绕法
- **b1-B 引擎迭代落地**（build 20261118a，用户选定完整版）：根节点 VCF 过滤后对幸存候选补 VCT 核杀（30 万节点/候选、≤45% 预算、<4s 跳过）；全证杀时不删候选、按 mate 败带分杀距挑拖延手（借鉴 Rapfi mate-in-value 编码；参考实现浅克隆在 `D:\refs\rapfi`）。实测：4 套测试全绿、夹具 8/9（唯一已知 m8 持平）、分数与基线逐位一致；VCT 节点上限 60 万→30 万（game12-m13 取证：60 万时两幸存者烧 3s 零收获、主搜索深度 10→9；30 万后恢复深度 10/-9093332）。diagonal game2 认输着法 (9,4)→(5,8)：24/24 候选 VCF 证杀（怎么走都败），防守探针紧界支持新选（≤11 vs ≤9 ply）、黑方攻击层先证链支持旧选（6 vs 7 四）——证据矛盾但差 ≤2 ply，测试仍绿（只查败带）。`_dbg/debug_block.txt` 已同步引擎杀距口径修正

## 待办（按优先级）

1. **NNUE Spike 迭代中**（stage 1-3 已入库；`--nnue` flag 默认关、古典兜底、wasm 零影响）：
   - 管线：`src/nnue.{h,cpp}` 单一特征规格（62 维：双方 10 棋型 x 3 区 + stm + 手数）→ CLI `dump`（selfplay 标签）→ `native/nnue/train.py`（纯 numpy、int8 导出）→ 引擎推理（免分配、1.2*tanh 有界输出）；
   - 战绩（20 局 2s 换先）：v0 6:5+9 和（持平）→ v2 **15:1**（完胜，且 NNUE 引擎每节点吞吐仅古典一半——评估质量压倒性）→ v3（深浅标签混合+输出截断）11:9 回落；
   - **Spike 结论：目标已达成**——三个独立配置 A/B（2s，20 局）全部 NNUE 占优：v2 15:1、v4（随机开局深标签，val_mse 0.099）13:5、截断版 8:5；且 NNUE 引擎每节点吞吐仅古典一半（评估质量压倒性）。
   - 教训：输出截断有害（15:1→8:5，已移除）；深标签须配 `dump --open N` 随机开局（否则对局过短分布偏移）；val_mse 与棋力非单调，A/B 是唯一判据。
   - ⚠️ **两个本地提交待推**（GitHub 链路当时不可达：外网通但 github.com:443 拒连）：`b0331b2`（截断移除 + --open）、`12af228`（v4 判定）。网络恢复后 `git push origin main` 即可。
   - 后续：确认配方 → 增量特征累加器（设计已记录于 AGENTS.md，实现两次写乱已回滚——宁可没有不要半成品；差分测试 nnueVerifyAccumulator 先行）→ 20s 档抽检 + 夹具重标定（默认启用的硬规矩）→ wasm 权重内嵌上线。
2. **b1-B 之后的引擎选项**：搜索效率（Rapfi 的 mate distance pruning——分值限界，已借鉴其 mate-in-value 编码未引剪枝）；VCT 补枪实证收益（当前零拒绝记录，需深链杀靶局面）；game12-m13 深度边界的逐层 instrumentation（MT 重构后 wasm 在该点 21k vs 旧 44k 节点/s，着法未变、原因待查——跨编译器节点数逐位相同证明无逻辑回归）
3. ~~多线程~~：已落地（2026-11-19，保守 SMP + 原生 CLI，详见 AGENTS.md 契约 #11）
4. ~~发布 b1-B 到 Pages~~：已完成（build 20261118a 线上，SHA256 核对一致）
2. **换工具链重建 build/ + docs/**（刻意未做）：本机 emsdk 7.0.0 与旧机构建产物不同源。本会话特意保留仓库内 20261114b 产物不动（`package.ps1` 只写 dist/）。若某次引擎改动需要重建，build.ps1 重编后**必须重跑 4 套 + 夹具基线**再提交产物
3. **浏览器冒烟缺口**：本机无 Chrome（仅 Edge；playwright MCP 只要真 Chrome，未提权装不了）。AGENTS.md 发布流程里的「真浏览器冒烟」目前只有 node 层 + 静态核对替代——装 Chrome 后补冒烟（对以后的 UI 改动也是刚需）
4. 用户会持续丢实战棋谱（困难 20s / 超长考）→ 按 AGENTS.md 的取证流程复盘

## 已知观察项

- `test_loss_claims` T1 在 20s 档 = `-9110780`：测试通过但不远于败带（旧笔记遗留，仍待某天复核）
- m8 新局面：白方两种幸存下法都 ≈ -368 万（大局已劣）；「探针幸存但深搜大负」是中盘盲区的典型形态，是 b1/搜索深度改进的靶子
- 「铺垫手 + 冲四链」型杀法超出 VCF/VCT 探针射程（多盘实战主败因）→ 靠搜索深度/评估改进解决
- ±5 千万越界评分历史上出现过、近期未见
- 校准实验原始日志在 `%TEMP%\budget_cal.log`（旧机文件，本机不存在；结论已入 README 路线图）

## 踩坑速查

- 仓库 .ps1 一律用 `pwsh -ExecutionPolicy Bypass -File xxx.ps1` 跑（PS 5.1 读 UTF-8 无 BOM 中文串会解析失败）
- 中文输出必须加 PowerShell 编码前缀（`chcp 65001 …`，见 AGENTS.md）
- 网络不稳：失败先重试；普通 `curl.exe -sS` 可用
- emsdk 手动安装绕法见 AGENTS.md（本机已装好，新机才需要）
- 用户风格：指令简短（中文）；硬规矩 = 强度第一 / 先测量再动手 / 绿了才发 /「不劣于旧」/ 回滚先请示；偏好 A/B 选项；发棋谱 = 要复盘

## Suggested skills（建议新会话加载）

- `design-taste-frontend`：任何 UI 改动走真浏览器门禁（方向 framed-sheet，seed 235953385）——本机装 Chrome 后
- `diagnosing-bugs`：引擎取证类循环（复现 → 隔离 → 测量 → 修复）
- `handoff`：会话收尾时产出下一份交接
- `writing-for-agents`：维护 AGENTS.md 时
- `opencode`：涉及本工具链/配置的问题

## 快速验证（接手先跑）

```powershell
cd D:\gomoku
node test_engine.js
node test_session_state.js
node test_loss_claims.js        # ~2 分钟（T1/T2/T4）
node test_diagonal_defense.js   # ~10 分钟级（19 项，发布底线）
node _dbg/fixtures_replay.js    # 20s 档夹具基线（期望 8/9 MATCH，m8 两手皆可）
```

期望：前 4 个 exit 0 / 19 PASS；fixtures 8/9 MATCH（game11-m8 见 AGENTS.md 正名说明）。