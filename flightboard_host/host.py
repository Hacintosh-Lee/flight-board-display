#!/usr/bin/env python3
"""航班牌电脑端: 联网取机场航班, 经 USB 推给屏幕。

机场: 以电脑上选的为准(记在 airport.txt): 板子插上时, 它里面存的机场会被改成电脑上的。
在串口屏上触摸换机场时, 电脑跟着换。

    host.py            常驻: 每 140 分钟取数, 每分钟推一次(顺带对时)
    host.py once       取一次、推一次就退出
    host.py status     问屏幕当前状态
    host.py wifi       给屏幕配 WiFi(密码只在本机输入, 直接写进屏幕)
    host.py wifi-clear 清掉屏幕里存的 WiFi

数据源: AirLabs(https://airlabs.co, 免费档每月 1000 次, 请自己申请密钥)。
密钥放在同目录 airlabs_key.txt 里一行, 或环境变量 AIRLABS_KEY。

端口自动找: 向每个没被别的程序占用的 usbmodem 口发 "$?", 回 "$ID,flightboard" 的就是。
也可以用环境变量 FLIGHTBOARD_PORT 指定。
"""
import getpass
import json
import queue
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import re
import os
import subprocess
import sys
import time
import urllib.request
from datetime import datetime, timezone

import serial

# 系统设了代理(比如 VPN 软件)时: Python 在 macOS 上每次请求前要判断"这个地址要不要绕过代理",
# 判断时会先查一次 DNS; 这台 Mac 直连的 DNS 不通, 每个请求都白等 30 秒才走代理。
# 把系统代理放进环境变量, 判断就改成只看 no_proxy 列表, 不再查 DNS。
if not any(k.lower() in ("http_proxy", "https_proxy") for k in os.environ):
    for _k, _v in urllib.request.getproxies().items():
        if _k in ("http", "https"):
            os.environ[_k + "_proxy"] = _v
    os.environ.setdefault("no_proxy", "localhost,127.0.0.1,::1")
import serial.tools.list_ports

def _airport():
    """机场三字码: 环境变量 FLIGHTBOARD_AIRPORT > 同目录 airport.txt > 默认广州 CAN"""
    here = os.path.dirname(os.path.abspath(__file__))
    v = os.environ.get("FLIGHTBOARD_AIRPORT", "").strip()
    f = os.path.join(here, "airport.txt")
    if not v and os.path.exists(f):
        v = open(f).read().strip()
    v = (v or "CAN").upper()
    return v if len(v) == 3 and v.isalpha() else "CAN"


AIRPORT = _airport()
UA = "flightboard-host/1.0"
PUSH_EVERY = 60
MAX_FLIGHTS = 48


AIRLABS_API = "https://airlabs.co/api/v9/schedules"
_HERE = os.path.dirname(os.path.abspath(__file__))
SOURCES_FILE = os.path.join(_HERE, "sources.json")   # 各机场的时区("tz:MEL": 660), 从航班数据里算出来的
CACHE_FILE = os.path.join(_HERE, "cache.json")       # 上次取到的数据, 重启后不用重新取(省额度)


def airlabs_key():
    """AirLabs 密钥: airlabs_key.txt(不进 git) 或环境变量 AIRLABS_KEY。没有就取不了数。"""
    k = os.environ.get("AIRLABS_KEY", "").strip()
    f = os.path.join(_HERE, "airlabs_key.txt")
    if not k and os.path.exists(f):
        k = open(f).read().strip()
    return k


def load_json(path, default):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def save_json(path, obj):
    try:
        with open(path, "w") as f:
            json.dump(obj, f)
    except OSError:
        pass


SOURCES = load_json(SOURCES_FILE, {})


# ---------------- 控制页面用的共享状态 ----------------
UI_PORT = 8377                      # 只听本机: http://127.0.0.1:8377
CMDQ = queue.Queue()                # 页面发来的操作, 主循环醒来就处理
SETTINGS_FILE = os.path.join(_HERE, "settings.json")
SETTINGS = dict({"lang": "zh"}, **load_json(SETTINGS_FILE, {}))   # lang: 屏幕和页面的语种 zh / en / th
LANGS = ("zh", "en", "th")
BOARD = {"tft": False, "th": False}  # 连着的屏幕: 是不是 TFT 彩屏 / 能不能显示泰文
# status: offline 屏幕没连 / loading 正在取数 / ok 有数据 / error 取数失败(error 里是原因)
STATE = {"connected": False, "status": "offline", "error": "", "dep": 0, "arr": 0, "updated": 0}


