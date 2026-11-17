#!/usr/bin/env python3
"""把航司图标做成 TFT 版航班牌用的小图 include/fb_logos.h。

来源(先克隆/下载到 assets/, 不进 git):
    git clone --depth 1 https://github.com/spydogenesis/airlines-logo.git assets/airlines-logo   按 IATA 两字码
    git clone --depth 1 https://github.com/Jxck-S/airline-logos.git assets/jxck                  按 ICAO 三字码
    tools/fetch_wikidata_logos.py                                 维基数据: 现役航司的 ICAO 对照 + 官方标志
    curl -o assets/openflights_airlines.dat https://raw.githubusercontent.com/jpatokal/openflights/master/data/airlines.dat
                                                                  (ICAO -> IATA 对照, 给第二个图库用)
哪家都没有的航司, 可以自己放一张 assets/extra/<两字码>.png 补上(最优先)。
图标是各航司的商标, 来源仓库没有授权声明: 自己桌上用来辨认航司没问题, 不要连同固件公开分发。
assets/ 不进 git。

收哪些航司: 图库里有的全收, 这样换到任何机场都有图标。做不出来的(见下)固件自动退回彩色徽章。

每个图标做成 22x17 的白底小牌(和原徽章一样大): 裁掉原图四周留白, 等比缩进 20x15, 居中贴在白底上。
白底是有意的: 这些标志都是按白底设计的, 深蓝色的(深航、山航)放在深色背景上会看不见。
"""
import csv
import hashlib
import json
import os
import re
import sys

from PIL import Image, ImageChops

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
A = os.path.join(ROOT, "assets")
REPO = os.path.join(A, "airlines-logo", "airlines-logo")
JXCK = os.path.join(A, "jxck")
# 第二个图库里的目录, 按优先顺序: 方形标在前, 横幅在后(横幅多半是文字, 只有"图标+文字"的能用上)
JXCK_DIRS = ("custom_logos", "flightaware_logos", "radarbox_logos", "avcodes_banners", "fr24_banners", "radarbox_banners")
W, H, PAD = 22, 17, 1


def md5(path):
    return hashlib.md5(open(path, "rb").read()).hexdigest()


