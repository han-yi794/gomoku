// main.cpp - Emscripten 绑定层
// 将 C++ 引擎暴露给 JavaScript (WASM)

#include <emscripten/emscripten.h>
#include "engine.h"
#include <cstdio>

// 全局棋盘实例
static Board g_board;
static Difficulty g_currentDifficulty = Difficulty::MEDIUM;

// ============= WASM 导出函数 =============

extern "C" {

// 初始化游戏
EMSCRIPTEN_KEEPALIVE
void initGame() {
    g_board.reset();
    setDifficulty(g_currentDifficulty);
    resetStats();
    printf("Game initialized\n");
}

// 设置难度
EMSCRIPTEN_KEEPALIVE
void setDifficultyLevel(int level) {
    g_currentDifficulty = (Difficulty)level;
    setDifficulty(g_currentDifficulty);
    printf("Difficulty set to %d\n", level);
}

// 设置时间预算（毫秒）
EMSCRIPTEN_KEEPALIVE
void setTimeLimit(int ms) {
    setTimeBudget(ms);
    printf("Time budget set to %dms\n", ms);
}

// 玩家落子
// 返回: 0=正常, 1=黑棋胜, 2=白棋胜, 3=平局, -1=无效位置
EMSCRIPTEN_KEEPALIVE
int playerMove(int x, int y) {
    if (x < 0 || x >= BOARD_SIZE || y < 0 || y >= BOARD_SIZE) {
        return -1;
    }
    if (g_board.get(x, y) != EMPTY) {
        return -1;
    }
    
    int player = g_board.getPlayer();
    g_board.makeMove(x, y);
    
    int winner = g_board.checkWin(x, y);
    if (winner == BLACK) return 1;
    if (winner == WHITE) return 2;
    if (g_board.isFull()) return 3;
    
    return 0;
}

// AI 思考并落子
// 返回: 0=正常, 1=黑棋胜, 2=白棋胜, 3=平局
EMSCRIPTEN_KEEPALIVE
int aiMove() {
    if (g_board.isFull()) return 3;
    
    SearchResult result = findBestMove(g_board);
    
    if (result.bestX < 0 || result.bestY < 0) {
        printf("AI failed to find a move\n");
        return -1;
    }
    
    int x = result.bestX;
    int y = result.bestY;
    int player = g_board.getPlayer();
    
    g_board.makeMove(x, y);
    
    printf("AI move: (%d,%d) for player %d, depth=%d, nodes=%lld, time=%lldms, score=%lld\n",
           x, y, player, result.searchDepth,
           (long long)result.nodesVisited,
           (long long)result.elapsedMs,
           (long long)result.bestScore);
    
    int winner = g_board.checkWin(x, y);
    if (winner == BLACK) return 1;
    if (winner == WHITE) return 2;
    if (g_board.isFull()) return 3;
    
    return 0;
}

// 悔棋
EMSCRIPTEN_KEEPALIVE
void undo() {
    if (g_board.moveCount() > 0) {
        g_board.undo();
        printf("Undo: move count now %d\n", g_board.moveCount());
    }
}

// 悔棋 n 步
EMSCRIPTEN_KEEPALIVE
void undoN(int n) {
    for (int i = 0; i < n && g_board.moveCount() > 0; i++) {
        g_board.undo();
    }
    printf("Undo %d: move count now %d\n", n, g_board.moveCount());
}

// 获取棋盘数据（复制到 JS 缓冲区）
EMSCRIPTEN_KEEPALIVE
void getBoardData(uint8_t* out) {
    memcpy(out, g_board.data(), TOTAL_CELLS);
}

// 获取棋盘某个点的值
EMSCRIPTEN_KEEPALIVE
int getCell(int x, int y) {
    if (x < 0 || x >= BOARD_SIZE || y < 0 || y >= BOARD_SIZE) return -1;
    return g_board.get(x, y);
}

// 获取当前玩家 (1=黑, 2=白)
EMSCRIPTEN_KEEPALIVE
int getCurrentPlayer() {
    return g_board.getPlayer();
}

// 获取已落子数
EMSCRIPTEN_KEEPALIVE
int getMoveCount() {
    return g_board.moveCount();
}

// 获取游戏状态
// 0=进行中, 1=黑棋胜, 2=白棋胜, 3=平局
EMSCRIPTEN_KEEPALIVE
int getGameStatus() {
    if (g_board.moveCount() == 0) return 0;
    
    // 检查最后一个落子是否获胜
    const Move& last = g_board.getHistory(g_board.moveCount() - 1);
    int winner = g_board.checkWin(last.pos.x, last.pos.y);
    if (winner == BLACK) return 1;
    if (winner == WHITE) return 2;
    
    if (g_board.isFull()) return 3;
    return 0;
}

// 获取棋盘大小
EMSCRIPTEN_KEEPALIVE
int getBoardSize() {
    return BOARD_SIZE;
}

// 获取搜索统计（简化版）
EMSCRIPTEN_KEEPALIVE
int wasmGetLastSearchDepth() {
    return ::getLastSearchDepth();
}

EMSCRIPTEN_KEEPALIVE
long long wasmGetLastSearchNodes() {
    return (long long)::getLastSearchNodes();
}

EMSCRIPTEN_KEEPALIVE
long long wasmGetLastSearchTimeMs() {
    return (long long)::getLastSearchTimeMs();
}

// 获取上一次搜索的根节点评分（用于自检：是否给出「全部已证败」结论）
EMSCRIPTEN_KEEPALIVE
long long wasmGetLastSearchScore() {
    return (long long)::getLastSearchScore();
}

// 评估当前局面（调试用）
EMSCRIPTEN_KEEPALIVE
long long evaluateBoard() {
    return (long long)evaluatePosition(g_board);
}

// 获取候选点数量（调试用）
EMSCRIPTEN_KEEPALIVE
int getCandidateCount(int limit) {
    auto candidates = generateCandidates(g_board, limit, true);
    return (int)candidates.size();
}

// 设置当前难度级别字符串
EMSCRIPTEN_KEEPALIVE
void printInfo() {
    printf("=== Gomoku Engine Info ===\n");
    printf("Board size: %d x %d\n", BOARD_SIZE, BOARD_SIZE);
    printf("Difficulty: %d\n", (int)g_currentDifficulty);
    printf("Max depth: %d\n", getMaxDepth());
    printf("Time budget: %dms\n", getTimeBudgetMs());
    printf("Move count: %d\n", g_board.moveCount());
    printf("Current player: %d\n", g_board.getPlayer());
    printf("===========================\n");
}

} // extern "C"
