#!/usr/bin/env python3
# native/nnue/train.py - NNUE spike v0 训练器（纯 numpy，无 PyTorch 依赖）
#
# 输入：native CLI dump 的输出（score<TAB>50 个整数特征）
# 标签：tanh(score / 1e6) —— 古典搜索分的蒸馏目标（Stockfish 式缩放）
# 网络：FC 50 -> H1(ReLU) -> H2(ReLU) -> 1
# 导出：标准头二进制（引擎端按头解析；int8 对称逐张量量化 + float32 bias）
#
# 用法： python train.py --data dump_v1.txt --out nnue_v1.bin [--iters 3000]
import argparse
import struct
import sys

import numpy as np

MAGIC = 0x454E4E55  # 'UNNE' little-endian -> bytes 'UNNE'? see writer (uses b'NNUE')
VERSION = 1


def load_dump(path):
    labels, feats = [], []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            if not line or line.startswith("#"):
                continue
            label_s, feat_s = line.rstrip("\n").split("\t")
            labels.append(float(label_s))
            feats.append([float(v) for v in feat_s.split()])
    y = np.array(labels, dtype=np.float64)
    x = np.array(feats, dtype=np.float64)
    if x.shape[1] < 10:
        raise SystemExit(f"unexpected feature dim {x.shape[1]}")
    return x, y


def target_map(y):
    # 搜索分 -> [-1, 1]：评估分用 tanh 压缩，mate 分（|s|>=9.9M）贴到 ±1
    t = np.tanh(y / 1.0e6)
    t = np.where(y >= 9.9e6, 1.0, t)
    t = np.where(y <= -9.9e6, -1.0, t)
    return t


class Adam:
    def __init__(self, params, lr=3e-3):
        self.params = params          # list of numpy arrays (gradient targets)
        self.m = [np.zeros_like(p) for p in params]
        self.v = [np.zeros_like(p) for p in params]
        self.lr = lr
        self.t = 0

    def step(self, grads):
        self.t += 1
        b1, b2, eps = 0.9, 0.999, 1e-8
        for i, (p, g) in enumerate(zip(self.params, grads)):
            self.m[i] = b1 * self.m[i] + (1 - b1) * g
            self.v[i] = b2 * self.v[i] + (1 - b2) * g * g
            mh = self.m[i] / (1 - b1 ** self.t)
            vh = self.v[i] / (1 - b2 ** self.t)
            p -= self.lr * mh / (np.sqrt(vh) + eps)


def forward(params, x, train=True):
    # params: [W1,b1,W2,b2,W3,b3]; 返回 (cache, output)
    # 输出层：1.2*tanh(z/1.2) 有界（与引擎推理端同式；防越界预测）
    W1, b1, W2, b2, W3, b3 = params
    z1 = x @ W1 + b1
    a1 = np.maximum(z1, 0.0)
    z2 = a1 @ W2 + b2
    a2 = np.maximum(z2, 0.0)
    z3 = a2 @ W3 + b3
    out = 1.2 * np.tanh(z3 / 1.2)
    cache = (x, z1, a1, z2, a2, z3)
    return cache, out


def loss_and_grads(params, x, t):
    cache, out = forward(params, x)
    diff = (out - t.reshape(-1, 1))
    loss = float(np.mean(diff * diff))
    n = x.shape[0]

    x_in, z1, a1, z2, a2, z3 = cache
    dout = 2.0 * diff / n
    # 输出层 tanh 的导数：do/dz3 = 1 - (o/1.2)^2
    dz3 = dout * (1.0 - (out / 1.2) ** 2)
    gW3 = a2.T @ dz3
    gb3 = dz3.sum(axis=0)
    da2 = dz3 @ W3.T if (W3 := params[4]) is not None else None
    dz2 = da2 * (z2 > 0)
    gW2 = a1.T @ dz2
    gb2 = dz2.sum(axis=0)
    da1 = dz2 @ W2.T if (W2 := params[2]) is not None else None
    dz1 = da1 * (z1 > 0)
    gW1 = x_in.T @ dz1
    gb1 = dz1.sum(axis=0)
    return loss, [gW1, gb1, gW2, gb2, gW3, gb3]


