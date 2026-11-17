#!/usr/bin/env python3
"""屏幕预览: 让固件把发给串口屏的命令镜像到 USB, 在电脑上按同样的命令渲染成 PNG。

    tools/preview.py [秒数=28] [输出目录=preview] [frames] [tft]

加 tft: 接的是 TFT 版固件, 文字用固件同一份字模渲染。

加 frames 会把每条命令之后的画面都存一张 frame_NNN.png, 用来看扫描动画的中间状态。

固件每画完一页(之后 0.8 秒没有新的内容命令)存一张 page_N.png。
这是模拟器: 字体用系统字体近似屏幕的点阵字库, 颜色和版式是准的, 字形细节会有出入。
运行前要先停掉 host.py(它占着串口)。
"""
import os
import re
import sys
import time

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "flightboard_host"))
import host  # noqa: E402

W, H, SCALE = 320, 240, 3

# 屏幕上电时的默认调色板 (说明书"颜色对应表"), 之后被 SCC 覆盖
DEFAULT565 = [0x0000, 0xF800, 0x07E0, 0x001F, 0xFFE0, 0x07FF, 0xF81F, 0xCE79,
              0x4208, 0xC208, 0x4608, 0x4218, 0xCE68, 0x4679, 0xCA39, 0xFFFF]

CJK_FONT = "/System/Library/Fonts/Supplemental/Songti.ttc"
ASC_FONT = "/System/Library/Fonts/Menlo.ttc"


