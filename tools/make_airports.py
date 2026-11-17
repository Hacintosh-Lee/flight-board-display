#!/usr/bin/env python3
"""生成电脑端选机场用的清单 flightboard_host/airports.json。

来源: OurAirports (公有领域) 的 airports.csv / countries.csv, 先下到 assets/ourairports/:
    https://davidmegginson.github.io/ourairports-data/airports.csv
    https://davidmegginson.github.io/ourairports-data/countries.csv
只收有 IATA 三字码、有定期航班的机场。中文名和屏幕用的是同一份(make_fb_gbk.all_names: 手写表 + 维基数据)。

每条: [三字码, 城市(英文), 机场名(英文), 国家代码, 国家中文名, 中文名或"", 规模 0大/1中/2小, 屏幕英文名, 屏幕泰文名或""]
屏幕英文名: 服务城市的英文名(维基数据, 没有就用所在地), 同城多机场时加区分, 见 en_names()。
另外生成 countries.json: 国家下拉用的分组, [[组名, [[国家代码, 中文名], ...]], ...], 已排好序。
需要 pip install babel pypinyin (只在生成时用)。
"""
import csv
import json
import os
import re
import sys

from PIL import ImageFont
import unicodedata

from babel import Locale
from pypinyin import lazy_pinyin

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_fb_gbk as g  # noqa: E402

SRC = os.path.join(HERE, "..", "assets", "ourairports")
# 国家/地区中文名: 用 CLDR 的标准译名(babel 库), 所有国家都有; 下面只改几个太长或习惯叫法不同的
NAME_FIX = {"HK": "中国香港", "MO": "中国澳门", "TW": "中国台湾", "AE": "阿联酋", "BA": "波黑",
            "CD": "刚果（金）", "CG": "刚果（布）", "CF": "中非", "DO": "多米尼加", "PG": "巴新",
            "TT": "特立尼达和多巴哥", "VC": "圣文森特", "KN": "圣基茨和尼维斯", "AG": "安提瓜和巴布达",
            "ST": "圣多美和普林西比", "GS": "南乔治亚岛", "TC": "特克斯和凯科斯", "MF": "法属圣马丁"}
# 选机场时国家下拉的分组。"中国"单独一组放最前, 大陆和港澳台排在一起。
GROUPS = [("中国", None), ("亚洲", "AS"), ("欧洲", "EU"), ("北美洲", "NA"), ("南美洲", "SA"), ("大洋洲", "OC"), ("非洲", "AF")]
CHINA = ["CN", "HK", "MO", "TW"]
GROUP_NAMES = {"中国": {"zh": "中国", "en": "China", "th": "จีน"},
               "亚洲": {"zh": "亚洲", "en": "Asia", "th": "เอเชีย"},
               "欧洲": {"zh": "欧洲", "en": "Europe", "th": "ยุโรป"},
               "北美洲": {"zh": "北美洲", "en": "North America", "th": "อเมริกาเหนือ"},
               "南美洲": {"zh": "南美洲", "en": "South America", "th": "อเมริกาใต้"},
               "大洋洲": {"zh": "大洋洲", "en": "Oceania", "th": "โอเชียเนีย"},
               "非洲": {"zh": "非洲", "en": "Africa", "th": "แอฟริกา"},
               "其他": {"zh": "其他", "en": "Other", "th": "อื่น ๆ"}}
EN_FIX = {"HK": "Hong Kong", "MO": "Macao", "TW": "Taiwan", "CN": "China"}
TH_FIX = {"HK": "ฮ่องกง", "MO": "มาเก๊า", "TW": "ไต้หวัน", "CN": "จีน"}  # 标准译名是完整官方称谓, 太长


HAND = g.hand_names()
PICK = g.city_pick()  # 通过校验的服务城市, 见 make_fb_gbk.city_pick
EN_MAX = 14  # 屏幕上英文地名最多 14 个字母
STOP = {"international", "intl", "airport", "airfield", "aerodrome", "air", "base", "field", "regional",
        "municipal", "civil", "the", "of", "and", "domestic", "intercontinental"}


ascii_fold = g.fold


CITY_FIX = {"New York City": "New York", "Washington, D.C.": "Washington"}
# 维基数据里"服务城市"写的是所在地、不是大家叫的城市的几个: 按惯例改过来(英文、泰文)
CITY_OVERRIDE = {"TPE": ("Taipei", "ไทเป"), "TSA": ("Taipei", "ไทเป"), "YNY": ("Yangyang", "")}


