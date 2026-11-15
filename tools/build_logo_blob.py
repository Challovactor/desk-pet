#!/usr/bin/env python3
"""把 LOGO.png 转成墨水屏开机图：SD 资产块 + STM32 内置兜底位图。

用法：
    python tools/build_logo_blob.py [LOGO.png]

输出：
    ecw6700/main/assets/logo.bin     SD 资产块（ECW 首次启动写入 LBA 1536）
    stm32f407/main/epd_logo.c        STM32 内置兜底位图（同一份数据）

图像处理要点：
  - PNG 通常是 RGBA/带透明背景。必须先按 alpha 合成到白底再转灰度；
    直接 convert("L") 会把透明背景变成黑色，整屏发黑（v1 的 bug）。
  - 原图等比缩放进 800×480 居中，不裁切、不拉伸。
  - 1 位黑白屏上用 8×8 Bayer 有序抖动近似灰阶，得到"黑白灰"观感。

块格式（小端）：
  0  char[8]  magic "MOCELGO1"
  8  u16      version = 1
  10 u16      header_size = 32
  12 u16      width
  14 u16      height
  16 u8       bpp = 1
  17 u8       flags（bit0=1 表示 1=白）
  18 u16      reserved
  20 u32      data_bytes
  24 u32      data_crc32
  28 u32      reserved
  32 位图数据：行优先，MSB 在前，1=白
"""
from __future__ import annotations

import struct
import zlib
from pathlib import Path

from PIL import Image

W, H = 800, 480
MAGIC = b"MOCELGO1"
VERSION = 1
HEADER_SIZE = 32

ROOT = Path(__file__).resolve().parents[1]
OUT_BLOB = ROOT / "ecw6700" / "main" / "assets" / "logo.bin"
OUT_C = ROOT / "stm32f407" / "main" / "epd_logo.c"

# 8x8 Bayer 阈值矩阵（0..63），用于 1 位有序抖动
BAYER8 = [
    0, 32, 8, 40, 2, 34, 10, 42,
    48, 16, 56, 24, 50, 18, 58, 26,
    12, 44, 4, 36, 14, 46, 6, 38,
    60, 28, 52, 20, 62, 30, 54, 22,
    3, 35, 11, 43, 1, 33, 9, 41,
    51, 19, 59, 27, 49, 17, 57, 25,
    15, 47, 7, 39, 13, 45, 5, 37,
    63, 31, 55, 23, 61, 29, 53, 21,
]


def load_gray(path: Path) -> Image.Image:
    img = Image.open(path)
    if img.mode in ("RGBA", "LA", "P"):
        img = img.convert("RGBA")
        bg = Image.new("RGBA", img.size, (255, 255, 255, 255))
        img = Image.alpha_composite(bg, img)
    return img.convert("L")


def fit_centered(img: Image.Image) -> Image.Image:
    scale = min(W / img.width, H / img.height)
    nw = max(1, round(img.width * scale))
    nh = max(1, round(img.height * scale))
    img = img.resize((nw, nh), Image.LANCZOS)
    canvas = Image.new("L", (W, H), 255)
    canvas.paste(img, ((W - nw) // 2, (H - nh) // 2))
    return canvas


def dither_to_bits(img: Image.Image) -> bytearray:
    """Bayer 8x8 有序抖动，返回 1bpp 位图（MSB 在前，1=白）。"""
    px = img.load()
    row_bytes = (W + 7) // 8
    out = bytearray(row_bytes * H)
    for y in range(H):
        for x in range(W):
            v = px[x, y]
            t = (BAYER8[(y % 8) * 8 + (x % 8)] + 0.5) * (256.0 / 64.0)
            white = v > t
            if white:
                out[y * row_bytes + x // 8] |= 0x80 >> (x % 8)
    return out


def build_blob(bits: bytes) -> bytes:
    crc = zlib.crc32(bits) & 0xFFFFFFFF
    head = struct.pack(
        "<8sHHHHBBHIII",
        MAGIC,
        VERSION,
        HEADER_SIZE,
        W,
        H,
        1,      # bpp
        1,      # flags: 1=white
        0,
        len(bits),
        crc,
        0,
    )
    assert len(head) == HEADER_SIZE, len(head)
    return head + bits


def write_c(bits: bytes, path: Path) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("/* 自动生成：tools/build_logo_blob.py，勿手改 */\n")
        f.write("#include <stdint.h>\n\n")
        f.write("const unsigned char epd_logo_bits[%d] = {\n" % len(bits))
        for i in range(0, len(bits), 16):
            f.write("    " + ",".join("0x%02X" % b for b in bits[i:i + 16]) + ",\n")
        f.write("};\n")
        f.write("\nconst unsigned int epd_logo_bytes = %d;\n" % len(bits))


def main() -> None:
    import sys

    src = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "LOGO.png"
    gray = fit_centered(load_gray(src))
    bits = dither_to_bits(gray)
    blob = build_blob(bits)

    OUT_BLOB.parent.mkdir(parents=True, exist_ok=True)
    OUT_BLOB.write_bytes(blob)
    write_c(bits, OUT_C)

    crc = zlib.crc32(bits) & 0xFFFFFFFF
    print("source : %s" % src)
    print("logo   : %dx%d 1bpp, %d bytes, crc32=0x%08X" % (W, H, len(bits), crc))
    print("blob   : %s (%d bytes)" % (OUT_BLOB, len(blob)))
    print("c array: %s" % OUT_C)


if __name__ == "__main__":
    main()
