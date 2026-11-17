#!/usr/bin/env python3
"""给每个机场的"服务城市"取英文名和泰文名, 三种语言取自同一个城市, 写进 assets/wikidata/zh_names.json
的 city_en / city_th, 全部候选城市存在 cities 里。(一个机场在维基数据里常挂好几个城市: 仁川机场挂了首尔和仁川; 各语言要用同一个,
优先挑中文名和我们现有中文名一致的那个。) 查询接口限流, 分批、每批间隔一分钟。"""
import json
import os
import sys
import time
import urllib.parse
import urllib.request

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUT = os.path.join(ROOT, "assets", "wikidata", "zh_names.json")
Q = """SELECT ?iata ?c ?zh ?en ?th WHERE {
  VALUES ?iata { %s }
  ?ap wdt:P238 ?iata ; wdt:P931 ?c .
  OPTIONAL { ?c rdfs:label ?zh . FILTER(LANG(?zh) IN ("zh-hans","zh-cn","zh")) }
  OPTIONAL { ?c rdfs:label ?en . FILTER(LANG(?en) = "en") }
  OPTIONAL { ?c rdfs:label ?th . FILTER(LANG(?th) = "th") }
}"""


def run(codes):
    url = "https://query.wikidata.org/sparql?" + urllib.parse.urlencode(
        {"query": Q % " ".join('"%s"' % c for c in codes), "format": "json"})
    req = urllib.request.Request(url, headers={"User-Agent": "flightboard-names/1.0 (personal airport display)"})
    with urllib.request.urlopen(req, timeout=90) as r:
        return json.load(r)["results"]["bindings"]


def main():
    have = json.load(open(OUT, encoding="utf-8"))
    todo = [c for c, v in have.items() if "cities" not in v]
    for i in range(0, len(todo), 400):
        batch = todo[i:i + 400]
        for _ in range(5):
            try:
                rows = run(batch)
                break
            except Exception as e:
                print("retry", i, e, file=sys.stderr)
                time.sleep(65)
        else:
            continue
        cities = {}
        for r in rows:
            d = cities.setdefault(r["iata"]["value"], {}).setdefault(r["c"]["value"], {})
            for k in ("zh", "en", "th"):
                if k in r and (k not in d or r[k].get("xml:lang") in ("zh-hans", "zh-cn")):
                    d[k] = r[k]["value"]
        for c in batch:
            e = have[c]
            opts = list(cities.get(c, {}).values())
            e["cities"] = opts  # 全部候选城市的三语名字, make_airports.py 按中文名对齐时用
            if opts:
                # 和现有中文城市名一致的优先; 其次标签最全的
                best = max(opts, key=lambda d: (d.get("zh") == e.get("city"), len(d)))
                e["city_en"], e["city_th"] = best.get("en", ""), best.get("th", "")
            e["same_city"] = 1
        json.dump(have, open(OUT, "w", encoding="utf-8"), ensure_ascii=False)
        print("%d/%d" % (min(i + 400, len(todo)), len(todo)), flush=True)
        time.sleep(62)
    print("done")


if __name__ == "__main__":
    main()