def short_city(name):
    name = CITY_FIX.get(name, name)
    name = ascii_fold(name)
    for p in ("Metro ", "Greater ", "Metropolitan "):
        if name.startswith(p):
            name = name[len(p):]
    for t in (" Metropolitan Area", " Prefecture", " Metropolis"):
        if name.endswith(t):
            name = name[: -len(t)].split("(")[0].split(",")[0].strip()
    if len(name) > EN_MAX and name.startswith("Saint "):
        name = "St " + name[6:]  # Saint Petersburg -> St Petersburg
    if len(name) > EN_MAX and name.endswith(" City"):
        name = name[:-5]  # Ho Chi Minh City -> Ho Chi Minh
    if len(name) > EN_MAX:
        # 按整词截: Las Palmas de Gran Canaria -> Las Palmas; 不停在 de / of 这种虚词上
        out = ""
        for w in name.split():
            if len(out) + len(w) + (1 if out else 0) > EN_MAX:
                break
            out = (out + " " + w).strip()
        while out and out.split()[-1].lower() in FILLER:
            out = " ".join(out.split()[:-1])
        name = out
    return name


FILLER = {"de", "del", "da", "do", "dos", "das", "di", "la", "le", "el", "of", "the", "and", "y", "am", "an", "upon"}


def align(rows, pick, zh):
    """按中文名对齐城市: 三字码 -> (英文城市, 泰文城市)。
    中文名(手写表里的尤其)写的是大家叫的城市: 台北桃园、纽约纽瓦克、首尔仁川。维基数据里机场挂的
    "服务城市"有时是所在地(桃园、纽瓦克、仁川)。所以拿中文名最长的前缀(台北、纽约、首尔), 去通过
    校验的城市里找中文叫这个名字的那个城市, 用它的英文名、泰文名。找不到就不对齐。"""
    # 只在同一个国家里对: 中文同名的城市不少(德里/Derry、奥克兰/Oakland、圣保罗/Saint Paul)
    country = {r[0]: r[3] for r in rows}
    d = {}
    for code, c in pick.items():
        z = g._short(c.get("zh", ""))
        k = (z, country.get(code))
        if z and c.get("en") and (k not in d or (not d[k][1] and c.get("th"))):
            d[k] = (c["en"], c.get("th", ""))
    out = {}
    for r in rows:
        name = zh.get(r[0], "")
        for n in range(len(name), 1, -1):
            if (name[:n], r[3]) in d:
                out[r[0]] = d[(name[:n], r[3])]
                break
    return out


def en_names(rows, wiki, aligned):
    """三字码 -> 屏幕上的英文地名。
    默认就是城市名; 同一个城市有好几个大中型机场时加区分, 和机场大屏的习惯一样:
    "Tokyo Narita" / "Tokyo Haneda"; 一组里有谁放不下(超过 14 个字母)就整组统一用三字码:
    "London LHR" / "London LGW"。"""
    city = {}
    for r in rows:
        if r[0] in CITY_OVERRIDE:
            city[r[0]] = CITY_OVERRIDE[r[0]][0]
            continue
        city[r[0]] = base_en(r, wiki, aligned)
    groups = {}
    for r in rows:
        if r[6] <= 1:  # 只和大中型机场比; 旁边的小机场不算"同城"
            groups.setdefault((city[r[0]], r[3]), []).append(r)
    out = dict(city)
    for (c, _), members in groups.items():
        if len(members) < 2:
            continue
        # 主机场保持纯城市名, 其余加区分。谁是主机场跟中文名走(手写表里叫纯城市名的那个:
        # 墨尔本 vs 阿瓦隆, 北京 vs 北京大兴); 东京成田/东京羽田这种中文就都带区分的, 英文也都带。
        hand = {r[0]: HAND[r[0]] for r in members if r[0] in HAND}
        shortest = min(map(len, hand.values())) if hand else 0
        main = [code for code, n in hand.items() if len(n) == shortest]
        if len(main) != 1:
            big = [r[0] for r in members if r[6] == 0]
            main = big if len(big) == 1 else []
        todo = [r for r in members if r[0] not in main]
        cw = {t.lower() for t in re.split(r"[\s\-]+", c)}
        words = {}
        for r in todo:
            toks = re.split(r"[\s\-–/(),.']+", ascii_fold(r[2]))
            w = [t for t in toks if t and t.lower() not in STOP and t.lower() not in cw]
            # 整串太长就只取第一个词: Aeroparque Jorge Newbery -> Aeroparque
            words[r[0]] = " ".join(w) if len(" ".join(w)) <= EN_MAX else (w[0] if w else "")
        named = {code: "%s %s" % (c, w) for code, w in words.items() if w}
        if len(named) == len(todo) and all(len(v) <= EN_MAX for v in named.values()) and len(set(named.values())) == len(named):
            out.update(named)
        else:  # 一组里统一: 有谁放不下就都用三字码
            for r in todo:  # 城市名太长带不下三字码时: 只写机场自己的名字(Lanseria), 再不行才是三字码
                w = words.get(r[0], "")
                out[r[0]] = ("%s %s" % (c, r[0])) if len(c) + 4 <= EN_MAX else (w if w and len(w) <= EN_MAX else r[0])
    return out


