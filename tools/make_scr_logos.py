#!/usr/bin/env python3
"""给串口屏版生成航司标志: include/fb_scr_logos.h(从 include/fb_logos.h 转出来, 和它一样不进仓库)。

串口屏只能收文字命令(38400 波特率, 每秒约 3.8 KB), 没法传图片。所以每个标志:
  1. 缩成 4 种颜色(第 0 种是底色, 一般是白)
  2. 拆成一组实心矩形, 按层画: 先铺底色圆角块, 再一层层盖矩形; 后面会被盖住的地方前面可以随便涂,
     这样矩形数从五十多个降到四十左右
  3. 固件画的时候, 每行占 4 个调色板号(32 + 4*行号 起), 先用 SCC 把这 4 个号设成这个标志的颜色,
     再发 CBOF / BOXF; 8 行正好用满 32..63, 同一屏上的标志互不抢颜色

    python3 tools/make_scr_logos.py           # 生成 include/fb_scr_logos.h
    python3 tools/make_scr_logos.py check.png # 另外把前 60 个标志的效果和原图并排画出来
"""
import itertools
import os
import re
import sys

from PIL import Image, ImageDraw

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
W, H, K = 22, 17, 4


def load():
    src = open(os.path.join(ROOT, "include", "fb_logos.h")).read()
    codes = re.search(r'FB_LOGO_CODE\[\] PROGMEM = "([^"]*)"', src).group(1)
    body = src[src.index("FB_LOGO_PX[] PROGMEM = {") + 24:]
    px = [int(v) for v in re.sub(r"//[^\n]*", "", body[:body.index("};")]).replace("\n", "").split(",") if v.strip()]
    return codes, px


def rgb(v):
    return ((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)


def to565(c):
    r, g, b = c
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def layered(grid, order):
    """按 order 一层层画; 画第 i 层时, 后面层的颜色所在的像素可以顺手涂掉(反正会被盖)"""
    rank = {c: i for i, c in enumerate(order)}
    out = []
    for i, col in enumerate(order):
        need = [[grid[y][x] == col for x in range(W)] for y in range(H)]
        ok = [[grid[y][x] == col or rank.get(grid[y][x], -1) > i for x in range(W)] for y in range(H)]
        done = [[False] * W for _ in range(H)]
        for y in range(H):
            for x in range(W):
                if not need[y][x] or done[y][x]:
                    continue
                maxw = 0
                while x + maxw < W and ok[y][x + maxw]:
                    maxw += 1
                best = None
                for w in range(1, maxw + 1):
                    h = 1
                    while y + h < H and all(ok[y + h][x + k] for k in range(w)):
                        h += 1
                    gain = sum(need[yy][xx] and not done[yy][xx] for yy in range(y, y + h) for xx in range(x, x + w))
                    if best is None or gain > best[0]:
                        best = (gain, w, h)
                _, w, h = best
                for yy in range(y, y + h):
                    for xx in range(x, x + w):
                        done[yy][xx] = True
                out.append((x, y, w, h, col))
    return out


def convert(pixels):
    """一个标志 -> (4 个 RGB565 颜色, 底色在前; [(x, y, w, h, 颜色序号)])"""
    im = Image.new("RGB", (W, H))
    im.putdata([rgb(v) for v in pixels])
    q = im.quantize(colors=K, method=Image.Quantize.MEDIANCUT)
    pal = q.getpalette()[:3 * K]
    d = list(q.getdata())
    grid = [d[r * W:(r + 1) * W] for r in range(H)]
    used = sorted(set(d), key=d.count, reverse=True)
    bg, others = used[0], used[1:]
    best = min((layered(grid, list(p)) for p in itertools.permutations(others)), key=len)
    order = [bg] + others
    colors = [to565(tuple(pal[3 * c:3 * c + 3])) for c in order] + [0xFFFF] * (K - len(order))
    idx = {c: i for i, c in enumerate(order)}
    return colors, [(x, y, w, h, idx[c]) for x, y, w, h, c in best]


def draw(colors, rects):
    """按固件的画法复原, 检查用"""
    im = Image.new("RGB", (W, H), (6, 10, 18))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, W - 1, H - 1), radius=3, fill=rgb(colors[0]))
    for x, y, w, h, c in rects:
        d.rectangle((x, y, x + w - 1, y + h - 1), fill=rgb(colors[c]))
    return im


def main():
    codes, px = load()
    n = len(codes) // 2
    logos = [convert(px[i * W * H:(i + 1) * W * H]) for i in range(n)]
    data, offs = bytearray(), []
    for colors, rects in logos:
        offs.append((len(data) // 3, len(rects)))
        for x, y, w, h, c in rects:
            v = x | (y << 5) | ((w - 1) << 10) | ((h - 1) << 15) | (c << 20)  # 22 位: x5 y5 w5 h5 c2
            data += bytes((v & 255, (v >> 8) & 255, v >> 16))
    out = ["// 由 tools/make_scr_logos.py 从 fb_logos.h 生成, 不要手改。串口屏用的航司标志: 4 色 + 矩形。",
           "// 航司标志是各航司的商标, 仅用于在航班牌上辨认航司。", "#pragma once", "#include <Arduino.h>", "",
           "#define FB_SL_N %d" % n,
           'static const char FB_SL_CODE[] PROGMEM = "%s";' % codes,
           "static const uint16_t FB_SL_PAL[] PROGMEM = {%s};" % ",".join(str(c) for cs, _ in logos for c in cs),
           "// 每个标志: 矩形起始序号, 矩形个数",
           "static const uint16_t FB_SL_IDX[] PROGMEM = {%s};" % ",".join("%d,%d" % o for o in offs),
           "// 矩形: 3 字节小端, x 5 位 | y 5 位 | w-1 5 位 | h-1 5 位 | 颜色序号 2 位",
           "static const uint8_t FB_SL_RECT[] PROGMEM = {"]
    for i in range(0, len(data), 30):
        out.append("  " + ",".join(str(b) for b in data[i:i + 30]) + ",")
    out.append("};")
    path = os.path.join(ROOT, "include", "fb_scr_logos.h")
    open(path, "w").write("\n".join(out) + "\n")
    total = sum(len(r) for _, r in logos)
    print("wrote %s: %d logos, %d rects (avg %.1f), %d KB" % (os.path.normpath(path), n, total, total / n, len(data) // 1024))
    if len(sys.argv) > 1:
        cols, s = 10, 6
        sheet = Image.new("RGB", (cols * (W * 2 + 6) * s, 6 * (H + 4) * s), (24, 24, 28))
        for i in range(min(60, n)):
            orig = Image.new("RGB", (W, H))
            orig.putdata([rgb(v) for v in px[i * W * H:(i + 1) * W * H]])
            x0, y0 = (i % cols) * (W * 2 + 6) * s, (i // cols) * (H + 4) * s
            sheet.paste(orig.resize((W * s, H * s), Image.NEAREST), (x0, y0))
            sheet.paste(draw(*logos[i]).resize((W * s, H * s), Image.NEAREST), (x0 + (W + 2) * s, y0))
        sheet.save(sys.argv[1])
        print("check image:", sys.argv[1])


if __name__ == "__main__":
    main()
