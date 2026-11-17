#!/usr/bin/env python3
"""生成 include/fb_gbk.h: 航班牌固件用到的中文(GBK 编码)。

串口屏只认 GBK, 而源码是 UTF-8, 所以界面文字和机场中文名都在这里转好再编进固件。
改了文字或加了机场后重跑:  python3 tools/make_fb_gbk.py
"""
import csv
import json
import os
import unicodedata

import opencc

_cc = opencc.OpenCC("t2s")

STRINGS = {
    "S_JICHANG": "机场",
    "S_DEP": "出发",
    "S_ARR": "到达",
    "S_C_TIME": "时间",
    "S_C_FLIGHT": "航班",
    "S_C_DEST": "目的地",
    "S_C_ORIG": "出发地",
    "S_C_TERM": "航站",
    "S_C_STATUS": "状态",
    "S_ST_SCHED": "计划",
    "S_ST_BOARD": "登机",
    "S_ST_DEPARTED": "起飞",
    "S_ST_ARRIVED": "到达",
    "S_ST_DELAY": "延误",
    "S_ST_EST": "预计",
    "S_ST_CANCEL": "取消",
    "S_ST_UNKNOWN": "--",
    "S_UPDATED": "更新",
    "S_STALE": "数据过期",
    "S_WK0": "周日", "S_WK1": "周一", "S_WK2": "周二", "S_WK3": "周三",
    "S_WK4": "周四", "S_WK5": "周五", "S_WK6": "周六",
    "S_WAIT1": "等待航班数据",
    "S_WAIT2": "接电脑运行 host.py",
    "S_WAIT3": "或配置 WiFi 后自动更新",
    "S_NOFLIGHT": "暂无航班",
    "S_W_NOWIFI": "还没有配置 WiFi",
    "S_W_CONN": "正在连接 WiFi",
    "S_W_FETCH": "正在获取航班数据",
    "S_W_QUOTA": "查询次数用完了",
    "S_W_QUOTA2": "稍后会自动恢复",
    "S_W_AP": "用手机连接这个 WiFi",
    "S_W_AP2": "然后打开 192.168.4.1",
    "S_W_NOKEY": "还没有数据源密钥",
    "S_W_NOKEY2": "在设置页面里填写",
    "S_W_NET": "网络不通 正在重试",
    "S_W_HTTP": "数据源暂时出错 稍后重试",
    "S_W_NOFL": "暂时没有航班",
    "S_W_TAP": "点一下屏幕进入设置",
    "S_W_TAP_APT": "点一下屏幕可以更换机场",
    "S_SETTINGS": "设置",
    "S_BACK": "返回",
    "S_M_APT": "更换机场",
    "S_M_WIFI": "连接 WiFi",
    "S_M_CAL": "触摸校准",
    "S_M_CAL_SUB": "点不准的时候用",
    "S_NOTCONN": "未连接",
    "S_IDLE_HINT": "一段时间不操作会自动回到航班牌",
    "S_PREV": "上一页",
    "S_NEXT": "下一页",
    "S_RESCAN": "重新搜索",
    "S_SCANNING": "正在搜索 WiFi ...",
    "S_NONET": "没有找到 WiFi",
    "S_OPEN": "开放",
    "S_PICK_APT": "选择机场",
    "S_APT_COMMON": "常用",
    "S_APT_CODE": "输入三字码",
    "S_CODE_HINT": "输入机场三字码 如 NRT",
    "S_CODE_BAD": "没有这个机场",
    "S_OK": "确定",
    "S_PICK_WIFI": "选择 WiFi",
    "S_PWD_HINT": "输入密码",
    "S_CONNECT": "连接",
    "S_SPACE": "空格",
    "S_CONNECTING": "正在连接",
    "S_CONN_OK": "已连接",
    "S_CONN_FAIL": "连接失败 请检查密码",
    "S_CAL_WAIT": "依次点击四个角上的十字",
}

