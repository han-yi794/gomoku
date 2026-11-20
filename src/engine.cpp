// engine.cpp - 五子棋核心引擎实现
// C++17 + Emscripten (WASM)
// 参考: algorithms.md 调研文档

#include "engine.h"
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstdlib>

// ============= 全局状态 =============

// ============= 全局状态 =============
// 搜索配置：搜索期间只读，跨线程共享（setDifficulty/setTimeBudget 在搜索之外设置）
static int64_t g_timeBudgetMs = 3000;
static int g_maxDepth = 8;  // 默认搜索深度
//
// 多线程（2026-11-19）：其余可变状态收进 SearchContext（定义见 VCT 节之后），
// 经 thread_local 当前上下文 ctx() 访问。单线程/wasm 路径始终用默认实例，行为与
// 旧实现逐位一致；原生 CLI 并行搜索每线程一份私有上下文，共享无锁主 TT、各带
// 私有威胁 TT。zobrist 表只读共享（call_once 式惰性初始化）。

// ============= Zobrist 哈希 =============

static uint64_t zobrist[BOARD_SIZE][BOARD_SIZE][3];  // [x][y][player+1]

static void initZobrist() {
    // C++11 函数局部静态量的初始化是线程安全的——多线程首搜并发调用也只会填一次
    static const bool initialized = []() {
        uint64_t state = 0x123456789ABCDEF0ULL;
        for (int y = 0; y < BOARD_SIZE; y++) {
            for (int x = 0; x < BOARD_SIZE; x++) {
                for (int pp = 0; pp < 3; pp++) {
                    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
                    zobrist[x][y][pp] = state;
                }
            }
        }
        return true;
    }();
    (void)initialized;
}
static uint64_t computeHash(const Board& board) {
    initZobrist();
    uint64_t hash = 0;
    for (int y = 0; y < BOARD_SIZE; y++) {
        for (int x = 0; x < BOARD_SIZE; x++) {
            int cell = board.get(x, y);
            if (cell != EMPTY) {
                hash ^= zobrist[x][y][cell];
            }
        }
    }
    return hash;
}

// ============= 置换表 =============

// ============= 置换表 =============
//
// 无锁 TTEntry（主 TT 多线程共享写安全；借 Rapfi/Stockfish 的 Xor 键校验）：
//   word1 = 打包数据先写，word0 = hash ^ data 后写；读方校验 (word0 ^ word1) == hash
//   才认账。任何写一半的中间态过不了校验、按 miss 处理，无需加锁。
// 语义注意：这不是事务——旧条目可能被「整体替换过程中的半新状态」淘汰，但半新
// 状态一律被拒，不会把脏数据当命中；同位置重写由深度/替换策略自然收敛。
struct TTEntry {
    std::atomic<uint64_t> word0;   // hash ^ data（校验字，后写）
    std::atomic<uint64_t> word1;   // 打包负载（先写）
};

// data 打包（小端）：score(int32, 0-31) | depth(uint8, 32-39) | flag(uint8, 40-47) | bestMove(int16, 48-63)
static inline uint64_t ttPackData(int32_t score, int depth, int flag, int bestMove) {
    return (uint64_t)(uint32_t)score
         | ((uint64_t)(uint8_t)(depth < 0 ? 0 : depth) << 32)
         | ((uint64_t)(uint8_t)flag << 40)
         | ((uint64_t)(uint16_t)bestMove << 48);
}
static inline int32_t ttScoreOf(uint64_t data)   { return (int32_t)(uint32_t)(data & 0xFFFFFFFFULL); }
static inline int     ttDepthOf(uint64_t data)   { return (int)((data >> 32) & 0xFF); }
static inline int     ttFlagOf(uint64_t data)    { return (int)((data >> 40) & 0xFF); }
static inline int     ttBestMoveOf(uint64_t data){ return (int16_t)(uint16_t)((data >> 48) & 0xFFFF); }

static constexpr int TT_SIZE = 1 << 20;  // 1M entries
static TTEntry g_ttStorage[TT_SIZE];     // 主置换表存储（默认上下文；条目无锁）
#ifndef __EMSCRIPTEN__
static TTEntry g_ttRefStorage[TT_SIZE];  // 并行参照线程（线程 0）专用主 TT
static TTEntry g_ttSmpStorage[TT_SIZE];  // 并行辅助线程共享主 TT
static int g_parallelMergeInfo = 0;      // 合并决策供测量记录（0=参照线程胜出）

int getParallelMergeInfo() { return g_parallelMergeInfo; }
#endif

struct ThreatTTEntry {
    uint64_t hash;
    int8_t depth;      // 立此结论时还剩多少手威胁
    uint8_t result;    // 0=未证必胜, 1=已证必胜
    int16_t bestMove;  // y*15+x，必胜时的第一手
};

static constexpr int THREAT_TT_SIZE = 1 << 18;
static ThreatTTEntry g_threatTTStorage[THREAT_TT_SIZE];  // 默认上下文（单线程）的私有实例

// ============= 搜索上下文（多线程状态容器） =============
//
// 单线程/wasm：全局唯一默认实例（上面的两个存储），行为与旧实现一致。
// 原生 CLI 并行：每线程一个实例——主 TT 指针共享（条目无锁）、威胁 TT 私有
// （探测计数器、时限标志、节点上限等逐线程独立，互不干扰）。
struct SearchContext {
    TTEntry* tt;                       // 主置换表（多线程共享；条目无锁）
    ThreatTTEntry* threatTT;           // 威胁搜索置换表（私有）
    int64_t nodesVisited = 0;
    bool timedOut = false;
    std::chrono::steady_clock::time_point searchStartTime;
    // 最后一次搜索的统计
    int lastSearchDepth = 0;
    int64_t lastSearchNodes = 0;
    int64_t lastSearchTimeMs = 0;
    int64_t lastSearchScore = 0;
    bool inPostScan = false;           // 防止复核里再触发复核
    int vcfNodes = 0;                  // VCF 探针节点计数（每次探针前清零）
    int vctNodes = 0;                  // VCT 探针节点计数
    int vcfWinFours = 0;               // 最近一次 VCF 证明的手数（攻方视角，需按初始预算偏移换算）
    int vctWinMovesLeft = 0;           // 必胜证明节点的剩余威胁手数（杀距比较用）
    int vcfNodeLimit = 3000000;        // VCF 节点上限兜底
    int vctNodeLimit = 600000;         // VCT 节点上限兜底（45 万节点 ~0.85s）
};
static SearchContext g_defaultCtx{g_ttStorage, g_threatTTStorage};
// wasm 无 pthread：thread_local 会走模拟 TLS 慢路径（实测热路径吞吐 ~2x 拖慢），
// 且指针间接本身也让 LLVM 无法静态寻址（每节点多两次内存往返）。单线程 wasm 直接
// 以宏引用默认实例——热路径与旧全局变量版同速；原生并行构建才用 thread_local。
#ifdef __EMSCRIPTEN__
#define ctx() g_defaultCtx
#else
static thread_local SearchContext* t_ctxPtr = &g_defaultCtx;
static inline SearchContext& ctx() { return *t_ctxPtr; }
#endif

// ============= 置换表访问（无锁条目） =============
static inline int ttIndex(uint64_t hash) { return (int)(hash & (TT_SIZE - 1)); }

static inline void ttStore(uint64_t hash, int depth, int64_t score, int bestX, int bestY, int64_t alpha, int64_t beta) {
    TTEntry& entry = ctx().tt[ttIndex(hash)];
    const int32_t s = (score > INT32_MAX) ? INT32_MAX : (score < INT32_MIN) ? INT32_MIN : (int32_t)score;
    int flag;
    if (score <= alpha) flag = 2;       // UPPER
    else if (score >= beta) flag = 1;   // LOWER
    else flag = 0;                      // EXACT
    const int bestMove = (bestX >= 0 && bestY >= 0) ? (bestY * BOARD_SIZE + bestX) : -1;
    const uint64_t data = ttPackData(s, depth, flag, bestMove);
    entry.word1.store(data, std::memory_order_relaxed);        // 数据先写
    entry.word0.store(hash ^ data, std::memory_order_relaxed); // 校验字后写（其他线程至此才可能见到完整条目）
}

// 查找结果（条目是 packed 的，回传解包值；hit=false 即 miss）
struct TTProbe { bool hit = false; int32_t score = 0; int depth = 0; int flag = 0; int bestMove = -1; };
static inline TTProbe ttLookup(uint64_t hash, int depth) {
    TTProbe out;
    const TTEntry& entry = ctx().tt[ttIndex(hash)];
    const uint64_t w1 = entry.word1.load(std::memory_order_relaxed);
    const uint64_t w0 = entry.word0.load(std::memory_order_relaxed);
    if ((w0 ^ w1) != hash) return out;      // 键校验失败（写完前/被覆盖）：按 miss
    const int d = ttDepthOf(w1);
    if (d < depth) return out;              // 深度不足
    out.hit = true; out.score = ttScoreOf(w1); out.depth = d; out.flag = ttFlagOf(w1); out.bestMove = ttBestMoveOf(w1);
    return out;
}

// ============= 威胁搜索置换表（每线程私有，无需无锁） =============
static inline void threatTtClear() {
    memset(ctx().threatTT, 0, sizeof(ThreatTTEntry) * THREAT_TT_SIZE);
}

static inline const ThreatTTEntry* threatTtLookup(uint64_t hash) {
    const ThreatTTEntry& e = ctx().threatTT[hash & (THREAT_TT_SIZE - 1)];
    return (e.depth != 0 && e.hash == hash) ? &e : nullptr;
}

static inline void threatTtStore(uint64_t hash, int depth, bool result, int bestX, int bestY) {
    ThreatTTEntry& e = ctx().threatTT[hash & (THREAT_TT_SIZE - 1)];
    if (depth <= 0 || depth > 127) return;
    if (e.depth == 0 || e.hash != hash || !result) {   // 已证必胜的结论优先保留
        e.hash = hash;
        e.depth = (int8_t)depth;
        e.result = result ? 1 : 0;
    }
    if (result && bestX >= 0 && bestY >= 0) {
        e.bestMove = (int16_t)(bestY * BOARD_SIZE + bestX);
    }
}

// ============= 时间管理 =============

static inline int64_t getElapsedMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - ctx().searchStartTime).count();
}

static inline bool checkTime() {
    if ((ctx().nodesVisited & 0x1FF) == 0) {  // 每 512 节点检查一次
        if (getElapsedMs() > g_timeBudgetMs) {
            ctx().timedOut = true;
            return true;
        }
    }
    return ctx().timedOut;
}

// ============= 评估函数 =============

// 中心分查找表（离中心越近分越高）
// 15x15 棋盘，中心 (7,7) 最高分
static constexpr int64_t CENTER_SCORE[BOARD_SIZE][BOARD_SIZE] = {
    {50, 80, 120, 160, 200, 240, 280, 300, 280, 240, 200, 160, 120, 80, 50},
    {80, 120, 180, 240, 280, 320, 360, 380, 360, 320, 280, 240, 180, 120, 80},
    {120, 180, 260, 320, 360, 400, 440, 460, 440, 400, 360, 320, 260, 180, 120},
    {160, 240, 320, 400, 440, 480, 520, 540, 520, 480, 440, 400, 320, 240, 160},
    {200, 280, 360, 440, 480, 520, 560, 580, 560, 520, 480, 440, 360, 280, 200},
    {240, 320, 400, 480, 520, 560, 600, 620, 600, 560, 520, 480, 400, 320, 240},
    {280, 360, 440, 520, 560, 600, 640, 660, 640, 600, 560, 520, 440, 360, 280},
    {300, 380, 460, 540, 580, 620, 660, 680, 660, 620, 580, 540, 460, 380, 300},
    {280, 360, 440, 520, 560, 600, 640, 660, 640, 600, 560, 520, 440, 360, 280},
    {240, 320, 400, 480, 520, 560, 600, 620, 600, 560, 520, 480, 400, 320, 240},
    {200, 280, 360, 440, 480, 520, 560, 580, 560, 520, 480, 440, 360, 280, 200},
    {160, 240, 320, 400, 440, 480, 520, 540, 520, 480, 440, 400, 320, 240, 160},
    {120, 180, 260, 320, 360, 400, 440, 460, 440, 400, 360, 320, 260, 180, 120},
    {80, 120, 180, 240, 280, 320, 360, 380, 360, 320, 280, 240, 180, 120, 80},
    {50, 80, 120, 160, 200, 240, 280, 300, 280, 240, 200, 160, 120, 80, 50},
};

// 计算单个玩家的中心分总分
static int64_t evaluateCenterScore(const Board& board, int player) {
    int64_t score = 0;
    for (int y = 0; y < BOARD_SIZE; y++) {
        for (int x = 0; x < BOARD_SIZE; x++) {
            if (board.get(x, y) == player) {
                score += CENTER_SCORE[x][y];
            }
        }
    }
    return score;
}