def tile(path):
    try:
        im = Image.open(path).convert("RGBA")
    except Exception:
        return None
    white = Image.new("RGBA", im.size, (255, 255, 255, 255))
    flat = Image.alpha_composite(white, im).convert("RGB")
    diff = ImageChops.difference(flat, Image.new("RGB", flat.size, (255, 255, 255))).convert("L")
    mask = diff.point(lambda v: 255 if v > 24 else 0)
    box = mask.getbbox()
    if not box:
        return None  # 白色标志贴白底等于没有
    flat, mask = flat.crop(box), mask.crop(box)
    if flat.height > 3.6 * flat.width:
        return None  # 只剩一条竖线之类的残图
    if flat.width > 2.4 * flat.height:
        # 横长的商标。"图标 + 一行小字"的: 图标和字之间有一道空白, 只留左边的图标。
        cols = [mask.crop((x, 0, x + 1, mask.height)).getbbox() is not None for x in range(mask.width)]
        gap = next((x for x in range(2, int(mask.width * 0.45)) if not cols[x] and not cols[x + 1]), None)
        if gap is not None:
            sub = mask.crop((0, 0, gap, mask.height)).getbbox()
            if not sub:
                return None
            flat = flat.crop((0, 0, gap, flat.height)).crop(sub)
        elif flat.width > 3.6 * flat.height:
            return None  # 纯文字的长商标缩到 22 像素宽就是一条线, 放弃; ANA、SAS 这种短的还认得出, 留着
    k = min((W - 2 * PAD) / flat.width, (H - 2 * PAD) / flat.height)
    small = flat.resize((max(1, round(flat.width * k)), max(1, round(flat.height * k))), Image.LANCZOS)
    out = Image.new("RGB", (W, H), (255, 255, 255))
    out.paste(small, ((W - small.width) // 2, (H - small.height) // 2))
    return out


def main():
    if not os.path.isdir(REPO):
        print(__doc__)
        return 1
    files = {}  # 两字码 -> [候选文件, 按优先顺序]

    def add(code, path):
        if os.path.exists(path):
            files.setdefault(code, []).append(path)

    def scan(d):
        return {os.path.splitext(n)[0]: os.path.join(d, n) for n in os.listdir(d) if n.endswith(".png")} if os.path.isdir(d) else {}

    for code, path in scan(os.path.join(A, "extra")).items():
        add(code, path)
    for v in ("200x200_v2", "200x200"):  # 新版优先
        for code, path in scan(os.path.join(REPO, v)).items():
            add(code, path)
    # 第二个图库按 ICAO 命名: 用 OpenFlights 和第一个图库自带的对照表换成 IATA。
    # 一个 IATA 可能对过好几家(代码会回收再分配), 还在运营的排前面。
    to_icao = {}
    # 维基数据里还在运营的航司排最前: 代码被回收给新航司时(比如 9G), 以现在用这个代码的为准
    wd = os.path.join(A, "wikidata", "airlines.json")
    if os.path.exists(wd):
        for r in json.load(open(wd))["results"]["bindings"]:
            iata, icao = r["iata"]["value"], r.get("icao", {}).get("value", "")
            if re.fullmatch(r"[A-Z0-9]{2}", iata) and re.fullmatch(r"[A-Z]{3}", icao) and icao not in to_icao.get(iata, []):
                to_icao.setdefault(iata, []).append(icao)
    of = os.path.join(A, "openflights_airlines.dat")
    if os.path.exists(of):
        rows = list(csv.reader(open(of, encoding="utf-8")))
        for r in sorted(rows, key=lambda r: r[7:8] != ["Y"]):
            if len(r) > 7 and re.fullmatch(r"[A-Z0-9]{2}", r[3] or "") and re.fullmatch(r"[A-Z]{3}", r[4] or ""):
                to_icao.setdefault(r[3], []).append(r[4])
    mp = os.path.join(REPO, "..", "icao-iata-mapping.json")
    if os.path.exists(mp):
        for icao, iata in json.load(open(mp)).items():
            if icao not in to_icao.setdefault(iata, []):
                to_icao[iata].append(icao)
    jx = [scan(os.path.join(JXCK, d)) for d in JXCK_DIRS]
    for code, icaos in to_icao.items():
        for d in jx:
            for icao in icaos:
                if icao in d:
                    add(code, d[icao])
    # 最后: 维基数据上的官方标志(tools/fetch_wikidata_logos.py 只给前面都没有的航司下载)
    for name in sorted(os.listdir(os.path.join(A, "wikidata", "logos"))) if os.path.isdir(os.path.join(A, "wikidata", "logos")) else []:
        m = re.fullmatch(r"([A-Z0-9]{2})_\d+\.png", name)
        if m:
            add(m.group(1), os.path.join(A, "wikidata", "logos", name))
    # 图库对没有图标的航司放的是同一张灰色尾翼占位图(几百份)。几家共用一张的是姐妹航司(捷星、亚航), 要留着;
    # 出现 20 次以上的才是占位图。
    seen = {}
    for paths in files.values():
        for p in paths:
            seen[md5(p)] = seen.get(md5(p), 0) + 1
    got, missing = [], []
    for c in sorted(files):
        t = None
        for p in files[c]:
            if seen[md5(p)] < 20:
                t = tile(p)
                if t:
                    break
        (got.append((c, t)) if t else missing.append(c))
    out = ["// 由 tools/make_logos.py 生成, 不要手改。每个图标 %dx%d, RGB565, 白底。" % (W, H),
           "// 航司标志是各公司的商标, 仅用于在航班牌上辨认航司。",
           "#pragma once", "#include <Arduino.h>", "",
           "#define FB_LOGO_N %d" % len(got), "#define FB_LOGO_W %d" % W, "#define FB_LOGO_H %d" % H,
           'static const char FB_LOGO_CODE[] PROGMEM = "%s";  // 每两个字符一家, 已排序' % "".join(c for c, _ in got),
           "static const uint16_t FB_LOGO_PX[] PROGMEM = {"]
    for c, t in got:
        px = [((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3) for r, g, b in t.getdata()]
        out.append("  " + ",".join(str(v) for v in px) + ",  // " + c)
    out.append("};")
    path = os.path.join(ROOT, "include", "fb_logos.h")
    open(path, "w", encoding="utf-8").write("\n".join(out) + "\n")
    # 一张总览图, 放大 3 倍方便肉眼检查
    cols, z = 32, 3
    sheet = Image.new("RGB", (cols * (W * z + 4) + 4, ((len(got) + cols - 1) // cols) * (H * z + 4) + 4), (13, 20, 36))
    for i, (c, t) in enumerate(got):
        sheet.paste(t.resize((W * z, H * z), Image.NEAREST), (4 + (i % cols) * (W * z + 4), 4 + (i // cols) * (H * z + 4)))
    os.makedirs(os.path.join(ROOT, "preview"), exist_ok=True)
    sheet.save(os.path.join(ROOT, "preview", "logos-sheet.png"))
    print("wrote %s: %d logos (%d KB); 做不出图标的 %d 家: %s" % (
        os.path.normpath(path), len(got), len(got) * W * H * 2 // 1024, len(missing), " ".join(missing)))


if __name__ == "__main__":
    sys.exit(main())
