// nnue.h - NNUE spike 共享特征规格（v0）
//
// 设计约束（AGENTS.md「NNUE 预研」）：
//  - 特征规格只此一处定义：dump（native CLI）与将来的量化推理共用，杜绝漂移；
//  - 特征全部是整数计数/开关，供 numpy 训练与 int8 量化推理两端对齐；
//  - v0 只数「连续段」（不跨空点），跳三/跳四类暂不建模——已知局限，记录在案。

#ifndef GOMOKU_NNUE_H
#define GOMOKU_NNUE_H

#include "engine.h"

namespace Nnue {

// 棋型（连续段）类型。序即特征块内的索引序。
// v1 起加入「下一手威胁」类（对该方每个空点试落子后的强威胁计数）——跳三/跳四
// 盲区的直接代理；教师的深搜标签能编码这类价值，但连续段计数表达不了。
enum PatternType {
    FIVE = 0,    // 成五（终局）
    OPEN4,       // 活四（两端皆空）
    CLOSED4,     // 冲四（一端空）
    OPEN3,       // 活三
    SLEEP3,      // 眠三
    OPEN2,       // 活二
    SLEEP2,      // 眠二
    OPEN1,       // 活一
    O4THREAT,    // 下一手可成活四的空点数（双端威胁）
    C4THREAT,    // 下一手可成冲四的空点数（单端强制）
    PATTERN_NB
};

// 三区（按离中心 7,7 的 Chebyshev 距离）：
//   Z0 中心（<=2）、Z1 中腹（<=4）、Z2 外围（其余）
enum Zone {
    Z_CENTER = 0,
    Z_MID,
    Z_OUTER,
    ZONE_NB
};

constexpr int FEATURE_DIM = 2 * PATTERN_NB * ZONE_NB   // 双方棋型计数 = 48
                          + 2;                          // 先手 flag + 手数
constexpr int FEATURE_OFFSET_SIDE = 2 * PATTERN_NB * ZONE_NB;
constexpr int FEATURE_OFFSET_SIDE_TO_MOVE = FEATURE_OFFSET_SIDE;
constexpr int FEATURE_OFFSET_MOVE_COUNT = FEATURE_OFFSET_SIDE + 1;

// 从棋盘提取 v0 特征（整数向量，长度 FEATURE_DIM）。
// 布局：[当前方 8 型 x 3 区 = 24][对手 8 型 x 3 区 = 24][先手白=1/黑=0][已落子数]
void extractFeatures(const Board& board, int* out);

// 便于人读的特征名（dump 头/调试用）
const char* patternName(int type);
const char* zoneName(int zone);

// ---- 量化推理（train.py 导出的标准头二进制） ----
// 导出格式（小端）：header{magic='NNUE', version, in/h1/h2/out dims} +
//   float32 mean[in] + float32 std[in] +
//   每层：float32 scale + int8 W[out][in] + float32 bias[out]
// 推理：加载即反量化为 float（网络极小 ~2.1k MAC，不在意这点开销；真正的 int8
// 热路径是后续优化项）。默认关闭，flag 控制。
bool nnueLoad(const char* path);
// 从内嵌权重（src/nnue_weights.h）加载——wasm/浏览器路径（无文件系统）的入口；
// 原生构建也可用（权重与 CLI --nnue 的文件同源同期）。
bool nnueLoadEmbedded();
bool nnueLoaded();
void nnueSetEnabled(bool on);
bool nnueEnabled();
// 网络原始输出 × EVAL_SCALE 后的评估值（走子方视角，与 evaluatePosition 同口径）
int64_t nnueEvaluate(const Board& board);
// 直接从已算好的特征向量前向（增量累加器路径；跳过特征提取）
int64_t nnueEvaluateRaw(const int* feats);

constexpr int64_t NNUE_EVAL_SCALE = 1000000;   // 网络输出(~±1) -> 引擎评估分量程

// ---- 增量特征累加器（NNUE 提速的关键路径） ----
// 差分测试先行：nnueVerifyAccumulator 用随机走子序列逐 push/pop 与全量提取
// 比对，零不一致才允许接入 negamax 热路径。
class Accumulator {
public:
    void reset(const Board& board);             // 全量构建一次
    void push(const Board& b, int x, int y, int player);   // 落子后调用（b 为落子后棋盘，扫描绕点进行）
    void pop(const Board& b, int x, int y, int player);    // 悔棋后调用（b 为悔棋后棋盘，与 push 严格互逆）
    const int* features() const { return feats_; }

private:
    int feats_[FEATURE_DIM] = {0};

    // 归因空间 = attrSide0 所指颜色的「行棋方」；段/威胁 delta 在该空间内施加，
    // 之后由调用方统一做 us/opp 整块互换（见 push/pop 注释）。
    void runDelta(const Board& b, int x, int y, int player, int d, int attrSide0, bool merged, int sign);
    // 对手邻接段的降级/恢复：落子方的子会把对手紧邻 (x,y) 的「活段」封成「眠段」
    // （段本身不变，但 (x,y) 端的开放状态翻转 → 类型可能变，甚至不再计入）。
    // demote=true：开放端 → 封闭（push）；false：封闭 → 开放（pop，与 push 互逆）。
    void blockDelta(const Board& b, int x, int y, int blocker, int d, int attrSide0, bool demote);
    void threatDelta(const Board& b, int x, int y, int mover, int attrSide0, bool fromVirtual, bool toVirtual, int sign);
};

// 差分测试：对 board 现状施加给定种子驱动的 make/undo 序列，比对累加器与全量提取。
// 返回不一致次数（0 = 一致）；mismatchDetail 非空时给出首个不一致的特征索引。
int nnueVerifyAccumulator(const Board& board, int moves, int seed, int* mismatchDetail = nullptr);

}  // namespace Nnue

#endif  // GOMOKU_NNUE_H