// 计算单个玩家的棋型总分
// 使用序列扫描法：每个方向扫描连续序列，避免重复计算
// outFour/outOpen3/outClosed3 可选：回传加尾分前的三/四计数（供增量评估使用）
int64_t evaluatePatterns(const Board& board, int player,
                         int* outFour, int* outOpen3, int* outClosed3) {
    int64_t score = 0;
    int fourCount = 0;
    int openThreeCount = 0;
    int closedThreeCount = 0;
    
    for (int d = 0; d < 4; d++) {
        int dx = DX[d], dy = DY[d];
        
        for (int y = 0; y < BOARD_SIZE; y++) {
            for (int x = 0; x < BOARD_SIZE; x++) {
                // 只从序列的起始位置开始计数（前一个不是自己的棋子）
                int px = x - dx, py = y - dy;
                if (board.inBounds(px, py) && board.get(px, py) == player) continue;
                
                // 统计连续棋子数
                int count = 0;
                int cx = x, cy = y;
                while (board.inBounds(cx, cy) && board.get(cx, cy) == player) {
                    count++;
                    cx += dx;
                    cy += dy;
                }
                
                if (count == 0) continue;
                
                // 检查两端开放情况
                int bx = x - dx, by = y - dy;
                bool bopen = board.inBounds(bx, by) && board.get(bx, by) == EMPTY;
                bool aopen = board.inBounds(cx, cy) && board.get(cx, cy) == EMPTY;
                int openEnds = (bopen ? 1 : 0) + (aopen ? 1 : 0);
                
                if (count >= 5) {
                    score += FIVE;
                } else if (count == 4) {
                    if (openEnds == 2) { score += OPEN_FOUR; ++fourCount; }
                    else if (openEnds == 1) { score += CLOSED_FOUR; ++fourCount; }
                } else if (count == 3) {
                    if (openEnds == 2) { score += OPEN_THREE; ++openThreeCount; }
                    else if (openEnds == 1) { score += SLEEP_THREE; ++closedThreeCount; }
                } else if (count == 2) {
                    if (openEnds == 2) score += OPEN_TWO;
                    else if (openEnds == 1) score += SLEEP_TWO;
                } else if (count == 1) {
                    if (openEnds == 2) score += OPEN_ONE;
                }
            }
        }
    }

    // 双重威胁：两条独立的杀线远比线性相加更致命
    if (fourCount >= 2) {
        score += DOUBLE_FOUR;
    } else if (fourCount == 1 && (openThreeCount + closedThreeCount) >= 1) {
        score += FOUR_THREE;
    } else if (openThreeCount >= 2) {
        score += DOUBLE_THREE;
    } else if (openThreeCount == 1 && closedThreeCount >= 1) {
        // 一攻一守的双三同样难以兼顾，按双三的一半计
        score += DOUBLE_THREE / 2;
    }
    
    if (outFour) *outFour = fourCount;
    if (outOpen3) *outOpen3 = openThreeCount;
    if (outClosed3) *outClosed3 = closedThreeCount;
    return score;
}

// ============= 增量棋型评估 =============
// 与全盘扫描 evaluatePatterns 逐位一致的增量版本。搜索中每个节点的每个候选都要
// 评估「在 (x,y) 放子后该颜色的全盘分」，原实现是临时放子后全盘扫两遍（每候选）。
// 这里改为：每个节点先算一次基准分（本盘面固定，所有候选共用），候选只修正穿过
// (x,y) 的四条线上的「跑」——语义完全一致，开销从 O(盘面) 降到 O(线长)。

struct PatternBase {
    int64_t score = 0;  // 普通线分 + 尾分（与 evaluatePatterns 同口径）
    int four = 0;       // 加尾分前的三/四计数
    int open3 = 0;
    int closed3 = 0;
};

// 单条「跑」的贡献与计数（规则逐字对齐 evaluatePatterns 内联逻辑）
static inline void addRunContribution(int64_t& score, int count, int openEnds,
                                      int& four, int& open3, int& closed3) {
    if (count >= 5) {
        score += FIVE;
    } else if (count == 4) {
        if (openEnds == 2) { score += OPEN_FOUR; ++four; }
        else if (openEnds == 1) { score += CLOSED_FOUR; ++four; }
    } else if (count == 3) {
        if (openEnds == 2) { score += OPEN_THREE; ++open3; }
        else if (openEnds == 1) { score += SLEEP_THREE; ++closed3; }
    } else if (count == 2) {
        if (openEnds == 2) score += OPEN_TWO;
        else if (openEnds == 1) score += SLEEP_TWO;
    } else if (count == 1) {
        if (openEnds == 2) score += OPEN_ONE;
    }
}

// 双重威胁尾分（与 evaluatePatterns 末尾规则一致）
static inline int64_t tailThreatBonus(int four, int open3, int closed3) {
    if (four >= 2) return DOUBLE_FOUR;
    if (four == 1 && (open3 + closed3) >= 1) return FOUR_THREE;
    if (open3 >= 2) return DOUBLE_THREE;
    if (open3 == 1 && closed3 >= 1) return DOUBLE_THREE / 2;
    return 0;
}

// 双色一趟扫完：与分别调用两次 evaluatePatterns 逐位一致
static void evaluatePatternsBoth(const Board& board, PatternBase& black, PatternBase& white) {
    black = PatternBase();
    white = PatternBase();
    for (int d = 0; d < 4; d++) {
        int dx = DX[d], dy = DY[d];
        for (int y = 0; y < BOARD_SIZE; y++) {
            for (int x = 0; x < BOARD_SIZE; x++) {
                int v = board.get(x, y);
                if (v == EMPTY) continue;
                int px = x - dx, py = y - dy;
                if (board.inBounds(px, py) && board.get(px, py) == v) continue;
                int count = 0;
                int cx = x, cy = y;
                while (board.inBounds(cx, cy) && board.get(cx, cy) == v) {
                    count++;
                    cx += dx;
                    cy += dy;
                }
                if (count == 0) continue;
                bool bopen = board.inBounds(px, py) && board.get(px, py) == EMPTY;
                bool aopen = board.inBounds(cx, cy) && board.get(cx, cy) == EMPTY;
                int openEnds = (bopen ? 1 : 0) + (aopen ? 1 : 0);
                PatternBase& t = (v == BLACK) ? black : white;
                addRunContribution(t.score, count, openEnds, t.four, t.open3, t.closed3);
            }
        }
    }
    black.score += tailThreatBonus(black.four, black.open3, black.closed3);
    white.score += tailThreatBonus(white.four, white.open3, white.closed3);
}

// 增量：棋盘（不含 (x,y) 处的棋子）在 stone 颜色下基准为 base 时，
// 计算在 (x,y) 放置 stone 后的全盘分（与放子后全盘扫描逐位一致）。
static int64_t patternScoreAfterPlace(const Board& board, int x, int y, int stone, const PatternBase& base) {
    int64_t preScore = 0, addScore = 0;
    int preFour = 0, preOpen3 = 0, preClosed3 = 0;
    int addFour = 0, addOpen3 = 0, addClosed3 = 0;
    for (int d = 0; d < 4; d++) {
        int dx = DX[d], dy = DY[d];
        // 反向 / 正向相邻的同色连子长度
        int backLen = 0;
        while (board.inBounds(x - (backLen + 1) * dx, y - (backLen + 1) * dy) &&
               board.get(x - (backLen + 1) * dx, y - (backLen + 1) * dy) == stone) ++backLen;
        int fwdLen = 0;
        while (board.inBounds(x + (fwdLen + 1) * dx, y + (fwdLen + 1) * dy) &&
               board.get(x + (fwdLen + 1) * dx, y + (fwdLen + 1) * dy) == stone) ++fwdLen;
        // 被合并的两条旧跑（放子前，靠 (x,y) 一侧必为开放——(x,y) 放子前是空位）
        if (backLen > 0) {
            int bx = x - (backLen + 1) * dx, by = y - (backLen + 1) * dy;
            bool farOpen = board.inBounds(bx, by) && board.get(bx, by) == EMPTY;
            addRunContribution(preScore, backLen, farOpen ? 2 : 1, preFour, preOpen3, preClosed3);
        }
        if (fwdLen > 0) {
            int fx = x + (fwdLen + 1) * dx, fy = y + (fwdLen + 1) * dy;
            bool farOpen = board.inBounds(fx, fy) && board.get(fx, fy) == EMPTY;
            addRunContribution(preScore, fwdLen, farOpen ? 2 : 1, preFour, preOpen3, preClosed3);
        }
        // 合并后的新跑
        int bx = x - (backLen + 1) * dx, by = y - (backLen + 1) * dy;
        int fx = x + (fwdLen + 1) * dx, fy = y + (fwdLen + 1) * dy;
        int openEnds = 0;
        if (board.inBounds(bx, by) && board.get(bx, by) == EMPTY) ++openEnds;
        if (board.inBounds(fx, fy) && board.get(fx, fy) == EMPTY) ++openEnds;
        addRunContribution(addScore, backLen + fwdLen + 1, openEnds, addFour, addOpen3, addClosed3);
    }
    int64_t result = base.score + addScore - preScore;
    // 尾分随计数变化重新结算：先减基准尾分，再加新计数的尾分
    result += tailThreatBonus(base.four + addFour - preFour,
                              base.open3 + addOpen3 - preOpen3,
                              base.closed3 + addClosed3 - preClosed3);
    result -= tailThreatBonus(base.four, base.open3, base.closed3);
    return result;
}

// 完整评估（当前视角的分数）。棋型用双色单遍扫描、中心分合并成一趟——
// 与原实现（两次 evaluatePatterns + 两次 evaluateCenterScore）逐位一致。
int64_t evaluatePosition(const Board& board) {
    int player = board.getPlayer();
    
    PatternBase black, white;
    evaluatePatternsBoth(board, black, white);
    int64_t ownScore = (player == BLACK) ? black.score : white.score;
    int64_t enemyScore = (player == BLACK) ? white.score : black.score;
    
    // 中心分（权重较低，辅助决策）：一趟合并计算（黑为正、白为负）
    int64_t centerDiff = 0;
    for (int y = 0; y < BOARD_SIZE; y++) {
        for (int x = 0; x < BOARD_SIZE; x++) {
            int v = board.get(x, y);
            if (v == BLACK) centerDiff += CENTER_SCORE[x][y];
            else if (v == WHITE) centerDiff -= CENTER_SCORE[x][y];
        }
    }
    
    // 略偏防守：对手威胁权重稍高
    int64_t result = ownScore - (int64_t)(enemyScore * DEFENSE_WEIGHT) 
                    + ((player == BLACK) ? centerDiff : -centerDiff) / 10;
    
    // 限制评估分数，避免超过 WIN_THRESHOLD（只有 checkWin 才能返回 WIN_THRESHOLD）
    if (result > WIN_THRESHOLD - 1000) result = WIN_THRESHOLD - 1000;
    if (result < -(WIN_THRESHOLD - 1000)) result = -(WIN_THRESHOLD - 1000);
    
    return result;
}

// ============= 快速威胁检测 =============

// 检查在 (x,y) 落子后是否形成连五
static bool createsFive(const Board& board, int x, int y, int player) {
    for (int d = 0; d < 4; d++) {
        int count = 1;
        // 正向
        int cx = x + DX[d], cy = y + DY[d];
        while (board.inBounds(cx, cy) && board.get(cx, cy) == player) {
            count++; cx += DX[d]; cy += DY[d];
        }
        // 反向
        cx = x - DX[d]; cy = y - DY[d];
        while (board.inBounds(cx, cy) && board.get(cx, cy) == player) {
            count++; cx -= DX[d]; cy -= DY[d];
        }
        if (count >= 5) return true;
    }
    return false;
}

// 检查在 (x,y) 落子后是否形成活四/冲四
// 返回 1=活四, 0=冲四, -1=无
static int createsFour(const Board& board, int x, int y, int player) {
    int best = -1;
    
    for (int d = 0; d < 4; d++) {
        int dx = DX[d], dy = DY[d];
        
        // 统计连续棋子
        int count = 1;
        int cx = x + dx, cy = y + dy;
        while (board.inBounds(cx, cy) && board.get(cx, cy) == player) {
            count++; cx += dx; cy += dy;
        }
        int ex = cx, ey = cy;  // 正向结束位置
        
        cx = x - dx; cy = y - dy;
        while (board.inBounds(cx, cy) && board.get(cx, cy) == player) {
            count++; cx -= dx; cy -= dy;
        }
        int sx = cx, sy = cy;  // 反向结束位置
        
        if (count == 4) {
            // 检查两端是否开放
            bool end1_open = board.inBounds(ex, ey) && board.get(ex, ey) == EMPTY;
            bool end2_open = board.inBounds(sx, sy) && board.get(sx, sy) == EMPTY;
            if (end1_open && end2_open) {
                if (best < 1) best = 1;  // 活四
            } else if (end1_open || end2_open) {
                if (best < 0) best = 0;  // 冲四
            }
        } else if (count == 3) {
            // 跳活三检查：XXX0X 或 X0XXX
            // 正向检查
            if (board.inBounds(ex, ey) && board.get(ex, ey) == EMPTY) {
                int nx = ex + dx, ny = ey + dy;
                if (board.inBounds(nx, ny) && board.get(nx, ny) == player) {
                    int nx2 = nx + dx, ny2 = ny + dy;
                    bool end_open = board.inBounds(nx2, ny2) && board.get(nx2, ny2) == EMPTY;
                    bool start_open = board.inBounds(sx, sy) && board.get(sx, sy) == EMPTY;
                    if (end_open && start_open && best < 0) best = 0;  // 跳活三≈冲四
                }
            }
            // 反向检查
            cx = x + dx; cy = y + dy;
            // 实际上跳活三检测比较复杂，简化处理
        }
    }
    
    return best;
}