def _airport_db():
    """世界机场清单(tools/make_airports.py 生成): 三字码 -> [码, 城市, 机场名, 国家码, 国家, 中文名, 规模]"""
    return {a[0]: a for a in load_json(os.path.join(_HERE, "airports.json"), [])}


AIRPORT_DB = _airport_db()


def city_en(code, n=14):
    """屏幕上的英文地名(tools/make_airports.py 算好的, 同城多机场已区分: Tokyo Narita / Tokyo Haneda)"""
    a = AIRPORT_DB.get(code)
    return clean(a[7] if a and len(a) > 7 else (a[1] if a else ""), n)


def screen_lang():
    """屏幕实际用的语种: 串口屏没有泰文字库, 泰文退回英文"""
    lang = SETTINGS["lang"]
    return "en" if lang == "th" and not BOARD["th"] else lang


def place(code):
    """发给屏幕的地名。中文模式下屏幕用自己存的中文名, 这里给英文兜底; 泰文模式给泰文名, 没有就英文。"""
    a = AIRPORT_DB.get(code)
    if screen_lang() == "th" and a and len(a) > 8 and a[8]:
        return "".join(ch for ch in a[8] if ch not in ",;'")
    return city_en(code)


def log(*a):
    line = " ".join(str(x) for x in a).replace(airlabs_key() or "\0", "***")
    print(time.strftime("[%m-%d %H:%M:%S]"), line, flush=True)


# ---------------- 取数 ----------------
def clean(s, n):
    s = "".join(ch for ch in str(s or "") if 32 <= ord(ch) < 127 and ch not in ",';")
    return s[:n]


def pick(rows):
    """rows: [(计划时间, 状态码, 记录行)]。屏幕只存 48 条: 优先留还没过点的。"""
    rows.sort()
    now = time.time()
    # 延误的过了原定时间也还没走, 刚取消的留一小时 (和固件的显示规则一致)
    def live(r):
        return (r[0] >= now - 600 or (r[1] == "L" and r[0] >= now - 4 * 3600)
                or (r[1] == "C" and r[0] >= now - 3600))
    upcoming = [r for r in rows if live(r)]
    keep = upcoming[:MAX_FLIGHTS] if upcoming else rows[-MAX_FLIGHTS:]
    return [r[2] for r in keep]


def utc_epoch(s):
    """'2026-11-17 08:05'(UTC) -> 秒数; 没有就 0"""
    try:
        return int(datetime.strptime(s, "%Y-%m-%d %H:%M").replace(tzinfo=timezone.utc).timestamp())
    except (TypeError, ValueError):
        return 0


