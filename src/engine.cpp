// engine.cpp - 五子棋核心引擎实现
// C++17 + Emscripten (WASM)
// 参考: algorithms.md 调研文档

#include "engine.h"
#include <cstdio>
#include <cmath>
#include <cstdlib>

// ============= 全局状态 =============

static int64_t g_nodesVisited = 0;
static int64_t g_timeBudgetMs = 3000;
static int g_maxDepth = 8;  // 默认搜索深度
static bool g_timedOut = false;
static std::chrono::steady_clock::time_point g_searchStartTime;

// 最后一次搜索的统计
static int g_lastSearchDepth = 0;
static int64_t g_lastSearchNodes = 0;
static int64_t g_lastSearchTimeMs = 0;

// ============= Zobrist 哈希 =============

static uint64_t zobrist[BOARD_SIZE][BOARD_SIZE][3];  // [x][y][player+1]
static bool g_zobristInitialized = false;

static void initZobrist() {
    if (g_zobristInitialized) return;
    // 使用 LCG 生成伪随机数（可重现）
    uint64_t state = 0x123456789ABCDEF0ULL;
    for (int y = 0; y < BOARD_SIZE; y++) {
        for (int x = 0; x < BOARD_SIZE; x++) {
            for (int p = 0; p < 3; p++) {
                state = state * 6364136223846793005ULL + 1442695040888963407ULL;
                zobrist[x][y][p] = state;
            }
        }
    }
    g_zobristInitialized = true;
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

struct TTEntry {
    uint64_t hash;
    int8_t depth;
    int32_t score;
    uint8_t flag;  // 0=exact, 1=lower, 2=upper
    int16_t bestMove;  // 编码为 y*15+x
    int16_t padding;
};

static constexpr int TT_SIZE = 1 << 20;  // 1M entries
static TTEntry g_tt[TT_SIZE];

static inline int ttIndex(uint64_t hash) {
    return (hash & (TT_SIZE - 1));
}

static inline void ttStore(uint64_t hash, int depth, int64_t score, int bestX, int bestY, int64_t alpha, int64_t beta) {
    int idx = ttIndex(hash);
    TTEntry& entry = g_tt[idx];
    
    if (bestX >= 0 && bestY >= 0) {
        entry.bestMove = bestY * BOARD_SIZE + bestX;
    }
    entry.depth = (int8_t)depth;
    entry.hash = hash;
    
    // 限制分数到 int32 范围
    int32_t s = (score > INT32_MAX) ? INT32_MAX : (score < INT32_MIN) ? INT32_MIN : (int32_t)score;
    entry.score = s;
    
    if (score <= alpha) entry.flag = 2;  // UPPER
    else if (score >= beta) entry.flag = 1;  // LOWER
    else entry.flag = 0;  // EXACT
}

static inline const TTEntry* ttLookup(uint64_t hash, int depth) {
    int idx = ttIndex(hash);
    const TTEntry& entry = g_tt[idx];
    if (entry.hash == hash && entry.depth >= depth) {
        return &entry;
    }
    return nullptr;
}

// ============= 时间管理 =============

static inline int64_t getElapsedMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - g_searchStartTime).count();
}

