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
//   gomoku_cli dump [--games N] [--budget MS] [--threads T] [--out FILE]
//                                                    — 自对弈采集 NNUE 蒸馏数据
//                                                     （每行：score TAB 特征向量）
#include "engine.h"
#include "nnue.h"

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

    // --nnue <path>：所有命令通用的权重加载（先于各解析；默认启用网络路径）
    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        const char* path = nullptr;
        if (a == "--nnue" && i + 1 < argc) path = argv[++i];
        else if (a.rfind("--nnue=", 0) == 0) path = a.c_str() + 7;
        if (path) {
            if (!Nnue::nnueLoad(path)) return 1;
            Nnue::nnueSetEnabled(true);
            std::fprintf(stderr, "[cli] nnue enabled\n");
        }
    }

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

    // match：古典 vs NNUE 同时限对局（换先各半），A/B 判定「不劣于旧」
    if (cmd == "match") {
        int games = 10;
        int matchBudget = 20000;
        int matchThreads = 1;
        std::string nnuePath;
        for (int i = 2; i < argc; i++) {
            const std::string a = argv[i];
            if (a == "--games" && i + 1 < argc) games = std::atoi(argv[++i]);
            else if (a.rfind("--games=", 0) == 0) games = std::atoi(a.c_str() + 8);
            else if (a == "--budget" && i + 1 < argc) matchBudget = std::atoi(argv[++i]);
            else if (a == "--threads" && i + 1 < argc) matchThreads = std::atoi(argv[++i]);
            else if (a == "--nnue" && i + 1 < argc) nnuePath = argv[++i];
            else if (a.rfind("--budget=", 0) == 0) matchBudget = std::atoi(a.c_str() + 9);
            else if (a.rfind("--threads=", 0) == 0) matchThreads = std::atoi(a.c_str() + 10);
            else if (a.rfind("--nnue=", 0) == 0) nnuePath = a.c_str() + 7;
            else { std::fprintf(stderr, "bad argument: %s\n", a.c_str()); return 1; }
        }
        if (nnuePath.empty()) { std::fprintf(stderr, "match needs --nnue <file>\n"); return 1; }

        int classicalWins = 0, nnueWins = 0, draws = 0;
        for (int g = 0; g < games; g++) {
            const bool classicalIsBlack = (g % 2 == 0);
            Board board;
            board.reset();
            clearSearchTables();
            setDifficulty(Difficulty::HARD);
            setTimeBudget(matchBudget);
            int winner = 0;
            while (board.moveCount() < TOTAL_CELLS) {
                const int player = board.getPlayer();
                const bool useNnue = (player == BLACK) != classicalIsBlack;  // 异或：黑方持古典?
                Nnue::nnueSetEnabled(useNnue);
                const SearchResult r = findBestMoveParallel(board, matchThreads);
                if (r.bestX < 0) break;
                board.makeMove(r.bestX, r.bestY);
                if (board.checkWin(r.bestX, r.bestY) != 0) { winner = player; break; }
            }
            Nnue::nnueSetEnabled(false);
            if (winner == 0) draws++;
            else if ((winner == BLACK) == classicalIsBlack) classicalWins++;
            else nnueWins++;
            std::fprintf(stderr, "game %d/%d: %s (classical black=%d)\n", g + 1, games,
                         winner == 0 ? "draw" : (winner == BLACK ? "black wins" : "white wins"),
                         classicalIsBlack ? 1 : 0);
        }
        std::printf("match: classical=%d nnue=%d draws=%d (of %d)\n",
                    classicalWins, nnueWins, draws, games);
        return 0;
    }

    // dump：自对弈采集 NNUE 蒸馏数据（每步：局面特征 + 该局面引擎根评分）
    if (cmd == "dump") {
        int games = 10;
        int dumpBudget = 20000;
        int dumpThreads = 1;
        std::string outPath = "nnue_dump.txt";
        for (int i = 2; i < argc; i++) {
            const std::string a = argv[i];
            if (a == "--games" && i + 1 < argc) games = std::atoi(argv[++i]);
            else if (a.rfind("--games=", 0) == 0) games = std::atoi(a.c_str() + 8);
            else if (a == "--budget" && i + 1 < argc) dumpBudget = std::atoi(argv[++i]);
            else if (a == "--threads" && i + 1 < argc) dumpThreads = std::atoi(argv[++i]);
            else if (a == "--out" && i + 1 < argc) outPath = argv[++i];
            else if (a == "--nnue") { ++i; }        // 已被预扫描处理
            else if (a.rfind("--nnue=", 0) == 0) {}
            else if (a.rfind("--budget=", 0) == 0) dumpBudget = std::atoi(a.c_str() + 9);
            else if (a.rfind("--threads=", 0) == 0) dumpThreads = std::atoi(a.c_str() + 10);
            else if (a.rfind("--out=", 0) == 0) outPath = a.c_str() + 6;
            else {
                std::fprintf(stderr, "bad argument: %s\n", a.c_str());
                return 1;
            }
        }
        std::FILE* f = std::fopen(outPath.c_str(), "w");
        if (!f) {
            std::fprintf(stderr, "cannot open %s\n", outPath.c_str());
            return 1;
        }
        std::fprintf(f, "# NNUE dump v0: label=root search score (side-to-move perspective)\n");
        std::fprintf(f, "# features: [us 8 pattern types x 3 zones][opp 8x3][stm_white=1][move_count]\n");
        std::fprintf(f, "# dim=%d score<TAB>f0 f1 ... f%d\n", Nnue::FEATURE_DIM, Nnue::FEATURE_DIM - 1);

        int feats[Nnue::FEATURE_DIM];
        long long lines = 0;
        for (int g = 0; g < games; g++) {
            Board board;
            board.reset();
            clearSearchTables();
            setDifficulty(Difficulty::HARD);
            setTimeBudget(dumpBudget);
            while (board.moveCount() < TOTAL_CELLS) {
                Nnue::extractFeatures(board, feats);
                const SearchResult r = findBestMoveParallel(board, dumpThreads);
                if (r.bestX < 0) break;
                // 标签行：当前局面的根评分（走子方视角）；终局分可能是 mate 分，训练端处理
                std::fprintf(f, "%lld\t", (long long)r.bestScore);
                for (int i = 0; i < Nnue::FEATURE_DIM; i++) {
                    std::fprintf(f, "%s%d", i ? " " : "", feats[i]);
                }
                std::fprintf(f, "\n");
                lines++;
                board.makeMove(r.bestX, r.bestY);
                if (board.checkWin(r.bestX, r.bestY) != 0) break;   // 分出了胜负即止
            }
            std::fprintf(stderr, "game %d/%d done (%d moves, %lld lines total)\n",
                         g + 1, games, board.moveCount(), lines);
        }
        std::fclose(f);
        std::printf("dump done: %s (%lld lines, dim=%d)\n", outPath.c_str(), lines, Nnue::FEATURE_DIM);
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
        if (a == "--nnue") { ++i; continue; }       // 已被预扫描处理
        if (a.rfind("--nnue=", 0) == 0) continue;
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