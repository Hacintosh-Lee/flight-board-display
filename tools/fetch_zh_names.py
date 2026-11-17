#!/usr/bin/env python3
"""从维基数据(Wikidata, CC0)批量取机场和它服务的城市的中文名、英文名、泰文名, 存到 assets/wikidata/zh_names.json。
(英文城市名比机场数据库里的"所在地"准: 拉萨贡嘎机场的所在地是山南, 服务的城市是拉萨。)
只在生成时用; 结果由 make_fb_gbk.py / make_airports.py 读取。分批查询, 一次查太多会超时。"""
import json
import os
import sys
import time
import urllib.parse
import urllib.request

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUT = os.path.join(ROOT, "assets", "wikidata", "zh_names.json")
Q = """SELECT ?iata ?ap ?apl ?cityl ?cityen ?cityth ?apth WHERE {
  VALUES ?iata { %s }
  ?ap wdt:P238 ?iata .
  OPTIONAL { ?ap rdfs:label ?apl . FILTER(LANG(?apl) IN ("zh-hans","zh-cn","zh")) }
  OPTIONAL { ?ap wdt:P931 ?c . ?c rdfs:label ?cityl . FILTER(LANG(?cityl) IN ("zh-hans","zh-cn","zh")) }
  OPTIONAL { ?ap wdt:P931 ?c2 . ?c2 rdfs:label ?cityen . FILTER(LANG(?cityen) = "en") }
  OPTIONAL { ?ap wdt:P931 ?c3 . ?c3 rdfs:label ?cityth . FILTER(LANG(?cityth) = "th") }
  OPTIONAL { ?ap rdfs:label ?apth . FILTER(LANG(?apth) = "th") }
}"""
BATCH = 400


def run(codes):
    q = Q % " ".join('"%s"' % c for c in codes)
    url = "https://query.wikidata.org/sparql?" + urllib.parse.urlencode({"query": q, "format": "json"})
    req = urllib.request.Request(url, headers={"User-Agent": "flightboard-names/1.0 (personal airport display)"})
    with urllib.request.urlopen(req, timeout=90) as r:
        return json.load(r)["results"]["bindings"]


def main():
    airports = json.load(open(os.path.join(ROOT, "flightboard_host", "airports.json"), encoding="utf-8"))
    codes = [a[0] for a in airports]
    have = json.load(open(OUT, encoding="utf-8")) if os.path.exists(OUT) else {}
    todo = [c for c in codes if c not in have or "city_th" not in have[c]]
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    for i in range(0, len(todo), BATCH):
        batch = todo[i:i + BATCH]
        for attempt in range(5):
            try:
                rows = run(batch)
                break
            except Exception as e:  # 查询接口限流时一分钟只给一次
                print("retry", i, e, file=sys.stderr)
                time.sleep(65)
        else:
            continue
        for c in batch:
            e = have.setdefault(c, {"ap": "", "city": ""})
            e.update(city_en=e.get("city_en", ""), city_th="", ap_th="")
        for r in rows:
            c = r["iata"]["value"]
            for k, key in (("cityen", "city_en"), ("cityth", "city_th"), ("apth", "ap_th")):
                if r.get(k, {}).get("value") and not have[c][key]:
                    have[c][key] = r[k]["value"]
            for k, key in (("apl", "ap"), ("cityl", "city")):
                v = r.get(k, {})
                # 简体优先: zh-hans / zh-cn 覆盖 zh
                if v.get("value") and (not have[c][key] or v.get("xml:lang") in ("zh-hans", "zh-cn")):
                    have[c][key] = v["value"]
        json.dump(have, open(OUT, "w", encoding="utf-8"), ensure_ascii=False)
        print("%d/%d" % (min(i + BATCH, len(todo)), len(todo)), flush=True)
        time.sleep(62)
    n_ap = sum(1 for v in have.values() if v["ap"])
    n_city = sum(1 for v in have.values() if v["city"])
    print("done: %d airports, 有机场中文名 %d, 有城市中文名 %d" % (len(have), n_ap, n_city))


if __name__ == "__main__":
    main()