def fetch_airlabs(kind):
    """AirLabs: 全球机场都有, 但免费档每月只有 1000 次, 所以更新得慢(见 fetch_every)。"""
    if not airlabs_key():
        raise RuntimeError("没有 AirLabs 密钥: 到 https://airlabs.co 免费申请, 放进 airlabs_key.txt")
    me, other = ("dep", "arr") if kind == "D" else ("arr", "dep")
    # 这家的密钥只能放在网址参数里
    # 免费档一次最多 100 行、不能翻页, 而且不加条件时是从两小时前排起, 还没到的航班排不进来。
    # 所以按状态要: 出发只要还没走的(scheduled); 到达要还没起飞的 + 已经在天上的(active)。
    flights = []
    for status in (("scheduled",) if kind == "D" else ("scheduled", "active")):
        # _fields: 只要用得到的字段, 响应小好几倍(加了它就不再给 *_ts 时间戳, 时间从 UTC 字符串算)
        fields = ",".join(["flight_iata", "cs_flight_iata", "status", other + "_iata"] +
                          [me + k for k in ("_delayed", "_terminal", "_time", "_time_utc", "_estimated_utc")])
        url = "%s?%s_iata=%s&status=%s&_fields=%s&api_key=%s" % (AIRLABS_API, me, AIRPORT, status, fields, airlabs_key())
        req = urllib.request.Request(url, headers={"User-Agent": UA, "Accept": "application/json"})
        with urllib.request.urlopen(req, timeout=30) as r:
            data = json.load(r)
        if data.get("error"):
            raise RuntimeError("AirLabs: %s" % (data["error"].get("message") or data["error"]))
        flights += data.get("response") or []
    # 顺手记下机场的时区: 每条航班同时给了当地时间和 UTC 时间, 一减就是(夏令时也自动对)
    for f in flights:
        try:
            loc = datetime.strptime(f[me + "_time"], "%Y-%m-%d %H:%M")
            utc = datetime.strptime(f[me + "_time_utc"], "%Y-%m-%d %H:%M")
        except (KeyError, TypeError, ValueError):
            continue
        tz = int((loc - utc).total_seconds() // 60)
        if SOURCES.get("tz:" + AIRPORT) != tz:
            SOURCES["tz:" + AIRPORT] = tz
            save_json(SOURCES_FILE, SOURCES)
        break
    rows = []
    for f in flights:
        if f.get("cs_flight_iata"):
            continue  # 代码共享的重复行: 同一架飞机挂了别家的航班号, 只留实际承运的那条
        sched = utc_epoch(f.get(me + "_time_utc"))
        if not sched or not f.get(other + "_iata") or not f.get("flight_iata"):
            continue
        est = utc_epoch(f.get(me + "_estimated_utc"))
        late = int(f.get(me + "_delayed") or 0)
        raw = (f.get("status") or "").lower()
        if raw == "cancelled":
            st = "C"
        elif raw == "landed":
            st = "A" if kind == "A" else "D"
        elif raw == "active":           # 已经在天上
            st = "D" if kind == "D" else ("L" if late >= 15 else "S")
        else:
            st = "L" if late >= 15 else "S"
        rows.append((sched, st, "$F,%s,%d,%d,%s,%s,%s,%s,%s" % (
            kind, sched, est if est != sched else 0,
            clean(f.get("flight_iata"), 8), clean(f.get(other + "_iata"), 3),
            clean(f.get(me + "_terminal"), 2), st, city_en(f.get(other + "_iata")))))
    # 离现在远的航班, AirLabs 不标代码共享, 同一班飞机会以好几家的航班号各出现一次
    # (比如澳航飞布里斯班的一班, 还会以阿联酋 EK5073、斐济 FJ5403 的名义再列一遍)。
    # 挂名的航班号一般是四位数: 同一时刻、同一目的地里只要有三位以内的号, 就把四位数的去掉。
    def num(line):
        m = re.search(r"(\d+)$", line.split(",")[4])
        return int(m.group(1)) if m else 0
    groups = {}
    for r in rows:
        f = r[2].split(",")
        groups.setdefault((r[0], f[5]), []).append(r)
    rows = []
    for g in groups.values():
        real = [r for r in g if num(r[2]) < 1000]
        rows += real or g
    if not rows:
        raise RuntimeError("AirLabs 返回 0 条")
    return int(time.time()), pick(rows)


def fetch_every():
    # AirLabs 免费档 1000 次/月, 一轮三次(出发 1 + 到达 2): 140 分钟一轮 ≈ 940 次/月, 留点给出错重试
    return 140 * 60


def fetch(kind):
    """kind: 'D' 出发 / 'A' 到达。返回 (抓取时间 epoch, [记录行...])"""
    return fetch_airlabs(kind)


# ---------------- 串口 ----------------
def port_busy(dev):
    """别的程序(比如监控屏的采集端)正开着的口不去碰, 免得往人家协议里塞垃圾。"""
    try:
        out = subprocess.run(["lsof", dev], capture_output=True, text=True, timeout=5).stdout
        return bool(out.strip())
    except Exception:
        return False


def ask(ser, line, prefix, timeout=2.0):
    ser.reset_input_buffer()
    ser.write((line + "\n").encode())
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        buf += ser.read(256)
        for ln in buf.decode(errors="replace").splitlines():
            if ln.startswith(prefix):
                return ln
    return None


# 问过、确认不是航班牌的板子(按 USB 序列号即 MAC 记): 拔掉之前不再去碰。
# 这种板子每打开一次串口就可能被复位一次, 每 5 秒探一回的话它就没法正常跑了(比如电波水族箱)。
NOT_BOARDS = set()
# 认出过的航班牌(MAC)记在 boards.json: 这些板子刚上电时可能正忙着自己联网取数, 一时不回话,
# 不能因此当成"不是航班牌"拉黑, 只是下一轮再问
KNOWN_FILE = os.path.join(_HERE, "boards.json")


def known_boards():
    return set(load_json(KNOWN_FILE, []))


def remember_board(sn):
    if sn and sn not in known_boards():
        save_json(KNOWN_FILE, sorted(known_boards() | {sn}))


def open_board():
    forced = os.environ.get("FLIGHTBOARD_PORT")
    ports = [p for p in serial.tools.list_ports.comports() if "usbmodem" in p.device]
    present = {p.serial_number for p in ports if p.serial_number}
    NOT_BOARDS.intersection_update(present)  # 拔掉的就忘掉, 下次插上重新认
    sn = {p.device: p.serial_number for p in ports}
    cands = [forced] if forced else [p.device for p in ports if p.serial_number not in NOT_BOARDS]
    for dev in cands:
        if not forced and port_busy(dev):
            continue
        try:
            ser = serial.Serial(dev, 115200, timeout=0.2, write_timeout=3)
        except (serial.SerialException, OSError):
            continue
        time.sleep(0.3)
        for _ in range(3):  # 刚上电时板子可能还在启动
            ident = ask(ser, "$?", "$ID,flightboard", 1.5)
            if ident:
                remember_board(sn.get(dev))
                return ser, dev, ident
        ser.close()
        if not forced and sn.get(dev) and sn[dev] not in known_boards():
            NOT_BOARDS.add(sn[dev])
            log("%s 不是航班牌(%s), 拔掉之前不再探测" % (dev, sn[dev]))
    return None, None, None


def set_airport(code):
    """换机场并记到 airport.txt。返回是否真的变了。"""
    global AIRPORT
    code = (code or "").strip().upper()
    if len(code) != 3 or not code.isalpha() or code == AIRPORT:
        return False
    AIRPORT = code
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "airport.txt"), "w") as f:
        f.write(code + "\n")
    return True


