// engine.h - 五子棋核心引擎
// C++17 + Emscripten (WASM)
// 参考: algorithms.md 调研文档

#ifndef GOMOKU_ENGINE_H
#define GOMOKU_ENGINE_H

#include <cstdint>
#include <cstring>
#include <vector>
#include <array>
#include <algorithm>
#include <chrono>
#include <cfloat>

// ============= 常量 =============
constexpr int BOARD_SIZE = 15;
constexpr int TOTAL_CELLS = BOARD_SIZE * BOARD_SIZE;

// 棋子
constexpr int EMPTY = 0;
constexpr int BLACK = 1;  // 先手
constexpr int WHITE = 2;  // 后手

// 方向向量（水平、垂直、两条对角线）
static constexpr int DX[4] = {0, 1, 1, 1};
static constexpr int DY[4] = {1, 0, 1, -1};

// 棋型分数（优化后，增强威胁识别）
constexpr int64_t FIVE           = 10000000;
constexpr int64_t OPEN_FOUR      = 5000000;
constexpr int64_t CLOSED_FOUR    = 200000;
constexpr int64_t DOUBLE_FOUR    = 8000000;
constexpr int64_t FOUR_THREE     = 3000000;
constexpr int64_t DOUBLE_THREE   = 2000000;
constexpr int64_t OPEN_THREE     = 100000;
constexpr int64_t JUMP_THREE     = 80000;
constexpr int64_t SLEEP_THREE    = 5000;
constexpr int64_t OPEN_TWO       = 10000;
constexpr int64_t SLEEP_TWO      = 2000;
constexpr int64_t OPEN_ONE       = 200;

// 搜索参数（增强搜索深度）
constexpr int NEIGHBOR_RADIUS = 2;
constexpr int ROOT_CANDIDATE_LIMIT = 24;
constexpr int CHILD_CANDIDATE_LIMIT = 16;
constexpr int MAX_SEARCH_DEPTH = 12;
constexpr double DEFENSE_WEIGHT = 1.15;
constexpr int WIN_THRESHOLD = 10000000;

// 难度级别对应的搜索参数
enum class Difficulty {
    EASY   = 0,  // depth 4-5
    MEDIUM = 1,  // depth 7-8
    HARD   = 2   // depth 10-12
};

// ============= 数据结构 =============

struct Point {
    int x, y;
    Point() : x(-1), y(-1) {}
    Point(int x, int y) : x(x), y(y) {}
    bool valid() const { return x >= 0 && x < BOARD_SIZE && y >= 0 && y < BOARD_SIZE; }
    bool operator==(const Point& o) const { return x == o.x && y == o.y; }
};

struct Move {
    Point pos;
    int player;
    int64_t score;  // 排序用缓存分数
    int64_t tactical_adjustment = 0;  // 根节点战术调整
};

// ============= 棋盘类 =============

class Board {
public:
    Board() { reset(); }
    
    void reset() {
        memset(cells, 0, sizeof(cells));
        move_count = 0;
        memset(history, 0, sizeof(history));
        current_player = BLACK;
    }
    
    int get(int x, int y) const {
        if (!inBounds(x, y)) return -1;
        return cells[y * BOARD_SIZE + x];
    }
    
    void set(int x, int y, int val) {
        cells[y * BOARD_SIZE + x] = val;
    }
    
    bool inBounds(int x, int y) const {
        return x >= 0 && x < BOARD_SIZE && y >= 0 && y < BOARD_SIZE;
    }
    
    int getPlayer() const { return current_player; }
    
    bool makeMove(int x, int y) {
        if (!inBounds(x, y) || cells[y * BOARD_SIZE + x] != EMPTY) return false;
        cells[y * BOARD_SIZE + x] = current_player;
        history[move_count].pos = Point(x, y);
        history[move_count].player = current_player;
        move_count++;
        current_player = (current_player == BLACK) ? WHITE : BLACK;
        return true;
    }
    
    void undo() {
        if (move_count == 0) return;
        move_count--;
        const Move& last = history[move_count];
        cells[last.pos.y * BOARD_SIZE + last.pos.x] = EMPTY;
        current_player = last.player;
    }
    
    int moveCount() const { return move_count; }
    const Move& getHistory(int i) const { return history[i]; }
    
    // 判断胜负：返回获胜玩家，0=未结束
    int checkWin(int x, int y) const {
        int player = cells[y * BOARD_SIZE + x];
        if (player == EMPTY) return 0;
        for (int d = 0; d < 4; d++) {
            int count = 1;
            // 正向
            int cx = x + DX[d], cy = y + DY[d];
            while (inBounds(cx, cy) && cells[cy * BOARD_SIZE + cx] == player) {
                count++; cx += DX[d]; cy += DY[d];
            }
            // 反向
            cx = x - DX[d]; cy = y - DY[d];
            while (inBounds(cx, cy) && cells[cy * BOARD_SIZE + cx] == player) {
                count++; cx -= DX[d]; cy -= DY[d];
            }
            if (count >= 5) return player;
        }
        return 0;
    }
    
    bool isFull() const { return move_count >= TOTAL_CELLS; }
    
    // 获取棋盘数据指针（用于 JS 访问）
    const uint8_t* data() const { return cells; }
    
private:
    uint8_t cells[TOTAL_CELLS];
    Move history[TOTAL_CELLS];
    int move_count;
    int current_player;
};

// ============= 评估函数 =============

// 计算单个玩家的棋型总分
int64_t evaluatePatterns(const Board& board, int player, int* outFour = nullptr, int* outOpen3 = nullptr, int* outClosed3 = nullptr);

// 完整评估（当前视角的分数）
int64_t evaluatePosition(const Board& board);

// ============= 候选点生成 =============

std::vector<Move> generateCandidates(Board& board, int maxCandidates, bool isRoot = false);

// ============= 搜索 =============

struct SearchResult {
    int bestX = -1;
    int bestY = -1;
    int64_t bestScore = 0;
    int searchDepth = 0;
    int64_t nodesVisited = 0;
    int64_t elapsedMs = 0;
    bool timedOut = false;
};

// 设置搜索参数
void setDifficulty(Difficulty level);
void setTimeBudget(int ms);

// 获取当前参数
int getMaxDepth();
int getTimeBudgetMs();

// AI 主入口
SearchResult findBestMove(Board& board);

// ============= 调试 =============
int64_t getNodesVisited();
void resetStats();
// 清空置换表（新局 / 悔棋时调用，保证同一局面在干净状态下可复现）
void clearSearchTables();

// 获取最后一次搜索的统计信息
int getLastSearchDepth();
int64_t getLastSearchNodes();
int64_t getLastSearchTimeMs();
// 上一次搜索的根节点评分：处于 [-WIN_THRESHOLD, -(WIN_THRESHOLD-999)] 区间
// 表示「所有着法都已证败」（该结论只在候选完整扫描后才成立）
int64_t getLastSearchScore();
// 当前局面轮走方是否存在连续冲四必胜（VCF），供自检/测试
bool hasVcfWin(Board& board);

#endif // GOMOKU_ENGINE_H