def rgb565(v):
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return (r * 255 // 31, g * 255 // 63, b * 255 // 31)


class Screen:
    def __init__(self):
        self.img = Image.new("RGB", (W, H))
        self.d = ImageDraw.Draw(self.img)
        self.d.fontmode = "1"  # 不抗锯齿, 接近点阵字
        self.pal = {i: rgb565(v) for i, v in enumerate(DEFAULT565)}
        self.fonts = {}
        self.unknown = set()

    def col(self, c):
        return self.pal.get(int(c), (255, 0, 255))

    def font(self, path, px):
        key = (path, px)
        if key not in self.fonts:
            self.fonts[key] = ImageFont.truetype(path, px)
        return self.fonts[key]

    def text(self, size, x, y, s, c, xs=1, ys=1):
        """size 点阵: 半角占 size/2 宽, 全角占 size 宽; DSD 再按 xs/ys 整数倍放大"""
        w = sum(size if ord(ch) > 127 else size // 2 for ch in s)
        tmp = Image.new("1", (max(w, 1), size))
        td = ImageDraw.Draw(tmp)
        cx = 0
        for ch in s:
            if ord(ch) > 127:
                td.text((cx, -size // 8), ch, font=self.font(CJK_FONT, size), fill=1)
                cx += size
            else:
                # 12 点阵的半角字符是 5x7 字模放在 6x12 格子里
                px = 10 if size == 12 else size - 3
                td.text((cx, 1 if size == 12 else 0), ch, font=self.font(ASC_FONT, px), fill=1)
                cx += size // 2
        if xs != 1 or ys != 1:
            tmp = tmp.resize((tmp.width * xs, tmp.height * ys), Image.NEAREST)
        self.img.paste(self.col(c), (x, y), tmp)

    def run(self, line):
        for m in re.finditer(r"([A-Z]+\d*)(?:\(((?:[^()']|'[^']*')*)\))?;", line):
            name, argstr = m.group(1), m.group(2) or ""
            args = [a.strip() for a in re.findall(r"'[^']*'|[^,]+", argstr)]
            num = lambda i: int(args[i])  # noqa: E731
            s = lambda i: args[i][1:-1]   # noqa: E731
            d = self.d
            if name == "SCC":
                self.pal[num(0)] = rgb565(num(1))
            elif name == "SBC" or re.fullmatch(r"DR\d", name):
                pass
            elif name == "CLS":
                d.rectangle((0, 0, W, H), fill=self.col(num(0)))
            elif name == "BOXF":
                d.rectangle((num(0), num(1), num(2), num(3)), fill=self.col(num(4)))
            elif name == "BOX":
                d.rectangle((num(0), num(1), num(2), num(3)), outline=self.col(num(4)))
            elif name == "CBOF":
                d.rounded_rectangle((num(0), num(1), num(2), num(3)), radius=num(4), fill=self.col(num(5)))
            elif name == "CBOX":
                d.rounded_rectangle((num(0), num(1), num(2), num(3)), radius=num(4), outline=self.col(num(5)))
            elif name == "PL":
                d.line((num(0), num(1), num(2), num(3)), fill=self.col(num(4)))
            elif name == "PS":
                d.point((num(0), num(1)), fill=self.col(num(2)))
            elif name in ("CIR", "CIRF"):
                x, y, r = num(0), num(1), num(2)
                box = (x - r, y - r, x + r, y + r)
                d.ellipse(box, **({"fill": self.col(num(3))} if name == "CIRF" else {"outline": self.col(num(3))}))
            elif name in ("DS12", "DS16"):
                self.text(int(name[2:]), num(0), num(1), s(2), num(3))
            elif name == "DSD":
                self.text(num(0), num(1), num(2), s(3), num(6), num(4), num(5))
            else:
                self.unknown.add(name)

    def save(self, path):
        self.img.resize((W * SCALE, H * SCALE), Image.NEAREST).save(path)


class TftScreen(Screen):
    """TFT 版: 文字不用系统字体近似, 直接用固件里那份 fb_font.h 的灰度字模, 和真屏逐像素一致。"""

    def __init__(self):
        super().__init__()
        src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "include", "fb_font.h")).read()
        self.arr = {}
        for m in re.finditer(r"(FB_\w+)\[\] PROGMEM = \{([^}]*)\}", src):
            self.arr[m.group(1)] = [int(v, 0) for v in m.group(2).replace("\n", "").split(",") if v.strip()]
        self.bg = (0, 0, 0)
        self.lang = 0
        lg = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "include", "fb_logos.h")).read()
        self.logo_w = int(re.search(r"FB_LOGO_W (\d+)", lg).group(1))
        self.logo_h = int(re.search(r"FB_LOGO_H (\d+)", lg).group(1))
        body = lg[lg.index("FB_LOGO_PX[] PROGMEM = {") + 24:]
        self.logo_px = [int(v) for v in re.sub(r"//[^\n]*", "", body[:body.index("};")]).replace("\n", "").split(",") if v.strip()]

    def logo(self, x, y, idx):
        n = self.logo_w * self.logo_h
        px = self.img.load()
        for r in range(self.logo_h):
            inset = 1 if r in (0, self.logo_h - 1) else 0
            for c in range(inset, self.logo_w - inset):
                px[x + c, y + r] = rgb565(self.logo_px[idx * n + r * self.logo_w + c])

    def run(self, line):
        for m in re.finditer(r"SBC\((\d+)\);", line):
            pass
        # SBC 要按出现顺序生效, 所以逐条拆开交给父类, 遇到 SBC 记下底色
        for m in re.finditer(r"[A-Z]+\d*(?:\((?:[^()']|'[^']*')*\))?;", line):
            one = m.group(0)
            sb = re.fullmatch(r"SBC\((\d+)\);", one)
            lgm = re.fullmatch(r"LOGO\((\d+),(\d+),(\d+)\);", one)
            lang = re.fullmatch(r"LANG\((\d+)\);", one)
            if lang:
                self.lang = int(lang.group(1))
            elif lgm:
                self.logo(*(int(v) for v in lgm.groups()))
            elif sb:
                self.bg = self.col(sb.group(1))
            else:
                super().run(one)

    def glyph(self, x, y, data, w, h, fg, sx, sy):
        stride = (w + 1) // 2
        px = self.img.load()
        for r in range(h):
            for c in range(w):
                v = data[r * stride + c // 2]
                a = (v & 15) if c & 1 else (v >> 4)
                col = tuple(self.bg[i] + (fg[i] - self.bg[i]) * a // 15 for i in range(3))
                for dy in range(sy):
                    for dx in range(sx):
                        X, Y = x + c * sx + dx, y + r * sy + dy
                        if 0 <= X < W and 0 <= Y < H:
                            px[X, Y] = col

    TH_MARKS = {0x0E31, *range(0x0E34, 0x0E3B), *range(0x0E47, 0x0E4F)}
    TH_UPPER = {0x0E31, 0x0E34, 0x0E35, 0x0E36, 0x0E37, 0x0E47, 0x0E4D}

    TH_TALL = {0x0E1B, 0x0E1D, 0x0E1F, 0x0E2C}

    def thai(self, size, x, y, raw, fg):
        """和固件 tft.h 的 thaiText 同一套规则, 见 tools/make_font.py 的说明"""
        gw, h = (12, 20) if size == 16 else (10, 16)
        stride = (gw + 1) // 2
        data, adv, ink = self.arr["FB_TH%d" % size], self.arr["FB_TH%d_ADV" % size], self.arr["FB_TH%d_INK" % size]
        line = [[0] * 320 for _ in range(h)]
        pen, base_x, base_ink, base_cp, upper = 0, None, 0, 0, False

        def put(i, gx):
            g = data[i * stride * h:(i + 1) * stride * h]
            for r in range(h):
                for c in range(gw):
                    v = g[r * stride + c // 2]
                    a = (v & 15) if c & 1 else (v >> 4)
                    if a and 0 <= gx + c < 320:
                        line[r][gx + c] = max(line[r][gx + c], a)

        def mark(i):
            if base_x is None:
                return
            shift = 2 if base_cp in self.TH_TALL else 0
            put(i, base_x + base_ink - ink[i] - shift)

        for b in raw:
            if b < 0x80:  # 泰文里夹的数字、空格、点
                ch = b - 0x20 if 0x20 <= b < 0x7F else ord("?") - 0x20
                m = (size // 2 + 1) // 2 * size
                g = self.arr["FB_ASC%d" % size][ch * m:(ch + 1) * m]
                for r in range(size):
                    for c in range(size // 2):
                        v = g[r * ((size // 2 + 1) // 2) + c // 2]
                        a = (v & 15) if c & 1 else (v >> 4)
                        if a and pen + c < 320:
                            line[r + 3][pen + c] = max(line[r + 3][pen + c], a)
                pen += size // 2
                base_x = None
                continue
            if not 0xA1 <= b <= 0xFB:
                continue
            cp = b - 0xA1 + 0x0E01
            i = cp - 0x0E01
            if cp == 0x0E33:  # ำ = ํ 叠在前一个字母上 + า
                mark(0x0E4D - 0x0E01)
                i, cp = 0x0E32 - 0x0E01, 0x0E32
            if cp in self.TH_MARKS:
                if upper and 0x0E48 <= cp <= 0x0E4C:
                    i = 91 + cp - 0x0E48
                mark(i)
                upper = upper or cp in self.TH_UPPER
            else:
                put(i, pen)
                base_x, base_ink, base_cp, upper = pen, ink[i], cp, False
                pen += adv[i]
        rows = [r for r in range(h) if any(line[r][:pen])]
        if rows:
            for r in range(rows[0], rows[-1] + 1):
                self.glyph_alpha(x, y - 3 + r, line[r][:pen], pen, 1, fg)
        return x + pen

    def latin(self, size, x, y, raw, fg):
        gw = 12 if size == 16 else 10
        stride = (gw + 1) // 2
        data, adv = self.arr["FB_LAT%d" % size], self.arr["FB_LAT%d_ADV" % size]
        w = sum(adv[(b if 0x20 <= b < 0x7F else 63) - 0x20] for b in raw)
        w = min(w, W - x)
        rows = [[0] * w for _ in range(size)]
        pen = 0
        for b in raw:
            i = (b if 0x20 <= b < 0x7F else 63) - 0x20
            for r in range(size):
                for k in range(gw):
                    if pen + k >= w:
                        break
                    v = data[i * stride * size + r * stride + k // 2]
                    a = (v & 15) if k & 1 else (v >> 4)
                    rows[r][pen + k] = max(rows[r][pen + k], a)
            pen += adv[i]
        for r in range(size):
            self.glyph_alpha(x, y + r, rows[r], w, 1, fg)
        return x + w

    def glyph_alpha(self, x, y, alpha, w, h, fg):
        px = self.img.load()
        for r in range(h):
            for c in range(w):
                a = alpha[r * w + c]
                X, Y = x + c, y + r
                if 0 <= X < W and 0 <= Y < H:
                    px[X, Y] = tuple(self.bg[i] + (fg[i] - self.bg[i]) * a // 15 for i in range(3))

    def text(self, size, x, y, s, c, xs=1, ys=1):
        fg = self.col(c)
        if self.lang == 2 and not (size == 16 and xs == 2):  # 大时钟永远是数字(和固件一致)
            return self.thai(size, x, y, s.encode("latin-1", errors="replace"), fg)
        raw = s.encode("latin-1", errors="replace")
        if xs == 1 and ys == 1 and all(b < 0x80 for b in raw) and any(97 <= b <= 122 for b in raw):
            return self.latin(size, x, y, raw, fg)   # 英文单词: 窄体按字宽排(和固件 latinText 一致)
        s = raw.decode("gbk", errors="replace")
        big = size == 16 and xs == 2 and ys == 2
        for ch in s:
            if ord(ch) > 127:
                code = int.from_bytes(ch.encode("gbk", errors="replace"), "big")
                n = size * size // 2
                try:
                    i = self.arr["FB_HZ_CODE"].index(code)
                    self.glyph(x, y, self.arr["FB_HZ%d" % size][i * n:(i + 1) * n], size, size, fg, xs, ys)
                except ValueError:
                    self.d.rectangle((x + 2, y + 2, x + size * xs - 3, y + size * ys - 3), fill=fg)
                x += size * xs
            else:
                i = ord(ch) - 0x20
                if big:
                    self.glyph(x, y, self.arr["FB_ASC32"][i * 256:(i + 1) * 256], 16, 32, fg, 1, 1)
                else:
                    n = (size // 2 + 1) // 2 * size
                    self.glyph(x, y, self.arr["FB_ASC%d" % size][i * n:(i + 1) * n], size // 2, size, fg, xs, ys)
                x += size // 2 * xs


def main():
    tft = "tft" in sys.argv
    if tft:
        sys.argv.remove("tft")
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 28
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "preview")
    os.makedirs(out, exist_ok=True)
    ser, dev, ident = host.open_board()
    if ser is None:
        print("没找到航班牌(host.py 还占着串口?)")
        return 1
    print(dev, ident)
    scr = TftScreen() if tft else Screen()
    ser.write(("$T,%d\n" % int(time.time())).encode())
    ser.write(b"$M,1\n")
    frames = len(sys.argv) > 3 and sys.argv[3] == "frames"
    fn = 0
    buf, last, n, pending, maxlen = b"", time.time(), 0, False, 0
    end = time.time() + secs
    try:
        while time.time() < end:
            buf += ser.read(4096)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                if raw.startswith(b"$S,"):
                    maxlen = max(maxlen, len(raw) - 3)
                    scr.run(raw[3:].decode("latin-1" if tft else "gbk", errors="replace"))
                    # 进度条每半秒走一格, 不算"页面在变", 否则永远等不到一页画完
                    if not re.fullmatch(rb"BOXF\(\d+,32,\d+,33,\d+\);", raw[3:].strip()):
                        last, pending = time.time(), True
                    if frames:
                        fn += 1
                        scr.save(os.path.join(out, "frame_%03d.png" % fn))
            if pending and time.time() - last > 0.8:
                n += 1
                path = os.path.join(out, "page_%d.png" % n)
                scr.save(path)
                print("saved", os.path.normpath(path))
                pending = False
    finally:
        ser.write(b"$M,0\n")
        time.sleep(0.3)
        ser.close()
    print("最长一行命令 %d 字节 (屏幕缓冲 1024)" % maxlen)
    if scr.unknown:
        print("模拟器没实现的命令:", sorted(scr.unknown))
    return 0


if __name__ == "__main__":
    sys.exit(main())