def board_airport(ident):
    for part in (ident or "").split(","):
        if part.startswith("apt="):
            return part[4:].strip()
    return ""


def wait_events(ser, secs):
    """两次推送之间听着串口: 屏幕上触摸换了机场会发 $AP,<三字码>, 返回它; 否则到点返回 None"""
    end = time.time() + secs
    buf = b""
    while time.time() < end and CMDQ.empty():  # 控制页面有操作就马上回去处理
        buf += ser.read(256)
        while b"\n" in buf:
            ln, buf = buf.split(b"\n", 1)
            ln = ln.decode(errors="replace").strip()
            if ln.startswith("$AP,"):
                return ln[4:]
    return None


# ---------------- 控制页面 (本机网页, 也是"航班牌"App 里显示的内容) ----------------
class UI(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="application/json; charset=utf-8"):
        if not isinstance(body, bytes):
            body = json.dumps(body, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]
        static = {"/": ("ui.html", "text/html; charset=utf-8"), "/airports.json": ("airports.json", None),
                  "/countries.json": ("countries.json", None)}
        if path in static:
            name, ctype = static[path]
            try:
                self._send(200, open(os.path.join(_HERE, name), "rb").read(), ctype or "application/json; charset=utf-8")
            except OSError:
                self._send(404, {"error": name + " missing"})
        elif path == "/api/state":
            a = AIRPORT_DB.get(AIRPORT)
            self._send(200, dict(STATE, airport=AIRPORT, zh=a[5] if a else "", city=a[1] if a else "",
                                 country=a[4] if a else "", en=a[7] if a and len(a) > 7 else "",
                                 th=a[8] if a and len(a) > 8 else "", lang=SETTINGS["lang"]))
        else:
            self._send(404, {"error": "not found"})

    def do_POST(self):
        # 只接受本页面发来的请求: 别的网站的脚本带不了这个自定义头
        if self.headers.get("X-Flightboard") != "1" or self.headers.get("Host", "").split(":")[0] not in ("127.0.0.1", "localhost"):
            return self._send(403, {"error": "forbidden"})
        try:
            body = json.loads(self.rfile.read(int(self.headers.get("Content-Length") or 0)) or b"{}")
        except ValueError:
            body = {}
        if self.path == "/api/airport":
            code = str(body.get("code", "")).strip().upper()
            if len(code) != 3 or not code.isalpha():
                return self._send(400, {"error": "bad code"})
            NOT_BOARDS.clear()  # 用户在操作: 可能刚把别的板子刷成了航班牌, 重新认一遍
            CMDQ.put(code)
            self._send(200, {"ok": True})
        elif self.path == "/api/lang":
            lang = body.get("lang")
            if lang not in LANGS:
                return self._send(400, {"error": "lang: zh / en / th"})
            SETTINGS["lang"] = lang
            save_json(SETTINGS_FILE, SETTINGS)
            CMDQ.put(None)  # 只是叫醒主循环, 马上推一次
            self._send(200, {"ok": True})
        else:
            self._send(404, {"error": "not found"})


