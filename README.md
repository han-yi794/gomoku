# 五子棋 AI - Gomoku AI

基于 C++ + WebAssembly 的五子棋游戏，支持人机对战和机机对战。

## 特性

- **C++17 核心引擎**：高性能 Alpha-Beta 剪枝搜索
- **VCF 连冲四杀搜索**：只搜造四着法，能在全宽搜索够不到的深度上提前看到连续冲四必胜/必败
- **VCT 连续威胁搜索**：把「造活四威胁的三」也当强制手，能看穿「先做三、再转双三/冲四」型组合杀
- **WebAssembly 部署**：浏览器直接运行，无需安装
- **多种难度**：简单 (Depth 4-5)、中等 (Depth 7-8)、困难 (Depth 10-12)
- **人机对战**：玩家执黑先手，AI 执白应战
- **机机对战**：观看 AI 自我对弈，支持暂停/调速
- **搜索统计**：实时显示搜索深度、节点数、思考时间
- **悔棋功能**：支持人机模式悔棋

## 技术栈

- **后端**：C++17 + Emscripten (WASM)
- **前端**：HTML5 Canvas + 原生 JavaScript
- **AI 算法**：Alpha-Beta 剪枝 + 迭代加深 + 置换表 + Zobrist 哈希

## 快速开始

### 1. 编译

```powershell
# PowerShell
cd D:\gomoku
.\build.ps1
```

或手动编译：

```powershell
$env:EMSDK = 'D:\emsdk'
$env:Path = "D:\emsdk;D:\emsdk\upstream\emscripten;$env:Path"
em++ src/engine.cpp src/main.cpp -o build/gomoku.js -O2 -I src
```

### 2. 运行

```powershell
# 方式 1：用浏览器打开（需要 HTTP 服务器）
cd D:\gomoku
python -m http.server 8080
# 然后访问 http://localhost:8080/index.html

# 方式 2：用 VSCode Live Server 插件
# 安装 Live Server 扩展，右键 index.html -> Open with Live Server
```

> 开发用 `build/gomoku.wasm` 是外部文件，浏览器通过 `fetch` 加载，
> 因此 `file://` 双击打开会被 CORS 拦住。要免服务器直接玩，用下面的打包方式。

### 3. 打包给别人

```powershell
cd D:\gomoku
.\package.ps1
```