// 检查在 (x,y) 落子后是否形成活三
static bool createsOpenThree(const Board& board, int x, int y, int player) {
    for (int d = 0; d < 4; d++) {
        int dx = DX[d], dy = DY[d];
        
        int count = 1;
        int cx = x + dx, cy = y + dy;
        while (board.inBounds(cx, cy) && board.get(cx, cy) == player) {
            count++; cx += dx; cy += dy;
        }
        int ex = cx, ey = cy;
        
        cx = x - dx; cy = y - dy;
        while (board.inBounds(cx, cy) && board.get(cx, cy) == player) {
            count++; cx -= dx; cy -= dy;
        }
        int sx = cx, sy = cy;
        
        if (count == 3) {
            bool end1_open = board.inBounds(ex, ey) && board.get(ex, ey) == EMPTY;
            bool end2_open = board.inBounds(sx, sy) && board.get(sx, sy) == EMPTY;
            if (end1_open && end2_open) return true;
        }
    }
    return false;
}

static int splitThreeDirection(const Board& board, int x, int y, int player) {
    for (int d = 0; d < 4; ++d) {
        int dx = DX[d];
        int dy = DY[d];

        if (board.get(x + dx, y + dy) == player &&
            board.get(x + 2 * dx, y + 2 * dy) == EMPTY &&
            board.get(x + 3 * dx, y + 3 * dy) == player &&
            board.get(x + 4 * dx, y + 4 * dy) == EMPTY) {
            return d;
        }

        int startX = x - 4 * dx;
        int startY = y - 4 * dy;
        if (board.get(startX, startY) == EMPTY &&
            board.get(startX + dx, startY + dy) == player &&
            board.get(startX + 2 * dx, startY + 2 * dy) == EMPTY &&
            board.get(startX + 3 * dx, startY + 3 * dy) == player) {
            return d;
        }
    }
    return -1;
}

static bool isHorizontalOpenThreeEndpoint(const Board& board, int x, int y, int player) {
    const int dx = 1;
    const int dy = 0;
    if (board.get(x + dx, y + dy) == player &&
        board.get(x + 2 * dx, y + 2 * dy) == player &&
        board.get(x + 3 * dx, y + 3 * dy) == player &&
        board.get(x + 4 * dx, y + 4 * dy) == EMPTY) {
        return true;
    }
    int startX = x - 4 * dx;
    int startY = y - 4 * dy;
    return board.get(startX, startY) == EMPTY &&
           board.get(startX + dx, startY + dy) == player &&
           board.get(startX + 2 * dx, startY + 2 * dy) == player &&
           board.get(startX + 3 * dx, startY + 3 * dy) == player;
}

static int64_t openThreeEndpointPriority(const Board& board, int x, int y, int player) {
    int64_t best = 0;
    for (int d = 0; d < 4; ++d) {
        int dx = DX[d];
        int dy = DY[d];
        if (board.get(x + dx, y + dy) == player &&
            board.get(x + 2 * dx, y + 2 * dy) == player &&
            board.get(x + 3 * dx, y + 3 * dy) == player &&
            board.get(x + 4 * dx, y + 4 * dy) == EMPTY) {
            int64_t value = OPEN_THREE * 100;
            for (int k = 1; k <= 3; ++k) {
                int sx = x + k * dx;
                int sy = y + k * dy;
                value += CENTER_SCORE[sx][sy];
            }
            best = std::max(best, value);
        }

        int startX = x - 4 * dx;
        int startY = y - 4 * dy;
        if (board.get(startX, startY) == EMPTY &&
            board.get(startX + dx, startY + dy) == player &&
            board.get(startX + 2 * dx, startY + 2 * dy) == player &&
            board.get(startX + 3 * dx, startY + 3 * dy) == player) {
            int64_t value = OPEN_THREE * 100;
            for (int k = 1; k <= 3; ++k) {
                value += CENTER_SCORE[startX + k * dx][startY + k * dy];
            }
            best = std::max(best, value);
        }
    }
    return best;
}

static bool isCrossThreatIntersection(const Board& board, int x, int y, int player) {
    if (board.get(x, y) != EMPTY) return false;

    int axes = 0;
    for (int d = 0; d < 4; ++d) {
        int dx = DX[d];
        int dy = DY[d];
        if (board.get(x - 2 * dx, y - 2 * dy) == EMPTY &&
            board.get(x - dx, y - dy) == player &&
            board.get(x + dx, y + dy) == player &&
            board.get(x + 2 * dx, y + 2 * dy) == EMPTY) {
            ++axes;
        }
    }
    return axes >= 2;
}

static constexpr int64_t CROSS_THREAT_BONUS = 40000000;
static constexpr int64_t DIAGONAL_SPLIT_THREE_BONUS = 30000000;
static constexpr int64_t OPEN_THREE_ENDPOINT_BONUS = 5;
// 争夺激烈程度：仅用于打破近似并列，不改变真实威胁的优先级
static constexpr int64_t CONTESTED_WEIGHT = 100;
// 开局贴邻加成：棋盘还很稀疏时（前几手），贴着已有棋子下（切比雪夫距离 <=1）。
// 用户指定的行为：AI 的第一/第二个回应必须贴着下，不能隔一格跳开。
// 只在前几手生效，量级可调；不改搜索内容，只参与同分/近分的取舍。
static constexpr int64_t OPENING_ADJACENT_BONUS = 300000;

// ===== 将杀距离 =====
// 终局分不再固定为 +-WIN_THRESHOLD，而是把「距离」编进分值：
//   赢得越快分越高、输得越晚分越高。
// 否则引擎对「立刻输」和「十步后输」一视同仁（都是 -1000 万），
// 必败局面里的取舍就只能靠位置类加成猜，实测会挑到输得更快的一手。
// 距离一格用 8 分，保证同距离比较不会被子项 tie-break（最多 +-3）盖过。
static constexpr int64_t MATE_PLY_VALUE = 8;
static constexpr int64_t MATE_MAX_PLY = 32;
static constexpr int64_t WIN_SCORE_FLOOR = WIN_THRESHOLD - 1 - MATE_MAX_PLY * MATE_PLY_VALUE;
static constexpr int64_t LOSS_SCORE_CEIL = -WIN_SCORE_FLOOR;
static constexpr int64_t TERMINAL_TIE = 3;   // 终局分只允许这么点的 tie-break
static constexpr int POST_SCAN_MIN_BUDGET = 4000;     // 复核要求的最低时间预算（低难度复核太浅，直接跳过）
static constexpr int VCT_FILTER_MIN_BUDGET = 4000;    // b1-B：VCT 防守补枪的最低预算（探针开销占比过大，低档跳过）
static constexpr int VCT_FILTER_NODE_LIMIT = 300000;  // b1-B：补枪探针的节点上限（原 60 万 ≈1.2-1.7s/候选，
                                                      // 实测 game12-m13 两个幸存者烧 3s 把主搜索从深度 10 压到 9
                                                      // 且零收获；30 万 ≈0.6s/候选，保留能力、砍半成本）
// 根节点战术加成限幅：不得把分数线推离搜索语义（未限幅时实测可达 ±4800 万，盖过搜索结论）
static constexpr int64_t MAX_TACTICAL_ADJUST = 10000000;
static constexpr size_t POST_SCAN_MAX_FINALISTS = 5;  // 决赛候选数超过这个就不复核

// 距根 k 层发生：k 越大越晚
static inline int64_t mateWinScore(int64_t k) {
    if (k > MATE_MAX_PLY) k = MATE_MAX_PLY;
    return WIN_THRESHOLD - 1 - k * MATE_PLY_VALUE;
}
static inline int64_t mateLossScore(int64_t k) { return -mateWinScore(k); }
// 节点剩余深度 d 对应的距离（同一轮搜索内单调即可）
static inline int64_t mateDistFromDepth(int64_t d) { return MATE_MAX_PLY - d; }

static int64_t potentialThreatScore(Board& board, int player) {
    int64_t score = 0;
    for (int y = 0; y < BOARD_SIZE; ++y) {
        for (int x = 0; x < BOARD_SIZE; ++x) {
            if (board.get(x, y) != EMPTY) continue;

            bool nearStone = false;
            for (int dy = -NEIGHBOR_RADIUS; dy <= NEIGHBOR_RADIUS && !nearStone; ++dy) {
                for (int dx = -NEIGHBOR_RADIUS; dx <= NEIGHBOR_RADIUS && !nearStone; ++dx) {
                    int nx = x + dx;
                    int ny = y + dy;
                    if (board.inBounds(nx, ny) && board.get(nx, ny) != EMPTY) {
                        nearStone = true;
                    }
                }
            }
            if (!nearStone) continue;

            if (createsFive(board, x, y, player)) {
                score += FIVE;
                continue;
            }
            int four = createsFour(board, x, y, player);
            if (four == 1) {
                score += OPEN_FOUR;
            } else if (four == 0) {
                score += CLOSED_FOUR;
            } else if (createsOpenThree(board, x, y, player)) {
                score += OPEN_THREE * 10;
            }
        }
    }
    return score;
}

// 计算候选点的快速排序分数（ownBase/enemyBase 为本节点盘面的基准棋型分，见
// patternScoreAfterPlace：它是与全盘扫描逐位一致的增量评估）
static int64_t quickMoveScore(Board& board, int x, int y,
                              const PatternBase& ownBase, const PatternBase& enemyBase) {
    int player = board.getPlayer();
    int enemy = (player == BLACK) ? WHITE : BLACK;
    
    int64_t score = 0;
    
    // 1. 己方立即成五
    if (createsFive(board, x, y, player)) {
        return 50000000;
    }
    
    // 2. 挡对方立即成五
    if (createsFive(board, x, y, enemy)) {
        return 45000000;
    }

    if (isCrossThreatIntersection(board, x, y, enemy)) {
        return CROSS_THREAT_BONUS;
    }

    // 3. 己方成活四
    int ownFour = createsFour(board, x, y, player);
    if (ownFour == 1) return 30000000;  // 活四
    if (ownFour == 0) return 15000000;  // 冲四
    
    // 4. 挡对方活四
    int enemyFour = createsFour(board, x, y, enemy);
    if (enemyFour == 1) return 25000000;
    if (enemyFour == 0) return 12000000;
    
    // 5. 己方成活三
    if (createsOpenThree(board, x, y, player)) {
        score += 8000000;
    }
    
    // 6. 挡对方活三
    if (createsOpenThree(board, x, y, enemy)) {
        score += 6000000;
    }

    // 7. 中心加权 + 棋型评分
    int cx = BOARD_SIZE / 2;
    int cy = BOARD_SIZE / 2;
    int dist = std::max(std::abs(x - cx), std::abs(y - cy));
    score += std::max(0, 100 - dist * 16) * 10;  // 中心分
    
    // 8. 简单棋型评分（评估该点周围的棋型）——增量版：基准分按节点算一次，
    //    这里只修正穿过 (x,y) 的四条线；与原「临时放子 + 全盘扫描」逐位一致。
    int64_t newOwnScore = patternScoreAfterPlace(board, x, y, player, ownBase);
    int64_t newEnemyScore = patternScoreAfterPlace(board, x, y, enemy, enemyBase);
    
    score += (newOwnScore - (int64_t)(newEnemyScore * 1.0)) / 100;  // 缩放
    
    return score;
}

// ============= 候选点生成 =============

