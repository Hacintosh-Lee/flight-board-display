#!/usr/bin/env python3
"""生成 TFT 版航班牌用的平滑字库 include/fb_font.h。

串口屏自带点阵字库, 直驱的 TFT 屏没有, 所以把用得到的字用电脑上的字体渲染成 4 位灰度(16 级)点阵:
  - 汉字: 只收 make_fb_gbk.py 里出现过的字(界面文字 + 机场中文名), 12 和 16 两种大小, 按 GBK 码查
  - ASCII: 0x20~0x7E 全收, 12 / 16 / 32 三种大小(32 给顶栏的大时钟), 等宽, 宽度是字号的一半
格子大小和串口屏的字库一致(半角 size/2 宽, 全角 size 宽), 所以版面坐标一个都不用改。

改了界面文字或机场表之后: 先跑 make_fb_gbk.py, 再跑本脚本。
要用到哪些汉字, 从 make_fb_gbk.py 生成的 include/fb_gbk.h 里读(每行末尾的注释就是原文),
所以只要有 fb_gbk.h 就能生成, 不需要 assets 里那些原始数据。

字体用的是 macOS 自带的冬青黑体 / Menlo / DIN Condensed / Thonburi, 所以本脚本只能在 Mac 上跑;
这些字体不能随仓库分发, fb_font.h 也就不进仓库, 编译 TFT 版之前先在 Mac 上跑一次本脚本。
"""
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFont

import re

GBK_H = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "include", "fb_gbk.h")


def used_text():
    """fb_gbk.h 里界面文字(S_xxx)和机场中文名两种行的行尾注释, 拼起来就是固件会显示的全部中文"""
    out = []
    for ln in open(GBK_H, encoding="utf-8"):
        m = re.match(r'(?:static const char S_\w+\[\] = .*;|  \{"\w{3}", ".*"\},)  // (.*)$', ln.rstrip("\n"))
        if m:
            out.append(m.group(1))
    return "".join(out)

HEI = "/System/Library/Fonts/Hiragino Sans GB.ttc"   # index 0 = W3, 2 = W6
MONO = "/System/Library/Fonts/Menlo.ttc"             # index 0 = Regular, 1 = Bold


def render(ch, font, w, h, baseline):
    im = Image.new("L", (w, h), 0)
    ImageDraw.Draw(im).text((w / 2, baseline), ch, font=font, fill=255, anchor="ms")
    px = im.load()
    out = bytearray()
    for y in range(h):
        for x in range(0, w, 2):
            a = px[x, y] >> 4
            b = px[x + 1, y] >> 4 if x + 1 < w else 0
            out.append((a << 4) | b)
    return bytes(out)


def c_array(name, data):
    lines = ["static const uint8_t %s[] PROGMEM = {" % name]
    for i in range(0, len(data), 24):
        lines.append("  " + ",".join(str(b) for b in data[i:i + 24]) + ",")
    lines.append("};")
    return "\n".join(lines)


# ---------------- 英文单词 ----------------
# 地名、状态这些英文单词用窄体无衬线字(DIN Condensed Bold, 机场指示牌常见的那类), 按字母实际宽度排,
# 比等宽的 Menlo 好认, 也放得下更长的地名。没有小写字母的(时间、航班号、三字码)仍用等宽字, 保证对齐。
LATIN = "/System/Library/Fonts/Supplemental/DIN Condensed Bold.ttf"
LAT_GW = {16: 12, 12: 10}


def latin():
    lines = ["", "// 英文单词用的窄体: 95 个字符(0x20..0x7E), 字模宽 FB_LATx_GW, 高 = 字号; FB_LATx_ADV 字宽",
             "#define FB_LAT_GW16 %d" % LAT_GW[16], "#define FB_LAT_GW12 %d" % LAT_GW[12]]
    for size, px, base in ((16, 17, 13), (12, 13, 10)):
        f = ImageFont.truetype(LATIN, px)
        gw = LAT_GW[size]
        cells, adv = [], []
        for c in range(0x20, 0x7F):
            im = Image.new("L", (gw + 6, size), 0)
            ImageDraw.Draw(im).text((0, base), chr(c), font=f, fill=255, anchor="ls")
            a = round(f.getlength(chr(c)))
            right = (im.getbbox() or (0, 0, 0, 0))[2]
            if right > gw:
                im = im.crop((0, 0, right, size)).resize((gw, size), Image.LANCZOS)
            cells.append(pack4(im.crop((0, 0, gw, size))))
            adv.append(min(gw, max(a, 1)) + (1 if size == 12 else 0))  # 小号字母挤, 多给 1 像素
        lines.append(c_array("FB_LAT%d" % size, b"".join(cells)))
        lines.append("static const uint8_t FB_LAT%d_ADV[] PROGMEM = {%s};" % (size, ",".join(map(str, adv))))
    return lines


