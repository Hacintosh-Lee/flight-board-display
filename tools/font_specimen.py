#!/usr/bin/env python3
"""字库效果图: 不用接屏幕, 按固件的版式和绘图命令画一页香港出发的示例航班, 用指定的 fb_font.h 渲染。

    python3 tools/font_specimen.py <fb_font.h> <输出.png> [标题]

一张图里横排中文 / 英文 / 泰文三页, 放大 3 倍。换字体前后各出一张, 对比用。
渲染走 tools/preview.py 的 TftScreen(和真屏逐像素一致), 航司标志不画(用彩色徽章)。
"""
import json
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import preview  # noqa: E402

ROOT = os.path.join(HERE, "..")

# ---- 固件里的常量(src/flightboard/main.cpp) ----
PALETTE = {0: 0x060A12, 1: 0x0C1322, 2: 0x151E32, 3: 0x0F2D5C, 4: 0xFFC107, 5: 0xFFFFFF, 6: 0x7C8798, 7: 0x26324A,
           9: 0x22C55E, 10: 0xE5343A, 11: 0x38BDF8, 12: 0x0A0F1A, 13: 0xC9D3E0, 14: 0xFF8A1F, 15: 0x1F4C8F,
           20: 0xD7141A, 22: 0x0A66C2, 23: 0x14306E, 26: 0x0B6B66, 29: 0x5B2A86, 30: 0x4A5568}
C_BG, C_ROW_A, C_ROW_B, C_HDR, C_AMBER, C_WHITE, C_DIM, C_LINE = 0, 1, 2, 3, 4, 5, 6, 7
C_GREEN, C_RED, C_INK, C_SOFT, C_ORANGE = 9, 10, 12, 13, 14
HDR_H, COL_Y, ROW_Y0, ROW_H, FOOT_TOP = 32, 35, 48, 21, 217
X_BADGE, X_TIME, X_FLIGHT, X_CITY, X_TERM, PILL_X1, CLOCK_X = 4, 31, 77, 137, 204, 222, 236

# 示例航班: 时间, 航班号, 目的地, 航站楼, 状态, 预计晚多少分钟, 徽章色
ROWS = [("17:05", "CX611", "BKK", "1", "S", 0, 26), ("17:10", "UO702", "KIX", "1", "B", 0, 29),
        ("17:20", "KA208", "TPE", "1", "S", 20, 26), ("17:35", "CX548", "HND", "1", "L", 45, 26),
        ("17:40", "HX301", "PEK", "1", "S", 0, 20), ("17:55", "SQ891", "SIN", "1", "C", 0, 23),
        ("18:05", "CX255", "LHR", "1", "S", 0, 26), ("18:15", "NH860", "NRT", "1", "S", 0, 22)]


def strings():
    """界面文字三种语言: 从生成的 fb_gbk.h 里读 _L 表(和固件同一份)"""
    src = open(os.path.join(ROOT, "include", "fb_gbk.h"), encoding="utf-8").read()
    gbk = {}
    for m in re.finditer(r'static const char (S_\w+)\[\] = ("(?:[^"\\]|\\.)*");', src):
        gbk[m.group(1)] = cbytes(m.group(2))
    out = {}
    for m in re.finditer(r'static const char \*const (S_\w+)_L\[3\] = \{(\w+|"(?:[^"\\]|\\.)*"), ("(?:[^"\\]|\\.)*"), ("(?:[^"\\]|\\.)*")\};', src):
        zh = gbk[m.group(2)] if not m.group(2).startswith('"') else cbytes(m.group(2))
        out[m.group(1)] = [zh, cbytes(m.group(3)), cbytes(m.group(4))]
    names = {}
    for m in re.finditer(r'  \{"(\w{3})", ("(?:[^"\\]|\\.)*")\},  //', src):
        names[m.group(1)] = cbytes(m.group(2))
    return out, names


def cbytes(lit):
    """C 字符串字面量 -> bytes(只处理生成器用到的 \\ooo 八进制转义)"""
    body = lit[1:-1]
    out, i = bytearray(), 0
    while i < len(body):
        if body[i] == "\\":
            if body[i + 1] in "01234567":
                out.append(int(body[i + 1:i + 4], 8))
                i += 4
            else:
                out.append(ord(body[i + 1]))
                i += 2
        else:
            out.append(ord(body[i]))
            i += 1
    return bytes(out)