std::vector<Move> generateCandidates(Board& board, int maxCandidates, bool isRoot) {
    std::vector<Move> candidates;
    candidates.reserve(64);
    
    // 空棋盘特殊情况
    if (board.moveCount() == 0) {
        candidates.push_back(Move{Point(BOARD_SIZE / 2, BOARD_SIZE / 2), board.getPlayer(), 0});
        return candidates;
    }
    
    // 本节点盘面固定：基准棋型分只算一次，供所有候选的增量评估共用
    PatternBase ownBase, enemyBase;
    {
        int player = board.getPlayer();
        int enemy = (player == BLACK) ? WHITE : BLACK;
        ownBase.score = evaluatePatterns(board, player, &ownBase.four, &ownBase.open3, &ownBase.closed3);
        enemyBase.score = evaluatePatterns(board, enemy, &enemyBase.four, &enemyBase.open3, &enemyBase.closed3);
    }
    
    // 1. 邻域过滤：只考虑已有棋子附近的空位
    for (int y = 0; y < BOARD_SIZE; y++) {
        for (int x = 0; x < BOARD_SIZE; x++) {
            if (board.get(x, y) != EMPTY) continue;
            
            // 检查是否在已有棋子的半径 NEIGHBOR_RADIUS 内
            bool nearStone = false;
            for (int dy = -NEIGHBOR_RADIUS; dy <= NEIGHBOR_RADIUS && !nearStone; dy++) {
                for (int dx = -NEIGHBOR_RADIUS; dx <= NEIGHBOR_RADIUS && !nearStone; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    int nx = x + dx, ny = y + dy;
                    if (board.inBounds(nx, ny) && board.get(nx, ny) != EMPTY) {
                        nearStone = true;
                    }
                }
            }
            
            if (nearStone) {
                Move m;
                m.pos = Point(x, y);
                m.player = board.getPlayer();
                m.score = quickMoveScore(board, x, y, ownBase, enemyBase);
                candidates.push_back(m);
            }
        }
    }
    
    // 2. 排序（按分数降序）
    std::stable_sort(candidates.begin(), candidates.end(),
        [](const Move& a, const Move& b) {
            return a.score > b.score;
        });

    // 3. 截取 top N
    if (candidates.size() > maxCandidates) {
        candidates.resize(maxCandidates);
    }

    return candidates;
}

// ============= NegaMax 搜索 =============

static int64_t negamax(Board& board, int64_t alpha, int64_t beta, int depth, uint64_t hash) {
    ctx().nodesVisited++;
    
    // 时间检查
    if (checkTime()) {
        return 0;
    }
    
    // 胜负检查
    if (board.moveCount() > 0) {
        const Move& lastMove = board.getHistory(board.moveCount() - 1);
        if (board.checkWin(lastMove.pos.x, lastMove.pos.y) != 0) {
            return mateLossScore(mateDistFromDepth(depth));  // 上一手赢了，当前方输了（越晚输分越高）
        }
    }
    
    if (board.isFull()) return 0;
    
    if (depth <= 0) {
        return evaluatePosition(board);
    }
    
    // 置换表查找
    TTProbe tt = ttLookup(hash, depth);
    if (tt.hit) {
        int32_t ttScore = tt.score;
        if (tt.flag == 0) return ttScore;  // EXACT
        if (tt.flag == 1 && ttScore > alpha) alpha = ttScore;  // LOWER
        if (tt.flag == 2 && ttScore < beta) beta = ttScore;  // UPPER
        if (alpha >= beta) return ttScore;
    }
    
    // 生成候选点
    int limit = (depth >= g_maxDepth) ? ROOT_CANDIDATE_LIMIT : CHILD_CANDIDATE_LIMIT;
    std::vector<Move> candidates = generateCandidates(board, limit, depth >= g_maxDepth);
    
    if (candidates.empty()) return 0;
    
    // 如果有置换表的最佳走法，移到最前面
    if (tt.hit && tt.bestMove >= 0) {
        int bm = tt.bestMove;
        int bmX = bm % BOARD_SIZE;
        int bmY = bm / BOARD_SIZE;
        for (auto& m : candidates) {
            if (m.pos.x == bmX && m.pos.y == bmY) {
                std::swap(m, candidates[0]);
                break;
            }
        }
    }
    
    int64_t bestScore = -INT64_MAX;
    int bestX = -1, bestY = -1;
    
    for (const auto& move : candidates) {
        // 落子并切换到对手回合
        int player = board.getPlayer();
        board.makeMove(move.pos.x, move.pos.y);
        uint64_t newHash = hash ^ zobrist[move.pos.x][move.pos.y][player];
        
        // 检查是否获胜
        int64_t score;
        if (board.checkWin(move.pos.x, move.pos.y) != 0) {
            score = mateWinScore(mateDistFromDepth(depth));
        } else {
            score = -negamax(board, -beta, -alpha, depth - 1, newHash);
        }
        
        // 收子并恢复当前回合
        board.undo();

        // 超时后当前候选的结果不完整，不能覆盖已完成候选
        if (ctx().timedOut) break;

        if (score > bestScore) {
            bestScore = score;
            bestX = move.pos.x;
            bestY = move.pos.y;
        }
        
        alpha = std::max(alpha, bestScore);
        if (alpha >= beta) break;  // Beta 剪枝
    }
    
    // 仅缓存完整搜索结果，超时结果不能污染置换表
    if (!ctx().timedOut) {
        ttStore(hash, depth, bestScore, bestX, bestY, alpha, beta);
    }
    
    return bestScore;
}

// ============= VCF（连续冲四杀）搜索 =============
//
// 常规 alpha-beta 在困难档 12s 只能到 8-9 层，而「连冲四」型杀棋常需要 10 层
// 以上才看得见（实测：某必败局面的证明恰好从 depth 8 才出现，从更早一手出发
// 需要 depth 10，引擎够不到，于是把必败局面评成了 +4600 万）。
//
// VCF 只考虑造四的着法：对手每一手都被迫挡五，分支比全宽搜索窄一两个数量级，
// 同样时间能搜到十几手冲四的深度，正好补这个盲区。
//
// 约定：搜索期间攻击方始终轮到自己走；落子/撤子用 board.set，不触碰历史与回合。

static constexpr int VCF_MAX_FOURS = 16;       // 最多连续冲四手数（进攻用）
// 防守只挡「短连冲四」。长链（十几手冲四）实战几乎兑现不了，而被它剔掉的
// 往往是「挡一手」这类拖延着法——局面本来就输时反而更差。实测：某个已必败
// 局面上黑棋的取胜链长 13 手冲四，若一并剔除，引擎会放弃挡四直接输。
static constexpr int VCF_DEFENSE_FOURS = 6;
static constexpr int VCF_MAX_FOUR_MOVES = 16;  // 单节点最多尝试的造四着法
static constexpr int VCF_MAX_FIVE_PTS = 8;     // 单节点最多收集的五连点

struct VcfPoint { int x, y; };

// 攻击方在 (x,y) 落子后，经过该点的四条线上「再落一子即成五连」的空点。
// 新出现的五连点必然包含这一手，所以只看这四条线即可。
static int vcfFivePointsAround(const Board& b, int x, int y, int attacker,
                               VcfPoint* out, int maxOut) {
    int count = 0;
    for (int d = 0; d < 4; d++) {
        int val[9], px[9], py[9];
        for (int k = -4; k <= 4; k++) {
            int i = k + 4;
            int nx = x + DX[d] * k, ny = y + DY[d] * k;
            if (!b.inBounds(nx, ny)) {
                val[i] = -2; px[i] = -1; py[i] = -1;
            } else {
                val[i] = b.get(nx, ny); px[i] = nx; py[i] = ny;
            }
        }
        for (int s = 0; s + 5 <= 9; s++) {
            int own = 0, empt = 0, ei = -1;
            for (int t = 0; t < 5; t++) {
                if (val[s + t] == attacker) own++;
                else if (val[s + t] == EMPTY) { empt++; ei = s + t; }
            }
            if (own == 4 && empt == 1 && ei >= 0 && px[ei] >= 0) {
                bool dup = false;
                for (int i = 0; i < count && i < maxOut; i++) {
                    if (out[i].x == px[ei] && out[i].y == py[ei]) { dup = true; break; }
                }
                if (!dup) {
                    if (count < maxOut) { out[count].x = px[ei]; out[count].y = py[ei]; }
                    count++;
                }
            }
        }
    }
    return count;
}

// 某方「立即成五」的点数（最多计数 2 个即可区分 0/1/多），可选出第一个点
static int vcfCountImmediateFives(const Board& b, int player, VcfPoint* firstOut) {
    int count = 0;
    for (int y = 0; y < BOARD_SIZE; y++) {
        for (int x = 0; x < BOARD_SIZE; x++) {
            if (b.get(x, y) != EMPTY) continue;
            bool nearStone = false;
            for (int dy = -NEIGHBOR_RADIUS; dy <= NEIGHBOR_RADIUS && !nearStone; dy++) {
                for (int dx = -NEIGHBOR_RADIUS; dx <= NEIGHBOR_RADIUS && !nearStone; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    int nx = x + dx, ny = y + dy;
                    if (b.inBounds(nx, ny) && b.get(nx, ny) != EMPTY) nearStone = true;
                }
            }
            if (!nearStone) continue;
            if (createsFive(b, x, y, player)) {
                if (count == 0 && firstOut) { firstOut->x = x; firstOut->y = y; }
                if (++count >= 2) return count;
            }
        }
    }
    return count;
}

static inline bool vcfTimeUp(int64_t deadlineMs) {
    if (ctx().vcfNodes > ctx().vcfNodeLimit) return true;    // 节点上限兜底
    if ((ctx().vcfNodes & 0x3F) != 0) return false;      // 每 64 节点查一次时间
    return getElapsedMs() > deadlineMs;
}

// 递归核：attacker 轮到自己走，能否靠连续冲四取胜。
// rootX/rootY 仅在顶层传入，成功时写出取胜的第一手。
static bool vcfSearch(Board& b, int attacker, int defender, int foursLeft,
                      int64_t deadlineMs, int* rootX, int* rootY) {
    if (foursLeft <= 0) return false;
    ctx().vcfNodes++;
    if (vcfTimeUp(deadlineMs)) return false;

    // 后手若有立即成五的点，攻击方的四必须正好挡在它上面，否则后手抢先成五
    VcfPoint defFive;
    int defFiveCount = vcfCountImmediateFives(b, defender, &defFive);

    struct FourMove { int x, y, pts; VcfPoint first; };
    FourMove fm[VCF_MAX_FOUR_MOVES];
    int fmN = 0;

    for (int y = 0; y < BOARD_SIZE; y++) {
        for (int x = 0; x < BOARD_SIZE; x++) {
            if (b.get(x, y) != EMPTY) continue;
            bool nearStone = false;
            for (int dy = -NEIGHBOR_RADIUS; dy <= NEIGHBOR_RADIUS && !nearStone; dy++) {
                for (int dx = -NEIGHBOR_RADIUS; dx <= NEIGHBOR_RADIUS && !nearStone; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    int nx = x + dx, ny = y + dy;
                    if (b.inBounds(nx, ny) && b.get(nx, ny) != EMPTY) nearStone = true;
                }
            }
            if (!nearStone) continue;

            b.set(x, y, attacker);
            bool five = createsFive(b, x, y, attacker);
            VcfPoint pts[VCF_MAX_FIVE_PTS];
            int n = five ? 0 : vcfFivePointsAround(b, x, y, attacker, pts, VCF_MAX_FIVE_PTS);
            b.set(x, y, EMPTY);

            if (five) {
                // 直接成五，后手来不及
                if (rootX) { *rootX = x; *rootY = y; }
                ctx().vcfWinFours = VCF_MAX_FOURS - foursLeft + 1;
                return true;
            }
            if (n == 0) continue;            // 不是造四着法
            if (fmN < VCF_MAX_FOUR_MOVES) {
                fm[fmN].x = x; fm[fmN].y = y; fm[fmN].pts = n;
                fm[fmN].first = pts[0];
                fmN++;
            }
        }
    }

    if (fmN == 0) return false;              // 没有造四着法，VCF 到此为止

    for (int i = 0; i < fmN; i++) {
        int px = fm[i].x, py = fm[i].y;
        // 后手成五在先：除非这一手正好挡掉它（且只有一个成五点）
        if (defFiveCount >= 2) continue;
        if (defFiveCount == 1 && !(px == defFive.x && py == defFive.y)) continue;

        if (fm[i].pts >= 2) {
            // 活四/双四：对面挡不全
            if (rootX) { *rootX = px; *rootY = py; }
            ctx().vcfWinFours = VCF_MAX_FOURS - foursLeft + 1;
            return true;
        }

        b.set(px, py, attacker);
        b.set(fm[i].first.x, fm[i].first.y, defender);   // 被迫挡五
        bool ok = vcfSearch(b, attacker, defender, foursLeft - 1, deadlineMs, nullptr, nullptr);
        b.set(fm[i].first.x, fm[i].first.y, EMPTY);
        b.set(px, py, EMPTY);
        if (ok) {
            if (rootX) { *rootX = px; *rootY = py; }
            return true;                     // 手数由检测到必胜的那一层记录
        }
    }
    return false;
}

// 当前局面轮走方是否存在「连续冲四」必胜（VCF）。
// 供自检/测试使用：返回 true 表示存在只靠冲四就能赢的线索。
bool hasVcfWin(Board& board) {
    ctx().vcfNodes = 0;
    const int savedLimit = ctx().vcfNodeLimit;
    ctx().vcfNodeLimit = 200000;                 // 查询用途：快速给出结论
    int attacker = board.getPlayer();
    int defender = (attacker == BLACK) ? WHITE : BLACK;
    bool win = vcfSearch(board, attacker, defender, VCF_MAX_FOURS, INT64_MAX, nullptr, nullptr);
    ctx().vcfNodeLimit = savedLimit;    return win;
}