def base_en(r, wiki, aligned):
    """不带区分的英文城市名: 按中文对齐的 > 维基数据的服务城市 > 机场数据库的所在地"""
    if r[0] in CITY_OVERRIDE:
        return CITY_OVERRIDE[r[0]][0]
    muni = short_city(r[1])
    for raw in ((aligned.get(r[0]) or ("",))[0], (PICK.get(r[0]) or {}).get("en", ""), r[1]):
        full = ascii_fold(CITY_FIX.get(raw, raw)).split("(")[0].split(",")[0].strip()
        if full.endswith(" City") and full[:-5].lower() == muni.lower():
            full = full[:-5]  # Davao City -> Davao (所在地就叫 Davao)
        if len(full) > EN_MAX and muni and muni.lower() in full.lower():
            return muni       # San Miguel de Cozumel 放不下 -> Cozumel
        n = short_city(full)
        if n and len(n) <= EN_MAX:
            return n
    return r[0]


TH_FONT = "/System/Library/Fonts/Supplemental/Thonburi.ttc"
TH_MARKS = {0x0E31, *range(0x0E34, 0x0E3B), *range(0x0E47, 0x0E4F)}


def th_fits(name):
    """泰文名能不能放进屏幕的地名栏: 不超过 20 字节(TIS-620), 12 号字宽不超过 84 像素(和固件同一个字体)"""
    try:
        b = name.encode("tis-620")
    except UnicodeEncodeError:
        return False
    f = th_fits.font = getattr(th_fits, "font", None) or ImageFont.truetype(TH_FONT, 10)
    w = sum(6 if ord(ch) < 0x80 else (0 if ord(ch) in TH_MARKS else round(f.getlength(ch))) for ch in name)
    return len(b) <= 20 and w <= 84


def th_short(name):
    name = (name or "").split("(")[0].split(",")[0].strip()
    for p in ("จังหวัด", "อำเภอ", "เขต", "นคร", "เมโทร", "มหานคร"):
        if name.startswith(p) and len(name) > len(p) + 1:
            name = name[len(p):]
    if name.startswith("กรุงเทพ"):
        name = "กรุงเทพฯ"
    return name if name and th_fits(name) else ""


TH_AP_PREFIX = ("ท่าอากาศยานนานาชาติ", "ท่าอากาศยาน", "สนามบินนานาชาติ", "สนามบิน")


def th_from_airport(name):
    """泰文机场名去掉"国际机场"字样: ท่าอากาศยานนานาชาติเชียงใหม่ -> เชียงใหม่"""
    name = (name or "").split("(")[0].strip()
    for p in TH_AP_PREFIX:
        if name.startswith(p):
            name = name[len(p):].strip()
            break
    if name.endswith("นานาชาติ"):
        name = name[: -len("นานาชาติ")].strip()
    return th_short(name)


