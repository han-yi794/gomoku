#!/usr/bin/env python3
# native/nnue/export_weights_header.py - 把 int8 权重包转成 C 头（数组内嵌，供 wasm 构建）
# 生成的 src/nnue_weights.h 仅在 GOMOKU_WITH_NNUE 构建里被 nnueLoadEmbedded() 使用。
import argparse
import pathlib


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="train.py 导出的权重包")
    ap.add_argument("--out", default="src/nnue_weights.h")
    args = ap.parse_args()

    data = pathlib.Path(args.bin).read_bytes()
    if len(data) < 36:
        raise SystemExit("weights file too small")

    lines = [
        "// nnue_weights.h - 内嵌 NNUE 权重（由 export_weights_header.py 生成，勿手改）",
        "// 生成物与 train.py 的导出格式完全一致（标准头 + int8 层），nnueLoadEmbedded() 解析。",
        "#ifndef GOMOKU_NNUE_WEIGHTS_H",
        "#define GOMOKU_NNUE_WEIGHTS_H",
        "",
        "#include <cstddef>",
        "",
        f"// source: {pathlib.Path(args.bin).name} ({len(data)} bytes)",
        "static const unsigned char NNUE_WEIGHTS[] = {",
    ]
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append("    " + ",".join(str(b) for b in chunk) + ",")
    lines += [
        "};",
        "",
        "static const size_t NNUE_WEIGHTS_LEN = sizeof(NNUE_WEIGHTS);",
        "",
        "#endif  // GOMOKU_NNUE_WEIGHTS_H",
        "",
    ]
    out = pathlib.Path(args.out)
    out.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {out} ({len(data)} bytes embedded)")


if __name__ == "__main__":
    main()