生成 `dist\`，里面只有两个文件：

```
dist\
├── index.html     # 页面
└── gomoku.js      # 引擎，WASM 已内嵌为 base64（约 51KB）
```

**把整个 `dist\` 文件夹压缩成 zip 发给对方即可**，对方不需要装
Python / Node / Emscripten，也不需要联网，双击 `index.html` 就能玩。

要发布到网上，把 `dist\` 整个丢到任意静态托管（GitHub Pages、对象存储等）即可。

### 4. 测试

```powershell
node test_diagonal_defense.js
node test_engine.js
```

## 项目结构

```
D:\gomoku\
├── src/
│   ├── engine.h          # 引擎头文件
│   ├── engine.cpp        # 核心引擎实现
│   └── main.cpp          # Emscripten 绑定层
├── build/
│   ├── gomoku.js         # WASM 加载器
│   └── gomoku.wasm       # 编译后的 WASM 模块
├── index.html            # Web 前端界面
├── test_engine.js        # Node.js 测试脚本
├── test_diagonal_defense.js # 威胁防守行为回归
├── build.ps1             # 开发构建脚本（输出 build/）
├── package.ps1           # 可分发打包脚本（输出 dist/）
└── README.md             # 本文档
```

## AI 算法

### 搜索策略

- **Alpha-Beta 剪枝**：NegaMax 变体
- **迭代加深**：从 Depth 1 开始逐步加深
- **置换表**：1M entries，Zobrist 哈希
- **候选点过滤**：半径 2 邻居过滤
- **走法排序**：快速威胁评估排序
- **VCF 战术层**：根节点先做一次「连续冲四」专项搜索。进攻侧找到必胜就直接走（实测 2ms 出结果）；防守侧剔除「走完之后对手有短连冲四杀」的候选。只搜造四着法，分支极窄，同样时间能到十几手冲四的深度。
- **VCT 战术层**：连冲四找不到时，再查「三」施压型组合。攻方的强制手 = 四 + 造活四威胁的三；守方应手不是手写棋型，而是**按定义算出**「所有能让攻方下一手无法形成活四的空点」，攻方必须对每个应手都成立才算取胜。只用于进攻——实测把它用在防守会剔除必败局面里的拖延着法，反而速败。

### 评估函数

- **棋型评分**：活四、冲四、活三、眠三等
- **攻防权重**：对手威胁 × 1.10（略偏防守）
- **中心加权**：位置离中心越近分越高
- **威胁检测**：连五、活四、冲四、活三

### 难度级别

| 难度 | 搜索深度 | 时间预算 | 适用场景 |
|------|----------|----------|----------|
| 简单 | 4-5 | 500ms | 新手入门 |
| 中等 | 7-8 | 2000ms | 普通玩家 |
| 困难 | 10-12 | 12000ms | 老棋手 |

## API 参考

### 游戏控制

| 函数 | 说明 |
|------|------|
| `initGame()` | 初始化新游戏 |
| `setDifficultyLevel(level)` | 设置难度 (0/1/2) |
| `playerMove(x, y)` | 玩家落子 |
| `aiMove()` | AI 思考并落子 |
| `undo()` | 悔棋一步 |
| `undoN(n)` | 悔棋 n 步 |

### 状态查询

| 函数 | 说明 |
|------|------|
| `getBoardSize()` | 获取棋盘大小 |
| `getCurrentPlayer()` | 获取当前玩家 (1=黑, 2=白) |
| `getMoveCount()` | 获取已落子数 |
| `getGameStatus()` | 获取游戏状态 (0=进行中, 1=黑胜, 2=白胜, 3=平局) |
| `getCell(x, y)` | 获取指定位置的值 |
| `getBoardData(buffer)` | 获取完整棋盘数据 |

### 搜索统计

| 函数 | 说明 |
|------|------|
| `getLastSearchDepth()` | 获取最后搜索深度 |
| `getLastSearchNodes()` | 获取最后搜索节点数 |
| `getLastSearchTimeMs()` | 获取最后搜索耗时 (ms) |
| `getLastSearchScore()` | 获取最后搜索的根节点评分；落在 `[-(WIN_THRESHOLD+999), -(WIN_THRESHOLD-999)]` 区间表示「所有着法均已证败」（该结论只在候选完整扫描后成立） |
| `hasVcfWin()` | 当前局面轮走方是否存在「连续冲四」（VCF）必胜，供自检/分析使用 |

## 性能

- **WASM 大小**：~38KB
- **JS 加载器**：~11KB（开发版）；单文件分发版 ~51KB（WASM 已内嵌）
- **中等难度**：~13,000 节点/步，~2 秒/步
- **困难难度**：~50,000+ 节点/步，~5 秒/步

## 后续优化计划

- [x] VCF 战术搜索（连冲四杀检测）
- [x] VCT 战术搜索（含活三的连续威胁，进攻侧）
- [ ] VCT 防守侧（需要先解决「必败局面下剔除拖延着法」的问题，见下）
- [ ] 战术搜索加置换表 / 更强的相关性剪枝（目前深链杀法证明会超出节点预算）
- [ ] 收敛根节点战术加成的量级（实测可达 ±4600 万，会盖过搜索结论，把必败着法评成「大优」）
- [ ] NNUE 神经网络评估
- [ ] 多线程搜索（WASM SharedArrayBuffer）
- [ ] 开局书（Opening Book）
- [ ] 败势下的「拖延」评分（目前必败局面只在同分着法间随手选，不会优先延长战线）
- [ ] 棋型模式库优化
- [ ] 移动端适配

## 参考资源

- [Emscripten 文档](https://emscripten.org/docs/)
- [Gomoku Wikipedia](https://en.wikipedia.org/wiki/Gomoku)
- [五子棋 AI 算法调研](./gomoku_research/algorithms.md)
- [GitHub 五子棋项目调研](./gomoku_research/github_projects.md)

## License

MIT