def th_names(rows, wiki, en, aligned):
    """三字码 -> 屏幕泰文名。城市名来源依次: 按中文对齐的 > 维基数据的服务城市 > 泰文机场名去掉"国际机场"
    > 同城别的机场的泰文城市名。同城多机场时跟英文一样带三字码。实在没有的屏幕上显示英文。"""
    base = {}
    for r in rows:
        w = wiki.get(r[0]) or {}
        base[r[0]] = (CITY_OVERRIDE[r[0]][1] if r[0] in CITY_OVERRIDE else "") or \
            th_short((aligned.get(r[0]) or ("", ""))[1]) or th_short((PICK.get(r[0]) or {}).get("th", "")) or \
            th_from_airport(w.get("ap_th", ""))
    by_en = {}
    for r in rows:
        if base[r[0]]:
            by_en.setdefault((base_en(r, wiki, aligned), r[3]), base[r[0]])
    out = {}
    for r in rows:
        bare = base_en(r, wiki, aligned)
        city = base[r[0]] or by_en.get((bare, r[3]), "")
        if not city:
            continue
        qualified = en[r[0]] != bare  # 英文名带了区分(Tokyo Haneda / London LGW)
        name = "%s %s" % (city, r[0]) if qualified else city
        out[r[0]] = name if th_fits(name) else city
    return out


def main():
    zh = g.all_names()  # 和屏幕上用的是同一份中文名
    zh_names = Locale("zh").territories
    continent = {r["code"]: r["continent"] for r in csv.DictReader(open(os.path.join(SRC, "countries.csv"), encoding="utf-8"))}
    cname = lambda cc: NAME_FIX.get(cc) or zh_names.get(cc) or cc  # noqa: E731
    rank = {"large_airport": 0, "medium_airport": 1, "small_airport": 2}
    out, seen = [], set()
    for r in csv.DictReader(open(os.path.join(SRC, "airports.csv"), encoding="utf-8")):
        code = (r["iata_code"] or "").strip().upper()
        if len(code) != 3 or not code.isalpha() or code in seen or r["type"] not in rank:
            continue
        if r["scheduled_service"] != "yes" and code not in zh:
            continue
        seen.add(code)
        cc = r["iso_country"]
        out.append([code, r["municipality"] or r["name"], r["name"], cc, cname(cc),
                    zh.get(code, ""), rank[r["type"]]])
    wiki_path = os.path.join(HERE, "..", "assets", "wikidata", "zh_names.json")
    wiki = json.load(open(wiki_path, encoding="utf-8")) if os.path.exists(wiki_path) else {}
    aligned = align(out, PICK, zh)
    en = en_names(out, wiki, aligned)
    th = th_names(out, wiki, en, aligned)
    for a in out:
        a.append(en[a[0]])
        a.append(th.get(a[0], ""))
    out.sort(key=lambda a: (a[6], a[0]))
    path = os.path.join(HERE, "..", "flightboard_host", "airports.json")
    json.dump(out, open(path, "w", encoding="utf-8"), ensure_ascii=False, separators=(",", ":"))
    # 国家下拉: 按大洲分组(中国和港澳台单独一组放最前)。每个名字给三种语言, 组内顺序由页面按当前语言排。
    used = {a[3] for a in out}
    en_terr, th_terr = Locale("en").territories, Locale("th").territories
    tri = lambda cc: {"zh": cname(cc), "en": EN_FIX.get(cc) or en_terr.get(cc, cc), "th": TH_FIX.get(cc) or th_terr.get(cc, cc)}  # noqa: E731
    py = lambda cc: "".join(lazy_pinyin(cname(cc)))  # noqa: E731
    groups = []
    for title, cont in GROUPS:
        ccs = [c for c in CHINA if c in used] if cont is None else \
            sorted((c for c in used if continent.get(c) == cont and c not in CHINA), key=py)
        groups.append([GROUP_NAMES[title], [[c, tri(c)] for c in ccs]])
    rest = sorted(used - {c for _, g in groups for c, _ in g}, key=py)
    if rest:
        groups.append([GROUP_NAMES["其他"], [[c, tri(c)] for c in rest]])
    json.dump(groups, open(os.path.join(HERE, "..", "flightboard_host", "countries.json"), "w", encoding="utf-8"),
              ensure_ascii=False, separators=(",", ":"))
    print("国家: " + ", ".join("%s %d" % (t["zh"], len(g)) for t, g in groups))
    print("泰文名: %d 个机场" % sum(1 for a in out if a[8]))
    miss = sorted(set(zh) - seen)
    print("wrote %s: %d airports (大 %d / 中 %d / 小 %d), 有中文名 %d; 表里有但清单里没有的: %s" % (
        os.path.normpath(path), len(out), *(sum(1 for a in out if a[6] == k) for k in (0, 1, 2)),
        sum(1 for a in out if a[5]), " ".join(miss) or "无"))


if __name__ == "__main__":
    main()
