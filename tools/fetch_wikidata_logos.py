#!/usr/bin/env python3
"""给还缺图标的航司, 从维基数据(Wikimedia Commons 上的官方标志)补图。

用法: 先跑 make_logos.py, 再跑本脚本, 再跑一次 make_logos.py。加 --refresh 重新查维基数据。
  1. 查维基数据: 所有还在运营(没有"解散日期")的航司的 IATA、ICAO、小图标(P8972)和标志(P154),
     存到 assets/wikidata/airlines.json (make_logos.py 也用它的 ICAO 对照)。
  2. 只给 include/fb_logos.h 里还没有图标的航司下载, 存成 assets/wikidata/logos/<两字码>_<n>.png。
"""
import json
import os
import re
import sys
import time
import urllib.parse
import urllib.request

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
W = os.path.join(ROOT, "assets", "wikidata")
UA = {"User-Agent": "flightboard-logos/1.0 (personal airport display)", "Accept": "application/json"}
Q = """SELECT ?iata ?icao ?icon ?logo WHERE {
  ?a wdt:P229 ?iata .
  FILTER NOT EXISTS { ?a wdt:P576 ?end }
  OPTIONAL { ?a wdt:P230 ?icao }
  OPTIONAL { ?a wdt:P8972 ?icon }
  OPTIONAL { ?a wdt:P154 ?logo }
}"""


def main():
    os.makedirs(os.path.join(W, "logos"), exist_ok=True)
    cache = os.path.join(W, "airlines.json")
    if os.path.exists(cache) and "--refresh" not in sys.argv:
        data = open(cache, "rb").read()  # 查询接口限流很严(高峰时一分钟一次), 有缓存就用缓存
    else:
        url = "https://query.wikidata.org/sparql?" + urllib.parse.urlencode({"query": Q})
        data = urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=170).read()
        open(cache, "wb").write(data)
    imgs = {}
    for r in json.loads(data)["results"]["bindings"]:
        c = r["iata"]["value"]
        if re.fullmatch(r"[A-Z0-9]{2}", c):
            for k in ("icon", "logo"):  # 小图标优先
                if k in r and r[k]["value"] not in imgs.setdefault(c, []):
                    imgs[c].append(r[k]["value"])
    h = re.search(r'FB_LOGO_CODE\[\] PROGMEM = "([^"]*)"', open(os.path.join(ROOT, "include", "fb_logos.h")).read())
    have = {h.group(1)[i:i + 2] for i in range(0, len(h.group(1)), 2)} if h else set()
    n = 0
    for c in sorted(imgs):
        if c in have:
            continue
        for i, src in enumerate(imgs[c][:3]):
            out = os.path.join(W, "logos", "%s_%d.png" % (c, i))
            if os.path.exists(out):
                continue
            try:  # Special:FilePath 加 width 会把 SVG 渲染成 PNG
                png = urllib.request.urlopen(urllib.request.Request(src + "?width=240", headers=UA), timeout=30).read()
                if png[:4] == b"\x89PNG":
                    open(out, "wb").write(png)
                    n += 1
            except Exception as e:
                print(c, e)
            time.sleep(0.3)
    print("downloaded %d images" % n)


if __name__ == "__main__":
    main()
