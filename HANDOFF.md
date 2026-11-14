# 交接文档 — 五子棋 AI（Gomoku AI）

> 日期：2026-11-14 · 面向：接手本项目的新代理 / 新会话
> 本文件同时保存于：系统临时目录（handoff 技能惯例）与本仓库根目录 `HANDOFF.md`。

## 当前状态（一句话）

线上 **build 20261114b** 全绿；仓库 = `han-yi794/gomoku`（public，Apache-2.0），`main` = 源码树 + `docs/`（GitHub Pages 服务目录，`main:/docs`）；本机工作克隆 = `D:\gomoku`（克隆即开发，见 AGENTS.md）。

## 必读顺序

1. `AGENTS.md`（仓库根）——环境、命令、引擎铁律、回归夹具、发布/取证流程
2. `README.md`——项目文档与全部工程笔记/路线图（含校准史与技术决策）
3. `git log --oneline -12`——近期提交（动机与实测数据都在提交信息里）

## 本会话交付摘要（细节见提交信息与 README 路线图）

- 界面重设计（framed-sheet 方向）、显示/选择分离、胜率条方向修复
- 「全证败」抽查复活（根因 = 探针节点计数器污染）；败势拖延 b2；首层耗时缺陷修复
- 超长考（30s）档新增；**困难档 10s→20s 定档**（15/20/25s 实验：20s = 决策改变点）
- **b3 选定手核杀保护**（修复 n15 回归与第 13 局 m12 类漏着）
- 开源化：Apache-2.0（含 Copyright 行）、README 重写、`docs/` 重构、构建产物入库（克隆即开发）、字体 OFL 授权、预览图、仓库话题/描述

## 待办（按优先级）

1. **② 引擎下一迭代**（用户已表态要推进，可选项）：
   - b1 完整形态（根节点 VCT 剔除 + **杀距比较**——b3 已先吃掉最急需的一块）
   - 搜索效率（同等时间更深）
   - NNUE 预研 spike（本机 Python 3.12.5 可用；先小数据自战 → 小网络 → 与现引擎对打分；注意换评估需重标定全部夹具）
   - 多线程：最后做；且先做**本机原生 CLI 版**（浏览器 SharedArrayBuffer 需 COOP/COEP，GitHub Pages 无法设置响应头、便携 `file://` 版亦不可用）
2. **③ CI + Release**（小活）：GitHub Actions 跑 4 套测试（仓库带构建产物，无需 emsdk）；发 Release 便携 zip
3. 用户会持续丢实战棋谱（困难 20s / 超长考）→ 按 AGENTS.md 的取证流程复盘

## 已知观察项

- `test_loss_claims` T1 在 20s 档 = `-9110780`：测试通过但不远于败带——假证败夹具的深层结论值得某天复核
- 「铺垫手 + 冲四链」型杀法超出 VCF/VCT 探针射程（多盘实战主败因）→ 靠搜索深度/评估改进解决
- ±5 千万越界评分历史上出现过、近期未见（战术加成限幅 + 显示分离已处置已知来源）
- 校准实验原始日志在 `%TEMP%\budget_cal.log`（本机临时文件，可能被清理；结论已入 README 路线图）
- 本机另有 `_dbg` 取证调试构建与 `_dbg\debug_block.txt`（gitignored；重建方法见 AGENTS.md）

## 踩坑速查

- 中文输出必须加 PowerShell 编码前缀（`chcp 65001 …`，见 AGENTS.md）
- 网络不稳：失败先重试；普通 `curl.exe -sS` 可用，`-4` / 部分 raw 调用会 TLS 失败
- Pages CDN 在部署切换窗口会缓存旧 404/503（响应头 `Age` 可辨）→ 下一次部署或缓存过期自愈；核对仓库内容用 `git cat-file -s` 与 `raw.githubusercontent.com`
- 用户风格：指令简短（中文）；硬规矩 = 强度第一 / 先测量再动手 / 绿了才发 /「不劣于旧」/ 回滚先请示；偏好 A/B 选项；发棋谱 = 要复盘

## Suggested skills（建议新会话加载）

- `design-taste-frontend`：任何 UI 改动走真浏览器门禁（方向 framed-sheet，seed 235953385）
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
```

期望：全部 exit 0；diagonal 19 PASS。
