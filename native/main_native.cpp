// native/main_native.cpp — 本机原生 CLI（无 Emscripten 依赖）
// 用途：多线程研究平台 + 本机基准（交接文档：多线程先做原生 CLI，浏览器 WASM 受
// Pages 的 COOP/COEP 限制暂不支持 SharedArrayBuffer）。
//
// 命令：
//   gomoku_cli bench [threads]                      — AGENTS.md 夹具 20s 档复现 + 期望校验
//   gomoku_cli search [--threads N] [--budget MS] x,y x,y ...
//                                                    — 播棋后搜索（坐标 0..14，(列,行)）
//   gomoku_cli selfplay [--threads N] [--budget MS] — 机机对局到终局（打印棋谱）
//   gomoku_cli nps [threads] [budget]               — 单步纯节点吞吐测量
#include "engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Fixture {
    const char* tag;
    std::vector<std::pair<int, int>> line;
    int n;
    int expectX, expectY;
    int altX = -1, altY = -1;   // 可选替代期望（m8 持平局：两手皆可，见 AGENTS.md）
};

// AGENTS.md「回归夹具」节的 20s 档期望（game11-m8 为已记录的持平局，(5,9)/(9,5) 皆可）
const std::vector<Fixture>& fixtures() {
    static const std::vector<Fixture> f = {
        {"battery-n1",  {{7,7},{7,6},{6,6},{8,8},{6,7},{8,7},{6,8},{6,9},{6,5},{6,4},{8,6},{9,5},{5,9},{4,10},{5,7},{4,6},{4,8}}, 1, 7, 8},
        {"battery-n13", {{7,7},{7,6},{6,6},{8,8},{6,7},{8,7},{6,8},{6,9},{6,5},{6,4},{8,6},{9,5},{5,9},{4,10},{5,7},{4,6},{4,8}}, 13, 4, 10},
        {"battery-n15", {{7,7},{7,6},{6,6},{8,8},{6,7},{8,7},{6,8},{6,9},{6,5},{6,4},{8,6},{9,5},{5,9},{4,10},{5,7},{4,6},{4,8}}, 15, 5, 10},
        {"battery-n17", {{7,7},{7,6},{6,6},{8,8},{6,7},{8,7},{6,8},{6,9},{6,5},{6,4},{8,6},{9,5},{5,9},{4,10},{5,7},{4,6},{4,8}}, 17, 3, 9},
        {"game11-m8",   {{7,7},{8,7},{8,6},{7,6},{6,6},{8,8},{6,8},{9,5},{6,7},{6,9},{5,7},{7,9},{7,5},{8,4}}, 7, 5, 9, 9, 5},
        {"game11-m10",  {{7,7},{8,7},{8,6},{7,6},{6,6},{8,8},{6,8},{9,5},{6,7},{6,9},{5,7},{7,9},{7,5},{8,4}}, 9, 6, 5},
        {"game11-m12",  {{7,7},{8,7},{8,6},{7,6},{6,6},{8,8},{6,8},{9,5},{6,7},{6,9},{5,7},{7,9},{7,5},{8,4}}, 11, 6, 5},
        {"game12-m13",  {{7,7},{7,8},{8,6},{6,8},{8,8},{9,9},{9,7},{8,7},{9,6},{10,6},{9,5},{9,4},{7,5}}, 13, 6, 4},
        {"game13-m11",  {{7,7},{7,8},{6,8},{5,9},{8,6},{5,10},{9,5},{10,4},{9,7},{10,8},{10,6}}, 11, 5, 11},
    };
    return f;
}