class Specimen(preview.TftScreen):
    def __init__(self, font_h):
        preview.Screen.__init__(self)
        src = open(font_h).read()
        self.arr = {}
        for m in re.finditer(r"(FB_\w+)\[\] PROGMEM = \{([^}]*)\}", src):
            self.arr[m.group(1)] = [int(v, 0) for v in m.group(2).replace("\n", "").split(",") if v.strip()]
        self.bg = (0, 0, 0)
        self.lang = 0

    def width(self, size, raw):
        if self.lang == 2:
            adv = self.arr["FB_TH%d_ADV" % size]
            lat = self.arr["FB_LAT%d_ADV" % size]
            return sum(adv[b - 0xA1] if 0xA1 <= b < 0xA1 + len(adv) else (lat[b - 0x20] if 0x20 <= b < 0x7F else 0) for b in raw)
        if all(b < 0x80 for b in raw) and any(97 <= b <= 122 for b in raw):
            return sum(self.arr["FB_LAT%d_ADV" % size][b - 0x20] for b in raw)
        w, i = 0, 0
        while i < len(raw):
            if raw[i] >= 0x80:
                w += size
                i += 2
            else:
                w += size // 2
                i += 1
        return w


def page(font_h, lang, S, ZH, AP):
    sc = Specimen(font_h)
    T = lambda k: S[k][lang]  # noqa: E731
    cmds = []
    for idx, rgb in PALETTE.items():
        r, g, b = rgb >> 16, (rgb >> 8) & 255, rgb & 255
        cmds.append("SCC(%d,%d);" % (idx, ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)))
    cmds.append("LANG(%d);CLS(0);" % lang)
    sc.lang = lang

    def txt(size, x, y, raw, c):
        return "DS%d(%d,%d,'%s',%d);" % (size, x, y, raw.decode("latin-1"), c)

    def place(code):
        if lang == 0 and code in ZH:
            return ZH[code]
        a = AP.get(code)
        if lang == 2 and a and len(a) > 8 and a[8]:
            return "".join(ch for ch in a[8] if ch not in ",;'").encode("tis-620", errors="ignore")[:20]
        return re.sub(r" %s$" % code, "", (a[7] or a[1]) if a else code)[:14].encode()

    # 顶栏
    label = T("S_DEP")
    bw = sc.width(16, label) + 20
    cmds.append("BOXF(0,0,%d,%d,%d);" % (CLOCK_X - 5, HDR_H - 1, C_HDR))
    cmds.append("BOXF(0,0,%d,%d,%d);SBC(%d);" % (bw - 1, HDR_H - 1, C_AMBER, C_AMBER))
    cmds.append(txt(16, 10, 8, label, C_INK))
    cmds.append("SBC(%d);" % C_HDR)
    x = bw + 8
    city = place("HKG")
    cmds.append(txt(16, x, 8, city, C_WHITE))
    x += sc.width(16, city) + 6
    cmds.append(txt(16, x, 8, b"HKG", C_AMBER))
    x += 32
    if lang != 1 and x + 60 <= CLOCK_X - 6:
        cmds.append("LANG(1);" + txt(12, x, 12, b"DEPARTURES", C_SOFT) + "LANG(%d);" % lang)
    cmds.append("BOXF(%d,0,319,%d,%d);SBC(%d);" % (CLOCK_X - 4, HDR_H - 1, C_HDR, C_HDR))
    cmds.append("DSD(16,%d,0,'16:58',2,2,%d);" % (CLOCK_X, C_WHITE))
    cmds.append("BOXF(0,32,319,33,%d);BOXF(0,32,130,33,%d);" % (C_LINE, C_AMBER))
    # 列标题
    cmds.append("BOXF(0,%d,319,%d,%d);SBC(%d);" % (HDR_H + 2, ROW_Y0 - 1, C_BG, C_BG))
    for key, xx in (("S_C_TIME", X_TIME), ("S_C_FLIGHT", X_FLIGHT), ("S_C_DEST", X_CITY)):
        cmds.append(txt(12, xx, COL_Y, T(key), C_DIM))
    st_w = sc.width(12, T("S_C_STATUS"))
    cmds.append(txt(12, (PILL_X1 + 320 - st_w) // 2, COL_Y, T("S_C_STATUS"), C_DIM))
    # 航班行
    for row, (tm, fn, code, term, st, late, badge) in enumerate(ROWS):
        y = ROW_Y0 + row * ROW_H
        band = C_ROW_B if row % 2 else C_ROW_A
        ty = y + 3
        cmds.append("BOXF(0,%d,319,%d,%d);" % (y, y + ROW_H - 1, band))
        cmds.append("CBOF(%d,%d,%d,%d,3,%d);" % (X_BADGE, y + 2, X_BADGE + 21, y + 18, badge))
        cmds.append("BOXF(%d,%d,%d,%d,%d);SBC(%d);" % (X_BADGE + 3, y + 15, X_BADGE + 18, y + 16, C_AMBER, badge))
        cmds.append(txt(12, X_BADGE + 5, y + 3, fn[:2].encode(), C_WHITE))
        cmds.append("SBC(%d);" % band)
        cmds.append(txt(16, X_TIME, ty, tm.encode(), C_WHITE))
        cmds.append(txt(16, X_FLIGHT, ty, fn.encode(), C_AMBER))
        city = place(code)
        csz = 16 if sc.width(16, city) <= PILL_X1 - X_CITY - 2 else 12
        cmds.append(txt(csz, X_CITY, ty if csz == 16 else y + 5, city, C_WHITE))
        if sc.width(csz, city) <= X_TERM - X_CITY - 4:
            cmds.append(txt(12, X_TERM, y + 5, ("T" + term).encode(), C_DIM))
        h, m = map(int, tm.split(":"))
        e = "%02d:%02d" % ((h * 60 + m + late) // 60, (h * 60 + m + late) % 60)
        if st == "L" and late >= 30:
            bg, fg, text = C_ORANGE, C_INK, T("S_ST_DELAY") + b" " + e.encode()
        elif late >= 15:
            bg, fg, text = -1, C_ORANGE, T("S_ST_EST") + b" " + e.encode()
        elif st == "C":
            bg, fg, text = C_RED, C_WHITE, T("S_ST_CANCEL")
        elif st == "B":
            bg, fg, text = C_GREEN, C_INK, T("S_ST_BOARD")
        else:
            bg, fg, text = -1, C_SOFT, T("S_ST_SCHED")
        if bg >= 0:
            cmds.append("BOXF(%d,%d,319,%d,%d);SBC(%d);" % (PILL_X1, y, y + ROW_H - 2, bg, bg))
        cmds.append(txt(16, PILL_X1 + (320 - PILL_X1 - sc.width(16, text)) // 2, ty, text, fg))
    # 页脚
    cmds.append("BOXF(0,%d,319,239,%d);PL(0,%d,319,%d,%d);SBC(%d);" % (FOOT_TOP, C_BG, FOOT_TOP, FOOT_TOP, C_LINE, C_BG))
    cmds.append("CIRF(9,229,3,%d);" % C_GREEN)
    cmds.append(txt(12, 18, 223, b"WiFi " + T("S_UPDATED") + b" 16:52", C_DIM))
    for i in range(2):
        cmds.append("CIRF(%d,229,3,%d);" % (154 + i * 12, C_AMBER if i == 0 else C_LINE))
    date = b"11-17 " + T("S_WK2")
    cmds.append(txt(12, 316 - sc.width(12, date), 223, date, C_DIM))
    for c in cmds:
        sc.run(c)
    return sc.img


def main():
    font_h, out = sys.argv[1], sys.argv[2]
    title = sys.argv[3] if len(sys.argv) > 3 else ""
    S, ZH = strings()
    AP = {r[0]: r for r in json.load(open(os.path.join(ROOT, "flightboard_host", "airports.json"), encoding="utf-8"))}
    pages = [page(font_h, lang, S, ZH, AP) for lang in (0, 1, 2)]
    k, gap, top = 3, 24, 56 if title else 0
    W, H = preview.W * k, preview.H * k
    sheet = Image.new("RGB", (W * 3 + gap * 4, H + gap * 2 + top), (24, 24, 28))
    for i, im in enumerate(pages):
        sheet.paste(im.resize((W, H), Image.NEAREST), (gap + i * (W + gap), gap + top))
    if title:
        try:
            f = ImageFont.truetype(os.path.join(ROOT, "assets", "fonts", "NotoSansSC[wght].ttf"), 30)
        except OSError:
            f = ImageFont.load_default()
        ImageDraw.Draw(sheet).text((gap, 18), title, font=f, fill=(230, 230, 230))
    sheet.save(out)
    print("wrote", out)


if __name__ == "__main__":
    main()