// ============= 战术搜索置换表 =============
//
// 威胁搜索的递归里，同一局面会被不同的走法顺序反复展开（三的应手分支尤其
// 容易回到同一局面），没有置换表时深链证明的代价是节点数的平方级。
//
// 复用规则靠单调性，安全且简单：
//   - 已证必胜：手数够（>= 立此结论时的手数）就成立
//   - 未证必胜：手数更少时不成立
// 「未证必胜」只在搜索穷尽时才是结论；被节点/时间上限打断的返回不带结论，
// 但缓存它只会让搜索引擎更保守（少报必胜、少剔除候选），不会造成误报。

// 局面哈希 + 攻方（同一局面可能出现攻守互换的两套结论）
static inline uint64_t threatKey(const Board& b, int attacker) {
    return computeHash(b) ^ ((uint64_t)attacker * 0x9E3779B97F4A7C15ULL);
}

// ============= VCT（连续威胁杀）搜索 =============
//
// VCF 只搜造四着法。实战还有一类杀法：先用「三」施压（对手必须拆），再转成冲四杀。
// VCT 把「造四」和「造活四威胁的三」都当成强制手；对手的应手集合不是手写棋型规则，
// 而是按定义算出——所有能让攻方「下一手无法形成活四」的空点。少算一个应手就会把
// 败局误判成胜局，因此宁可保守：任何一处超出容量上限就判本路不成立。
//
// 两条防误判规则（都是这盘实战暴露出来的）：
//  - 后手有立即成五的点时，攻方不成五的着法一律不算强制（对手抢先）
//  - 后手存在造四着法时，攻方的「三」同样不成立（对手反先手）

static constexpr int VCT_MAX_MOVES = 8;          // 攻方最多连续威胁手数（进攻）
static constexpr int VCT_MAX_THREAT_MOVES = 16;  // 单节点最多枚举的威胁着法
static constexpr int VCT_MAX_DEF_REPLIES = 16;   // 单节点最多枚举的应手（超出即放弃）

// 注意：VCT 只用于「进攻」（找到自己的必胜线索立即走）。
// 不做防守剔除——实测它会把必败局面里「挡一手拖延」的着法也一并剔掉，
// 反而让引擎放弃防守直接速败；而它要证明的那类深链杀法又超出时间预算。

static inline bool vctTimeUp(int64_t deadlineMs) {
    if (ctx().vctNodes > ctx().vctNodeLimit) return true;
    if ((ctx().vctNodes & 0x3F) != 0) return false;
    return getElapsedMs() > deadlineMs;
}

// 邻域内是否有子（相关性剪枝：威胁必须发生在已有棋子附近）
static inline bool vctNearStone(const Board& b, int x, int y, int radius) {
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx == 0 && dy == 0) continue;
            int nx = x + dx, ny = y + dy;
            if (b.inBounds(nx, ny) && b.get(nx, ny) != EMPTY) return true;
        }
    }
    return false;
}

// 攻方在 (x,y) 落子后是否形成活四（下一手有两个以上成五点）
// 注意：必须先判空。后手挡在威胁点上时若直接覆盖落子，会把「威胁已被挡住」
// 误判成「威胁仍在」，进而把败局算成必胜（实测就是这里出的误报）。
static bool vctMakesOpenFour(Board& b, int x, int y, int attacker) {
    if (b.get(x, y) != EMPTY) return false;
    b.set(x, y, attacker);
    VcfPoint pts[VCF_MAX_FIVE_PTS];
    int n = vcfFivePointsAround(b, x, y, attacker, pts, VCF_MAX_FIVE_PTS);
    b.set(x, y, EMPTY);
    return n >= 2;
}

// 攻方「下一手能形成活四」的所有落点
static int vctOpenFourMoves(Board& b, int attacker, VcfPoint* out, int maxOut) {
    int count = 0;
    for (int y = 0; y < BOARD_SIZE; y++) {
        for (int x = 0; x < BOARD_SIZE; x++) {
            if (b.get(x, y) != EMPTY) continue;
            if (!vctNearStone(b, x, y, NEIGHBOR_RADIUS)) continue;
            if (vctMakesOpenFour(b, x, y, attacker)) {
                if (count < maxOut) { out[count].x = x; out[count].y = y; }
                if (++count >= maxOut) return count;
            }
        }
    }
    return count;
}

// 某方在 (x,y) 落子是否形成造四（下一手即可成五）
static bool vctIsFourMove(Board& b, int x, int y, int player) {
    if (b.get(x, y) != EMPTY) return false;
    b.set(x, y, player);
    bool five = createsFive(b, x, y, player);
    VcfPoint pts[VCF_MAX_FIVE_PTS];
    int n = five ? 1 : vcfFivePointsAround(b, x, y, player, pts, VCF_MAX_FIVE_PTS);
    b.set(x, y, EMPTY);
    return n > 0;
}

// 某方是否有造四着法（下一手即可成五）
static bool vctHasFourMove(Board& b, int player) {
    for (int y = 0; y < BOARD_SIZE; y++) {
        for (int x = 0; x < BOARD_SIZE; x++) {
            if (b.get(x, y) != EMPTY) continue;
            if (!vctNearStone(b, x, y, NEIGHBOR_RADIUS)) continue;
            b.set(x, y, player);
            VcfPoint pts[VCF_MAX_FIVE_PTS];
            int n = vcfFivePointsAround(b, x, y, player, pts, VCF_MAX_FIVE_PTS);
            b.set(x, y, EMPTY);
            if (n > 0) return true;
        }
    }
    return false;
}

// 攻方轮走，能否靠连续威胁（四 + 活四威胁的三）取胜
static bool vctSearch(Board& b, int attacker, int defender, int movesLeft,
                      int64_t deadlineMs, int* rootX, int* rootY) {
    if (movesLeft <= 0) return false;
    if (++ctx().vctNodes > ctx().vctNodeLimit || vctTimeUp(deadlineMs)) return false;

    // 置换表：同一局面被不同走法顺序反复展开时直接复用结论
    uint64_t th = threatKey(b, attacker);
    if (const ThreatTTEntry* e = threatTtLookup(th)) {
        if (e->result) {
            if (movesLeft >= e->depth) {
                if (rootX && e->bestMove >= 0) {
                    *rootX = e->bestMove % BOARD_SIZE;
                    *rootY = e->bestMove / BOARD_SIZE;
                }
                ctx().vctWinMovesLeft = e->depth;   // 复用证明：距离取立此结论时的手数
                return true;
            }
        } else if (movesLeft <= e->depth) {
            return false;
        }
    }
    const int ttDepth = movesLeft;

    // 攻方有成五点：直接成五
    {
        VcfPoint p;
        if (vcfCountImmediateFives(b, attacker, &p) > 0) {
            if (rootX) { *rootX = p.x; *rootY = p.y; }
            ctx().vctWinMovesLeft = movesLeft;      // 一手成五：距离本节点
            threatTtStore(th, ttDepth, true, p.x, p.y);
            return true;
        }
    }
    // 后手的成五点：攻方每一手都必须挡掉它，或者自己先成五
    VcfPoint defFive;
    int defFiveCount = vcfCountImmediateFives(b, defender, &defFive);
    if (defFiveCount >= 2) return false;   // 挡不全
    if (ctx().vctNodes > ctx().vctNodeLimit) return false;

    VcfPoint threats[VCT_MAX_THREAT_MOVES];
    int threatCount = 0;

    // 先按「邻近棋子数」排序候选，让强威胁（双三这类）先被试到。
    // 否则按 y,x 顺序扫描时，边角的四会把节点预算烧光，真正致命的一手还没轮到。
    struct VctCand { int x, y, near; };
    VctCand cands[64];
    int candCount = 0;
    for (int y = 0; y < BOARD_SIZE; y++) {
        for (int x = 0; x < BOARD_SIZE; x++) {
            if (b.get(x, y) != EMPTY) continue;
            if (!vctNearStone(b, x, y, NEIGHBOR_RADIUS)) continue;
            if (candCount >= 64) break;
            int near = 0;
            for (int dy = -2; dy <= 2; dy++) {
                for (int dx = -2; dx <= 2; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    int nx = x + dx, ny = y + dy;
                    if (b.inBounds(nx, ny) && b.get(nx, ny) != EMPTY) near++;
                }
            }
            cands[candCount].x = x; cands[candCount].y = y; cands[candCount].near = near;
            candCount++;
        }
    }
    for (int i = 1; i < candCount; i++) {          // 插入排序，就近优先
        VctCand key = cands[i];
        int j = i - 1;
        while (j >= 0 && cands[j].near < key.near) { cands[j + 1] = cands[j]; j--; }
        cands[j + 1] = key;
    }

    for (int ci = 0; ci < candCount && threatCount < VCT_MAX_THREAT_MOVES; ci++) {
        {
            const int x = cands[ci].x, y = cands[ci].y;
            if (b.get(x, y) != EMPTY) continue;

            if (!vctNearStone(b, x, y, NEIGHBOR_RADIUS)) continue;
            if (++ctx().vctNodes > ctx().vctNodeLimit || vctTimeUp(deadlineMs)) return false;
            if (defFiveCount == 1 && !(x == defFive.x && y == defFive.y)) continue;

            b.set(x, y, attacker);
            bool five = createsFive(b, x, y, attacker);
            VcfPoint pts[VCF_MAX_FIVE_PTS];
            int n = five ? 0 : vcfFivePointsAround(b, x, y, attacker, pts, VCF_MAX_FIVE_PTS);

            if (five) {                                  // 直接成五
                b.set(x, y, EMPTY);
                if (rootX) { *rootX = x; *rootY = y; }
                ctx().vctWinMovesLeft = movesLeft;      // 一手成五：距离本节点
                threatTtStore(th, ttDepth, true, x, y);
                return true;
            }
            if (n >= 2) {                                // 活四 / 双四
                b.set(x, y, EMPTY);
                if (rootX) { *rootX = x; *rootY = y; }
                ctx().vctWinMovesLeft = movesLeft;      // 活四即必胜：距离本节点
                threatTtStore(th, ttDepth, true, x, y);
                return true;
            }
            if (n == 1) {                                // 冲四：后手被迫挡
                b.set(pts[0].x, pts[0].y, defender);
                bool ok = vctSearch(b, attacker, defender, movesLeft - 1, deadlineMs, nullptr, nullptr);
                b.set(pts[0].x, pts[0].y, EMPTY);
                b.set(x, y, EMPTY);
                if (ok) { if (rootX) { *rootX = x; *rootY = y; } threatTtStore(th, ttDepth, true, x, y); return true; }
                continue;
            }

            // 不是四：看是否构成「三」威胁（下一手能形成活四）
            VcfPoint w[VCF_MAX_FIVE_PTS];
            int wc = vctOpenFourMoves(b, attacker, w, VCF_MAX_FIVE_PTS);
            if (wc == 0) { b.set(x, y, EMPTY); continue; }
            // 后手的造四着法不是一票否决：它只是必须被检查的一路应手。
            // 真正有没有否决力，交给递归判断——递归在下一层会要求攻方应对
            // 这个成五威胁（除非攻方能在应对过程中继续取胜）。

            // 后手必须用一手拆掉全部活四威胁点，否则攻方下一手成活四
            VcfPoint replies[VCT_MAX_DEF_REPLIES + 1];
            int rc = 0;
            for (int dy = 0; dy < BOARD_SIZE; dy++) {
                for (int dx = 0; dx < BOARD_SIZE; dx++) {
                    if (b.get(dx, dy) != EMPTY) continue;

                    if (!vctNearStone(b, dx, dy, 4)) continue;
                    if (++ctx().vctNodes > ctx().vctNodeLimit || vctTimeUp(deadlineMs)) { b.set(x, y, EMPTY); return false; }
                    bool isFour = vctIsFourMove(b, dx, dy, defender);
                    bool stillThreat = false;
                    if (!isFour) {
                        b.set(dx, dy, defender);
                        for (int i = 0; i < wc && !stillThreat; i++) {
                            if (vctMakesOpenFour(b, w[i].x, w[i].y, attacker)) stillThreat = true;
                        }
                        b.set(dx, dy, EMPTY);
                    }
                    if (isFour || !stillThreat) {
                        if (rc > VCT_MAX_DEF_REPLIES) continue;   // 超出容量：下面会放弃
                        replies[rc].x = dx; replies[rc].y = dy;
                        rc++;
                    }
                }
            }
            if (rc > VCT_MAX_DEF_REPLIES) { b.set(x, y, EMPTY); continue; }  // 保守放弃
            if (rc == 0) {                // 拆不掉 -> 必胜
                b.set(x, y, EMPTY);
                if (rootX) { *rootX = x; *rootY = y; }
                ctx().vctWinMovesLeft = movesLeft;      // 三威胁无解：距离本节点
                threatTtStore(th, ttDepth, true, x, y);
                return true;
            }
            bool allLose = true;
            for (int i = 0; i < rc && allLose; i++) {
                b.set(replies[i].x, replies[i].y, defender);
                bool ok = vctSearch(b, attacker, defender, movesLeft - 1, deadlineMs, nullptr, nullptr);
                b.set(replies[i].x, replies[i].y, EMPTY);
                if (!ok) allLose = false;
            }
            b.set(x, y, EMPTY);
            if (allLose) { if (rootX) { *rootX = x; *rootY = y; } threatTtStore(th, ttDepth, true, x, y); return true; }

            threatCount++;
        }
    }

    threatTtStore(th, ttDepth, false, -1, -1);
    return false;
}