bool parsePair(const char* s, int& x, int& y) {
    return std::sscanf(s, "%d,%d", &x, &y) == 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf(
            "usage:\n"
            "  gomoku_cli bench [threads]\n"
            "  gomoku_cli search [--threads N] [--budget MS] x,y x,y ...\n"
            "  gomoku_cli selfplay [--threads N] [--budget MS]\n"
            "  gomoku_cli nps [threads] [budget]\n");
        return 1;
    }
    const std::string cmd = argv[1];

    // bench [threads]：位置参数，先于通用解析处理
    // nps [threads] [budget]：位置参数，先于通用解析处理
    if (cmd == "nps") {
        const int t = (argc > 2 && std::isdigit((unsigned char)argv[2][0])) ? std::atoi(argv[2]) : 1;
        const int b = (argc > 3 && std::isdigit((unsigned char)argv[3][0])) ? std::atoi(argv[3]) : 20000;
        Board board;
        board.reset();
        setDifficulty(Difficulty::HARD);
        setTimeBudget(b);
        board.makeMove(7, 7);
        board.makeMove(8, 8);
        const SearchResult r = findBestMoveParallel(board, t);
        const double nps = r.elapsedMs > 0 ? 1000.0 * (double)r.nodesVisited / r.elapsedMs : 0.0;
        const int merge = getParallelMergeInfo();
        std::printf("nps threads=%d nodes=%lld ms=%lld -> %.0f nodes/s\n",
                    t, (long long)r.nodesVisited, (long long)r.elapsedMs, nps, merge);
        return 0;
    }

    // 公共参数解析
    int threads = 1;
    int budget = 20000;
    std::vector<std::pair<int, int>> moves;
    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        // bench 的位置线程参数已被快路消费，通用解析跳过它
        if (cmd == "bench" && i == 2 && std::isdigit((unsigned char)a[0])) continue;
        if (a == "--threads" && i + 1 < argc) threads = std::atoi(argv[++i]);
        else if (a == "--budget" && i + 1 < argc) budget = std::atoi(argv[++i]);
        else if (a.rfind("--threads=", 0) == 0) threads = std::atoi(a.c_str() + 10);
        else if (a.rfind("--budget=", 0) == 0) budget = std::atoi(a.c_str() + 10);
        else {
            int x, y;
            if (!parsePair(a.c_str(), x, y)) {
                std::fprintf(stderr, "bad argument: %s\n", a.c_str());
                return 1;
            }
            moves.emplace_back(x, y);
        }
    }
    if (cmd == "bench" && argc > 2 && std::isdigit((unsigned char)argv[2][0])) {
        threads = std::atoi(argv[2]);
    }

    if (cmd == "bench") {
        std::printf("=== bench (threads=%d, budget=20000ms) ===\n", threads);
        int pass = 0;
        for (const Fixture& fx : fixtures()) {
            Board board;
            board.reset();
            clearSearchTables();   // 与 wasm 测试的 initGame 对齐：夹具间 TT 不互渗
            setDifficulty(Difficulty::HARD);
            setTimeBudget(20000);
            for (int i = 0; i < fx.n; i++) board.makeMove(fx.line[i].first, fx.line[i].second);
            SearchResult r = findBestMoveParallel(board, threads);
            const bool ok = (r.bestX == fx.expectX && r.bestY == fx.expectY) ||
                           (fx.altX >= 0 && r.bestX == fx.altX && r.bestY == fx.altY);
            if (ok) pass++;
            const double nps = r.elapsedMs > 0 ? 1000.0 * (double)r.nodesVisited / r.elapsedMs : 0.0;
            const int merge = getParallelMergeInfo();
            std::printf("%-6s %-12s move=(%d,%d) expect=(%d,%d) score=%lld depth=%d nodes=%lld ms=%lld nps=%.0f merge=%d\n",
                        ok ? "PASS" : "DIFF", fx.tag, r.bestX, r.bestY, fx.expectX, fx.expectY,
                        (long long)r.bestScore, r.searchDepth, (long long)r.nodesVisited,
                        (long long)r.elapsedMs, nps, merge);
        }
        std::printf("=== bench: %d/%d pass ===\n", pass, (int)fixtures().size());
        return pass == (int)fixtures().size() ? 0 : 2;
    }

    if (cmd == "search") {
        Board board;
        board.reset();
        setDifficulty(Difficulty::HARD);
        setTimeBudget(budget);
        for (const auto& m : moves) {
            if (!board.makeMove(m.first, m.second)) {
                std::fprintf(stderr, "illegal move (%d,%d)\n", m.first, m.second);
                return 1;
            }
        }
        SearchResult r = findBestMoveParallel(board, threads);
        std::printf("move=(%d,%d) score=%lld depth=%d nodes=%lld ms=%lld timedOut=%d\n",
                    r.bestX, r.bestY, (long long)r.bestScore, r.searchDepth,
                    (long long)r.nodesVisited, (long long)r.elapsedMs, r.timedOut ? 1 : 0);
        return 0;
    }

    if (cmd == "selfplay") {
        Board board;
        board.reset();
        setDifficulty(Difficulty::HARD);
        setTimeBudget(budget);
        std::printf("# selfplay threads=%d budget=%dms\n", threads, budget);
        int ply = 0;
        while (board.moveCount() < TOTAL_CELLS) {
            SearchResult r = findBestMoveParallel(board, threads);
            if (r.bestX < 0) break;
            const int player = board.getPlayer();
            board.makeMove(r.bestX, r.bestY);
            std::printf("%d,%d %d %d %lld %d\n", r.bestX, r.bestY, player,
                        r.searchDepth, (long long)r.bestScore, (int)r.nodesVisited);
            if (board.checkWin(r.bestX, r.bestY) != 0) {
                std::printf("# game over: player %d wins at ply %d\n", player, ply + 1);
                return 0;
            }
            ply++;
        }
        std::printf("# game over: draw\n");
        return 0;
    }

    std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
    return 1;
}