# IATA -> 中文城市名 (最多 5 个汉字, 列宽就这么多)
AIRPORTS = """
PEK 北京|PKX 北京大兴|TSN 天津|SJW 石家庄|TYN 太原|HET 呼和浩特|BAV 包头|HLD 海拉尔
SHE 沈阳|DLC 大连|CGQ 长春|HRB 哈尔滨|YNJ 延吉|MDG 牡丹江|JMU 佳木斯
SHA 上海虹桥|PVG 上海浦东|NKG 南京|WUX 无锡|CZX 常州|NTG 南通|XUZ 徐州|YNZ 盐城|LYG 连云港|YTY 扬州
HGH 杭州|NGB 宁波|WNZ 温州|YIW 义乌|HSN 舟山|JUZ 衢州|HYN 台州
HFE 合肥|TXN 黄山|FOC 福州|XMN 厦门|JJN 泉州|WUS 武夷山|KHN 南昌|KOW 赣州|JDZ 景德镇
TNA 济南|TAO 青岛|YNT 烟台|WEH 威海|LYI 临沂|JNG 济宁
CGO 郑州|LYA 洛阳|WUH 武汉|YIH 宜昌|XFN 襄阳|ENH 恩施|CSX 长沙|DYG 张家界|HNY 衡阳
CAN 广州|SZX 深圳|ZUH 珠海|SWA 揭阳|ZHA 湛江|HUZ 惠州|FUO 佛山
NNG 南宁|KWL 桂林|BHY 北海|LZH 柳州|HAK 海口|SYX 三亚|BAR 琼海
CKG 重庆|WXN 万州|CTU 成都|TFU 成都天府|MIG 绵阳|YBP 宜宾|LZO 泸州|XIC 西昌|JZH 九寨沟
KWE 贵阳|ZYI 遵义|WMT 遵义茅台|KMG 昆明|LJG 丽江|DLU 大理|JHG 西双版纳|LUM 芒市|TCZ 腾冲|BSD 保山
LXA 拉萨|XIY 西安|UYN 榆林|ENY 延安|LHW 兰州|DNH 敦煌|JGN 嘉峪关|XNN 西宁|INC 银川
URC 乌鲁木齐|KHG 喀什|YIN 伊宁|KRL 库尔勒|AKU 阿克苏|HTN 和田
HKG 香港|MFM 澳门|TPE 台北桃园|TSA 台北松山|KHH 高雄|RMQ 台中
NRT 东京成田|HND 东京羽田|KIX 大阪|NGO 名古屋|FUK 福冈|CTS 札幌|OKA 冲绳|SDJ 仙台|HIJ 广岛
KOJ 鹿儿岛|KMQ 小松|OKJ 冈山|TAK 高松|NGS 长崎|FSZ 静冈|KIJ 新潟|TOY 富山|IBR 茨城|KMJ 熊本|MYJ 松山
ICN 首尔仁川|GMP 首尔金浦|PUS 釜山|CJU 济州|TAE 大邱|CJJ 清州|MWX 务安
SIN 新加坡|KUL 吉隆坡|PEN 槟城|BKI 亚庇|BKK 曼谷|DMK 曼谷廊曼|HKT 普吉|CNX 清迈|KBV 甲米|USM 苏梅
SGN 胡志明|HAN 河内|DAD 岘港|CXR 芽庄|PQC 富国岛|PNH 金边|KTI 金边|REP 暹粒|SAI 暹粒|VTE 万象|LPQ 琅勃拉邦
MNL 马尼拉|CEB 宿务|KLO 长滩岛|CGK 雅加达|DPS 巴厘岛|SUB 泗水|RGN 仰光|MDL 曼德勒
DEL 德里|BOM 孟买|CCU 加尔各答|MAA 金奈|BLR 班加罗尔|CMB 科伦坡|MLE 马累|KTM 加德满都|DAC 达卡
ISB 伊斯兰堡|KHI 卡拉奇|LHE 拉合尔|ALA 阿拉木图|NQZ 阿斯塔纳|TAS 塔什干|FRU 比什凯克|UBN 乌兰巴托
DXB 迪拜|AUH 阿布扎比|DOH 多哈|RUH 利雅得|JED 吉达|KWI 科威特|IST 伊斯坦布尔|TLV 特拉维夫|IKA 德黑兰
CAI 开罗|ADD 亚的斯|NBO 内罗毕|JNB 约翰内斯堡|MRU 毛里求斯|CMN 卡萨布兰卡
SVO 莫斯科|LED 圣彼得堡|VVO 海参崴|OVB 新西伯利亚|IKT 伊尔库茨克|KHV 伯力
LHR 伦敦|LGW 伦敦盖特|MAN 曼彻斯特|CDG 巴黎|FRA 法兰克福|MUC 慕尼黑|AMS 阿姆斯特丹|BRU 布鲁塞尔
ZRH 苏黎世|GVA 日内瓦|VIE 维也纳|FCO 罗马|MXP 米兰|VCE 威尼斯|MAD 马德里|BCN 巴塞罗那|LIS 里斯本
CPH 哥本哈根|ARN 斯德哥尔摩|OSL 奥斯陆|HEL 赫尔辛基|WAW 华沙|PRG 布拉格|BUD 布达佩斯|ATH 雅典|BEG 贝尔格莱德
JFK 纽约|EWR 纽约纽瓦克|BOS 波士顿|IAD 华盛顿|ORD 芝加哥|DTW 底特律|ATL 亚特兰大|DFW 达拉斯|IAH 休斯敦
LAX 洛杉矶|SFO 旧金山|SEA 西雅图|LAS 拉斯维加斯|HNL 檀香山|ANC 安克雷奇|GUM 关岛|SPN 塞班
YVR 温哥华|YYZ 多伦多|YUL 蒙特利尔|MEX 墨西哥城|GRU 圣保罗
ZAT 昭通|WNH 文山|SYM 普洱|LNJ 临沧|DIG 香格里拉|HZG 汉中|AKA 安康|LLV 吕梁|CIH 长治|DAT 大同|YCU 运城|HDG 邯郸
TVS 唐山|SHP 秦皇岛|ZQZ 张家口|CIF 赤峰|TGO 通辽|DSN 鄂尔多斯|WUA 乌海|HLH 乌兰浩特|XIL 锡林浩特|DDG 丹东|JNZ 锦州
NBS 长白山|TNH 通化|DQA 大庆|NDG 齐齐哈尔|HEK 黑河|OHE 漠河|JGS 井冈山|YIC 宜春|SQD 上饶|AQG 安庆|FUG 阜阳|WHA 芜湖
LCX 龙岩|SQJ 三明|DOY 东营|WEF 潍坊|RIZ 日照|NNY 南阳|XAI 信阳|SHS 荆州|WDS 十堰|CGD 常德|HJJ 怀化|LLF 永州|YYA 岳阳
MXZ 梅州|WUZ 梧州|AEB 百色|HCJ 河池|LLB 荔波|TEN 铜仁|KJH 凯里|BFJ 毕节|ACX 兴义|LPF 六盘水|AVA 安顺
DCY 稻城|KGT 康定|NAO 南充|GYS 广元|DAX 达州|LZY 林芝|RKZ 日喀则|BPX 昌都|GOQ 格尔木|YUS 玉树|IQN 庆阳|THQ 天水
ZHY 中卫|GYU 固原|AAT 阿勒泰|KRY 克拉玛依|TCG 塔城|HMI 哈密|TLQ 吐鲁番|KCA 库车|BPL 博乐
SYD 悉尼|MEL 墨尔本|BNE 布里斯班|PER 珀斯|ADL 阿德莱德|CNS 凯恩斯|AKL 奥克兰|CHC 基督城|NAN 楠迪
CBR 堪培拉|HBA 霍巴特|LST 朗塞斯顿|OOL 黄金海岸|MCY 阳光海岸|DRW 达尔文|TSV 汤斯维尔|NTL 纽卡斯尔|ASP 爱丽斯泉
AYQ 乌鲁鲁|MQL 米尔迪拉|ABX 奥尔伯里|DPO 德文波特|BNK 巴利纳|HTI 汉密尔顿岛|PPP 普罗瑟派恩|MKY 麦凯|ROK 罗克汉普顿
KGI 卡尔古利|BME 布鲁姆|KTA 卡拉萨|PHE 黑德兰港|WLG 惠灵顿|ZQN 皇后镇|DUD 但尼丁|POM 莫尔兹比港|NOU 努美阿|PPT 帕皮提
"""