def start_ui():
    try:
        srv = ThreadingHTTPServer(("127.0.0.1", UI_PORT), UI)
    except OSError as e:
        log("控制页面起不来(端口 %d 被占?): %s" % (UI_PORT, e))
        return
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    log("控制页面: http://127.0.0.1:%d" % UI_PORT)


def encode(line):
    """泰文按 TIS-620 单字节发(屏幕的泰文字库按它查); 其余都是 ASCII"""
    return line.encode("tis-620" if screen_lang() == "th" else "ascii", errors="replace")


def push(ser, cache):
    # 对时, 顺带告诉屏幕机场的时区(东经为正的分钟数), 屏上的时间都按机场当地时间显示。
    # 时区是从 AirLabs 的数据里算出来的; 还没取到过数据的机场先按北京时间。
    ser.write(("$T,%d,%d\n" % (int(time.time()), SOURCES.get("tz:" + AIRPORT, 480))).encode())
    ser.write(("$C,%d\n" % LANGS.index(screen_lang())).encode())  # 语种: 0 中 1 英 2 泰
    # 顶栏的地名后面本来就跟着三字码, 去掉名字里为区分同城机场加的三字码("东京 HND" -> "东京")
    ser.write(encode("$L,%s\n" % re.sub(r"\s+%s$" % AIRPORT, "", place(AIRPORT))))  # 没有中文名的机场, 顶栏显示这个英文城市名
    if airlabs_key():  # 拔掉电脑后板子自己走 WiFi 取 AirLabs 用
        ser.write(("$J,%s\n" % airlabs_key()).encode())
    for kind, (scraped, lines) in cache.items():
        if not lines:
            continue
        ser.write(("$B,%s,%d\n" % (kind, scraped)).encode())
        for ln in lines:
            # 最后一段是英文地名: 按三字码现查, 改了命名规则不用重新取数就生效
            f = ln.split(",", 8)
            if len(f) == 9 and place(f[5]):
                f[8] = place(f[5])
                ln = ",".join(f)
            ser.write(encode(ln + "\n"))
            time.sleep(0.004)
        ack = ask(ser, "$E,%s" % kind, "$OK,%s" % kind, 10.0)  # 屏幕正在画页面时要等它画完
        if not ack:
            raise serial.SerialException("屏幕没应答")


def refresh(cache):
    """取出发和到达。返回是否至少有一边成功。"""
    STATE.update(status="loading", error="")
    ok, err = False, ""
    for kind in ("D", "A"):
        try:
            scraped, lines = fetch(kind)
            cache[kind] = (scraped, lines)
            ok = True
            log("取数 %s: %d 条" % (kind, len(lines)))
        except Exception as e:  # 网络抖动不该让常驻进程退出, 沿用旧数据
            err = str(e).replace(airlabs_key() or "\0", "***")
            log("取数 %s 失败: %s" % (kind, err))
    if ok:
        save_json(CACHE_FILE, {"airport": AIRPORT, "time": time.time(), "data": cache})
    STATE["error"] = "" if ok else err
    return ok


def load_cache():
    """重启后接着用上次取的数据, 没过期就不重新取。返回 (cache, 取数时间)。"""
    c = load_json(CACHE_FILE, {})
    if c.get("airport") != AIRPORT or not c.get("data") or time.time() - c.get("time", 0) > fetch_every():
        return {}, 0
    return {k: (v[0], v[1]) for k, v in c.get("data", {}).items()}, c["time"]


def sync_state(ser, cache, last_fetch):
    n = {k: len(v[1]) for k, v in cache.items()}
    STATE.update(connected=ser is not None, dep=n.get("D", 0), arr=n.get("A", 0), updated=last_fetch)
    if ser is None:
        STATE["status"] = "offline"
    elif STATE["status"] != "loading":
        STATE["status"] = "ok" if n else "error"


