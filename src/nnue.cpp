// nnue.cpp - NNUE spike 特征提取与量化推理
//
// 复用与 evaluatePatterns 相同的「序列扫描」语义（只在序列起点计数、看两端开放
// 情况），但按 8 类棋型 x 3 区分别计数，并区分双方。连续段归属区 = 段中点所在区。
// v1 起加入「下一手威胁」计数（跳三/跳四盲区的直接代理）。
//
// 特征规格只此一处定义（nnue.h）：dump（native CLI）与量化推理共用，杜绝漂移。
#include "nnue.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

#ifdef GOMOKU_WITH_NNUE
#include "nnue_weights.h"   // 内嵌权重（export_weights_header.py 生成）
#endif

namespace Nnue {

const char* patternName(int type) {
    static const char* names[PATTERN_NB] = {"five", "o4", "c4", "o3", "s3", "o2", "s2", "o1", "o4t", "c4t"};
    return (type >= 0 && type < PATTERN_NB) ? names[type] : "?";
}

const char* zoneName(int zone) {
    static const char* names[ZONE_NB] = {"center", "mid", "outer"};
    return (zone >= 0 && zone < ZONE_NB) ? names[zone] : "?";
}

// Chebyshev 距离分区（棋盘 15x15，中心 (7,7)，最大距 7）
static inline int zoneOf(int x, int y) {
    const int dx = x > 7 ? x - 7 : 7 - x;
    const int dy = y > 7 ? y - 7 : 7 - y;
    const int d = dx > dy ? dx : dy;
    if (d <= 2) return Z_CENTER;
    if (d <= 4) return Z_MID;
    return Z_OUTER;
}

// (连续段长度, 开放端数) -> 棋型索引；无法归类返回 -1
static inline int classifyRun(int count, int openEnds) {
    if (count >= 5) return FIVE;
    switch (count) {
        case 4: return openEnds == 2 ? OPEN4 : (openEnds == 1 ? CLOSED4 : -1);
        case 3: return openEnds == 2 ? OPEN3 : (openEnds == 1 ? SLEEP3 : -1);
        case 2: return openEnds == 2 ? OPEN2 : (openEnds == 1 ? SLEEP2 : -1);
        case 1: return openEnds == 2 ? OPEN1 : -1;
        default: return -1;
    }
}

// 试在 (x,y) 落子后，沿方向 d 是否形成四及两端开放情况（下一手威胁计数用）。
// 返回：2=活四威胁(两端空)或直接成五，1=冲四威胁(一端空)，0=无
static int fourThreatOn(const Board& b, int x, int y, int player, int d) {
    int count = 1;  // 包含试落点
    int cx = x + DX[d], cy = y + DY[d];
    while (b.inBounds(cx, cy) && b.get(cx, cy) == player) { count++; cx += DX[d]; cy += DY[d]; }
    const bool aopen = b.inBounds(cx, cy) && b.get(cx, cy) == EMPTY;
    cx = x - DX[d]; cy = y - DY[d];
    while (b.inBounds(cx, cy) && b.get(cx, cy) == player) { count++; cx -= DX[d]; cy -= DY[d]; }
    const bool bopen = b.inBounds(cx, cy) && b.get(cx, cy) == EMPTY;
    if (count >= 5) return 2;                 // 直接成五也是最强威胁
    if (count == 4) return aopen && bopen ? 2 : (aopen || bopen ? 1 : 0);
    return 0;
}

// 空点周边是否有子（威胁只可能在已有棋子附近形成）
static bool nearAnyStone(const Board& b, int x, int y, int radius) {
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx == 0 && dy == 0) continue;
            const int nx = x + dx, ny = y + dy;
            if (b.inBounds(nx, ny) && b.get(nx, ny) != EMPTY) return true;
        }
    }
    return false;
}

