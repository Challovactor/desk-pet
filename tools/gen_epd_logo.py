#!/usr/bin/env python3
"""把 logo.png 转成墨水屏固件内嵌位图（stm32f407/main/epd_logo.c）。

用法：
    python3 tools/gen_epd_logo.py path/to/logo.png   # 任意尺寸 PNG
    python3 tools/gen_epd_logo.py                    # 无参数时生成内置 MOCE 占位图

输出：400×300 黑白位图，行优先、MSB 在前、1=白 0=黑（UC8176 0x24/0x26 RAM 格式）。
带 PNG 输入时需要 Pillow：pip3 install Pillow
"""
import sys

W, H = 400, 300
ROW = W // 8
OUT_DEFAULT = "stm32f407/main/epd_logo.c"

# 5×7 点阵（M O C E），纯 Python 占位图用
FONT = {
    "M": ["X...X", "XX.XX", "X.X.X", "X...X", "X...X", "X...X", "X...X"],
    "O": [".XXX.", "X...X", "X...X", "X...X", "X...X", "X...X", ".XXX."],
    "C": [".XXXX", "X....", "X....", "X....", "X....", "X....", ".XXXX"],
    "E": ["XXXXX", "X....", "X....", "XXXX.", "X....", "X....", "XXXXX"],
}


def write_c(bits, path, source):
    with open(path, "w") as f:
        f.write("/* 自动生成：tools/gen_epd_logo.py（来源：%s），勿手改 */\n" % source)
        f.write("#include <stdint.h>\n\n")
        f.write("const unsigned char epd_logo_bits[%d] = {\n" % len(bits))
        for i in range(0, len(bits), 16):
            f.write("    " + ",".join("0x%02X" % b for b in bits[i:i + 16]) + ",\n")
        f.write("};\n")
    print("written %s (%d bytes, %dx%d)" % (path, len(bits), W, H))


def bits_from_pixels(px):
    """px[(x,y)] = True 表示黑。打包为 UC8176 RAM 格式。"""
    bits = bytearray(ROW * H)
    for y in range(H):
        for x in range(W):
            if not px.get((x, y), False):
                bits[y * ROW + x // 8] |= 0x80 >> (x % 8)
    return bits


def from_png(path):
    from PIL import Image, ImageOps

    img = Image.open(path).convert("L")
    # 等比缩放进 400×300（不裁切），白底居中
    scale = min(W / img.width, H / img.height)
    nw, nh = max(1, round(img.width * scale)), max(1, round(img.height * scale))
    img = img.resize((nw, nh), Image.LANCZOS)
    canvas = Image.new("L", (W, H), 255)
    canvas.paste(img, ((W - nw) // 2, (H - nh) // 2))
    mono = canvas.point(lambda v: 0 if v < 128 else 255).convert("1")
    px = {}
    pix = mono.load()
    for y in range(H):
        for x in range(W):
            px[(x, y)] = pix[x, y] == 0
    return px


def placeholder():
    """白底 + 边框 + 居中 MOCE 字样（无 Pillow 时的内置占位图）。"""
    px = {}

    def black(x, y):
        if 0 <= x < W and 0 <= y < H:
            px[(x, y)] = True

    for t in range(4):  # 边框
        for x in range(W):
            black(x, t); black(x, H - 1 - t)
        for y in range(H):
            black(t, y); black(W - 1 - t, y)

    text, scale, gap = "MOCE", 12, 24
    gw, gh = 5 * scale, 7 * scale
    total = len(text) * gw + (len(text) - 1) * gap
    x0, y0 = (W - total) // 2, (H - gh) // 2
    for i, ch in enumerate(text):
        rows = FONT[ch]
        for r, line in enumerate(rows):
            for c, cell in enumerate(line):
                if cell == "X":
                    for dy in range(scale):
                        for dx in range(scale):
                            black(x0 + i * (gw + gap) + c * scale + dx,
                                  y0 + r * scale + dy)
    return px


def main():
    argv = sys.argv[1:]
    out = OUT_DEFAULT
    if "-o" in argv:
        i = argv.index("-o")
        out = argv[i + 1]
        del argv[i:i + 2]
    args = [a for a in argv if not a.startswith("-")]

    if args:
        write_c(bits_from_pixels(from_png(args[0])), out, args[0])
    else:
        write_c(bits_from_pixels(placeholder()), out, "内置占位图")
        print("提示：用自己的 logo 时运行 python3 tools/gen_epd_logo.py <logo.png>")


if __name__ == "__main__":
    main()