def run(once=False):
    cache, last_fetch = ({}, 0) if once else load_cache()
    ser = None
    want = None  # 控制页面选了机场、但还没来得及告诉屏幕的
    if not once:
        start_ui()
    while True:
        sync_state(ser, cache, last_fetch)
        while not CMDQ.empty():
            want = CMDQ.get() or want   # None = 只是叫醒(改了设置)
        if want and ser is None and set_airport(want):
            # 屏幕没插着也先记下来, 页面马上显示新机场; want 留着, 等屏幕接上再告诉它
            log("换机场(等屏幕接上再发): %s %s" % (AIRPORT, city_en(AIRPORT, 30)))
            cache, last_fetch = load_cache()
        try:
            if ser is None:
                ser, dev, ident = open_board()
                if ser is None:
                    if once:
                        log("没找到航班牌")
                        return 1
                    for _ in range(25):  # 等 5 秒再找, 期间页面有操作就提前醒
                        if not CMDQ.empty():
                            break
                        time.sleep(0.2)
                    continue
                log("已连接 %s  %s" % (dev, ident))
                BOARD["tft"] = ",tft=1" in ident
                BOARD["th"] = BOARD["tft"] or ",th=1" in ident  # 能不能显示泰文(旧串口屏固件不能)
                # 机场以电脑为准: 板子里存的(可能是很久以前的)机场和电脑上的不一样, 就改成电脑上的。
                # 几块板子轮流插拔时, 不会因为某块板子存着旧机场把电脑的设置改掉、白扣额度
                was = board_airport(ident)
                if not want and was and was != AIRPORT:
                    want = AIRPORT
                    log("屏幕里存的是 %s, 改成电脑上的 %s" % (was, AIRPORT))
            if want:
                ser.write(("$A,%s\n" % want).encode())
                if set_airport(want):
                    log("换机场: %s %s" % (AIRPORT, city_en(AIRPORT, 30)))
                    cache, last_fetch = load_cache()
                want = None
            # 连上屏幕、确认了机场之后才取数: 屏幕没插着就不白扣额度
            if time.time() - last_fetch >= fetch_every():
                sync_state(ser, cache, last_fetch)
                ok = refresh(cache)
                # 两边都失败: 两分钟后再试, 不等满一轮
                last_fetch = time.time() if ok else time.time() - fetch_every() + 120
                STATE["status"] = "ok" if cache else "error"
            push(ser, cache)
            if once:
                log("已推送")
                return 0
        except (serial.SerialException, OSError) as e:
            log("串口断开: %s" % e)
            try:
                ser.close()
            except Exception:
                pass
            ser = None
            if once:
                return 1
            time.sleep(3)
            continue
        sync_state(ser, cache, last_fetch)
        try:
            apt = wait_events(ser, PUSH_EVERY)
        except (serial.SerialException, OSError):
            apt = None
        if apt and set_airport(apt):
            log("屏幕上换了机场: %s" % AIRPORT)
            cache.clear()
            last_fetch = 0


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "run"
    if cmd in ("run", "once"):
        return run(once=cmd == "once")
    if cmd == "airport" and len(sys.argv) > 2:
        # 后台程序在跑(它占着串口)就交给它; 没在跑才自己开串口
        try:
            req = urllib.request.Request("http://127.0.0.1:%d/api/airport" % UI_PORT,
                                         data=json.dumps({"code": sys.argv[2]}).encode(),
                                         headers={"Content-Type": "application/json", "X-Flightboard": "1"})
            urllib.request.urlopen(req, timeout=3)
            print("已换成", sys.argv[2].upper())
            return 0
        except OSError:
            pass
    ser, dev, ident = open_board()
    if ser is None:
        print("没找到航班牌(没插, 或者端口被别的程序占着)")
        return 1
    if cmd == "status":
        print(dev, ident)
    elif cmd == "wifi":
        ssid = input("WiFi 名称: ").strip()
        pw = getpass.getpass("WiFi 密码(输入不回显): ")
        print(ask(ser, "$W,%s\t%s" % (ssid, pw), "$OK,W", 3) or "屏幕没应答")
        for _ in range(15):
            time.sleep(1)
            ident = ask(ser, "$?", "$ID", 1.5) or ""
            if "wifi=2" in ident:
                print("已连上:", ident)
                return 0
        print("15 秒内没连上, 检查名称/密码(只支持 2.4G):", ident)
    elif cmd == "airport":
        code = (sys.argv[2] if len(sys.argv) > 2 else "").upper()
        if len(code) != 3 or not code.isalpha():
            print("用法: host.py airport CAN")
            return 2
        ser.write(("$A,%s\n" % code).encode())
        set_airport(code)
        print("已换成", code, "(常驻的 host.py run 重新连上后会跟着换)")
    elif cmd == "wifi-clear":
        print(ask(ser, "$W-", "$OK,W", 3) or "屏幕没应答")
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
