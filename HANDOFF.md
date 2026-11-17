# 交接文档 — 五子棋 AI（Gomoku AI）

> 日期：2026-11-17 · 面向：接手本项目的新代理 / 新会话
> 本文件同时保存于：系统临时目录（handoff 技能惯例）与本仓库根目录 `HANDOFF.md`。

## 当前状态（一句话）

**新开发机迁移完成并已验证**：本机工作克隆 = `D:\gomoku`（HEAD 与线上一致，局部 git 身份 han-yi794）；emsdk 7.0.0 在 `D:\emsdk`（手动解包装法，见 AGENTS.md）；本机基线 4 套测试 + 20s 夹具基线复现全绿（8/9，唯一 DIFF 已取证定性，见下）。**Release v1.0.0 已发布**（便携单文件 zip）；**GitHub Actions CI 已上线**（每次 push/PR 自动跑 4 套测试，全绿才合）。

## 本会话交付摘要（细节见提交信息）

- **迁移**：克隆 D:\gomoku；git 局部身份（沿用 han-yi794 noreply 邮箱）；基线重跑 4 套全绿（diagonal 19 PASS 两次复核一致）
- **CI（交接待办 ③）**：`.github/workflows/ci.yml` 两 job（test-fast：engine/session_state/loss_claims，15 分钟上限；test-regression：diagonal 发布底线，30 分钟上限）；README 加 CI 徽标；顺带修了 README API 表遗漏 `wasm` 前缀的文档 bug、清理 test_diagonal_defense 的重复别名函数
- **Release v1.0.0**（交接待办 ③）：`package.ps1`（用 pwsh 7 跑，原因见 AGENTS.md）→ dist → zip；node 层冒烟（便携引擎评分与仓库版逐位一致 -25257）+ 静态核对（build 20261114b / 引擎内嵌 / 字体相对路径 / 桥接保留）；`gh release create v1.0.0` 已上线
- **取证工具重建**（旧机 `_dbg` 随 gitignore 丢失）：按 AGENTS.md 规格重写 `_dbg/debug_block.txt` + `_dbg/rebuild.ps1`（含 main.cpp 去 static 的补丁步骤）、夹具复现 `_dbg/fixtures_replay.js`、预算扫描 `_dbg/sweep.js`、m8 取证 `_dbg/forensic_m8.js`
- **m8 夹具取证**（20s 档唯一 DIFF，game11 前 7 手后白方）：探针扫描 **22/24 根候选被黑 VCF 21ply 证杀**（vcf=4 手冲四），仅 `(9,5)`/`(5,9)` 是探针幸存者；定深 11 真值 `(9,5)=-3688825`（黑应 (9,8)、108s/6.4M 节点）vs `(5,9)=-3685025`（黑应 (6,5)、97s/5.4M 节点）——**两手真值差 3800，在 20s 档噪声内，两手皆可**（15/20s 档随 tie-break 翻转是噪声不是回归）。AGENTS.md 夹具记录已按「对手侧核验」流程正名
- AGENTS.md 增补环境坑：仓库 .ps1 是 UTF-8 无 BOM（PS 5.1 解析必炸，用 pwsh 7+）；本机 emsdk 安装器 per-file move 确定性 WinError 5 的手动绕法

## 待办（按优先级）

1. **② 引擎下一迭代**（用户已表态要推进）：首选 **b1 完整形态（根节点 VCT 剔除 + 杀距比较）**——测量基础已就绪（m8 取证 + 夹具基线），方案已整理待用户确认（A/B）。其余选项：搜索效率（同等时间更深）、NNUE 预研 spike（本机 Python 3.13.7，注意与旧笔记 3.12.5 的差异；换评估需重标定全部夹具）、多线程（最后做，且先做本机原生 CLI 版）
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