void extractFeatures(const Board& board, int* out) {
    std::memset(out, 0, sizeof(int) * FEATURE_DIM);

    // 双方分别扫描；us 块在前、对手块在后
    for (int side = 0; side < 2; side++) {
        const int player = side == 0 ? board.getPlayer()
                                     : (board.getPlayer() == BLACK ? WHITE : BLACK);
        int* base = out + side * (PATTERN_NB * ZONE_NB);

        for (int d = 0; d < 4; d++) {
            const int dx = DX[d], dy = DY[d];
            for (int y = 0; y < BOARD_SIZE; y++) {
                for (int x = 0; x < BOARD_SIZE; x++) {
                    // 只从序列起点计数（前一格不是自己的棋子）
                    const int px = x - dx, py = y - dy;
                    if (board.inBounds(px, py) && board.get(px, py) == player) continue;

                    int count = 0;
                    int cx = x, cy = y;
                    while (board.inBounds(cx, cy) && board.get(cx, cy) == player) {
                        count++;
                        cx += dx;
                        cy += dy;
                    }
                    if (count == 0) continue;

                    const int bx = x - dx, by = y - dy;                       // 后端
                    const int ax = cx, ay = cy;                               // 前端（首个非己子）
                    const bool bopen = board.inBounds(bx, by) && board.get(bx, by) == EMPTY;
                    const bool aopen = board.inBounds(ax, ay) && board.get(ax, ay) == EMPTY;
                    const int openEnds = (bopen ? 1 : 0) + (aopen ? 1 : 0);

                    const int type = classifyRun(count, openEnds);
                    if (type < 0) continue;

                    // 段中点定区（离散中点，避免浮点）
                    const int mx = (x + (cx - dx)) / 2;
                    const int my = (y + (cy - dy)) / 2;
                    const int zone = zoneOf(mx, my);
                    base[type * ZONE_NB + zone]++;
                }
            }
        }
    }

    // v1：下一手威胁（每个空点试落子的最强威胁；只查周边有子的点，成本可控）
    for (int side = 0; side < 2; side++) {
        const int player = side == 0 ? board.getPlayer()
                                     : (board.getPlayer() == BLACK ? WHITE : BLACK);
        int* base = out + side * (PATTERN_NB * ZONE_NB);
        for (int y = 0; y < BOARD_SIZE; y++) {
            for (int x = 0; x < BOARD_SIZE; x++) {
                if (board.get(x, y) != EMPTY) continue;
                if (!nearAnyStone(board, x, y, NEIGHBOR_RADIUS)) continue;
                int best = 0;   // 该点的最强威胁（活四 > 冲四）
                for (int d = 0; d < 4; d++) {
                    const int t = fourThreatOn(board, x, y, player, d);
                    if (t > best) best = t;
                }
                if (best == 2) base[O4THREAT * ZONE_NB + zoneOf(x, y)]++;
                else if (best == 1) base[C4THREAT * ZONE_NB + zoneOf(x, y)]++;
            }
        }
    }

    out[FEATURE_OFFSET_SIDE_TO_MOVE] = (board.getPlayer() == WHITE) ? 1 : 0;
    out[FEATURE_OFFSET_MOVE_COUNT] = board.moveCount();
}

// ============= 量化推理 =============

namespace {

struct Layer {
    int in, out;
    std::vector<float> w;    // [out][in]（反量化后的 float）
    std::vector<float> b;    // [out]
};

bool g_loaded = false;
bool g_enabled = false;
int g_dimIn = 0;
std::vector<float> g_mean, g_std;
std::vector<Layer> g_layers;   // 3 层：in->h1, h1->h2, h2->1

bool readAll(std::FILE* f, void* buf, size_t bytes) {
    return std::fread(buf, 1, bytes, f) == bytes;
}

}  // namespace

