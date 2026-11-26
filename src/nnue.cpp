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
    return nnueEvaluateRaw(feats);
}

int64_t nnueEvaluateRaw(const int* feats) {
    if (!g_loaded) return 0;
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


// ============= 增量特征累加器 =============

// 虚拟落子威胁检测：(vx,vy) 视为 vPlayer 的落子（威胁 delta 的双态差分用）
static int threatOnVirt(const Board& b, int px, int py, int player, int d, int vx, int vy, int vPlayer) {
    auto cell = [&](int cx, int cy) -> int {
        if (cx == vx && cy == vy) return vPlayer;
        return b.inBounds(cx, cy) ? b.get(cx, cy) : -1;
    };
    int count = 1;
    int cx = px + DX[d], cy = py + DY[d];
    while (cell(cx, cy) == player) { count++; cx += DX[d]; cy += DY[d]; }
    const bool aopen = cell(cx, cy) == EMPTY;
    cx = px - DX[d]; cy = py - DY[d];
    while (cell(cx, cy) == player) { count++; cx -= DX[d]; cy -= DY[d]; }
    const bool bopen = cell(cx, cy) == EMPTY;
    if (count >= 5) return 2;
    if (count == 4) return aopen && bopen ? 2 : (aopen || bopen ? 1 : 0);
    return 0;
}

// 单方向 d 的段增量。merged=false 计入「左段/右段（靠 (x,y) 空点封开）」，
// merged=true 计入「lLen+1+rLen 合并段」。attrSide0 = 归因空间中 side0 的颜色。
void Accumulator::runDelta(const Board& b, int x, int y, int player, int d, int attrSide0, bool merged, int sign) {
    const int dx = DX[d], dy = DY[d];
    const int side = (player == attrSide0) ? 0 : 1;
    int* base = feats_ + side * (PATTERN_NB * ZONE_NB);

    auto scanLeft = [&](int& len, int& openEnd) {
        len = 0; int cx = x - dx, cy = y - dy;
        while (b.inBounds(cx, cy) && b.get(cx, cy) == player) { len++; cx -= dx; cy -= dy; }
        openEnd = (b.inBounds(cx, cy) && b.get(cx, cy) == EMPTY) ? 1 : 0;
    };
    auto scanRight = [&](int& len, int& openEnd) {
        len = 0; int cx = x + dx, cy = y + dy;
        while (b.inBounds(cx, cy) && b.get(cx, cy) == player) { len++; cx += dx; cy += dy; }
        openEnd = (b.inBounds(cx, cy) && b.get(cx, cy) == EMPTY) ? 1 : 0;
    };

    int lLen, lOpen, rLen, rOpen;
    scanLeft(lLen, lOpen);
    scanRight(rLen, rOpen);

    auto add = [&](int type, int mx, int my) {
        if (type < 0) return;
        base[type * ZONE_NB + zoneOf(mx, my)] += sign;
    };

    if (!merged) {
        if (lLen > 0) {
            const int t = classifyRun(lLen, lOpen + 1);   // 右端是空点 (x,y)：封开
            add(t, (x - dx * lLen + (x - dx)) / 2, (y - dy * lLen + (y - dy)) / 2);
        }
        if (rLen > 0) {
            const int t = classifyRun(rLen, 1 + rOpen);   // 左端是空点 (x,y)：封开
            add(t, ((x + dx) + (x + dx * rLen)) / 2, ((y + dy) + (y + dy * rLen)) / 2);
        }
    } else {
        const int total = lLen + 1 + rLen;
        const int t = classifyRun(total, lOpen + rOpen);
        add(t, ((x - dx * lLen) + (x + dx * rLen)) / 2, ((y - dy * lLen) + (y + dy * rLen)) / 2);
    }
}

// 威胁增量：受影响空点 = 4 个方向上距 (x,y) 不超过 4 格的空格（两方都重算——
// 落子可能同时封掉对手的威胁）。from/to 两态用「虚拟落子」开关差分。
void Accumulator::threatDelta(const Board& b, int x, int y, int mover, int attrSide0,
                              bool fromVirtual, bool toVirtual, int sign) {
    // 落子点自身：落子前是可能持有威胁的空点，落子后非空、威胁不再计入。
    // （窗口循环跳过 off=0，这里显式处理两侧。）
    for (int side = 0; side < 2; side++) {
        const int player = side == 0 ? attrSide0 : (attrSide0 == BLACK ? WHITE : BLACK);
        int* base = feats_ + side * (PATTERN_NB * ZONE_NB);
        int bestFrom = 0;
        for (int dd = 0; dd < 4; dd++) {
            const int t = threatOnVirt(b, x, y, player, dd, x, y, player);   // 假设在该点落此方子
            if (t > bestFrom) bestFrom = t;
        }
        const int zone = zoneOf(x, y);
        if (bestFrom == 2) base[O4THREAT * ZONE_NB + zone] += (fromVirtual ? 1 : -1);
        else if (bestFrom == 1) base[C4THREAT * ZONE_NB + zone] += (fromVirtual ? 1 : -1);
        // push（fromVirtual=false）：自身威胁随落子消失 → 减；pop：恢复 → 加
    }
    for (int d = 0; d < 4; d++) {
        for (int off = -4; off <= 4; off++) {
            if (off == 0) continue;
            const int px = x + DX[d] * off, py = y + DY[d] * off;
            if (!b.inBounds(px, py) || b.get(px, py) != EMPTY) continue;
            const int zone = zoneOf(px, py);
            for (int side = 0; side < 2; side++) {
                const int player = side == 0 ? attrSide0 : (attrSide0 == BLACK ? WHITE : BLACK);
                int* base = feats_ + side * (PATTERN_NB * ZONE_NB);
                // 精确优化：两条不同斜率的线只交于 p 自身，故除「穿过 (x,y) 的方向 d」
                // 外其余三方向的威胁与 (x,y) 落子与否无关——普通扫描算一次即可，只有
                // through 方向需要虚拟双态。原实现每点 8 次虚拟扫描，现为 3 普通 + 2 虚拟；
                // 且 already-最强（otherBest==2）的点直接跳过。
                int otherBest = 0;
                for (int dd = 0; dd < 4; dd++) {
                    if (dd == d) continue;
                    const int t = fourThreatOn(b, px, py, player, dd);
                    if (t > otherBest) otherBest = t;
                }
                if (otherBest == 2) continue;
                const int fromT = threatOnVirt(b, px, py, player, d, x, y, fromVirtual ? mover : EMPTY);
                const int toT = threatOnVirt(b, px, py, player, d, x, y, toVirtual ? mover : EMPTY);
                const int from = fromT > otherBest ? fromT : otherBest;
                const int to = toT > otherBest ? toT : otherBest;
                if (from == to) continue;
                if (from == 2) base[O4THREAT * ZONE_NB + zone] -= sign;
                else if (from == 1) base[C4THREAT * ZONE_NB + zone] -= sign;
                if (to == 2) base[O4THREAT * ZONE_NB + zone] += sign;
                else if (to == 1) base[C4THREAT * ZONE_NB + zone] += sign;
            }
        }
    }
}

// 对手邻接段的降级/恢复。blocker = 落在 (x,y) 的颜色；受影响的段是对手方
// （blocker 的相反色）在四个方向上紧邻 (x,y) 的段——(x,y) 端的开放状态翻转。
void Accumulator::blockDelta(const Board& b, int x, int y, int blocker, int d, int attrSide0, bool demote) {
    const int opp = (blocker == BLACK) ? WHITE : BLACK;
    const int dx = DX[d], dy = DY[d];
    const int side = (opp == attrSide0) ? 0 : 1;
    int* base = feats_ + side * (PATTERN_NB * ZONE_NB);

    auto scanLeft = [&](int& len, int& openEnd) {
        len = 0; int cx = x - dx, cy = y - dy;
        while (b.inBounds(cx, cy) && b.get(cx, cy) == opp) { len++; cx -= dx; cy -= dy; }
        openEnd = (b.inBounds(cx, cy) && b.get(cx, cy) == EMPTY) ? 1 : 0;
    };
    auto scanRight = [&](int& len, int& openEnd) {
        len = 0; int cx = x + dx, cy = y + dy;
        while (b.inBounds(cx, cy) && b.get(cx, cy) == opp) { len++; cx += dx; cy += dy; }
        openEnd = (b.inBounds(cx, cy) && b.get(cx, cy) == EMPTY) ? 1 : 0;
    };

    auto apply = [&](int len, int openEnd, int mx, int my) {
        if (len <= 0) return;
        // 旧态：(x,y) 为空 → 该端开放（openEnd + 1）；新态：(x,y) 为 blocker → 封闭
        const int oldType = classifyRun(len, openEnd + 1);
        const int newType = classifyRun(len, openEnd);
        if (demote) {
            if (oldType >= 0) base[oldType * ZONE_NB + zoneOf(mx, my)] -= 1;
            if (newType >= 0) base[newType * ZONE_NB + zoneOf(mx, my)] += 1;
        } else {
            if (newType >= 0) base[newType * ZONE_NB + zoneOf(mx, my)] -= 1;
            if (oldType >= 0) base[oldType * ZONE_NB + zoneOf(mx, my)] += 1;
        }
    };

    int lLen, lOpen, rLen, rOpen;
    scanLeft(lLen, lOpen);
    scanRight(rLen, rOpen);
    if (lLen > 0) apply(lLen, lOpen, (x - dx * lLen + (x - dx)) / 2, (y - dy * lLen + (y - dy)) / 2);
    if (rLen > 0) apply(rLen, rOpen, ((x + dx) + (x + dx * rLen)) / 2, ((y + dy) + (y + dy * rLen)) / 2);
}

void Accumulator::reset(const Board& board) {
    int tmp[FEATURE_DIM];
    extractFeatures(board, tmp);
    std::memcpy(feats_, tmp, sizeof(feats_));
}

void Accumulator::push(const Board& b, int x, int y, int player) {
    // 归因空间 = 行棋前（side0 = 落子方）；调用在 makeMove 之后，扫描绕点进行、
    // 两态（分裂/合并）均可由当前棋盘算出。delta 全部施加后整块互换得到行棋后真值。
    for (int d = 0; d < 4; d++) {
        runDelta(b, x, y, player, d, player, false, -1);          // 旧态：落子方分裂段移除
        runDelta(b, x, y, player, d, player, true, +1);           // 新态：落子方合并段计入
        blockDelta(b, x, y, player, d, player, true);             // 对手邻接活段被落子封成眠段
    }
    threatDelta(b, x, y, player, player, false, true, +1);        // 威胁差分（内部已覆盖 4 向，勿按方向重复调）
    for (int i = 0; i < PATTERN_NB * ZONE_NB; i++) {
        std::swap(feats_[i], feats_[PATTERN_NB * ZONE_NB + i]);
    }
    feats_[FEATURE_OFFSET_SIDE_TO_MOVE] ^= 1;
    feats_[FEATURE_OFFSET_MOVE_COUNT] += 1;
}

void Accumulator::pop(const Board& b, int x, int y, int player) {
    // 归因空间 = 含子态（side0 = 落子方的对手）：合并段移除、分裂段恢复；
    // delta 施加后整块互换得到无子态（行棋方 = 落子方）的真值。
    const int opp = (player == BLACK) ? WHITE : BLACK;
    for (int d = 0; d < 4; d++) {
        runDelta(b, x, y, player, d, opp, true, -1);             // 含子态：合并段移除
        runDelta(b, x, y, player, d, opp, false, +1);            // 无子态：分裂段恢复
        blockDelta(b, x, y, player, d, opp, false);              // 对手邻接眠段随落子移除重新开放
    }
    threatDelta(b, x, y, player, opp, true, false, +1);          // 威胁差分（内部已覆盖 4 向，勿按方向重复调）
    for (int i = 0; i < PATTERN_NB * ZONE_NB; i++) {
        std::swap(feats_[i], feats_[PATTERN_NB * ZONE_NB + i]);
    }
    feats_[FEATURE_OFFSET_SIDE_TO_MOVE] ^= 1;
    feats_[FEATURE_OFFSET_MOVE_COUNT] -= 1;
}

#ifdef GOMOKU_NNUE_DEBUG_COMPARE
// 调试用：枚举全量提取在 (player, type, zone) 桶里的具体点位
static void nnueDebugThreatPoints(const Board& b, int player, int wantType, int wantZone) {
    for (int y = 0; y < BOARD_SIZE; y++) {
        for (int x = 0; x < BOARD_SIZE; x++) {
            if (b.get(x, y) != EMPTY) continue;
            int best = 0;
            for (int dd = 0; dd < 4; dd++) {
                const int t = fourThreatOn(b, x, y, player, dd);
                if (t > best) best = t;
            }
            const int type = best == 2 ? O4THREAT : (best == 1 ? C4THREAT : -1);
            if (type == wantType && zoneOf(x, y) == wantZone) {
                std::fprintf(stderr, "[threat-point] player=%d type=%d zone=%d at (%d,%d)\n",
                             player, type, zoneOf(x, y), x, y);
            }
        }
    }
}
#endif

// 差分测试：确定性随机走子（邻域约束），每个 push / pop 后与全量提取逐位比对。
int nnueVerifyAccumulator(const Board& board, int moves, int seed, int* mismatchDetail) {
    Board b = board;             // Board 可拷贝（定长数组）
    Accumulator acc;
    acc.reset(b);
    int mismatches = 0;
    if (mismatchDetail) *mismatchDetail = -1;

    auto compare = [&](const char* phase) -> bool {
        int full[FEATURE_DIM];
        extractFeatures(b, full);
        bool ok = true;
        for (int i = 0; i < FEATURE_DIM; i++) {
            if (acc.features()[i] != full[i]) {
                if (mismatchDetail && *mismatchDetail < 0) *mismatchDetail = i;
                if (mismatches < 8) {
                    std::fprintf(stderr, "[nnue-verify] mismatch after %s: idx %d acc=%d full=%d\n",
                                 phase, i, acc.features()[i], full[i]);
                    if (mismatches == 0) {
                        std::fprintf(stderr, "[nnue-verify] history(%d):", b.moveCount());
                        for (int h = 0; h < b.moveCount(); h++) {
                            const Move& m = b.getHistory(h);
                            std::fprintf(stderr, " %d,%d", m.pos.x, m.pos.y);
                        }
                        std::fprintf(stderr, "\n");
#ifdef GOMOKU_NNUE_DEBUG_COMPARE
                        // 威胁桶分歧：枚举全量提取的具体点位（按 idx 反推 side/type/zone）
                        if (i >= 2 * PATTERN_NB * ZONE_NB - 2 + O4THREAT * ZONE_NB ||
                            (i % (PATTERN_NB * ZONE_NB)) / ZONE_NB >= O4THREAT) {
                            const int side = i / (PATTERN_NB * ZONE_NB);
                            const int within = i % (PATTERN_NB * ZONE_NB);
                            const int type = within / ZONE_NB;
                            const int zone = within % ZONE_NB;
                            if (type == O4THREAT || type == C4THREAT) {
                                const int player = side == 0 ? b.getPlayer()
                                                             : (b.getPlayer() == BLACK ? WHITE : BLACK);
                                std::fprintf(stderr, "[nnue-verify] threat bucket side=%d type=%d zone=%d player=%d:\n",
                                             side, type, zone, player);
                                nnueDebugThreatPoints(b, player, type, zone);
                            }
                        }
#endif
                    }
                }
                mismatches++;
                ok = false;
            }
        }
        return ok;
    };

    compare("reset");

    uint64_t st = 0x9E3779B97F4A7C15ULL ^ (uint64_t)(seed * 2654435761u);
    auto nextRand = [&]() { st = st * 6364136223846793005ULL + 1442695040888963407ULL; return (unsigned)(st >> 33); };

    const int histN0 = b.moveCount();
    struct Hist { int x, y, player; };
    Hist hist[600];
    int histN = histN0;
    for (int i = 0; i < histN; i++) {
        const Move& m = b.getHistory(i);
        hist[i] = {m.pos.x, m.pos.y, m.player};
    }

    for (int mv = 0; mv < moves && histN < 560; mv++) {
        int cands[256][2];
        int nc = 0;
        for (int y = 0; y < BOARD_SIZE && nc < 256; y++) {
            for (int x = 0; x < BOARD_SIZE && nc < 256; x++) {
                if (b.get(x, y) != EMPTY) continue;
                bool near = false;
                for (int dy = -2; dy <= 2 && !near; dy++) {
                    for (int dx = -2; dx <= 2; dx++) {
                        if (dx == 0 && dy == 0) continue;
                        if (b.inBounds(x + dx, y + dy) && b.get(x + dx, y + dy) != EMPTY) { near = true; break; }
                    }
                }
                if (!near) continue;
                cands[nc][0] = x; cands[nc][1] = y; nc++;
            }
        }
        if (nc == 0) break;
        const unsigned pick = nextRand() % (unsigned)nc;
        const int px = cands[pick][0], py = cands[pick][1];
        const int player = b.getPlayer();
        if (!b.makeMove(px, py)) break;
        hist[histN++] = {px, py, player};
        acc.push(b, px, py, player);
        if (!compare("push")) break;

        // 隔手做 pop 回测（覆盖 undo 路径的对称性）
        if (mv % 2 == 1) {
            b.undo();
            acc.pop(b, px, py, player);
            histN--;
            if (!compare("pop")) break;
        }
    }
    (void)histN0; (void)hist;
    return mismatches;
}

}  // namespace Nnue