_STRIP = ("大都会区", "都会区", "大都会", "国际机场", "機場", "机场", "市", "府", "省", "县", "区")


def _short(name):
    """维基数据里的名字整理成屏幕上放得下的短名: 繁转简, 去掉"市""省""机场"这类尾巴, 最多 5 个字。"""
    name = _cc.convert(name or "").replace("·", "").replace("－", "").replace("-", "").strip()
    for _ in range(2):
        for t in _STRIP:
            if name.endswith(t) and len(name) > len(t) + 1:
                name = name[: -len(t)]
    if name.endswith("都") and len(name) == 3:  # 东京都 -> 东京
        name = name[:2]
    ok = name and len(name) <= 5 and all("\u4e00" <= ch <= "\u9fff" for ch in name)
    return name if ok else ""


def hand_names():
    """手写表: 三字码 -> 中文名。同城多机场的叫法(东京成田/东京羽田、北京/北京大兴)在这里定。"""
    out = {}
    for item in AIRPORTS.replace("\n", "|").split("|"):
        item = item.strip()
        if item:
            code, name = item.split(None, 1)
            out[code] = name
    return out


_FOLD = str.maketrans({"ł": "l", "Ł": "L", "ø": "o", "Ø": "O", "đ": "d", "Đ": "D", "ð": "d", "þ": "th",
                       "æ": "ae", "Æ": "Ae", "œ": "oe", "ß": "ss", "ı": "i"})