static inline bool checkTime() {
    if ((g_nodesVisited & 0x1FF) == 0) {  // 每 512 节点检查一次
        if (getElapsedMs() > g_timeBudgetMs) {
            g_timedOut = true;
            return true;
        }
    }
    return g_timedOut;
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
int64_t evaluatePatterns(const Board& board, int player) {
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
    
    return score;
}

// 完整评估（当前视角的分数）
int64_t evaluatePosition(const Board& board) {
    int player = board.getPlayer();
    int enemy = (player == BLACK) ? WHITE : BLACK;
    
    int64_t ownScore = evaluatePatterns(board, player);
    int64_t enemyScore = evaluatePatterns(board, enemy);
    
    // 中心分（权重较低，辅助决策）
    int64_t ownCenter = evaluateCenterScore(board, player);
    int64_t enemyCenter = evaluateCenterScore(board, enemy);
    
    // 略偏防守：对手威胁权重稍高
    int64_t result = ownScore - (int64_t)(enemyScore * DEFENSE_WEIGHT) 
                    + (ownCenter - enemyCenter) / 10;
    
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

// 计算候选点的快速排序分数
static int64_t quickMoveScore(Board& board, int x, int y) {
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
    
    // 8. 简单棋型评分（评估该点周围的棋型）
    // 临时放置棋子，评估影响
    board.set(x, y, player);
    int64_t newOwnScore = evaluatePatterns(board, player);
    board.set(x, y, EMPTY);
    
    board.set(x, y, enemy);
    int64_t newEnemyScore = evaluatePatterns(board, enemy);
    board.set(x, y, EMPTY);
    
    score += (newOwnScore - (int64_t)(newEnemyScore * 1.0)) / 100;  // 缩放
    
    return score;
}

// ============= 候选点生成 =============

std::vector<Move> generateCandidates(Board& board, int maxCandidates, bool isRoot) {
    std::vector<Move> candidates;
    
    // 空棋盘特殊情况
    if (board.moveCount() == 0) {
        candidates.push_back(Move{Point(BOARD_SIZE / 2, BOARD_SIZE / 2), board.getPlayer(), 0});
        return candidates;
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
                m.score = quickMoveScore(board, x, y);
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
    g_nodesVisited++;
    
    // 时间检查
    if (checkTime()) {
        return 0;
    }
    
    // 胜负检查
    if (board.moveCount() > 0) {
        const Move& lastMove = board.getHistory(board.moveCount() - 1);
        if (board.checkWin(lastMove.pos.x, lastMove.pos.y) != 0) {
            return -WIN_THRESHOLD;  // 上一手赢了，当前方输了
        }
    }
    
    if (board.isFull()) return 0;
    
    if (depth <= 0) {
        return evaluatePosition(board);
    }
    
    // 置换表查找
    const TTEntry* ttEntry = ttLookup(hash, depth);
    if (ttEntry) {
        int32_t ttScore = ttEntry->score;
        if (ttEntry->flag == 0) return ttScore;  // EXACT
        if (ttEntry->flag == 1 && ttScore > alpha) alpha = ttScore;  // LOWER
        if (ttEntry->flag == 2 && ttScore < beta) beta = ttScore;  // UPPER
        if (alpha >= beta) return ttScore;
    }
    
    // 生成候选点
    int limit = (depth >= g_maxDepth) ? ROOT_CANDIDATE_LIMIT : CHILD_CANDIDATE_LIMIT;
    std::vector<Move> candidates = generateCandidates(board, limit, depth >= g_maxDepth);
    
    if (candidates.empty()) return 0;
    
    // 如果有置换表的最佳走法，移到最前面
    if (ttEntry && ttEntry->bestMove >= 0) {
        int bm = ttEntry->bestMove;
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
            score = WIN_THRESHOLD;
        } else {
            score = -negamax(board, -beta, -alpha, depth - 1, newHash);
        }
        
        // 收子并恢复当前回合
        board.undo();

        // 超时后当前候选的结果不完整，不能覆盖已完成候选
        if (g_timedOut) break;

        if (score > bestScore) {
            bestScore = score;
            bestX = move.pos.x;
            bestY = move.pos.y;
        }
        
        alpha = std::max(alpha, bestScore);
        if (alpha >= beta) break;  // Beta 剪枝
    }
    
    // 仅缓存完整搜索结果，超时结果不能污染置换表
    if (!g_timedOut) {
        ttStore(hash, depth, bestScore, bestX, bestY, alpha, beta);
    }
    
    return bestScore;
}

// ============= 迭代加深搜索 =============

SearchResult findBestMove(Board& board) {
    SearchResult result;
    g_nodesVisited = 0;
    g_timedOut = false;
    g_searchStartTime = std::chrono::steady_clock::now();
    
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
    const TTEntry* ttEntry = ttLookup(rootHash, 0);
    if (ttEntry && ttEntry->bestMove >= 0) {
        int bm = ttEntry->bestMove;
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

    // 迭代加深
    for (int depth = 1; depth <= g_maxDepth; depth++) {
        result.searchDepth = depth;
        
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
        int bestLiveX = -1, bestLiveY = -1;
        bool foundLive = false;

        for (const auto& move : rootCandidates) {
            // 落子并切换到对手回合
            int player = board.getPlayer();
            board.makeMove(move.pos.x, move.pos.y);
            uint64_t newHash = rootHash ^ zobrist[move.pos.x][move.pos.y][player];
            
            // 搜索（窗口必须使用原始分：含战术加成的分会污染后续候选的窗口）
            int64_t score;
            if (board.checkWin(move.pos.x, move.pos.y) != 0) {
                score = WIN_THRESHOLD;
            } else {
                score = -negamax(board, -WIN_THRESHOLD - 1, -bestRawInIter - 1, depth - 1, newHash);
            }
            
            // 收子并恢复当前回合
            board.undo();

            int64_t rawScore = score;
            if (move.tactical_adjustment != 0) {
                if (rawScore > -WIN_THRESHOLD && rawScore < WIN_THRESHOLD) {
                    score += move.tactical_adjustment;
                } else {
                    int64_t tieBreak = std::max<int64_t>(-999, std::min<int64_t>(999, move.tactical_adjustment));
                    score += tieBreak;
                }
            }

            // 超时后当前候选的结果不完整，不能覆盖已完成候选
            if (g_timedOut) break;

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
            if (rawScore > -WIN_THRESHOLD) {
                if (!foundLive || score > bestLiveScore) {
                    bestLiveScore = score;
                    bestLiveX = move.pos.x;
                    bestLiveY = move.pos.y;
                    foundLive = true;
                }
            }
            
            if (g_timedOut) break;
        }
        
        // 真实必胜优先于一切加成；未被证败者次之；全部被证败时才退回原有评选
        if (bestRawInIter >= WIN_THRESHOLD && bestRawXInIter >= 0) {
            bestXInIter = bestRawXInIter;
            bestYInIter = bestRawYInIter;
            bestInIter = bestRawInIter;
        } else if (foundLive) {
            bestXInIter = bestLiveX;
            bestYInIter = bestLiveY;
            bestInIter = bestLiveScore;
        }
        if (bestXInIter >= 0) {
            bestScore = bestInIter;
            bestRawScore = bestRawInIter;
            prevBestX = bestXInIter;
            prevBestY = bestYInIter;
            result.bestX = bestXInIter;
            result.bestY = bestYInIter;
            result.bestScore = bestInIter;
        }
        
        // 检查时间
        if (g_timedOut) break;
        
        // 只有真实搜索分达到必胜阈值才能提前结束，战术排序加成不能触发
        if (bestRawScore >= WIN_THRESHOLD) break;
        
        // 如果时间不足 10%，提前结束（避免半层被截断）
        if (getElapsedMs() > g_timeBudgetMs * 0.9) break;
    }
    
    result.nodesVisited = g_nodesVisited;
    result.elapsedMs = getElapsedMs();
    result.timedOut = g_timedOut;
    
    // 存储到全局变量
    g_lastSearchDepth = result.searchDepth;
    g_lastSearchNodes = result.nodesVisited;
    g_lastSearchTimeMs = result.elapsedMs;
    
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
            g_maxDepth = 12;
            g_timeBudgetMs = 12000;
            break;
    }
}

void setTimeBudget(int ms) {
    g_timeBudgetMs = ms;
}

int64_t getNodesVisited() {
    return g_nodesVisited;
}

void resetStats() {
    g_nodesVisited = 0;
    g_timedOut = false;
}

int getMaxDepth() {
    return g_maxDepth;
}

int getTimeBudgetMs() {
    return g_timeBudgetMs;
}

int getLastSearchDepth() {
    return g_lastSearchDepth;
}

int64_t getLastSearchNodes() {
    return g_lastSearchNodes;
}

int64_t getLastSearchTimeMs() {
    return g_lastSearchTimeMs;
}