# ---------------- 泰文 ----------------
# 泰文的元音、声调写在字母上下, 要叠在前一个字母上; 没有排版引擎, 规则自己定(固件 tft.h 和
# 模拟器 preview.py 用同一套):
#   字母: 用字体自己的字宽排开。
#   上下的符号: 不占宽度, 右边缘对齐前一个字母的右边缘(ป ฝ ฟ ฬ 有高出的竖笔, 再往左让 2 像素)。
#   声调跟在上方元音后面时整体抬高一层(另存一套, 序号 91 起)。
# 泰文比汉字高: 每个字模 TH_H = 字号 + 4 行, 画的时候往上多占 3 行, 空行不画。
# 编码 TIS-620: 字节 = 码位 - 0x0E00 + 0xA0; 序号 = 字节 - 0xA1 (0..90)。
THAI = "/System/Library/Fonts/Supplemental/Thonburi.ttc"
TH_MARKS = [0x0E31] + list(range(0x0E34, 0x0E3B)) + list(range(0x0E47, 0x0E4F))
TH_HIGH = list(range(0x0E48, 0x0E4D))
TH_GW = {16: 12, 12: 10}       # 字模宽
TH_LIFT = {16: 3, 12: 2}       # 高位声调抬几行


def pack4(im):
    px, (w, h) = im.load(), im.size
    out = bytearray()
    for y in range(h):
        for x in range(0, w, 2):
            a = px[x, y] >> 4
            b = px[x + 1, y] >> 4 if x + 1 < w else 0
            out.append((a << 4) | b)
    return bytes(out)


def thai():
    lines = ["", "// 泰文: 91 个码位(U+0E01..0E5B) + 5 个高位声调。字模宽 FB_TH_GW, 高 字号+4(上面多 3 行);",
             "// FB_THx_ADV: 字宽(符号为 0); FB_THx_INK: 墨迹右边缘(叠加符号时对齐用)",
             "#define FB_TH_N 96", "#define FB_TH_GW16 %d" % TH_GW[16], "#define FB_TH_GW12 %d" % TH_GW[12]]
    for size, px, base in ((16, 13, 15.0), (12, 10, 12.0)):
        f = ImageFont.truetype(THAI, px, index=0)
        gw, h = TH_GW[size], size + 4
        cells, adv, ink = [], [], []
        for cp in list(range(0x0E01, 0x0E5C)) + TH_HIGH:
            high = len(cells) >= 91
            big = Image.new("L", (gw + 8, h), 0)
            ImageDraw.Draw(big).text((0, base - (TH_LIFT[size] if high else 0)), chr(cp), font=f, fill=255, anchor="ls")
            bb = big.getbbox()
            right = bb[2] if bb else 0
            if right > gw:  # 太宽的字母(ฒ ฌ 之类)横向压进字模
                big = big.crop((0, 0, right, h)).resize((gw, h), Image.LANCZOS)
                right = gw
            cell = big.crop((0, 0, gw, h))
            mark = cp in TH_MARKS
            cells.append(pack4(cell))
            adv.append(0 if mark else min(gw, round(f.getlength(chr(cp)))))
            ink.append(right)
        lines.append(c_array("FB_TH%d" % size, b"".join(cells)))
        lines.append("static const uint8_t FB_TH%d_ADV[] PROGMEM = {%s};" % (size, ",".join(map(str, adv))))
        lines.append("static const uint8_t FB_TH%d_INK[] PROGMEM = {%s};" % (size, ",".join(map(str, ink))))
    return lines


def main():
    text = used_text()
    hanzi = sorted({c for c in text if ord(c) > 0x7F}, key=lambda c: c.encode("gbk"))
    out = ["// 由 tools/make_font.py 生成, 不要手改。4 位灰度点阵, 每字节两个像素(高 4 位在左)。",
           "#pragma once", "#include <Arduino.h>", ""]
    # 汉字
    codes = [int.from_bytes(c.encode("gbk"), "big") for c in hanzi]
    out.append("#define FB_HZ_N %d" % len(hanzi))
    out.append("static const uint16_t FB_HZ_CODE[] PROGMEM = {%s};" % ",".join("0x%04X" % c for c in codes))
    for size, idx, px, base in ((12, 0, 12, 10.4), (16, 2, 15, 13.6)):
        f = ImageFont.truetype(HEI, px, index=idx)
        out.append(c_array("FB_HZ%d" % size, b"".join(render(c, f, size, size, base) for c in hanzi)))
    # ASCII
    for size, idx, px, base in ((12, 0, 10, 9.6), (16, 0, 13, 12.6), (32, 1, 26, 25.4)):
        f = ImageFont.truetype(MONO, px, index=idx)
        out.append(c_array("FB_ASC%d" % size,
                           b"".join(render(chr(c), f, size // 2, size, base) for c in range(0x20, 0x7F))))
    out += latin()
    out += thai()
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "include", "fb_font.h")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(out) + "\n")
    print("wrote %s: %d hanzi, %d KB" % (os.path.normpath(path), len(hanzi), os.path.getsize(path) // 1024))


if __name__ == "__main__":
    main()