def fold(t):
    """São Paulo -> Sao Paulo, Łódź -> Lodz: 屏幕的英文字库只有 ASCII"""
    t = unicodedata.normalize("NFKD", (t or "").translate(_FOLD))
    return "".join(ch for ch in t if not unicodedata.combining(ch)).encode("ascii", "ignore").decode()


def _ourairports():
    """机场数据库(OurAirports): 三字码 -> (所在地, 机场名, 关键词)"""
    out = {}
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "ourairports", "airports.csv")
    if os.path.exists(path):
        for r in csv.DictReader(open(path, encoding="utf-8")):
            code = (r["iata_code"] or "").strip().upper()
            if len(code) == 3 and code not in out:
                out[code] = (r["municipality"] or "", r["name"] or "", r["keywords"] or "")
    return out


def _wiki():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "wikidata", "zh_names.json")
    return json.load(open(path, encoding="utf-8")) if os.path.exists(path) else {}


def city_pick():
    """三字码 -> 维基数据里通过校验的服务城市 {zh, en, th}。
    维基数据的"服务城市"很乱(贝伦机场挂成了"葡式蛋挞", 长滩岛机场挂成了美国的 Long Beach),
    所以只认英文名出现在机场数据库的所在地/机场名里的那个; 一个都对不上就不用。"""
    ap, out = _ourairports(), {}
    for code, e in _wiki().items():
        muni, name, kw = ap.get(code, ("", "", ""))
        hay = fold(" ".join((muni, name, kw))).lower()
        fm = fold(muni.split("(")[0].split(",")[0]).strip().lower()
        best, score = None, None
        for c in e.get("cities", []):
            en = fold(c.get("en", "").split("(")[0].split(",")[0]).strip().lower()
            if not en:
                continue
            # 和所在地一致的最好; 其次出现在机场名里的(越靠前越好: Minneapolis-Saint Paul 取 Minneapolis)
            if en == fm or (len(fm) > 2 and en.startswith(fm)):
                sc = (0, 0)
            elif en in hay:
                sc = (1, hay.find(en))
            else:
                continue
            sc = sc + (0 if c.get("zh") else 1,)
            if score is None or sc < score:
                best, score = c, sc
        if best:
            out[code] = best
    return out


