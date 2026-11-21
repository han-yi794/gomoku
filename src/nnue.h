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
bool nnueLoaded();
void nnueSetEnabled(bool on);
bool nnueEnabled();
// 网络原始输出 × EVAL_SCALE 后的评估值（走子方视角，与 evaluatePosition 同口径）
int64_t nnueEvaluate(const Board& board);

constexpr int64_t NNUE_EVAL_SCALE = 1000000;   // 网络输出(~±1) -> 引擎评估分量程

}  // namespace Nnue

#endif  // GOMOKU_NNUE_H