// ============= 杀距换算（b1-B） =============
//
// 「杀距比较」需要一个跨探针可比的距离度量。约定：换算成**与引擎败带同口径的
// mateLossScore(k) 分值**（k=ply）——距离编码进分值，比较即分值比较，且与深搜
// 自己的将杀距离原始分同尺度。设计借鉴 Rapfi（dotb1ue/rapfi，search/ab/search.cpp）：
// 其 mated_in/mate_in 直接用分值编码将杀距离并参与 alpha/beta 剪枝，VCF 防守侧
// 不做任何特殊「拖延」逻辑——拖延由 mate 分值的自然序产生。
//
// VCF：ctx().vcfWinFours = VCF_MAX_FOURS - foursLeft + 1，其中 foursLeft 是探针的
//      剩余预算。防守探针以 VCF_DEFENSE_FOURS 起步（进攻以 VCF_MAX_FOURS 起步），
//      换算回本探针实际冲四手数要减掉这个固定偏移：
//      foursUsed = ctx().vcfWinFours - (VCF_MAX_FOURS - initialFours)。
// VCT：ctx().vctWinMovesLeft 是必胜证明节点的剩余威胁手数，
//      attackerMoves = VCT_MAX_MOVES - 该值 + 1。
// ply ≈ 2*攻方强制手数 - 1（每手强制着法之间夹一手被迫应招，最后一手成五）。
//
// 注意（度量语义）：这是「证明长度」的下界排名度量，不是精确将杀手数——活四/
// 拆不掉类证明在手数上偏小（完成五的那手不计入递归深度）。用途是败势下「谁死得
// 慢」的候选排序；精确将杀距离仍由深搜 loss-band 原始分提供（次席信号）。
// 0 = 未被探针证杀（射程之外）——排名上高于任何有界证明（对手至少需要超出射程
// 的努力，拖延期望更长）。

static inline int64_t killValueFromMoves(int attackerMoves) {
    const int plies = attackerMoves > 0 ? 2 * attackerMoves - 1 : 1;
    // 防越界：证明长度超过 MATE_MAX_PLY 时封顶（败带只编码到 k=MATE_MAX_PLY）
    const int64_t k = std::min<int64_t>(plies, MATE_MAX_PLY);
    return mateLossScore(k);
}

static inline int64_t vcfKillValue(int initialFours) {
    const int foursUsed = ctx().vcfWinFours - (VCF_MAX_FOURS - initialFours);
    return killValueFromMoves(foursUsed);
}

static inline int64_t vctKillValue() {
    const int attackerMoves = VCT_MAX_MOVES - ctx().vctWinMovesLeft + 1;
    return killValueFromMoves(attackerMoves);
}

// ============= 迭代加深搜索 =============