def all_names():
    """三字码 -> 中文短名。手写表优先; 其余用通过校验的服务城市的中文名, 没有就用机场自己的中文名。
    城市名和别的机场撞了(比如墨尔本旁边的阿瓦隆机场也叫"墨尔本"), 改用机场自己的名字("阿瓦隆")。"""
    hand = hand_names()
    wiki, pick = _wiki(), city_pick()
    by_city = {code: _short((pick.get(code) or {}).get("zh")) for code in wiki if code not in hand}
    by_ap = {code: _short(v.get("ap")) for code, v in wiki.items() if code not in hand}
    count = {}
    for n in list(by_city.values()) + list(hand.values()):
        if n:
            count[n] = count.get(n, 0) + 1
    names = {}
    for code in by_city:
        n = by_city[code] or by_ap[code]   # 城市名放不下就用机场名
        if n and count.get(n, 0) > 1:
            ap = by_ap[code]
            if ap and ap != n:
                n = ap                     # 阿瓦隆
            elif len(n) <= 3:
                n = n + code               # 伦敦STN: 机场名太长放不下, 就城市名带三字码
            else:
                n = ""                     # 实在放不下: 不给中文名, 屏幕上显示英文
        if n:
            names[code] = n
    names.update(hand)
    return names


# 屏幕界面的英文、泰文版 (中文在 STRINGS 里)。切换语种时整屏的字都跟着换。
# 泰文用 TIS-620 单字节编码(屏幕的泰文字库按它查), 只有 TFT 彩屏能显示, 串口屏用英文代替。
TR = {
    "S_DEP": ("Departures", "ขาออก"),
    "S_ARR": ("Arrivals", "ขาเข้า"),
    "S_C_TIME": ("Time", "เวลา"),
    "S_C_FLIGHT": ("Flight", "เที่ยวบิน"),
    "S_C_DEST": ("To", "ปลายทาง"),
    "S_C_ORIG": ("From", "ต้นทาง"),
    "S_C_STATUS": ("Status", "สถานะ"),
    "S_ST_SCHED": ("Scheduled", "ตามกำหนด"),
    "S_ST_BOARD": ("Boarding", "ขึ้นเครื่อง"),
    "S_ST_DEPARTED": ("Departed", "ออกแล้ว"),
    "S_ST_ARRIVED": ("Landed", "ถึงแล้ว"),
    "S_ST_DELAY": ("Delayed", "ล่าช้า"),
    "S_ST_EST": ("Est", "คาดว่า"),
    "S_ST_CANCEL": ("Cancelled", "ยกเลิก"),
    "S_ST_UNKNOWN": ("--", "--"),
    "S_UPDATED": ("Updated", "อัปเดต"),
    "S_STALE": ("Stale", "ข้อมูลเก่า"),
    "S_WK0": ("Sun", "อา."), "S_WK1": ("Mon", "จ."), "S_WK2": ("Tue", "อ."), "S_WK3": ("Wed", "พ."),
    "S_WK4": ("Thu", "พฤ."), "S_WK5": ("Fri", "ศ."), "S_WK6": ("Sat", "ส."),
    "S_NOFLIGHT": ("No flights", "ไม่มีเที่ยวบิน"),
    "S_W_NOWIFI": ("WiFi not set up", "ยังไม่ได้ตั้งค่า WiFi"),
    "S_W_CONN": ("Connecting WiFi", "กำลังเชื่อมต่อ WiFi"),
    "S_W_FETCH": ("Loading flights", "กำลังโหลดเที่ยวบิน"),
    "S_W_QUOTA": ("Quota used up", "โควตาหมดแล้ว"),
    "S_W_QUOTA2": ("Will resume later", "จะกลับมาเองภายหลัง"),
    "S_W_AP": ("Join this WiFi on your phone", "ใช้มือถือเชื่อมต่อ WiFi นี้"),
    "S_W_AP2": ("then open 192.168.4.1", "แล้วเปิด 192.168.4.1"),
    "S_W_NOKEY": ("No data source key", "ยังไม่มีคีย์แหล่งข้อมูล"),
    "S_W_NOKEY2": ("Add it on the setup page", "ใส่ในหน้าตั้งค่า"),
    "S_W_NET": ("No network, retrying", "ไม่มีเครือข่าย กำลังลองใหม่"),
    "S_W_HTTP": ("Source error, retrying", "แหล่งข้อมูลขัดข้อง กำลังลองใหม่"),
    "S_W_NOFL": ("No flights right now", "ยังไม่มีเที่ยวบิน"),
    "S_W_TAP": ("Tap screen for settings", "แตะหน้าจอเพื่อตั้งค่า"),
    "S_W_TAP_APT": ("Tap screen to change airport", "แตะหน้าจอเพื่อเปลี่ยนสนามบิน"),
}