// 从内存解析权重包（与文件加载共用全部逻辑；wasm 无文件系统，走内嵌数组）
static bool nnueParse(const uint8_t* data, size_t len) {
    size_t off = 0;
    auto read = [&](void* buf, size_t n) -> bool {
        if (off + n > len) return false;
        std::memcpy(buf, data + off, n);
        off += n;
        return true;
    };

    uint32_t magic, version, dIn, dH1, dH2, dOut, r0, r1, r2;
    bool ok = read(&magic, 4) && read(&version, 4) &&
              read(&dIn, 4) && read(&dH1, 4) && read(&dH2, 4) &&
              read(&dOut, 4) && read(&r0, 4) && read(&r1, 4) && read(&r2, 4);
    if (!ok || magic != 0x4E4E5545u || version != 1 || dIn != (uint32_t)FEATURE_DIM || dOut != 1) {
        std::fprintf(stderr, "[nnue] bad header (magic=%08x ver=%u in=%u out=%u)\n", magic, version, dIn, dOut);
        return false;
    }

    g_mean.resize(dIn);
    g_std.resize(dIn);
    if (!read(g_mean.data(), dIn * 4) || !read(g_std.data(), dIn * 4)) {
        std::fprintf(stderr, "[nnue] truncated stats\n");
        return false;
    }

    const int dims[4] = {(int)dIn, (int)dH1, (int)dH2, (int)dOut};
    g_layers.clear();
    for (int li = 0; li < 3; li++) {
        Layer L;
        L.in = dims[li];
        L.out = dims[li + 1];
        float scale;
        if (!read(&scale, 4)) return false;
        std::vector<int8_t> q((size_t)L.in * L.out);
        if (!read(q.data(), q.size())) return false;
        L.w.resize((size_t)L.in * L.out);
        for (size_t i = 0; i < q.size(); i++) L.w[i] = (float)q[i] * scale;
        L.b.resize(L.out);
        if (!read(L.b.data(), L.out * 4)) return false;
        g_layers.push_back(std::move(L));
    }
    g_dimIn = (int)dIn;
    g_loaded = true;
    std::fprintf(stderr, "[nnue] loaded: %d->%d->%d->%d\n", dims[0], dims[1], dims[2], dims[3]);
    return true;
}

bool nnueLoad(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "[nnue] cannot open %s\n", path);
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) { std::fclose(f); return false; }
    std::vector<uint8_t> buf((size_t)size);
    const size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (got != buf.size()) return false;
    return nnueParse(buf.data(), buf.size());
}

// 内嵌权重（src/nnue_weights.h，由 export_weights_header.py 生成）——wasm 路径专用入口
bool nnueLoadEmbedded() {
#ifdef GOMOKU_WITH_NNUE
    return nnueParse(NNUE_WEIGHTS, NNUE_WEIGHTS_LEN);
#else
    std::fprintf(stderr, "[nnue] embedded weights not compiled in\n");
    return false;
#endif
}

bool nnueLoaded() { return g_loaded; }

void nnueSetEnabled(bool on) { g_enabled = on && g_loaded; }

bool nnueEnabled() { return g_enabled; }

int64_t nnueEvaluate(const Board& board) {
    if (!g_loaded) return 0;
    int feats[FEATURE_DIM];
    extractFeatures(board, feats);

    // 免分配前向：定长栈缓冲（维度上限写死；导出头里有校验）。
    constexpr int MAX_DIM = 128;
    float buf0[MAX_DIM], buf1[MAX_DIM];
    if (g_dimIn > MAX_DIM) return 0;
    for (int i = 0; i < g_dimIn; i++) {
        const float s = g_std[i] > 1e-6f ? g_std[i] : 1.0f;
        buf0[i] = ((float)feats[i] - g_mean[i]) / s;
    }
    float* cur = buf0;
    float* next = buf1;

    for (size_t li = 0; li < g_layers.size(); li++) {
        const Layer& L = g_layers[li];
        if (L.out > MAX_DIM) return 0;
        for (int o = 0; o < L.out; o++) {
            float acc = L.b[o];
            const float* w = &L.w[(size_t)o * L.in];
            for (int i = 0; i < L.in; i++) acc += w[i] * cur[i];
            next[o] = acc;
        }
        if (li + 1 < g_layers.size()) {
            // 隐藏层 ReLU
            for (int o = 0; o < L.out; o++) if (next[o] < 0.0f) next[o] = 0.0f;
        }
        // 输出层线性（无截断）：隔离实验判定截断会把 15:1 打到 8:5——无界输出的
        // 「过自信」信号（可达 ±5.4）正是 NNUE 强战术性的来源，不要截断。
        float* tmp = cur; cur = next; next = tmp;
    }
    return (int64_t)std::lround(cur[0] * (float)NNUE_EVAL_SCALE);
}

}  // namespace Nnue