SearchResult findBestMove(Board& board) {
    SearchResult result;
    ctx().nodesVisited = 0;
    ctx().timedOut = false;
    ctx().searchStartTime = std::chrono::steady_clock::now();
    
    uint64_t rootHash = computeHash(board);
    
        int64_t bestScore = -INT64_MAX;
        int64_t bestRawScore = -INT64_MAX;
        int prevBestX = -1, prevBestY = -1;

    
    // 生成根层候选点
    std::vector<Move> rootCandidates = generateCandidates(board, ROOT_CANDIDATE_LIMIT, true);
    if (rootCandidates.empty()) {
        return result;
    }

    // 如果有置换表的最佳走法，移到最前面
    TTProbe rootTT = ttLookup(rootHash, 0);
    if (rootTT.hit && rootTT.bestMove >= 0) {
        int bm = rootTT.bestMove;
        int bmX = bm % BOARD_SIZE;
        int bmY = bm / BOARD_SIZE;
        for (size_t i = 0; i < rootCandidates.size(); i++) {
            if (rootCandidates[i].pos.x == bmX && rootCandidates[i].pos.y == bmY) {
                std::swap(rootCandidates[i], rootCandidates[0]);
                break;
            }
        }
    }

    // 根节点优先处理斜线夹心三，再按中心位置评估普通活三端点
    int rootPlayer = board.getPlayer();
    int rootOpponent = (rootPlayer == BLACK) ? WHITE : BLACK;
    bool hasDiagonalSplitThreat = false;
    for (const auto& candidate : rootCandidates) {
        if (splitThreeDirection(board, candidate.pos.x, candidate.pos.y, rootOpponent) >= 2) {
            hasDiagonalSplitThreat = true;
            break;
        }
    }
    for (auto& candidate : rootCandidates) {
        if (createsFive(board, candidate.pos.x, candidate.pos.y, rootOpponent)) {
            continue;
        }

        int64_t adjustment = 0;
        if (isCrossThreatIntersection(board, candidate.pos.x, candidate.pos.y, rootOpponent)) {
            adjustment += CROSS_THREAT_BONUS;
        }
        int splitDirection = splitThreeDirection(board, candidate.pos.x, candidate.pos.y, rootOpponent);
        if (splitDirection >= 2) {
            adjustment += DIAGONAL_SPLIT_THREE_BONUS;
        }
        if (!hasDiagonalSplitThreat) {
            int64_t endpointPriority = openThreeEndpointPriority(
                board, candidate.pos.x, candidate.pos.y, rootOpponent);
            adjustment += endpointPriority * OPEN_THREE_ENDPOINT_BONUS;
        }
        // 并列决胜：优先争夺对方棋子更密集的格点
        int nearbyEnemy = 0;
        for (int ddy = -NEIGHBOR_RADIUS; ddy <= NEIGHBOR_RADIUS; ++ddy) {
            for (int ddx = -NEIGHBOR_RADIUS; ddx <= NEIGHBOR_RADIUS; ++ddx) {
                if (ddx == 0 && ddy == 0) continue;
                if (board.get(candidate.pos.x + ddx, candidate.pos.y + ddy) == rootOpponent) {
                    ++nearbyEnemy;
                }
            }
        }
        // 开局贴邻：前几手内，候选若紧贴（八个邻格内）任一对手棋子，加分
        if (board.moveCount() <= 3) {
            bool adjacent = false;
            for (int ddy = -1; ddy <= 1 && !adjacent; ++ddy) {
                for (int ddx = -1; ddx <= 1 && !adjacent; ++ddx) {
                    if (ddx == 0 && ddy == 0) continue;
                    if (board.inBounds(candidate.pos.x + ddx, candidate.pos.y + ddy) &&
                        board.get(candidate.pos.x + ddx, candidate.pos.y + ddy) == rootOpponent) {
                        adjacent = true;
                    }
                }
            }
            if (adjacent) adjustment += OPENING_ADJACENT_BONUS;
        }
        adjustment += (int64_t)nearbyEnemy * CONTESTED_WEIGHT;
        board.makeMove(candidate.pos.x, candidate.pos.y);
        adjustment -= potentialThreatScore(board, rootOpponent);
        board.undo();
        candidate.tactical_adjustment = adjustment;
        candidate.score += adjustment;
    }
    std::stable_sort(rootCandidates.begin(), rootCandidates.end(),
        [](const Move& a, const Move& b) {
            return a.score > b.score;
        });

    // ===== VCF 战术层 =====
    // 1) 进攻：自己能连续冲四取胜就直接走（已证必胜，无需再搜）
    {
        int vcfX = -1, vcfY = -1;
        ctx().vcfNodes = 0;
        if (vcfSearch(board, rootPlayer, rootOpponent, VCF_MAX_FOURS,
                      g_timeBudgetMs / 5, &vcfX, &vcfY)) {
            result.bestX = vcfX;
            result.bestY = vcfY;
            result.bestScore = WIN_THRESHOLD;
            result.searchDepth = 2 * ctx().vcfWinFours;   // 冲破四手数折算成层数
            result.nodesVisited = ctx().nodesVisited;
            result.elapsedMs = getElapsedMs();
            result.timedOut = false;
            ctx().lastSearchDepth = result.searchDepth;
            ctx().lastSearchNodes = result.nodesVisited;
            ctx().lastSearchTimeMs = result.elapsedMs;
            ctx().lastSearchScore = result.bestScore;
            return result;
        }
    }

    // 1b) 进攻（VCT）：连冲四找不到时，再查「三」施压型的连续威胁
    {
        int vctX = -1, vctY = -1;
        ctx().vctNodes = 0;
        if (vctSearch(board, rootPlayer, rootOpponent, VCT_MAX_MOVES, g_timeBudgetMs / 4, &vctX, &vctY)) {
            result.bestX = vctX;
            result.bestY = vctY;
            result.bestScore = WIN_THRESHOLD;
            result.searchDepth = 0;
            result.nodesVisited = ctx().nodesVisited;
            result.elapsedMs = getElapsedMs();
            result.timedOut = false;
            ctx().lastSearchDepth = result.searchDepth;
            ctx().lastSearchNodes = result.nodesVisited;
            ctx().lastSearchTimeMs = result.elapsedMs;
            ctx().lastSearchScore = result.bestScore;
            return result;
        }
    }

    // 2) 防守：排除「走完之后对手能连续冲四取胜」的候选（除非全部如此）
    //    b1-B（2026-11-18）两部分：
    //    a) VCF 之后对 VCF 幸存候选补 VCT 核杀（「三」施压型连续威胁——VCF 证明
    //       不了、深搜又容易看漏的深链杀）。只补幸存者：实测 24 个根候选里 22 个
    //       被 VCF 秒杀（每个 4 节点），幸存者通常 0-3 个，成本可控（旧的全候选
    //       VCT 实现实测吃掉 3-4.5s 加深时间，已废弃）。
    //    b) 杀距记录：全程记录每个被证杀候选的探针杀距（ply）。**全部证杀时一个
    //       都不删**（败势里删掉的可能就是拖得最久的着法）——杀距交给迭代加深后的
    //       「拖最久」选择做杀距比较。
    //    成本控制：VCF+VCT 合计硬顶 45% 预算；补枪节点上限 30 万（≈0.6s/候选）；
    //    低预算档（<4s）跳过 VCT，500ms 级回归测试天然隔离。
    struct KillDist { int x, y; int64_t value; };   // value = mate 败带分（0=未被证杀）
    std::vector<KillDist> killDist;
    killDist.reserve(rootCandidates.size());
    {
        ctx().vcfNodes = 0;
        ctx().vctNodes = 0;
        const int64_t vcfDeadline = g_timeBudgetMs * 3 / 10;   // VCF 阶段最多占 30%
        const int64_t vctDeadline = g_timeBudgetMs * 45 / 100; // VCF+VCT 合计最多占 45%
        std::vector<Move> safe;
        safe.reserve(rootCandidates.size());
        int banned = 0;
        for (const auto& c : rootCandidates) {
            if (getElapsedMs() > vcfDeadline) {   // 超时不再剔除，交给主搜索
                safe.push_back(c);
                continue;
            }
            board.set(c.pos.x, c.pos.y, rootPlayer);
            bool oppWin = vcfSearch(board, rootOpponent, rootPlayer, VCF_DEFENSE_FOURS,
                                    vcfDeadline, nullptr, nullptr);
            board.set(c.pos.x, c.pos.y, EMPTY);
            if (oppWin) {
                banned++;
                killDist.push_back({c.pos.x, c.pos.y, vcfKillValue(VCF_DEFENSE_FOURS)});
            } else {
                safe.push_back(c);
            }
        }

        // VCF 幸存者补 VCT 核杀（「三」施压型连续威胁杀）
        if (!safe.empty() && g_timeBudgetMs >= VCT_FILTER_MIN_BUDGET) {
            std::vector<Move> stillSafe;
            stillSafe.reserve(safe.size());
            for (const auto& c : safe) {
                if (getElapsedMs() > vctDeadline) {   // 超时不再剔除，交给主搜索
                    stillSafe.push_back(c);
                    continue;
                }
                ctx().vcfNodes = 0;   // 契约 4：两个计数器是累计的，每次探针前必须清零
                ctx().vctNodes = 0;
                const int savedVctLimit = ctx().vctNodeLimit;
                ctx().vctNodeLimit = VCT_FILTER_NODE_LIMIT;   // 补枪档：收紧节点上限保主搜索深度
                board.set(c.pos.x, c.pos.y, rootPlayer);
                bool oppWin = vctSearch(board, rootOpponent, rootPlayer, VCT_MAX_MOVES,
                                        vctDeadline, nullptr, nullptr);
                board.set(c.pos.x, c.pos.y, EMPTY);
                ctx().vctNodeLimit = savedVctLimit;
                if (oppWin) {
                    banned++;
                    killDist.push_back({c.pos.x, c.pos.y, vctKillValue()});
                } else {
                    stillSafe.push_back(c);
                }
            }
            if (banned > 0) safe.swap(stillSafe);
        }

        if (banned > 0 && !safe.empty()) {
            rootCandidates.swap(safe);
        }
        // banned > 0 但 safe 为空 = 全部候选都被探针证杀：不动候选集，killDist 已在
        // 上面记齐，供全证败分支做杀距比较。
    }

    // 迭代加深
    int64_t lastIterMs = 0;    // 上一层完整迭代的耗时（提前收手估算用）
    int64_t iterStartMs = 0;   // 本层迭代的开始时刻——逐层测真实耗时；旧实现把「搜索全程
                               // 耗时」当成首层耗时，任何搜索前阶段变慢都会误触提前收手
    for (int depth = 1; depth <= g_maxDepth; depth++) {
        iterStartMs = getElapsedMs();
        // 重新排序根层候选点（基于上一轮的结果）
        if (prevBestX >= 0) {
            for (size_t i = 0; i < rootCandidates.size(); i++) {
                if (rootCandidates[i].pos.x == prevBestX && rootCandidates[i].pos.y == prevBestY) {
                    std::swap(rootCandidates[i], rootCandidates[0]);
                    break;
                }
            }
        }
        
        int64_t bestInIter = -INT64_MAX;
        int64_t bestRawInIter = -INT64_MAX;
        int bestXInIter = -1, bestYInIter = -1;
        int bestRawXInIter = -1, bestRawYInIter = -1;
        // 非必败候选中的最优（已证必败的着法不参与评选，避免加成掩盖败局）
        int64_t bestLiveScore = -INT64_MAX;
        int64_t bestLiveRaw = 0;   // 同上候选的原始分（对外上报用：显示与选择分离）
        int bestLiveX = -1, bestLiveY = -1;
        bool foundLive = false;

        for (size_t ci = 0; ci < rootCandidates.size(); ++ci) {
            const Move& move = rootCandidates[ci];
            // 落子并切换到对手回合
            int player = board.getPlayer();
            board.makeMove(move.pos.x, move.pos.y);
            uint64_t newHash = rootHash ^ zobrist[move.pos.x][move.pos.y][player];
            
            // 搜索（窗口必须使用原始分：含战术加成的分会污染后续候选的窗口）
            int64_t score;
            if (board.checkWin(move.pos.x, move.pos.y) != 0) {
                score = mateWinScore(mateDistFromDepth(depth));
            } else {
                score = -negamax(board, -WIN_THRESHOLD - 1, -bestRawInIter - 1, depth - 1, newHash);
            }
            
            // 收子并恢复当前回合
            board.undo();

            int64_t rawScore = score;
            if (move.tactical_adjustment != 0) {
                if (rawScore > LOSS_SCORE_CEIL && rawScore < WIN_SCORE_FLOOR) {
                    score += (move.tactical_adjustment > MAX_TACTICAL_ADJUST) ? MAX_TACTICAL_ADJUST : ((move.tactical_adjustment < -MAX_TACTICAL_ADJUST) ? -MAX_TACTICAL_ADJUST : move.tactical_adjustment);
                } else {
                    int64_t tieBreak = std::max<int64_t>(-TERMINAL_TIE, std::min<int64_t>(TERMINAL_TIE, move.tactical_adjustment));
                    score += tieBreak;
                }
            }

            // 超时后当前候选的结果不完整，不能覆盖已完成候选
            if (ctx().timedOut) break;

            rootCandidates[ci].lastRawScore = rawScore;   // 供「选定手核杀保护」在收尾时换算候选分

            if (rawScore > bestRawInIter) {
                bestRawInIter = rawScore;
                bestRawXInIter = move.pos.x;
                bestRawYInIter = move.pos.y;
            }
            if (score > bestInIter) {
                bestInIter = score;
                bestXInIter = move.pos.x;
                bestYInIter = move.pos.y;
            }
            if (rawScore > LOSS_SCORE_CEIL) {
                if (!foundLive || score > bestLiveScore) {
                    bestLiveScore = score;
                    bestLiveRaw = rawScore;
                    bestLiveX = move.pos.x;
                    bestLiveY = move.pos.y;
                    foundLive = true;
                }
            }
            
            if (ctx().timedOut) break;
        }
        
        // 真实必胜优先于一切加成；未被证败者次之；全部被证败时才退回原有评选
        if (bestRawInIter >= WIN_SCORE_FLOOR && bestRawXInIter >= 0) {
            bestXInIter = bestRawXInIter;
            bestYInIter = bestRawYInIter;
            bestInIter = bestRawInIter;
        } else if (foundLive) {
            bestXInIter = bestLiveX;
            bestYInIter = bestLiveY;
            bestInIter = bestLiveScore;
        } else if (ctx().timedOut) {
            // 本迭代被超时截断：候选只扫了一部分。「所有着法都已证败」这个
            // 结论只在完整扫描后才成立，否则没来得及评估的活路会被当成必败，
            // 于是同一局面随预算变化在「必胜/必败」之间翻转（实测 8s 时只扫
            // 了 24 个候选里的 1 个就下了必败结论）。此处作废本轮评选，保
            // 留上一轮完整迭代的结果。
            bestXInIter = -1;
            bestYInIter = -1;
        } else {
            // 全证败（且本轮为完整扫描）：按「杀距比较」取「拖得最久」的一手。
            // b1-B（2026-11-18）起排名规则（分值为 mate 败带口径，k=ply）：
            //   1) 探针杀距分值（killDist）——探针证明过的杀是「对手最快能杀」的硬
            //      证据；未被证杀的候选（分值 0）排位高于任何有界证明——对手至少需要
            //      超出探针射程的努力，拖延期望更长；已证杀之间分值越大杀得越晚。
            //   2) 深搜将杀距离原始分（raw 越大 k 越大、被将杀越晚）次席。
            // 此前的实现只按原始分评选，在败势里会凭位置分随机挑一种死法（实测白方
            // 已必败后连走速败着法，黑方少花好几手）。
            int pickX = -1, pickY = -1;
            int64_t pickKill = 1;        // 杀距分值；0 = 未被证杀（高于任何有界证明）
            int64_t pickRaw = -INT64_MAX;
            for (const auto& c : rootCandidates) {
                int64_t killValue = 0;   // 默认：本候选未被探针证杀
                for (const auto& kd : killDist) {
                    if (kd.x == c.pos.x && kd.y == c.pos.y) { killValue = kd.value; break; }
                }
                const int64_t raw = (c.lastRawScore > INT64_MIN) ? c.lastRawScore : -INT64_MAX;
                // 排名：未被证杀(0) > 已证杀且更晚（分值更大）> 同分值比深搜原始分
                const bool better = pickX < 0
                    || killValue > pickKill
                    || (killValue == pickKill && raw > pickRaw);
                if (better) {
                    pickKill = killValue;
                    pickRaw = raw;
                    pickX = c.pos.x;
                    pickY = c.pos.y;
                }
            }
            if (pickX >= 0) {
                bestXInIter = pickX;
                bestYInIter = pickY;
                bestInIter = pickRaw;   // 对外上报原始分（显示与选择分离）
            } else if (bestRawXInIter >= 0) {
                bestXInIter = bestRawXInIter;
                bestYInIter = bestRawYInIter;
                bestInIter = bestRawInIter;
            }
        }

        // 「全证败」抽查：损失带的结论必须由对手的威胁杀（VCF/VCT）核实——
        // 实测偶层视界会把不输的局面误判成全证败（对 (7,6) 的搜索在深度 8/10
        // 崩向损失带、深度 7/9 正常）。对排名前几的候选抽查：只要有一个对手
        // 证不出杀，本轮损失结论不采信，保留上一轮结果；真败局面会被对手
        // 杀核实，不受影响。
        if (bestRawInIter <= LOSS_SCORE_CEIL && bestXInIter >= 0) {
            const int64_t verifyDeadline = g_timeBudgetMs * 3 / 2;  // 抽查最多用到 1.5 倍预算
            const bool savedTimedOut = ctx().timedOut;
            bool anySurvivor = false;
            int checkedTop = 0;
            for (const auto& c : rootCandidates) {
                if (checkedTop >= 2) break;
                if (getElapsedMs() > verifyDeadline) break;
                checkedTop++;
                ctx().vcfNodes = 0;   // 抽查前必须清零：两个计数器是累计的，超上限后搜索会「零节点
                ctx().vctNodes = 0;   // 即判超时」直接空转返回 false——实测把必败局误判成「对手证不出杀」
                board.set(c.pos.x, c.pos.y, rootPlayer);
                bool oppWin = vcfSearch(board, rootOpponent, rootPlayer,
                                        VCF_DEFENSE_FOURS, verifyDeadline, nullptr, nullptr);
                if (!oppWin) {
                    oppWin = vctSearch(board, rootOpponent, rootPlayer,
                                       VCT_MAX_MOVES, verifyDeadline, nullptr, nullptr);
                }
                board.set(c.pos.x, c.pos.y, EMPTY);
                if (!oppWin) { anySurvivor = true; break; }
            }
            ctx().timedOut = savedTimedOut;   // 抽查不改变主搜索的超时状态
            if (anySurvivor) {
                bestXInIter = -1;
                bestYInIter = -1;
            }
        }

        // 显示与选择分离：评选仍用含加成的分值，但对外上报「所选着法的原始分」——
        // 防止 4000~5000 万级战术加成把败局显示成「大优」（实测 +1011.7 万的假胜率）。
        int64_t reportScore = bestInIter;
        if (bestRawInIter >= WIN_SCORE_FLOOR && bestRawXInIter >= 0) {
            reportScore = bestRawInIter;
        } else if (foundLive && bestLiveX >= 0) {
            reportScore = bestLiveRaw;
        }
        if (bestXInIter >= 0) {
            bestScore = reportScore;
            bestRawScore = bestRawInIter;
            prevBestX = bestXInIter;
            prevBestY = bestYInIter;
            result.bestX = bestXInIter;
            result.bestY = bestYInIter;
            result.bestScore = reportScore;
            result.searchDepth = depth;  // 只上报被采纳结论所在的深度
        }
        
        // 记录本层完整迭代的真实耗时（用于下面的提前收手估算）
        {
            int64_t nowMs = getElapsedMs();
            lastIterMs = nowMs - iterStartMs;
        }
        
        // 检查时间
        if (ctx().timedOut) break;
        
        // 只有真实搜索分达到必胜阈值才能提前结束，战术排序加成不能触发
        if (bestRawScore >= WIN_SCORE_FLOOR) break;
        
        // 提前收手：层间耗时实测通常增长 5~8 倍，剩余预算不足 2 倍本层耗时时，
        // 下一层几乎不可能完成——而完不成也会被丢弃。直接结束省掉这段空转，
        // 不改变已采纳的结论（实测关键局面省 20%+ 等待，走法/评分不变）。
        if (lastIterMs > 0 && g_timeBudgetMs - getElapsedMs() < lastIterMs * 2) break;
        
        // 如果时间不足 10%，提前结束（避免半层被截断）
        if (getElapsedMs() > g_timeBudgetMs * 0.9) break;
    }
    
    // 兜底：若每轮都在扫描途中超时、从未得到可用评选，用排序后的首选候选，
    // 保证上层总能拿到一手棋（搜索分数此时未知，保持中性值）。
    if (result.bestX < 0 && !rootCandidates.empty()) {
        result.bestX = rootCandidates[0].pos.x;
        result.bestY = rootCandidates[0].pos.y;
        result.bestScore = 0;
    }
    
    // ===== 决赛候选的「对手应招复核」 =====
    // VCF 防守过滤会把根候选收窄到两三手（实测 total=3）。直接靠位置类加成在其中
    // 挑选会挑错——实测把必败的一手评成 +4600 万。这里对这批决赛候选逐个做一次
    // 「对手应招搜索」，取对手得分最低的一手：等于把富余时间花在最后一层深算上。
    // 触发条件是「候选数少」而不是「并列」（目标局面三手里最佳分唯一，靠并列根本收不到）。
    if (!ctx().inPostScan && result.bestX >= 0 &&
        g_timeBudgetMs >= POST_SCAN_MIN_BUDGET &&
        rootCandidates.size() >= 2 && rootCandidates.size() <= POST_SCAN_MAX_FINALISTS &&
        bestRawScore > LOSS_SCORE_CEIL && bestRawScore < WIN_SCORE_FLOOR) {
        ctx().inPostScan = true;
        const auto savedStart = ctx().searchStartTime;
        const int64_t savedBudget = g_timeBudgetMs;
        const int64_t savedNodes = ctx().nodesVisited;
        const bool savedTimedOut = ctx().timedOut;
        g_timeBudgetMs = (savedBudget / 2 < 8000) ? savedBudget / 2 : 8000;   // 每个决赛候选的复核预算（封顶 8s）
        int64_t bestOpp = INT64_MAX;
        int pickX = result.bestX, pickY = result.bestY;
        for (size_t i = 0; i < rootCandidates.size(); i++) {

            if (getElapsedMs() > savedBudget * 3 / 2) break;   // 复核总时长上限：主搜索之外再加半个预算（12s 档≈18s，20s 档≈30s）

            board.makeMove(rootCandidates[i].pos.x, rootCandidates[i].pos.y);

            SearchResult reply = findBestMove(board);
            board.undo();
            if (reply.bestX >= 0 && reply.bestScore < bestOpp) {
                bestOpp = reply.bestScore;
                pickX = rootCandidates[i].pos.x;
                pickY = rootCandidates[i].pos.y;
            }
        }
        ctx().inPostScan = false;
        ctx().searchStartTime = savedStart;
        g_timeBudgetMs = savedBudget;
        ctx().nodesVisited = savedNodes;
        ctx().timedOut = savedTimedOut;
        if (bestOpp != INT64_MAX) {
            result.bestX = pickX;
            result.bestY = pickY;
        }
    }

    // ===== 选定手核杀保护（2026-11-14） =====
    // 深度搜索对「对手威胁搜索一眼可见的杀」存在盲区：20s 定档核验里 n15 局面自评
    // +366 万的手 (4,6) 实为黑方 1.7s 即证杀；第 13 局 m12 同类（黑 43ms 证杀）。
    // 收尾时对最终选定的一手做一次对手 VCF/VCT 核杀：证出杀、且存在幸存候选时，
    // 换成「评分序最靠前的幸存候选」——幸存者的被杀深度在探针射程之外，方向保守。
    // 低预算档（<8s）跳过：探针开销占比过大（500ms 级回归测试因此天然隔离）。
    if (!ctx().inPostScan && result.bestX >= 0 && rootCandidates.size() >= 2 &&
        g_timeBudgetMs >= 8000 && bestRawScore > LOSS_SCORE_CEIL) {
        const int64_t pickDeadline = g_timeBudgetMs * 3 / 2;
        const bool savedTimedOutP = ctx().timedOut;
        const int savedVcfLimitP = ctx().vcfNodeLimit;
        const int savedVctLimitP = ctx().vctNodeLimit;
        ctx().vcfNodeLimit = 1500000;  // 收尾核杀：放宽节点上限（250k 实测会漏掉深链杀，
        ctx().vctNodeLimit = 600000;   // 如 n15 需要 ~60 万节点级），总开销仍受期限约束
        int pickIdx = -1;
        for (size_t i = 0; i < rootCandidates.size(); i++) {
            if (rootCandidates[i].pos.x == result.bestX && rootCandidates[i].pos.y == result.bestY) { pickIdx = (int)i; break; }
        }
        bool pickKilled = false;
        if (pickIdx >= 0 && getElapsedMs() < pickDeadline) {
            ctx().vcfNodes = 0; ctx().vctNodes = 0;
            board.set(result.bestX, result.bestY, rootPlayer);
            pickKilled = vcfSearch(board, rootOpponent, rootPlayer, VCF_DEFENSE_FOURS, pickDeadline, nullptr, nullptr);
            if (!pickKilled) {
                pickKilled = vctSearch(board, rootOpponent, rootPlayer, VCT_MAX_MOVES, pickDeadline, nullptr, nullptr);
            }
            board.set(result.bestX, result.bestY, EMPTY);
        }
        if (pickKilled) {
            // 按最近一次完整迭代的原始分降序，找第一个对手证不出杀的候选
            std::vector<std::pair<int64_t, int>> order;
            order.reserve(rootCandidates.size());
            for (size_t i = 0; i < rootCandidates.size(); i++) {
                if ((int)i == pickIdx) continue;
                order.emplace_back(rootCandidates[i].lastRawScore, (int)i);
            }
            std::stable_sort(order.begin(), order.end(),
                [](const std::pair<int64_t, int>& a, const std::pair<int64_t, int>& b) { return a.first > b.first; });
            int probed = 1;
            for (const auto& ent : order) {
                if (probed >= 8 || getElapsedMs() > pickDeadline) break;
                const Move& c = rootCandidates[ent.second];
                ctx().vcfNodes = 0; ctx().vctNodes = 0;
                board.set(c.pos.x, c.pos.y, rootPlayer);
                bool oppWin = vcfSearch(board, rootOpponent, rootPlayer, VCF_DEFENSE_FOURS, pickDeadline, nullptr, nullptr);
                if (!oppWin) {
                    oppWin = vctSearch(board, rootOpponent, rootPlayer, VCT_MAX_MOVES, pickDeadline, nullptr, nullptr);
                }
                board.set(c.pos.x, c.pos.y, EMPTY);
                probed++;
                if (!oppWin) {
                    result.bestX = c.pos.x;
                    result.bestY = c.pos.y;
                    if (c.lastRawScore > INT64_MIN) result.bestScore = c.lastRawScore;
                    break;
                }
            }
        }
        ctx().vcfNodeLimit = savedVcfLimitP;
        ctx().vctNodeLimit = savedVctLimitP;
        ctx().timedOut = savedTimedOutP;
    }
    result.nodesVisited = ctx().nodesVisited;
    result.elapsedMs = getElapsedMs();
    result.timedOut = ctx().timedOut;
    
    // 存储到全局变量
    ctx().lastSearchDepth = result.searchDepth;
    ctx().lastSearchNodes = result.nodesVisited;
    ctx().lastSearchTimeMs = result.elapsedMs;
    ctx().lastSearchScore = result.bestScore;
    
    return result;
}