def c_bytes(b: bytes) -> str:
    out = []
    for x in b:
        out.append(chr(x) if 0x20 <= x < 0x7F and chr(x) not in '"\\?' else "\\%03o" % x)
    return '"' + "".join(out) + '"'


def c_str(text: str) -> str:
    """GBK 字节 -> C 字符串字面量。非 ASCII 一律用 3 位八进制转义, 不会和后面的字符粘连。"""
    out = []
    for b in text.encode("gbk"):
        if 0x20 <= b < 0x7F and chr(b) not in '"\\?':
            out.append(chr(b))
        else:
            out.append("\\%03o" % b)
    return '"' + "".join(out) + '"'


def names_table():
    """英文/泰文地名表: 板子脱离电脑、自己走 WiFi 取数时用(规则和 host.py 的 place() 一样)。
    数据来自 make_airports.py 生成的 flightboard_host/airports.json。"""
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "flightboard_host", "airports.json")
    with open(src, encoding="utf-8") as f:
        rows = json.load(f)

    def ascii_clean(s, n):
        return "".join(ch for ch in str(s or "") if 32 <= ord(ch) < 127 and ch not in ",';\"\\")[:n]

    out = ["", "// 英文 / 泰文(TIS-620) 地名, 按三字码排序。同城多机场已带区分(Tokyo Haneda)。",
           "struct FbName { char iata[4]; const char *en; const char *th; };",
           "static const FbName FB_NAMES[] = {"]
    for r in sorted(rows, key=lambda r: r[0]):
        iata = r[0]
        if len(iata) != 3 or not iata.isalnum():
            continue
        en = ascii_clean(r[7] if len(r) > 7 and r[7] else r[1], 14)
        th = "".join(ch for ch in (r[8] if len(r) > 8 and r[8] else "") if ch not in ",;'")
        thb = th.encode("tis-620", errors="ignore")[:20]
        out.append(f'  {{"{iata}", {c_bytes(en.encode())}, '
                   f'{c_bytes(thb) if thb else "nullptr"}}},')
    out += [
        "};",
        "",
        "// lang: 1 英文 / 2 泰文(没有泰文名就给英文)。查不到返回 nullptr",
        "static inline const char *fbPlace(const char *iata, int lang) {",
        "  int lo = 0, hi = (int)(sizeof(FB_NAMES) / sizeof(FB_NAMES[0])) - 1;",
        "  while (lo <= hi) {",
        "    int mid = (lo + hi) / 2;",
        "    int c = strcmp(iata, FB_NAMES[mid].iata);",
        "    if (c == 0) return lang == 2 && FB_NAMES[mid].th ? FB_NAMES[mid].th : FB_NAMES[mid].en;",
        "    if (c < 0) hi = mid - 1; else lo = mid + 1;",
        "  }",
        "  return nullptr;",
        "}",
        "",
    ]
    return out