def quantize_tensor(w):
    # 逐张量对称 int8：scale = max|w| / 127
    amax = float(np.max(np.abs(w)))
    if amax < 1e-8:
        amax = 1e-8
    scale = amax / 127.0
    q = np.clip(np.round(w / scale), -127, 127).astype(np.int8)
    return q, scale


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--out", default="nnue_v1.bin")
    ap.add_argument("--iters", type=int, default=3000)
    ap.add_argument("--batch", type=int, default=128)
    ap.add_argument("--lr", type=float, default=3e-3)
    ap.add_argument("--h1", type=int, default=32)
    ap.add_argument("--h2", type=int, default=16)
    ap.add_argument("--seed", type=int, default=20261121)
    args = ap.parse_args()

    rng = np.random.default_rng(args.seed)
    x_raw, y = load_dump(args.data)
    t = target_map(y).reshape(-1, 1)

    n = x_raw.shape[0]
    idx = rng.permutation(n)
    n_val = max(1, n // 10)
    val_idx, tr_idx = idx[:n_val], idx[n_val:]
    x_tr_raw, y_tr = x_raw[tr_idx], t[tr_idx]
    x_va_raw, y_va = x_raw[val_idx], t[val_idx]

    # 标准化（仅用训练集统计量）
    mean = x_tr_raw.mean(axis=0)
    std = x_tr_raw.std(axis=0)
    std[std < 1e-6] = 1.0
    x_tr = (x_tr_raw - mean) / std
    x_va = (x_va_raw - mean) / std

    d_in, d_h1, d_h2 = x_tr.shape[1], args.h1, args.h2
    W1 = rng.normal(0, np.sqrt(2.0 / d_in), size=(d_in, d_h1))
    b1 = np.zeros(d_h1)
    W2 = rng.normal(0, np.sqrt(2.0 / d_h1), size=(d_h1, d_h2))
    b2 = np.zeros(d_h2)
    W3 = rng.normal(0, np.sqrt(2.0 / d_h2), size=(d_h2, 1)) * 0.1
    b3 = np.zeros(1)
    params = [W1, b1, W2, b2, W3, b3]
    opt = Adam(params, lr=args.lr)

    n_tr = x_tr.shape[0]
    for it in range(1, args.iters + 1):
        bi = rng.integers(0, n_tr, size=min(args.batch, n_tr))
        _, grads = loss_and_grads(params, x_tr[bi], y_tr[bi])
        opt.step(grads)
        if it % 500 == 0 or it == 1:
            _, out_va = forward(params, x_va)
            va = float(np.mean((out_va - y_va) ** 2))
            _, out_tr = forward(params, x_tr)
            tr = float(np.mean((out_tr - y_tr) ** 2))
            print(f"iter {it:5d}  train_mse={tr:.5f}  val_mse={va:.5f}", flush=True)

    # ---- 导出（int8 对称量化 + float32 bias；标准化统计随头导出） ----
    header = struct.pack(
        "<IIIIIIIII",
        0x4E4E5545,          # magic（小端字节序 E U N N；引擎端同常量校验）
        VERSION,
        d_in, d_h1, d_h2, 1,
        0, 0, 0,
    )
    with open(args.out, "wb") as f:
        f.write(header)
        f.write(struct.pack(f"<{d_in}f", *mean))
        f.write(struct.pack(f"<{d_in}f", *std))
        for W, b in [(params[0], params[1]), (params[2], params[3]), (params[4], params[5])]:
            # W 在 numpy 里是 (in, out)；导出与引擎读取按 row-major [out][in] -> 转置
            Wt = np.ascontiguousarray(W.T)
            q, scale = quantize_tensor(Wt)
            f.write(struct.pack("<f", scale))
            f.write(q.tobytes())                    # row-major [out][in]
            f.write(struct.pack(f"<{b.shape[0]}f", *[float(v) for v in b]))
    print(f"exported {args.out}")
    # 量化误差粗检（用训练全集）
    for name, xs in [("train", x_tr), ("val", x_va)]:
        _, out = forward(params, xs)
        print(f"{name}: float_pred range [{out.min():.4f}, {out.max():.4f}]")


if __name__ == "__main__":
    sys.exit(main())