// ============= 公共 API =============

void setDifficulty(Difficulty level) {
    switch (level) {
        case Difficulty::EASY:
            g_maxDepth = 4;
            g_timeBudgetMs = 1000;
            break;
        case Difficulty::MEDIUM:
            g_maxDepth = 8;
            g_timeBudgetMs = 3000;
            break;
        case Difficulty::HARD:
            g_maxDepth = 16;
            g_timeBudgetMs = 20000;   // 2026-11-14 定档复核（15/20/25s 实验）：20s（深度 10）起
                                      // 第 12 局关键堵点从「会输的一手」改为正确一手——是决策
                                      // 改变点；完整败局可见（深度 11）需 25-30s，留给超长考。
                                      // （旧注：2026-10-31 曾校准 10s 修复白4 质量问题）
            break;
        case Difficulty::LONG:
            g_maxDepth = 16;
            g_timeBudgetMs = 30000;   // 2026-11 诊断档：实测 30s（深度 10-11）能在中盘看清 10s（深度 9）
                                      // 完全看不到的败势（评分从 -592 摆到 -370 万，甚至直接证败）
            break;
    }
}

void setTimeBudget(int ms) {
    g_timeBudgetMs = ms;
}

int64_t getNodesVisited() {
    return g_defaultCtx.nodesVisited;
}

void resetStats() {
    ctx().nodesVisited = 0;
    ctx().timedOut = false;
}

// 清空搜索缓存（主置换表 + 威胁搜索置换表）。
// 新开一局或悔棋后调用：避免上一局 / 被撤销分支的残留条目影响后续选择。
// 实测不清空时，同一开局的应手会在并列着法间漂移（(6,6) -> (8,6)）。
void clearSearchTables() {
    // 三张主 TT（默认/参照/辅助共享）与威胁 TT 一并清——仅在没有线程搜索时调用。
    // 并行驱动每线程的威胁 TT 是新建vector（零初始化），无需在此处理。
    memset(g_ttStorage, 0, sizeof(TTEntry) * TT_SIZE);
#ifndef __EMSCRIPTEN__
    memset(g_ttRefStorage, 0, sizeof(TTEntry) * TT_SIZE);
    memset(g_ttSmpStorage, 0, sizeof(TTEntry) * TT_SIZE);
#endif
    threatTtClear();
}

int getMaxDepth() {
    return g_maxDepth;
}

int getTimeBudgetMs() {
    return g_timeBudgetMs;
}

int getLastSearchDepth() {
    return g_defaultCtx.lastSearchDepth;
}

int64_t getLastSearchNodes() {
    return g_defaultCtx.lastSearchNodes;
}

int64_t getLastSearchTimeMs() {
    return g_defaultCtx.lastSearchTimeMs;
}

int64_t getLastSearchScore() {
    return g_defaultCtx.lastSearchScore;
}

// ============= 并行搜索（保守 SMP；仅原生 CLI 启用） =============
//
// 2026-11-19 定档设计（曾实测经典全员共享 TT 的 lazy SMP，见下）：
//  - 线程 0 = 参照线程：独立 TT（谁都不污染它），结果与单线程路径逐位一致；
//  - 线程 1..N-1 = 辅助线程：互相共享另一张 TT（SMP 多样性来源），各带私有威胁 TT；
//  - 合并只接受「严格更优」（分高，或同分更深）——并行结果构造性 ≥ 单线程。
//  - 每线程跑完整 findBestMove（含 b1-B 过滤、post-scan、b3 收尾保护），互不等待；
//    各线程自己的耗时时钟（g_timeBudgetMs 共享只读）天然对齐，无线程间同步原语。
//
// 为什么不用全员共享 TT 的经典 lazy SMP（Rapfi/Stockfish 式）：实测辅助线程的 TT
// 互渗会把参照线程在「同分平局」上的 tie-break 带偏——game12-m13 从正确的 (6,4)
// 翻成错手 (10,8)（正是 10s/15s 档记录过的错误着法）。本仓库的精确夹具纪律
// （「不劣于旧」）不容这种抖动，故参照线程与辅助线程的 TT 物理隔离。
#ifndef __EMSCRIPTEN__
#include <thread>
#include <vector>
// g_ttRefStorage / g_ttSmpStorage / g_parallelMergeInfo 在 TT 节声明（clearSearchTables 要用）。

SearchResult findBestMoveParallel(Board& board, int numThreads) {
    if (numThreads <= 1) return findBestMove(board);

    std::vector<SearchContext> ctxs;
    std::vector<std::vector<ThreatTTEntry>> threatPools;
    ctxs.reserve(numThreads);
    threatPools.reserve(numThreads);
    for (int i = 0; i < numThreads; i++) {
        ctxs.push_back(g_defaultCtx);   // 拷贝配置型字段（tt 指针等）
        ctxs.back().tt = (i == 0) ? g_ttRefStorage : g_ttSmpStorage;
        threatPools.emplace_back(THREAT_TT_SIZE);   //  threat TT 每线程私有
        ctxs.back().threatTT = threatPools.back().data();
        ctxs.back().nodesVisited = 0;   // 搜索状态清零（拷贝来的可能是上轮残留）
        ctxs.back().timedOut = false;
        ctxs.back().inPostScan = false;
    }

    std::vector<Board> boards(numThreads, board);   // Board 可拷贝（定长数组）
    std::vector<SearchResult> results(numThreads);
    std::vector<std::thread> threads;
    threads.reserve(numThreads);
    for (int i = 0; i < numThreads; i++) {
        threads.emplace_back([&ctxs, &boards, &results, i]() {
            t_ctxPtr = &ctxs[i];   // thread_local：本线程后续全部访问走私有上下文
            results[i] = findBestMove(boards[i]);
        });
    }
    for (auto& t : threads) t.join();

    int best = 0;   // 参照线程的结果是基准（平局的归属）
    int improved = -1;
    for (int i = 1; i < numThreads; i++) {
        const SearchResult& a = results[best];
        const SearchResult& b = results[i];
        const bool strictlyBetter =
            b.bestScore > a.bestScore ||
            (b.bestScore == a.bestScore && b.searchDepth > a.searchDepth);
        if (strictlyBetter) { best = i; improved = i; }
    }
    // 胜者统计落到默认上下文（getLastSearch* 的读取处）
    g_defaultCtx.lastSearchDepth = results[best].searchDepth;
    g_defaultCtx.lastSearchNodes = results[best].nodesVisited;
    g_defaultCtx.lastSearchTimeMs = results[best].elapsedMs;
    g_defaultCtx.lastSearchScore = results[best].bestScore;
    // 合并决策供测量记录（辅助线程是否贡献了严格更优）
    g_parallelMergeInfo = improved >= 0 ? (numThreads * 100 + improved) : 0;
    return results[best];
}
#else
SearchResult findBestMoveParallel(Board& board, int numThreads) {
    (void)numThreads;
    return findBestMove(board);   // wasm：无 SAB/COOP-COEP 条件，退回单线程
}
#endif