def region_table():
    """触摸屏选机场用: 洲 -> 国家 -> 机场。分组同电脑端 countries.json, 国家按机场数排, 机场按规模排。"""
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "flightboard_host")
    groups = json.load(open(os.path.join(here, "countries.json"), encoding="utf-8"))
    apts = json.load(open(os.path.join(here, "airports.json"), encoding="utf-8"))
    by_cc = {}
    for a in apts:
        by_cc.setdefault(a[3], []).append(a)
    order, countries = [], []
    for gi, (_, cs) in enumerate(groups):
        cs = sorted(cs, key=lambda c: -len(by_cc.get(c[0], [])))  # 机场多的国家排前面
        for cc, names in cs:
            lst = sorted(by_cc.get(cc, []), key=lambda a: (a[6], a[7] or a[1]))
            if not lst:
                continue
            countries.append((gi, names["zh"], len(order), len(lst)))
            order += [a[0] for a in lst]
    out = ["", "// 触摸屏选机场: 洲 -> 国家 -> 机场(按规模排)",
           "static const char *const FB_REGION[] = {%s};" % ", ".join(c_str(g["zh"]) for g, _ in groups),
           "#define FB_REGION_N %d" % len(groups),
           "struct FbCountry { uint8_t region; const char *name; uint16_t first, n; };",
           "static const FbCountry FB_COUNTRY[] = {"]
    for gi, zh, first, n in countries:
        out.append("  {%d, %s, %d, %d},  // %s" % (gi, c_str(zh), first, n, zh))
    out += ["};", "#define FB_COUNTRY_N %d" % len(countries), "static const char FB_APT_ORDER[][4] = {"]
    for i in range(0, len(order), 16):
        out.append("  " + ", ".join('"%s"' % c for c in order[i:i + 16]) + ",")
    out += ["};", ""]
    return out


def main():
    for item in AIRPORTS.replace("\n", "|").split("|"):
        item = item.strip()
        if item:
            iata, name = item.split(None, 1)
            assert len(iata) == 3, item
            assert len(name) <= 5, f"{item}: 最多 5 个汉字"
    seen = {}
    for iata, name in all_names().items():
        try:
            name.encode("gbk")
        except UnicodeEncodeError:
            continue  # GBK 里没有的生僻字, 屏幕显示不了, 退回英文
        seen[iata] = name

    lines = [
        "// 由 tools/make_fb_gbk.py 生成, 不要手改。内容是 GBK 编码(串口屏只认 GBK)。",
        "#pragma once",
        "#include <string.h>",
        "",
    ]
    for k, v in STRINGS.items():
        lines.append(f"static const char {k}[] = {c_str(v)};  // {v}")
    lines += ["", "// 三种语种: [0] 中文(GBK) [1] 英文 [2] 泰文(TIS-620)。用法: T(S_DEP)"]
    for k, (en, th) in TR.items():
        zh = k if k in STRINGS else c_str(en)
        lines.append(f"static const char *const {k}_L[3] = {{{zh}, {c_bytes(en.encode())}, "
                     f"{c_bytes(th.encode('tis-620'))}}};  // {en} / {th}")
    lines += ["", "struct FbAirport { char iata[4]; const char *name; };",
              "static const FbAirport FB_AIRPORTS[] = {"]
    for iata in sorted(seen):
        lines.append(f'  {{"{iata}", {c_str(seen[iata])}}},  // {seen[iata]}')
    lines += [
        "};",
        "",
        "// 查不到返回 nullptr, 调用方退回英文城市名",
        "static inline const char *fbCityGbk(const char *iata) {",
        "  int lo = 0, hi = (int)(sizeof(FB_AIRPORTS) / sizeof(FB_AIRPORTS[0])) - 1;",
        "  while (lo <= hi) {",
        "    int mid = (lo + hi) / 2;",
        "    int c = strcmp(iata, FB_AIRPORTS[mid].iata);",
        "    if (c == 0) return FB_AIRPORTS[mid].name;",
        "    if (c < 0) hi = mid - 1; else lo = mid + 1;",
        "  }",
        "  return nullptr;",
        "}",
        "",
    ]
    lines += names_table()
    lines += region_table()
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "include", "fb_gbk.h")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(f"wrote {os.path.normpath(path)}: {len(STRINGS)} strings, {len(seen)} airports")


if __name__ == "__main__":
    main()
