// 机场航班牌 —— ESP32-C3 Super Mini + UsartGPU 串口屏(横屏 320x240)
//
// 两条数据来路, 谁新用谁:
//   1. USB: 电脑端 flightboard_host/host.py 联网取数后按行推过来 ($T/$B/$F/$E)
//   2. WiFi: 配过 WiFi 后自己去 AirLabs 取(电脑在线的时候不重复取)。
//      没配 WiFi(或连不上)时板子自己开一个热点, 手机连上打开 192.168.4.1 就能设置。
//
// 接线: 屏 RX <- GPIO21, 屏 TX -> GPIO20, 5V 供电。屏幕协议 UsartGPU, 38400 8N1。
// 屏幕能力(见 UsartGPU 说明书): 64 个颜色号可用 SCC 设成任意 RGB565(掉电复位),
// 一行命令缓冲 1024 字节, 内置 12/16 点阵字库, DSD 可整数倍放大, CBOF 画圆角矩形。
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include "lwip/dns.h"
#include <DNSServer.h>
#include <sys/time.h>
#include <time.h>

#include "fb_gbk.h"

// 屏幕有两种: 默认是 UsartGPU 串口屏; 加 -DDISPLAY_TFT 则是 SPI 直驱的 ST7789(见 tft.h)。
// 界面代码始终生成串口屏的命令行, TFT 版只是在本机把这些命令画出来。
#ifdef DISPLAY_TFT
#include "tft.h"
#endif

#define SCR_TX 21
#define SCR_RX 20
#define SCR_BAUD 38400
// 屏幕方向: 默认不发方向指令, 用屏幕自己存的方向。要强制就定义成 0~3 (DR0 横屏 / DR2 横屏倒立)
#ifndef SCR_DIR
#define SCR_DIR -1
#endif

#define DEFAULT_AIRPORT "CAN"  // 电脑端可用 $A 改, 存在 flash 里
#define AIRLABS_URL "https://airlabs.co/api/v9/schedules"

// ---------- 调色板: 颜色号 -> RGB888, 开机用 SCC 写进屏幕 ----------
enum {
  C_BG = 0,      // 页面底色
  C_ROW_A = 1,   // 斑马纹
  C_ROW_B = 2,
  C_HDR = 3,     // 顶栏
  C_AMBER = 4,   // 航班号 / 强调色 (机场翻牌的琥珀黄)
  C_WHITE = 5,
  C_DIM = 6,     // 次要文字
  C_LINE = 7,
  C_SLATE = 8,   // "计划"胶囊
  C_GREEN = 9,
  C_RED = 10,
  C_CYAN = 11,
  C_INK = 12,    // 亮色块上的深色字
  C_SOFT = 13,   // 比白暗一档
  C_ORANGE = 14,
  C_SCAN = 15,   // 扫描刷新时的行高亮
  // 航司品牌色
  B_RED = 20, B_MAROON, B_BLUE, B_NAVY, B_SKY, B_GREEN, B_TEAL, B_ORANGE, B_GOLD, B_PURPLE, B_GREY, B_YELLOW,
};

struct PalEntry { uint8_t idx; uint32_t rgb; };
static const PalEntry PALETTE[] = {
    {C_BG, 0x060A12},    {C_ROW_A, 0x0C1322}, {C_ROW_B, 0x151E32}, {C_HDR, 0x0F2D5C},
    {C_AMBER, 0xFFC107}, {C_WHITE, 0xFFFFFF}, {C_DIM, 0x7C8798},   {C_LINE, 0x26324A},
    {C_SLATE, 0x24324C}, {C_GREEN, 0x22C55E}, {C_RED, 0xE5343A},   {C_CYAN, 0x38BDF8},
    {C_INK, 0x0A0F1A},   {C_SOFT, 0xC9D3E0},  {C_ORANGE, 0xFF8A1F}, {C_SCAN, 0x1F4C8F},
    {B_RED, 0xD7141A},   {B_MAROON, 0x7A1232}, {B_BLUE, 0x0A66C2},  {B_NAVY, 0x14306E},
    {B_SKY, 0x2FA7E0},   {B_GREEN, 0x0E9F4F},  {B_TEAL, 0x0B6B66},  {B_ORANGE, 0xF26B1D},
    {B_GOLD, 0xE5B020},  {B_PURPLE, 0x5B2A86}, {B_GREY, 0x4A5568},  {B_YELLOW, 0xFFD400},
};

// ---------- 航司徽标: 品牌底色 + 尾翼色条 + 二字码 ----------
struct Airline { char code[3]; uint8_t bg, stripe, fg; };
static const Airline AIRLINES[] = {
    // 国内
    {"CZ", B_BLUE, B_RED, C_WHITE},     {"MU", B_NAVY, B_RED, C_WHITE},     {"CA", B_RED, B_GOLD, C_WHITE},
    {"HU", B_RED, B_YELLOW, C_WHITE},   {"ZH", B_RED, B_GOLD, C_WHITE},     {"3U", B_RED, C_WHITE, C_WHITE},
    {"MF", B_SKY, C_WHITE, C_WHITE},    {"SC", B_NAVY, B_ORANGE, C_WHITE},  {"FM", B_RED, C_WHITE, C_WHITE},
    {"HO", B_MAROON, B_GOLD, C_WHITE},  {"9C", B_GREEN, C_WHITE, C_WHITE},  {"AQ", B_RED, B_YELLOW, C_WHITE},
    {"GS", B_ORANGE, B_RED, C_WHITE},   {"JD", B_RED, B_GOLD, C_WHITE},     {"8L", B_ORANGE, B_GOLD, C_WHITE},
    {"PN", B_RED, B_ORANGE, C_WHITE},   {"EU", B_BLUE, B_GOLD, C_WHITE},    {"KY", B_GREEN, B_GOLD, C_WHITE},
    {"TV", B_RED, B_YELLOW, C_WHITE},   {"NS", B_BLUE, C_WHITE, C_WHITE},   {"G5", B_RED, B_GREY, C_WHITE},
    {"BK", B_BLUE, B_ORANGE, C_WHITE},  {"DZ", B_BLUE, B_SKY, C_WHITE},     {"GJ", B_SKY, B_GOLD, C_WHITE},
    {"QW", B_BLUE, B_SKY, C_WHITE},     {"DR", B_GOLD, B_RED, C_INK},       {"UQ", B_GREEN, B_YELLOW, C_WHITE},
    {"Y8", B_NAVY, B_GOLD, C_WHITE},    {"RY", B_RED, B_NAVY, C_WHITE},     {"A6", B_ORANGE, B_YELLOW, C_WHITE},
    {"GX", B_BLUE, B_GREEN, C_WHITE},   {"JR", B_RED, B_GOLD, C_WHITE},     {"KN", B_RED, B_BLUE, C_WHITE},
    {"GY", B_BLUE, B_GOLD, C_WHITE},    {"FU", B_RED, B_GOLD, C_WHITE},     {"LT", B_BLUE, B_RED, C_WHITE},
    {"OQ", B_BLUE, B_RED, C_WHITE},     {"CN", B_RED, B_GOLD, C_WHITE},     {"GT", B_BLUE, B_SKY, C_WHITE},
    // 港澳台
    {"CX", B_TEAL, C_WHITE, C_WHITE},   {"HX", B_RED, B_PURPLE, C_WHITE},   {"UO", B_PURPLE, B_ORANGE, C_WHITE},
    {"NX", B_RED, B_BLUE, C_WHITE},     {"CI", B_PURPLE, B_RED, C_WHITE},   {"BR", B_GREEN, B_ORANGE, C_WHITE},
    {"JX", B_GOLD, B_GREY, C_INK},      {"IT", B_YELLOW, B_GREY, C_INK},
    // 日韩
    {"JL", B_RED, C_WHITE, C_WHITE},    {"NH", B_NAVY, B_SKY, C_WHITE},     {"MM", B_PURPLE, B_RED, C_WHITE},
    {"KE", B_SKY, B_RED, C_WHITE},      {"OZ", B_GREY, B_RED, C_WHITE},     {"7C", B_ORANGE, C_WHITE, C_WHITE},
    {"LJ", B_GREEN, B_PURPLE, C_WHITE}, {"TW", B_RED, C_WHITE, C_WHITE},    {"BX", B_BLUE, B_SKY, C_WHITE},
    // 东南亚 / 南亚
    {"SQ", B_NAVY, B_GOLD, C_WHITE},    {"TR", B_YELLOW, B_GREY, C_INK},    {"MH", B_NAVY, B_RED, C_WHITE},
    {"AK", B_RED, C_WHITE, C_WHITE},    {"D7", B_RED, C_WHITE, C_WHITE},    {"FD", B_RED, C_WHITE, C_WHITE},
    {"TG", B_PURPLE, B_GOLD, C_WHITE},  {"SL", B_RED, B_ORANGE, C_WHITE},   {"VN", B_TEAL, B_GOLD, C_WHITE},
    {"VJ", B_RED, B_YELLOW, C_WHITE},   {"PR", B_NAVY, B_RED, C_WHITE},     {"5J", B_YELLOW, B_BLUE, C_INK},
    {"GA", B_TEAL, B_SKY, C_WHITE},     {"K6", B_NAVY, B_RED, C_WHITE},     {"AI", B_RED, B_ORANGE, C_WHITE},
    {"6E", B_NAVY, B_SKY, C_WHITE},     {"UL", B_RED, B_TEAL, C_WHITE},
    // 中东 / 非洲
    {"EK", B_RED, B_GOLD, C_WHITE},     {"QR", B_MAROON, B_GREY, C_WHITE},  {"EY", B_GOLD, B_GREY, C_INK},
    {"SV", B_GREEN, B_GOLD, C_WHITE},   {"TK", B_RED, C_WHITE, C_WHITE},    {"ET", B_GREEN, B_YELLOW, C_WHITE},
    {"MS", B_NAVY, B_GOLD, C_WHITE},    {"KQ", B_RED, B_GREY, C_WHITE},
    // 欧美澳
    {"LH", B_NAVY, B_YELLOW, C_WHITE},  {"AF", B_NAVY, B_RED, C_WHITE},     {"KL", B_SKY, C_WHITE, C_WHITE},
    {"BA", B_NAVY, B_RED, C_WHITE},     {"VS", B_RED, B_PURPLE, C_WHITE},   {"AY", B_NAVY, C_WHITE, C_WHITE},
    {"SK", B_NAVY, B_SKY, C_WHITE},     {"LX", B_RED, C_WHITE, C_WHITE},    {"OS", B_RED, C_WHITE, C_WHITE},
    {"IB", B_RED, B_YELLOW, C_WHITE},   {"AZ", B_GREEN, B_RED, C_WHITE},    {"SU", B_NAVY, B_ORANGE, C_WHITE},
    {"AA", B_GREY, B_RED, C_WHITE},     {"UA", B_BLUE, B_GOLD, C_WHITE},    {"DL", B_NAVY, B_RED, C_WHITE},
    {"AC", B_RED, B_GREY, C_WHITE},     {"QF", B_RED, C_WHITE, C_WHITE},    {"NZ", B_GREY, B_TEAL, C_WHITE},
};

static const Airline *findAirline(const char *fn) {
  for (const Airline &a : AIRLINES)
    if (fn[0] == a.code[0] && fn[1] == a.code[1]) return &a;
  return nullptr;
}

// ---------- 版式 ----------
#define HDR_H 32
#define COL_Y 35       // 列标题 (DS12)
#define ROWS 8
#define ROW_Y0 48
#define ROW_H 21
#define FOOT_TOP 217
#define X_BADGE 4
#define X_TIME 31
#define X_FLIGHT 77
#define X_CITY 137
#define X_TERM 204
#define PILL_X1 222
#define PILL_X2 315
#define CLOCK_X 236
#define PAGE_MS 8000
#define SCAN_MS 28       // 扫描光标在每行停留的时间
#define PROG_STEPS 16    // 顶栏下的翻页进度条分几段走完
#define MAX_PAGES 3  // 每个方向最多翻几页
#define MAXF 48

#define FETCH_RETRY_MS (45UL * 60 * 1000)  // 失败的请求也扣额度(一轮 3 次), 不能重试太勤
#define USB_FRESH_MS (3UL * 60 * 1000)     // 这段时间内电脑在线, WiFi 就不重复取
#define USB_DATA_FRESH_MS (150UL * 60 * 1000)  // 电脑最多 140 分钟推一次数据; 再久没推说明它取不到
#define STALE_SEC (170 * 60)               // 走 AirLabs 时 132 分钟才更新一轮, 太严会一直标过期

struct Flight {
  uint32_t sched;  // UTC epoch
  uint32_t est;    // 0 = 没有
  char fn[9];
  char iata[4];
  char term[3];
  char st;         // S计划 B登机 D起飞 A到达 L延误 C取消 U未知
  char city[21];   // 电脑端给的地名(英文, 或泰文模式下的 TIS-620 泰文), 最多 20 字节
};

struct Board {
  Flight f[MAXF];
  int n = 0;
  uint32_t dataTime = 0;  // 数据源抓取时间 (UTC epoch)
};

static Board boards[2];  // 0 出发  1 到达
static Board staging;
static int stagingKind = -1;
static bool srcWifi = false;
static bool dirty = true;
static bool mirror = false;  // 把发给屏幕的命令同时回显到 USB, 电脑端据此渲染预览图

static Preferences prefs;
static String wifiSsid, wifiPass;
static String alKey;  // AirLabs 密钥(电脑端 $J 带过来, 或在手机设置页里填)
static bool apOn = false;      // 设置热点开着
static char apName[24] = "FlightBoard";
static char airport[4] = DEFAULT_AIRPORT;
static char aptName[21] = "";
static uint8_t nameLang = 0;   // 屏幕语种: 0 中文 / 1 英文 / 2 泰文(只有 TFT 彩屏能显示); 电脑端 $C 设置, 存 flash
// 界面文字按语种取: T(S_DEP) -> 出发 / Departures / ขาออก。串口屏没有泰文字库, 泰文模式下用英文。
#ifdef DISPLAY_TFT
#define LANGI (nameLang)
#else
#define LANGI (nameLang > 1 ? 1 : nameLang)
#endif
#define T(k) (k##_L[LANGI])  // 机场表里没有中文名的机场: 电脑端给的英文城市名, 顶栏上用
static bool ntpStarted = false;
static uint32_t lastUsbMs = 0, nextFetchMs = 0;
static uint32_t lastUsbDataMs = 0;
static uint32_t usbSinceMs = 0;     // 这次电脑连上(开始发 $T)的时间
static bool retryOnly[2] = {false, false};  // 上一轮失败、要单独补取的方向  // 电脑上一次真的推来航班数据的时间

// ---------------- 时间 ----------------
// 屏上所有时间都按机场当地时间显示。时区由电脑端随对时一起给($T 的第二段, 东经为正的分钟数),
// 存 flash; 没给过就按北京时间。
static char tzStr[16] = "CST-8";
static int tzMin = 480;
static void applyTz(int minutes) {
  tzMin = minutes;
  int a = abs(minutes);
  // POSIX 的 TZ 字符串符号和习惯相反: 东十一区写成 -11
  // 时区名至少要 3 个字母, 否则整串无效、悄悄退回 UTC
  snprintf(tzStr, sizeof(tzStr), "LOC%c%d:%02d", minutes >= 0 ? '-' : '+', a / 60, a % 60);
  setenv("TZ", tzStr, 1);
  tzset();
}

static uint32_t nowEpoch() {
  time_t t = time(nullptr);
  return t > 1700000000 ? (uint32_t)t : 0;
}

static void hhmm(uint32_t epoch, char *out) {
  time_t t = epoch;
  struct tm tmv;
  localtime_r(&t, &tmv);
  snprintf(out, 6, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}

// ---------------- USB 收数缓冲 ----------------
// 画一页要阻塞一秒左右, 这期间电脑推来的数据会把 USB 驱动的小缓冲冲掉。
// 所以等屏幕应答的空档里也不停把 USB 字节搬进这个环形缓冲, 解析留到主循环。
static uint8_t usbRing[6144];
static volatile uint16_t usbHead = 0, usbTail = 0;

static void usbPump() {
  while (Serial.available()) {
    uint16_t next = (uint16_t)((usbHead + 1) % sizeof(usbRing));
    if (next == usbTail) break;  // 满了就先不读, 让 USB 那头等着
    usbRing[usbHead] = (uint8_t)Serial.read();
    usbHead = next;
  }
}

// ---------------- 屏幕 ----------------
// 发一行命令并等 "OK"。屏幕处理不过来时不等会丢命令。一行不能超过屏幕的 1024 字节缓冲。
// 屏幕除了 "OK" 还会主动上报触摸: 抬笔时发 [TP:x,y]。所有从屏幕读到的字节都过这里。
static volatile bool tpPending = false;
static int tpX = 0, tpY = 0;
static void scrRx(char c) {
  static char b[20];
  static uint8_t n = 0;
  if (c == '[') { n = 0; b[n++] = c; return; }
  if (!n) return;
  if (c == ']') {
    b[n] = 0;
    int x, y;
    if (sscanf(b, "[TP:%d,%d", &x, &y) == 2) { tpX = x; tpY = y; tpPending = true; }
    n = 0;
    return;
  }
  if (n < sizeof(b) - 1) b[n++] = c; else n = 0;
}

static bool scr(const String &line, uint32_t timeoutMs = 700) {
#ifndef DISPLAY_TFT
  // 串口屏一行命令最多 1024 字节: 长内容(航司标志)在里面用换行分段, 这里拆开一段段发、各等一次 OK
  int nl = line.indexOf('\n');
  if (nl >= 0) {
    bool ok = true;
    int from = 0;
    while (from <= (int)line.length()) {
      int to = line.indexOf('\n', from);
      if (to < 0) to = line.length();
      if (to > from) ok = scr(line.substring(from, to), timeoutMs) && ok;
      from = to + 1;
    }
    return ok;
  }
#endif
  if (mirror) {
    Serial.print("$S,");
    Serial.write((const uint8_t *)line.c_str(), line.length());
    Serial.print("\n");
  }
#ifdef DISPLAY_TFT
  (void)timeoutMs;
  tft::run(line.c_str());
  usbPump();
  return true;
#endif
  while (Serial1.available()) scrRx((char)Serial1.read());
  Serial1.print(line);
  Serial1.print("\r\n");
  uint32_t t0 = millis();
  char prev = 0;
  while (millis() - t0 < timeoutMs) {
    if (Serial1.available()) {
      char c = (char)Serial1.read();
      scrRx(c);
      if (prev == 'O' && c == 'K') return true;
      prev = c;
    } else {
      usbPump();
      delay(1);
    }
  }
  return false;
}

// 单引号是屏幕命令的字符串定界符, 文字里不能有
static void clean(char *s) {
  for (; *s; s++)
    if (*s == '\'' || *s == ';' || (uint8_t)*s < 0x20) *s = ' ';
}

static String txt(int size, int x, int y, const char *text, int color) {
  String s = size == 12 ? "DS12(" : "DS16(";
  s += x; s += ','; s += y; s += ",'"; s += text; s += "',"; s += color; s += ");";
  return s;
}

static String cmd(const char *fmt, ...) {
  char b[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  return String(b);
}

// GBK 文字的像素宽度: 每字节半个字宽
// 文字宽度: 半角半格, GBK 汉字两个字节一整格; 泰文按字宽算
static int textW(int size, const char *s) {
#ifdef DISPLAY_TFT
  return tft::textWidth(size, s);
#else
  return (int)strlen(s) * size / 2;
#endif
}

static void sendPalette() {
  String s;
  for (const PalEntry &p : PALETTE) {
    unsigned r = (p.rgb >> 16) & 0xFF, g = (p.rgb >> 8) & 0xFF, b = p.rgb & 0xFF;
    s += cmd("SCC(%d,%u);", p.idx, ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    if (s.length() > 800) {
      scr(s);
      s = "";
    }
  }
  if (s.length()) scr(s);
}

static int scrDir = SCR_DIR;  // 串口屏方向: -1 用屏幕自己存的; 0 横屏; 2 横屏倒立($O 设置, 存 flash)

static void screenInit() {
  for (int i = 0; i < 5; i++) {
    String s = scrDir >= 0 ? cmd("DR%d;", scrDir) : String();
    s += "CLS(0);";
    if (scr(s, 1000)) break;
    delay(400);
  }
  sendPalette();
}

// ---------------- 数据 ----------------
static void sortBoard(Board &b) {
  for (int i = 1; i < b.n; i++) {
    Flight key = b.f[i];
    int j = i - 1;
    while (j >= 0 && b.f[j].sched > key.sched) {
      b.f[j + 1] = b.f[j];
      j--;
    }
    b.f[j + 1] = key;
  }
}

static void commit(int kind, bool fromWifi) {
  sortBoard(staging);
  boards[kind] = staging;
  srcWifi = fromWifi;
  dirty = true;
}

// 哪些航班值得显示: 还没过点的。全都过点了说明数据是旧的, 那就显示最后一页并标"过期"。
static int visIdx[2][MAXF];
static int visN[2];
static bool visStale[2];

static void computeVis(int k) {
  Board &b = boards[k];
  uint32_t now = nowEpoch();
  visN[k] = 0;
  visStale[k] = false;
  for (int i = 0; i < b.n; i++) {
    const Flight &f = b.f[i];
    uint32_t ref = f.est ? f.est : f.sched;
    bool show = !now || ref + 600 >= now;
    // 延误的航班过了原定时间也还没走, 得留在榜上; 刚取消的留一小时让人看见
    if (f.st == 'L' && f.sched + 4 * 3600 >= now) show = true;
    if (f.st == 'C' && f.sched + 3600 >= now) show = true;
    // 数据源的到达表里会混进出发航班(同一航班号同一时刻两边都有): 以出发表为准
    if (k == 1 && show) {
      for (int j = 0; j < boards[0].n; j++)
        if (boards[0].f[j].sched == f.sched && !strcmp(boards[0].f[j].fn, f.fn)) {
          show = false;
          break;
        }
    }
    if (show) visIdx[k][visN[k]++] = i;
  }
  if (visN[k] == 0 && b.n > 0) {
    for (int i = max(0, b.n - ROWS); i < b.n; i++) visIdx[k][visN[k]++] = i;
    visStale[k] = true;
  }
  if (now && b.dataTime && now > b.dataTime + STALE_SEC) visStale[k] = true;
}

static int pagesOf(int k) {
  int p = (visN[k] + ROWS - 1) / ROWS;
  return min(p, MAX_PAGES);
}

// ---------------- 画面 ----------------
static int shownKind = -1;
static bool needCls = true;
static int shownMin = -2;        // 屏上时钟显示的分钟, 变了才重画
static int progStep = 0;         // 进度条已经走了几段
static uint32_t rowSig[ROWS];    // 每行当前显示内容的指纹: 没变的行不重画
static uint32_t footSig = 0;

// 出发用琥珀黄, 到达用青色: 一眼分清当前是哪块牌
static int accentOf(int kind) { return kind == 0 ? C_AMBER : C_CYAN; }

static int curMinute() {
  uint32_t now = nowEpoch();
  return now ? (int)(now / 60 % 1440) : -1;
}

static uint32_t fnv(const String &s) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < s.length(); i++) {
    h ^= (uint8_t)s[i];
    h *= 16777619u;
  }
  return h;
}

static String clockCmd() {
  char clk[6] = "--:--";
  uint32_t now = nowEpoch();
  if (now) hhmm(now, clk);
  // 16 点阵放大 2x2 = 32px 高, 正好撑满顶栏
  String s = cmd("BOXF(%d,0,319,%d,%d);SBC(%d);", CLOCK_X - 4, HDR_H - 1, C_HDR, C_HDR);
  s += cmd("DSD(16,%d,0,'%s',2,2,%d);", CLOCK_X, clk, C_WHITE);
  return s;
}

static void drawHeader(int kind) {
  // [出发] 广州 CAN  DEPARTURES                 18:21
  // [Departures] Tokyo Haneda HND              18:21
  int acc = accentOf(kind);
  const char *label = kind == 0 ? T(S_DEP) : T(S_ARR);
  // 出发/到达: 从顶到底的一整块强调色, 不用圆角胶囊
  int bw = textW(16, label) + 20;
  String s = cmd("BOXF(0,0,%d,%d,%d);", CLOCK_X - 5, HDR_H - 1, C_HDR);
  s += cmd("BOXF(0,0,%d,%d,%d);SBC(%d);", bw - 1, HDR_H - 1, acc, acc);
  s += txt(16, 10, 8, label, C_INK);
  s += cmd("SBC(%d);", C_HDR);
  int x = bw + 8;
  const char *city = LANGI == 0 ? fbCityGbk(airport) : nullptr;
  char nm[24];
  if (!city) {
    const char *pl = fbPlace(airport, LANGI ? LANGI : 1);
    if (pl) {
      // 去掉为区分同城机场加的三字码("Tokyo HND" -> "Tokyo"), 后面本来就跟着三字码
      strlcpy(nm, pl, sizeof(nm));
      size_t n = strlen(nm);
      if (n > 4 && nm[n - 4] == ' ' && !strcmp(nm + n - 3, airport)) nm[n - 4] = 0;
      city = nm;
    }
  }
  if (!city && aptName[0]) city = aptName;
  int room = CLOCK_X - 6 - x - 24 - 6;  // 留给地名的宽度(右边还有三字码)
  if (city) {
    int csz = textW(16, city) <= room ? 16 : textW(12, city) <= room ? 12 : 0;
    if (csz) {
      s += txt(csz, x, csz == 16 ? 8 : 12, city, C_WHITE);
      x += textW(csz, city) + 6;
    }
  }
  s += txt(16, x, 8, airport, acc);
  x += 24 + 8;
  // 中文、泰文模式下再配一行英文小字, 和真机场的双语大屏一样; 英文模式不重复
  const char *en = kind == 0 ? "DEPARTURES" : "ARRIVALS";
  if (LANGI != 1 && x + (int)strlen(en) * 6 <= CLOCK_X - 6) {  // 全大写, 按等宽字算
    s += "LANG(1);";  // 这几个字母按英文画
    s += txt(12, x, 12, en, C_SOFT);
    s += cmd("LANG(%d);", nameLang);
  }
  s += clockCmd();
  scr(s);
  shownMin = curMinute();
}

// 顶栏下那条线是翻页进度条: 每页从暗线开始, 用强调色从左往右走满就翻页
static void progressReset() {
  scr(cmd("BOXF(0,%d,319,%d,%d);", HDR_H, HDR_H + 1, C_LINE));
  progStep = 0;
}

static void progressTick(int kind, uint32_t elapsedMs) {
  int step = (int)min<uint32_t>(PROG_STEPS, elapsedMs * PROG_STEPS / PAGE_MS);
  if (step <= progStep) return;
  int seg = 320 / PROG_STEPS;
  scr(cmd("BOXF(%d,%d,%d,%d,%d);", progStep * seg, HDR_H, step * seg - 1, HDR_H + 1, accentOf(kind)));
  progStep = step;
}

static void drawColumns(int kind) {
  String s = cmd("BOXF(0,%d,319,%d,%d);SBC(%d);", HDR_H + 2, ROW_Y0 - 1, C_BG, C_BG);
  s += txt(12, X_TIME, COL_Y, T(S_C_TIME), C_DIM);
  s += txt(12, X_FLIGHT, COL_Y, T(S_C_FLIGHT), C_DIM);
  s += txt(12, X_CITY, COL_Y, kind == 0 ? T(S_C_DEST) : T(S_C_ORIG), C_DIM);
  s += txt(12, (PILL_X1 + 320 - textW(12, T(S_C_STATUS))) / 2, COL_Y, T(S_C_STATUS), C_DIM);
  scr(s);
}

// 状态 -> 胶囊底色/字色/文字
// 状态分级: 晚 30 分钟以上(或数据源直接标了延误、没给预计时间)才用橙色整块"延误 21:20";
// 晚 15~29 分钟只用橙色字写"预计 21:20", 不加色块; 15 分钟以内算准点, 照常"计划"。
#define LATE_MINOR_MIN 15
#define LATE_MAJOR_MIN 30
static void statusStyle(const Flight *f, int &bg, int &fg, char *text, size_t n) {
  int late = f->est ? (int)((int32_t)(f->est - f->sched) / 60) : -1;
  char st = f->st;
  bool open = st == 'S' || st == 'U' || st == 'L';  // 还没起飞/落地的
  char e[6] = "";
  if (f->est) hhmm(f->est, e);
  if (open && (late >= LATE_MAJOR_MIN || (st == 'L' && late < 0))) {
    bg = C_ORANGE; fg = C_INK;
    if (e[0]) snprintf(text, n, "%s %s", T(S_ST_DELAY), e);   // 延误 21:20 / Delayed 21:20 / ล่าช้า 21:20
    else strlcpy(text, T(S_ST_DELAY), n);
  } else if (open && late >= LATE_MINOR_MIN) {
    bg = -1; fg = C_ORANGE;
    snprintf(text, n, "%s %s", T(S_ST_EST), e);                // 预计 21:20 / Est 21:20 / คาดว่า 21:20
  } else if (st == 'C') {
    bg = C_RED; fg = C_WHITE; strlcpy(text, T(S_ST_CANCEL), n);
  } else if (st == 'B') {
    bg = C_GREEN; fg = C_INK; strlcpy(text, T(S_ST_BOARD), n);
  } else if (st == 'D') {
    bg = -1; fg = C_CYAN; strlcpy(text, T(S_ST_DEPARTED), n);
  } else if (st == 'A') {
    bg = -1; fg = C_GREEN; strlcpy(text, T(S_ST_ARRIVED), n);
  } else if (st == 'S' || st == 'L') {  // 标了延误、但预计时间其实只晚一点: 按准点
    bg = -1; fg = C_SOFT; strlcpy(text, T(S_ST_SCHED), n);  // 最常见的状态不加底色, 把视线留给异常
  } else {
    bg = -1; fg = C_DIM; strlcpy(text, T(S_ST_UNKNOWN), n);
  }
}

// ---------- 串口屏的航司标志(tools/make_scr_logos.py 生成, 本地有才编进去) ----------
#if !defined(DISPLAY_TFT) && __has_include("fb_scr_logos.h")
#include "fb_scr_logos.h"
#define HAVE_SCR_LOGOS 1
static int scrLogoFind(const char *code) {
  for (int i = 0; i < FB_SL_N; i++)
    if (pgm_read_byte(&FB_SL_CODE[i * 2]) == code[0] && pgm_read_byte(&FB_SL_CODE[i * 2 + 1]) == code[1]) return i;
  return -1;
}
// 第 row 行的标志: 这行专用 4 个调色板号(32 + 4*row 起), 先设颜色, 再铺圆角底, 再一层层盖矩形。
// 前后和每 30 个矩形插一个换行, scr() 按换行拆成多条命令发(每条不超过屏幕的 1024 字节)
static String scrLogoCmd(int row, int x, int y, int idx) {
  int base = 32 + 4 * row;
  String s = "\n";
  for (int k = 0; k < 4; k++) s += cmd("SCC(%d,%u);", base + k, (unsigned)pgm_read_word(&FB_SL_PAL[idx * 4 + k]));
  s += cmd("CBOF(%d,%d,%d,%d,3,%d);", x, y, x + 21, y + 16, base);
  uint16_t start = pgm_read_word(&FB_SL_IDX[idx * 2]), n = pgm_read_word(&FB_SL_IDX[idx * 2 + 1]);
  for (int i = 0; i < n; i++) {
    const uint8_t *p = FB_SL_RECT + (size_t)(start + i) * 3;
    uint32_t v = pgm_read_byte(p) | (pgm_read_byte(p + 1) << 8) | ((uint32_t)pgm_read_byte(p + 2) << 16);
    int rx = v & 31, ry = (v >> 5) & 31, w = ((v >> 10) & 31) + 1, h = ((v >> 15) & 31) + 1, c = (v >> 20) & 3;
    if (i && i % 30 == 0) s += "\n";
    s += cmd("BOXF(%d,%d,%d,%d,%d);", x + rx, y + ry, x + rx + w - 1, y + ry + h - 1, base + c);
  }
  return s + "\n";
}
#endif

static String rowCmd(int row, int kind, const Flight *f) {
  int y = ROW_Y0 + row * ROW_H;
  int band = row % 2 ? C_ROW_B : C_ROW_A;
  String s = cmd("BOXF(0,%d,319,%d,%d);", y, y + ROW_H - 1, f ? band : C_BG);
  if (f) {
    // 已起飞/已到达的行整体压暗, 视线留给还没走的
    bool gone = f->st == 'D' || f->st == 'A' || f->st == 'C';
    int ty = y + 3;

    // 航司徽标: 圆角色块 + 尾翼色条 + 二字码
    const Airline *al = findAirline(f->fn);
    int bbg = al ? al->bg : B_GREY, bst = al ? al->stripe : C_DIM, bfg = al ? al->fg : C_WHITE;
    char code[3] = {f->fn[0], f->fn[1], 0};
    int lg = -1;
#ifdef DISPLAY_TFT
    lg = tft::findLogo(code);  // 彩屏有真图标就用图标, 没有的航司还是彩色徽章
#endif
#ifdef HAVE_SCR_LOGOS
    lg = scrLogoFind(code);
#endif
    if (lg >= 0) {
#ifdef HAVE_SCR_LOGOS
      s += scrLogoCmd(row, X_BADGE, y + 2, lg);
#else
      s += cmd("LOGO(%d,%d,%d);", X_BADGE, y + 2, lg);
#endif
    } else {
      s += cmd("CBOF(%d,%d,%d,%d,3,%d);", X_BADGE, y + 2, X_BADGE + 21, y + 18, bbg);
      s += cmd("BOXF(%d,%d,%d,%d,%d);SBC(%d);", X_BADGE + 3, y + 15, X_BADGE + 18, y + 16, bst, bbg);
      s += txt(12, X_BADGE + 5, y + 3, code, bfg);
    }

    s += cmd("SBC(%d);", band);
    char t[6];
    hhmm(f->sched, t);
    s += txt(16, X_TIME, ty, t, gone ? C_DIM : C_WHITE);
    s += txt(16, X_FLIGHT, ty, f->fn, gone ? C_DIM : C_AMBER);

    const char *city = nameLang ? nullptr : fbCityGbk(f->iata);
    char en[21];
    if (!city) {
      // 英文/泰文地名查板子自己的表(脱离电脑也有), 表里没有才用数据源给的
      const char *pl = fbPlace(f->iata, LANGI ? LANGI : 1);
      strlcpy(en, pl ? pl : f->city[0] ? f->city : f->iata, sizeof(en));
      city = en;
    }
    // 这一栏宽 85 像素: 16 号字放 10 个字母; 更长的英文名(比如 "Tokyo Haneda")换 12 号字, 放得下 14 个
    int csz = textW(16, city) <= PILL_X1 - X_CITY - 2 ? 16 : 12;
    s += txt(csz, X_CITY, csz == 16 ? ty : y + 5, city, gone ? C_DIM : C_WHITE);
    if (f->term[0] && textW(csz, city) <= X_TERM - X_CITY - 4) {
      char tm[4];
      snprintf(tm, sizeof(tm), "T%s", f->term);
      s += txt(12, X_TERM, y + 5, tm, C_DIM);
    }

    int bg, fg;
    char st[20];
    statusStyle(f, bg, fg, st, sizeof(st));
    int w = textW(16, st);
    // 状态: 异常的(延误、取消、登机)整列铺满这一行的高度, 一直到屏幕右边; 底下留 1 像素, 连着几行也分得开
    if (bg >= 0) {
      s += cmd("BOXF(%d,%d,319,%d,%d);SBC(%d);", PILL_X1, y, y + ROW_H - 2, bg, bg);
    }
    s += txt(16, PILL_X1 + (320 - PILL_X1 - w) / 2, ty, st, fg);
  }
  return s;
}

// 扫描光标: 整行提亮 + 左侧一条强调色
static String scanCmd(int row, int kind) {
  int y = ROW_Y0 + row * ROW_H;
  return cmd("BOXF(0,%d,319,%d,%d);BOXF(0,%d,2,%d,%d);", y, y + ROW_H - 1, C_SCAN, y, y + ROW_H - 1,
             accentOf(kind));
}

// 逐行扫描刷新: 只动内容变了的行。光标比内容领先一行 —— 每条命令里先落下这一行的新内容,
// 再点亮下一行, 所以看到的是一条亮带从上往下扫过去, 身后留下新内容, 而不是整页一闪。
static void drawRows(int kind, int page) {
  String content[ROWS];
  int todo[ROWS], m = 0;
  for (int r = 0; r < ROWS; r++) {
    int i = page * ROWS + r;
    content[r] = rowCmd(r, kind, i < visN[kind] ? &boards[kind].f[visIdx[kind][i]] : nullptr);
    uint32_t h = fnv(content[r]);
    // 清屏后的空行本来就是底色, 不用再扫一遍
    bool blankOnBlank = rowSig[r] == 0 && i >= visN[kind];
    if (h != rowSig[r] && !blankOnBlank) todo[m++] = r;
    rowSig[r] = h;
  }
  if (!m) return;
  scr(scanCmd(todo[0], kind));
  for (int j = 0; j < m; j++) {
    delay(SCAN_MS);
    String s = content[todo[j]];
    if (j + 1 < m) s += scanCmd(todo[j + 1], kind);
    scr(s);
  }
}

static void drawFooter(int kind, int page, int pages) {
  String s = cmd("BOXF(0,%d,319,239,%d);PL(0,%d,319,%d,%d);SBC(%d);", FOOT_TOP, C_BG, FOOT_TOP, FOOT_TOP, C_LINE, C_BG);
  bool stale = visStale[kind];
  s += cmd("CIRF(9,229,3,%d);", stale ? C_RED : C_GREEN);
  char buf[40];
  if (boards[kind].dataTime) {
    char t[6];
    hhmm(boards[kind].dataTime, t);
    snprintf(buf, sizeof(buf), "%s %s %s", srcWifi ? "WiFi" : "USB", stale ? T(S_STALE) : T(S_UPDATED), t);
  } else {
    strlcpy(buf, srcWifi ? "WiFi" : "USB", sizeof(buf));
  }
  s += txt(12, 18, 223, buf, stale ? C_RED : C_DIM);

  // 页码圆点
  for (int i = 0; i < pages; i++)
    s += cmd("CIRF(%d,229,3,%d);", 160 - (pages - 1) * 6 + i * 12, i == page ? C_AMBER : C_LINE);

  uint32_t now = nowEpoch();
  if (now) {
    const char *const WK[] = {T(S_WK0), T(S_WK1), T(S_WK2), T(S_WK3), T(S_WK4), T(S_WK5), T(S_WK6)};
    time_t t = now;
    struct tm tmv;
    localtime_r(&t, &tmv);
    snprintf(buf, sizeof(buf), "%02d-%02d %s", tmv.tm_mon + 1, tmv.tm_mday, WK[tmv.tm_wday]);
    s += txt(12, 316 - textW(12, buf), 223, buf, C_DIM);
  }
  uint32_t h = fnv(s);
  if (h != footSig) scr(s);
  footSig = h;
}

static void drawPage(int kind, int page) {
  // 每次翻页都重发调色板: 颜色号只存在屏幕内存里, 屏幕掉电或某条 SCC 传坏了
  // 颜色就会一直错下去。不清屏以后没有别的机会纠正, 所以在这里顺手发一遍(约 0.1 秒)。
  sendPalette();
  scr("TPN(1);");  // 触摸: 返回坐标模式 (CLS 会把触摸关掉, 所以每页都开一次)
  if (needCls) {
    scr(cmd("LANG(%d);CLS(0);", nameLang));
    needCls = false;
    shownKind = -1;
    footSig = 0;
    memset(rowSig, 0, sizeof(rowSig));
  }
  if (shownKind != kind) {
    drawHeader(kind);
    drawColumns(kind);
    shownKind = kind;
  }
  progressReset();
  drawRows(kind, page);
  drawFooter(kind, page, pagesOf(kind));
}

static int waitState = -1;  // 取数状态(F_*), 由 WiFi 取数那边更新
static void drawWaiting() {
  sendPalette();
  scr(cmd("LANG(%d);CLS(0);TPN(1);", nameLang));
  drawHeader(0);
  String s = cmd("BOXF(0,%d,319,%d,%d);SBC(%d);", HDR_H, HDR_H + 1, C_AMBER, C_BG);
  // 脱机使用时全靠这两行字知道它卡在哪一步
  const char *l1 = T(S_W_FETCH), *l2 = T(S_W_TAP);
  // 插着电脑: 数据由电脑经 USB 推过来, 这里只说"正在获取", 底栏写 USB, 不再显示 WiFi 状态
  bool usbOn = lastUsbMs && millis() - lastUsbMs < USB_FRESH_MS;
  bool apHint = !usbOn && apOn && WiFi.status() != WL_CONNECTED;
  if (usbOn) l2 = "";
  else if (apHint) l1 = T(S_W_AP), l2 = T(S_W_AP2);
  else if (!wifiSsid.length()) l1 = T(S_W_NOWIFI);
  else if (WiFi.status() != WL_CONNECTED) l1 = T(S_W_CONN);
  else if (waitState == 6) l1 = T(S_W_NOKEY), l2 = T(S_W_NOKEY2);
  else if (waitState == 3) l1 = T(S_W_QUOTA), l2 = T(S_W_QUOTA2);
  else if (waitState == 1) l1 = T(S_W_NET);
  else if (waitState == 2) l1 = T(S_W_HTTP);
  else if (waitState == 5) l1 = T(S_W_NOFL), l2 = T(S_W_TAP_APT);
#ifdef DISPLAY_TFT
  if (l2 == T(S_W_TAP) || l2 == T(S_W_TAP_APT)) l2 = "";  // 彩屏没有触摸, 不提示"点一下"
#endif
  if (apHint) {
    // 用手机连接这个 WiFi / FlightBoard-XXXX / 然后打开 192.168.4.1
    s += txt(16, (320 - textW(16, l1)) / 2, 76, l1, C_AMBER);
    s += "LANG(1);";
    s += txt(16, (320 - textW(16, apName)) / 2, 108, apName, C_WHITE);
    s += cmd("LANG(%d);", nameLang);
    s += txt(16, (320 - textW(16, l2)) / 2, 140, l2, C_SOFT);
  } else {
    s += txt(16, (320 - textW(16, l1)) / 2, 96, l1, waitState >= 1 && waitState <= 4 ? C_ORANGE : C_AMBER);
    s += txt(12, (320 - textW(12, l2)) / 2, 132, l2, C_SOFT);
  }
  String st = "WiFi ";
  if (usbOn) st = "USB";
  else if (!wifiSsid.length()) st += "-";
  else if (WiFi.status() == WL_CONNECTED) st += WiFi.localIP().toString();
  else st += "...";
  s += cmd("PL(0,%d,319,%d,%d);", FOOT_TOP, FOOT_TOP, C_LINE);
  s += txt(12, 8, 223, st.c_str(), C_DIM);
  scr(s);
  needCls = true;
  shownKind = -1;
}

// ---------------- WiFi 取数 ----------------
// 最近一次 WiFi 取数的结果, 在 $? 里带出去, 脱机用时出问题好查
static char fetchDiag[2][96] = {"-", "-"};
#define DIAG(...) snprintf(fetchDiag[kind], sizeof(fetchDiag[kind]), __VA_ARGS__)

// 取数结果。FR_NET 是请求根本没到服务器(不扣额度), 可以很快重试; 其余都扣了额度, 不能急。
enum { FR_OK, FR_NET, FR_HTTP, FR_QUOTA, FR_EMPTY, FR_NOKEY, FR_CUT };  // FR_CUT 下载到一半断了
static int fetchState = -1;  // 最近一轮的结果, -1 = 还没取过
static int netFails = 0;

// 一次响应有一百多 KB, 经 TLS 分很多段到, 段与段之间会停顿。
// 默认的流读取停 1 秒就当结束, JSON 会读到一半断掉; 这里只要连接还在就多等一会儿。
struct SlowReader {
  // 带 1 KB 缓冲: JSON 解析器是一个字一个字要的, 直接逐字节读 TLS 连接, 每个字节都要过一遍解密层,
  // 一次 100 KB 的响应要几十秒。整块读进缓冲再一个个给, 快一个数量级。
  WiFiClient &c;
  uint32_t bytes = 0;  // 读到多少字节, 出错时带进诊断
  char why = '-';      // 为什么停: c 对方关了连接 / t 等太久
  uint8_t buf[1024];
  int pos = 0, len = 0;
  explicit SlowReader(WiFiClient &cl) : c(cl) {}
  bool wait() {
    uint32_t t0 = millis();
    while (!c.available()) {
      if (!c.connected()) return why = 'c', c.available() > 0;
      if (millis() - t0 > 15000) return why = 't', false;
      usbPump();
      delay(2);
    }
    return true;
  }
  bool fill() {
    // TLS 偶尔报"有数据"却读出 -1(记录还没解完): 再等等, 不能当成结束
    for (int i = 0; i < 50; i++) {
      if (!wait()) return false;
      int r = c.read(buf, sizeof(buf));
      if (r > 0) {
        pos = 0, len = r, bytes += (uint32_t)r;
        return true;
      }
      delay(5);
    }
    return false;
  }
  int read() {
    if (pos >= len && !fill()) return -1;
    return buf[pos++];
  }
  size_t readBytes(char *b, size_t n) {
    // 要多少给多少: 少给了 JSON 解析器会以为数据到头了(IncompleteInput)
    size_t got = 0;
    while (got < n) {
      if (pos >= len && !fill()) break;
      size_t k = min(n - got, (size_t)(len - pos));
      memcpy(b + got, buf + pos, k);
      pos += k, got += k;
    }
    return got;
  }
};


// ---- AirLabs(全球机场) ----
// 和电脑端 host.py 的 fetch_airlabs 同一套规则。免费档一次最多 100 行、不能翻页, 所以按状态分开要:
// 出发只要还没走的(scheduled); 到达要还没起飞的 + 已经在天上的(active)。一轮 3 次请求。
#define AIRLABS_EVERY_MS (140UL * 60 * 1000)  // 免费档 1000 次/月: 140 分钟一轮 ≈ 940 次/月, 留点给出错重试

static uint32_t epochOf(int y, int mo, int d, int h, int mi) {
  y -= mo <= 2;
  int era = y / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = (long)era * 146097 + (long)doe - 719468;
  return (uint32_t)(days * 86400L + h * 3600 + mi * 60);
}

// "2026-11-17 01:20" -> 按 UTC 算的秒数(只用来求当地时间和 UTC 的差)
static uint32_t parseYmdHm(const char *s) {
  int y, mo, d, h, mi;
  if (!s || sscanf(s, "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return 0;
  return epochOf(y, mo, d, h, mi);
}

static int flightNo(const char *fn) {
  const char *p = fn + strlen(fn);
  while (p > fn && isdigit((unsigned char)p[-1])) p--;
  return atoi(p);
}

// 放进 staging; 满了就挤掉最晚的一条, 最后留下的是最早的 48 条
static void stageKeep(const Flight &f) {
  if (staging.n < MAXF) {
    staging.f[staging.n++] = f;
    return;
  }
  int worst = 0;
  for (int i = 1; i < MAXF; i++)
    if (staging.f[i].sched > staging.f[worst].sched) worst = i;
  if (f.sched < staging.f[worst].sched) staging.f[worst] = f;
}

static int airlabsPart(int kind, const char *status, uint32_t now, int &total) {
  const String me = kind == 0 ? "dep" : "arr", other = kind == 0 ? "arr" : "dep";
  WiFiClientSecure cli;
  cli.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setUserAgent("flightboard-esp32/1.0");
  http.setTimeout(20000);
  // 这家的密钥只能放在网址参数里
  // _fields: 只要用得到的九个字段, 响应从 100 多 KB 降到 20 KB, WiFi 下载快很多。
  // 注意加了 _fields 后 AirLabs 不再给 *_ts 时间戳, 时间从 UTC 时间字符串算
  String url = String(AIRLABS_URL) + "?" + me + "_iata=" + airport + "&status=" + status +
               "&_fields=flight_iata,cs_flight_iata,status," + other + "_iata," + me + "_delayed," + me +
               "_terminal," + me + "_time," + me + "_time_utc," + me + "_estimated_utc" + "&api_key=" + alKey;
  if (!http.begin(cli, url)) {
    DIAG("begin");
    return FR_NET;
  }
  int code = http.GET();
  if (code != 200) {
    DIAG("airlabs http %d", code);
    http.end();
    return code < 0 ? FR_NET : code == 429 ? FR_QUOTA : FR_HTTP;
  }
  JsonDocument filter;
  filter["error"] = true;
  JsonObject ff = filter["response"][0].to<JsonObject>();
  for (const char *k : {"flight_iata", "cs_flight_iata", "status"}) ff[k] = true;
  ff[other + "_iata"] = true;
  for (const char *k : {"_delayed", "_terminal", "_time", "_time_utc", "_estimated_utc"}) ff[me + k] = true;

  JsonDocument doc;
  SlowReader rd{http.getStream()};
  DeserializationError err = deserializeJson(doc, rd, DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    DIAG("airlabs json %s at %u %c heap %u", err.c_str(), (unsigned)rd.bytes, rd.why, (unsigned)ESP.getFreeHeap());
    return err == DeserializationError::IncompleteInput ? FR_CUT : FR_HTTP;
  }
  if (!doc["error"].isNull()) {
    const char *msg = doc["error"]["message"] | (const char *)nullptr;
    const char *ec = doc["error"]["code"] | "";
    DIAG("airlabs %s", msg ? msg : ec);
    String e = String(msg ? msg : "") + ec;
    e.toLowerCase();
    return e.indexOf("limit") >= 0 ? FR_QUOTA : e.indexOf("key") >= 0 ? FR_NOKEY : FR_HTTP;
  }
  bool tzDone = false;
  for (JsonObject o : doc["response"].as<JsonArray>()) {
    total++;
    // 时区: 每条航班同时给了当地时间和 UTC 时间, 一减就是(夏令时也自动对)
    if (!tzDone) {
      uint32_t loc = parseYmdHm(o[me + "_time"] | (const char *)nullptr);
      uint32_t utc = parseYmdHm(o[me + "_time_utc"] | (const char *)nullptr);
      if (loc && utc) {
        tzDone = true;
        int m = (int)((int32_t)(loc - utc) / 60);
        if (m != tzMin && m >= -720 && m <= 840) {
          applyTz(m);
          prefs.putShort("tz", (int16_t)m);
          needCls = true;
        }
      }
    }
    const char *cs = o["cs_flight_iata"] | "";
    if (cs[0]) continue;  // 代码共享的重复行: 只留实际承运的那条
    Flight f;
    memset(&f, 0, sizeof(f));
    f.sched = parseYmdHm(o[me + "_time_utc"] | (const char *)nullptr);
    uint32_t est = parseYmdHm(o[me + "_estimated_utc"] | (const char *)nullptr);
    f.est = est != f.sched ? est : 0;
    strlcpy(f.fn, o["flight_iata"] | "", sizeof(f.fn));
    strlcpy(f.iata, o[other + "_iata"] | "", sizeof(f.iata));
    if (!f.sched || !f.fn[0] || !f.iata[0]) continue;
    strlcpy(f.term, o[me + "_terminal"] | "", sizeof(f.term));
    int late = o[me + "_delayed"] | 0;
    String raw = o["status"] | "";
    raw.toLowerCase();
    if (raw == "cancelled") f.st = 'C';
    else if (raw == "landed") f.st = kind ? 'A' : 'D';
    else if (raw == "active") f.st = kind == 0 ? 'D' : late >= 15 ? 'L' : 'S';  // 已经在天上
    else f.st = late >= 15 ? 'L' : 'S';
    uint32_t t = f.est ? f.est : f.sched;
    bool keep = t + 600 >= now;
    if (f.st == 'L') keep = f.sched + 4 * 3600 >= now;
    if (f.st == 'C') keep = f.sched + 3600 >= now;
    if (!keep) continue;
    clean(f.fn);
    clean(f.term);
    stageKeep(f);
  }
  return FR_OK;
}

static int fetchAirlabs(int kind) {
  uint32_t fetchT0 = millis();
  uint32_t now = nowEpoch();
  staging.n = 0;
  staging.dataTime = now;
  int total = 0;
  const char *parts[] = {"scheduled", "active"};
  for (int i = 0; i < (kind == 0 ? 1 : 2); i++) {
    int t0 = total, n0 = staging.n;
    int r = airlabsPart(kind, parts[i], now, total);
    if (r == FR_CUT) {
      // 下到一半断了: 马上再要一次(多扣 1 次额度), 比等 20 分钟强
      total = t0;
      staging.n = n0;
      delay(1500);
      r = airlabsPart(kind, parts[i], now, total);
    }
    if (r != FR_OK) return r == FR_CUT ? FR_HTTP : r;
  }
  // 离现在远的航班不标代码共享, 同一班飞机会以好几家的航班号各出现一次(挂名的一般是四位数):
  // 同一时刻、同一对端机场里只要有三位以内的号, 就把四位数的去掉
  int w = 0;
  for (int i = 0; i < staging.n; i++) {
    const Flight &a = staging.f[i];
    bool drop = false;
    if (flightNo(a.fn) >= 1000)
      for (int j = 0; j < staging.n && !drop; j++)
        drop = j != i && staging.f[j].sched == a.sched && !strcmp(staging.f[j].iata, a.iata) &&
               flightNo(staging.f[j].fn) < 1000;
    if (!drop) staging.f[w++] = a;
  }
  staging.n = w;
  if (staging.n == 0) {
    DIAG("airlabs empty of %d", total);
    return FR_EMPTY;
  }
  DIAG("airlabs ok %d of %d in %ums heap %u", staging.n, total, (unsigned)(millis() - fetchT0), (unsigned)ESP.getFreeHeap());
  commit(kind, true);
  return FR_OK;
}

static void wifiStart() {
  if (!wifiSsid.length()) return;
  WiFi.mode(apOn ? WIFI_AP_STA : WIFI_STA);  // 设置热点开着时不能关掉它
  WiFi.setAutoReconnect(!apOn);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  // Super Mini 的陶瓷天线匹配很差, 满功率反而连不上, 降功率是通用解法
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  ntpStarted = false;
  WiFi.setSleep(false);  // 省电模式下有的路由器会丢发给它的包
  nextFetchMs = millis() + 20000;  // 连上后稍等: 电脑在线的话它几秒内就会来对时, 由它取数, 不白扣额度
}

// 路由器自带的 DNS 时灵时不灵(电脑那边也遇到过解析失败), 域名查不到就整轮取数失败。
// 所以优先用公共 DNS(阿里 / 腾讯), 路由器的退到最后; lwIP 一个服务器超时会自动换下一个。
// DHCP 续租会把它改回去, 所以每次取数前都重新设一遍。
static void ensureDns() {
  IPAddress router = WiFi.gatewayIP();
  ip_addr_t a;
  IP_ADDR4(&a, 223, 5, 5, 5);
  dns_setserver(0, &a);
  IP_ADDR4(&a, 119, 29, 29, 29);
  dns_setserver(1, &a);
#if DNS_MAX_SERVERS > 2
  if (router) {
    IP_ADDR4(&a, router[0], router[1], router[2], router[3]);
    dns_setserver(2, &a);
  }
#endif
}

// 实测: 板子断电/刷机后第一次连上 WiFi, 能连到路由器却出不了外网(DNS 和 TCP 都不通),
// 断开重连一次就好(软重启也好)。所以出不了外网时就重连一次, 不扣额度。
static void wifiKick() {
  WiFi.disconnect();
  delay(200);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
}

static void wifiTask() {
  static uint32_t upSince = 0;  // 这次连上 WiFi 的时间
  if (!wifiSsid.length() || WiFi.status() != WL_CONNECTED) {
    upSince = 0;
    return;
  }
  if (!upSince) upSince = millis() ? millis() : 1;
  static bool dnsSet = false;
  if (!dnsSet) {
    ensureDns();  // 连上后先设好, NTP 也要查域名
    dnsSet = true;
  }
  if (!ntpStarted) {
    configTzTime(tzStr, "ntp.aliyun.com", "pool.ntp.org");
    ntpStarted = true;
  }
  // 电脑在线、而且它最近真推过数据: 取数归它管。电脑插着但自己断网取不到时, 板子照样自己取
  // 插着电脑就走 USB: 电脑在线时一律不自己取, 哪怕它还没推来数据(刚连上、刚换机场, 它正在取)。
  // 只有电脑连着却 150 分钟都没推来新数据(它自己断网了), 板子才接手。
  if (lastUsbMs && millis() - lastUsbMs < USB_FRESH_MS) {
    uint32_t since = lastUsbDataMs && lastUsbDataMs - usbSinceMs < 0x80000000UL ? lastUsbDataMs : usbSinceMs;
    if (millis() - since < USB_DATA_FRESH_MS) return;
  }
  if ((int32_t)(millis() - nextFetchMs) < 0) return;
  if (!nowEpoch()) {
    // 还没对上时, 分不出哪些航班已经走了; 等 NTP, 不扣额度。连上一分钟还没对上, 就是出不了外网: 重连
    if (millis() - upSince > 60000) {
      upSince = 0;
      ensureDns();
      wifiKick();
    }
    return;
  }
  ensureDns();
  bool partial = false;
  int r;
  if (!alKey.length()) {
    r = FR_NOKEY;  // 没密钥: 不取, 等手机设置页或电脑把密钥给过来
  } else {
    int (*fetch)(int) = fetchAirlabs;
    // 上一轮只成功了一边: 这次只补失败的那边, 不白扣另一边的额度
    bool doD = !retryOnly[0] && !retryOnly[1] ? true : retryOnly[0];
    bool doA = !retryOnly[0] && !retryOnly[1] ? true : retryOnly[1];
    int r0 = doD ? fetch(0) : FR_OK;
    r = r0;
    int r1 = FR_OK;
    if (doA && r0 != FR_NET && r0 != FR_QUOTA && r0 != FR_NOKEY) {
      r1 = fetch(1);
      if (r1 == FR_OK) r = FR_OK;  // 有一边成功就算成功(深夜常有一边没航班)
    }
    retryOnly[0] = retryOnly[1] = false;
    // 一边成功、另一边是半路出错(不是没航班): 20 分钟后单独补那一边
    bool bad0 = doD && (r0 == FR_HTTP || r0 == FR_NET), bad1 = doA && (r1 == FR_HTTP || r1 == FR_NET);
    if (r == FR_OK && (bad0 || bad1)) {
      retryOnly[0] = bad0;
      retryOnly[1] = bad1;
      partial = true;
    }
  }
  netFails = r == FR_NET ? netFails + 1 : 0;
  if (netFails && netFails % 2 == 0) wifiKick();  // 连着两次(约 1 分钟)出不了外网
  uint32_t wait = partial ? 20UL * 60 * 1000
                  : r == FR_NOKEY ? (alKey.length() ? FETCH_RETRY_MS : 30000UL)
                  : r == FR_OK || r == FR_EMPTY ? AIRLABS_EVERY_MS
                  : r == FR_NET ? (netFails < 10 ? 30000UL : 180000UL)  // 没到服务器, 不扣额度
                  : r == FR_QUOTA ? 60UL * 60 * 1000
                  : FETCH_RETRY_MS;
  nextFetchMs = millis() + wait;
  if (r != fetchState) dirty = true;
  fetchState = r;
  waitState = r;
}

// ---------------- 触摸界面 ----------------
// 屏幕是电阻触摸, 工作在"返回坐标"模式(TPN(1)): 抬笔时上报 [TP:x,y], 热区由这里自己判断。
// 航班牌上点任意位置进设置菜单: 换机场 / 连 WiFi(屏幕键盘输密码) / 触摸校准。
enum UiMode { UI_BOARD, UI_MENU, UI_APT, UI_WIFI, UI_KEYB };
static UiMode ui = UI_BOARD;
static uint32_t uiTouchMs = 0;
static bool waitingShown = false;
static bool wifiNeedRestart = false;
#define UI_IDLE_MS 90000UL

enum {
  Z_BACK = 300, Z_APT, Z_WIFI, Z_CAL, Z_PREV, Z_NEXT, Z_RESCAN,
  Z_SHIFT, Z_DEL, Z_LAYER, Z_SPACE, Z_OK,
  Z_ITEM = 400,
};
struct Zone { int16_t x1, y1, x2, y2, id; };
static Zone zones[56];
static int nZones = 0;

static void zone(int x1, int y1, int x2, int y2, int id) {
  if (nZones < (int)(sizeof(zones) / sizeof(zones[0])))
    zones[nZones++] = {(int16_t)x1, (int16_t)y1, (int16_t)x2, (int16_t)y2, (int16_t)id};
}

static const Zone *zoneAt(int x, int y) {
  for (int i = 0; i < nZones; i++)
    if (x >= zones[i].x1 && x <= zones[i].x2 && y >= zones[i].y1 && y <= zones[i].y2) return &zones[i];
  return nullptr;
}

// 屏幕命令里 ' ; , 是语法字符, 不能放进字符串, 这三个用画线的办法画出来 (8x16 的格子)
static String glyph(int x, int y, char c, int color) {
  if (c == '\'') return cmd("BOXF(%d,%d,%d,%d,%d);", x + 3, y + 2, x + 4, y + 6, color);
  if (c == ',')
    return cmd("BOXF(%d,%d,%d,%d,%d);PL(%d,%d,%d,%d,%d);", x + 3, y + 11, x + 4, y + 12, color,
               x + 4, y + 13, x + 2, y + 15, color);
  return cmd("BOXF(%d,%d,%d,%d,%d);BOXF(%d,%d,%d,%d,%d);", x + 3, y + 5, x + 4, y + 6, color,
             x + 3, y + 11, x + 4, y + 12, color) +
         cmd("PL(%d,%d,%d,%d,%d);", x + 4, y + 13, x + 2, y + 15, color);
}

// 任意 ASCII 文字 (16 点阵): 普通字符成段用 DS16, 语法字符逐个画
static String asciiCmd(int x, int y, const char *t, int color) {
  String s, run;
  int runX = x;
  for (; *t; t++, x += 8) {
    char c = *t;
    if (c == '\'' || c == ';' || c == ',') {
      if (run.length()) s += txt(16, runX, y, run.c_str(), color);
      run = "";
      s += glyph(x, y, c, color);
      runX = x + 8;
    } else {
      run += (c < 0x20 || c > 0x7E) ? '?' : c;
    }
  }
  if (run.length()) s += txt(16, runX, y, run.c_str(), color);
  return s;
}

// WiFi 名称可能有中文(UTF-8), 屏幕字库是 GBK, 显示时非 ASCII 的换成 ?
static String dispName(const String &v, int maxLen) {
  String o;
  for (size_t i = 0; i < v.length() && (int)o.length() < maxLen; i++) {
    uint8_t c = (uint8_t)v[i];
    if (c >= 0x80) {
      if ((c & 0xC0) != 0x80) o += '?';  // 一个多字节字符只出一个 ?
    } else {
      o += (c < 0x20 || c == 0x7F) ? '?' : (char)c;
    }
  }
  return o;
}

static String btn(int x1, int y1, int x2, int y2, int bg, int fg, const char *label, int size, int id) {
  zone(x1, y1, x2, y2, id);
  String s = cmd("CBOF(%d,%d,%d,%d,4,%d);SBC(%d);", x1, y1, x2, y2, bg, bg);
  s += txt(size, x1 + (x2 - x1 + 1 - textW(size, label)) / 2, y1 + (y2 - y1 + 1 - size) / 2, label, fg);
  return s;
}

// 清屏 + 顶栏(标题, 右上角"返回")。CLS 会关掉触摸, 所以紧跟着重新打开。
static void uiFrame(const char *title, bool ascii = false) {
  nZones = 0;
  sendPalette();
  String s = "CLS(0);TPN(1);";
  s += cmd("BOXF(0,0,319,%d,%d);BOXF(0,%d,319,%d,%d);SBC(%d);", HDR_H - 1, C_HDR, HDR_H, HDR_H + 1,
           C_AMBER, C_HDR);
  s += ascii ? asciiCmd(10, 8, title, C_WHITE) : txt(16, 10, 8, title, C_WHITE);
  s += btn(262, 4, 315, 27, C_SLATE, C_WHITE, S_BACK, 12, Z_BACK);
  s += cmd("SBC(%d);", C_BG);
  scr(s, 1000);
  needCls = true;
  shownKind = -1;
}

static void uiClose() {
  ui = UI_BOARD;
  if (wifiNeedRestart) {
    wifiNeedRestart = false;
    wifiStart();
  }
  needCls = true;
  dirty = true;
  waitingShown = false;
  tpPending = false;
}

static void setAirport(const char *code) {
  // 换机场: 旧机场的航班作废, WiFi 立刻重新取
  if (strlen(code) != 3 || strcmp(code, airport) == 0) return;
  strlcpy(airport, code, sizeof(airport));
  prefs.putString("apt", airport);
  aptName[0] = 0;  // 旧机场的名字作废, 等电脑端再给
  prefs.remove("aname");
  boards[0].n = boards[1].n = 0;
  stagingKind = -1;
  nextFetchMs = millis();
  retryOnly[0] = retryOnly[1] = false;
  fetchState = -1;
  waitState = -1;
  netFails = 0;
  dirty = true;
}

// ---- 菜单 ----
static String menuCard(int y, const char *title, const String &sub, bool subAscii, int id) {
  zone(8, y, 311, y + 51, id);
  String s = cmd("CBOF(8,%d,311,%d,6,%d);BOXF(8,%d,11,%d,%d);SBC(%d);", y, y + 51, C_ROW_B, y + 6,
                 y + 45, C_AMBER, C_ROW_B);
  s += txt(16, 22, y + 8, title, C_WHITE);
  s += subAscii ? txt(12, 22, y + 31, sub.c_str(), C_SOFT) : txt(12, 22, y + 31, sub.c_str(), C_SOFT);
  s += txt(16, 292, y + 18, ">", C_AMBER);
  return s;
}

static void uiMenu() {
  ui = UI_MENU;
  uiFrame(S_SETTINGS);
  String apt;
  const char *city = fbCityGbk(airport);
  if (city) { apt += city; apt += ' '; }
  apt += airport;
  String wf;
  if (!wifiSsid.length()) {
    wf = S_NOTCONN;
  } else {
    wf = dispName(wifiSsid, 24);
    wf.replace('\'', ' ');
    wf.replace(';', ' ');
    wf.replace(',', ' ');
    wf += "  ";
    wf += WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String(S_NOTCONN);
  }
  scr(menuCard(40, S_M_APT, apt, false, Z_APT) + menuCard(98, S_M_WIFI, wf, false, Z_WIFI));
  String s = menuCard(156, S_M_CAL, String(S_M_CAL_SUB), false, Z_CAL);
  s += cmd("SBC(%d);", C_BG);
  s += txt(12, (320 - textW(12, S_IDLE_HINT)) / 2, 220, S_IDLE_HINT, C_DIM);
  scr(s);
}

// ---- 机场 ----
static const char APTS[][4] = {
    "CAN", "PEK", "PKX", "SHA", "PVG", "SZX", "CTU", "TFU", "CKG", "KMG", "XIY", "HGH", "NKG", "WUH", "CSX", "XMN",
    "TAO", "HAK", "SYX", "URC", "TSN", "CGO", "DLC", "SHE", "HRB", "TNA", "FOC", "NNG", "KWE", "LHW", "HKG", "TPE",
};
#define APT_N ((int)(sizeof(APTS) / sizeof(APTS[0])))
#define APT_PER 16
static int aptPage = 0;

static String pager(int page, int pages, bool rescan) {
  String s = btn(4, 215, 83, 238, C_SLATE, C_WHITE, S_PREV, 12, Z_PREV);
  s += btn(236, 215, 315, 238, C_SLATE, C_WHITE, S_NEXT, 12, Z_NEXT);
  if (rescan) {
    s += btn(120, 215, 199, 238, C_SLATE, C_WHITE, S_RESCAN, 12, Z_RESCAN);
  } else {
    char b[8];
    snprintf(b, sizeof(b), "%d/%d", page + 1, pages);
    s += cmd("SBC(%d);", C_BG);
    s += txt(16, (320 - textW(16, b)) / 2, 219, b, C_SOFT);
  }
  return s;
}

static void uiAirports() {
  ui = UI_APT;
  uiFrame(S_PICK_APT);
  String s;
  for (int i = 0; i < APT_PER; i++) {
    int idx = aptPage * APT_PER + i;
    if (idx >= APT_N) break;
    int x = 4 + (i % 4) * 79, y = 38 + (i / 4) * 44;
    bool cur = strcmp(APTS[idx], airport) == 0;
    int bg = cur ? C_AMBER : C_ROW_B;
    zone(x, y, x + 75, y + 39, Z_ITEM + idx);
    s += cmd("CBOF(%d,%d,%d,%d,4,%d);SBC(%d);", x, y, x + 75, y + 39, bg, bg);
    const char *name = fbCityGbk(APTS[idx]);
    if (name) {
      int sz = textW(16, name) <= 72 ? 16 : 12;
      s += txt(sz, x + (76 - textW(sz, name)) / 2, y + (sz == 16 ? 4 : 6), name, cur ? C_INK : C_WHITE);
    }
    s += txt(12, x + (76 - 18) / 2, y + 24, APTS[idx], cur ? C_INK : C_AMBER);
    if (s.length() > 700) {
      scr(s);
      s = "";
    }
  }
  s += pager(aptPage, (APT_N + APT_PER - 1) / APT_PER, false);
  scr(s);
}

// ---- WiFi 列表 ----
#define MAXNET 15
#define NET_PER 5
static String netSsid[MAXNET];
static int8_t netRssi[MAXNET];
static bool netOpen[MAXNET];
static int netN = 0, netPage = 0, netSel = -1;

static void uiWifiList() {
  ui = UI_WIFI;
  uiFrame(S_PICK_WIFI);
  String s;
  if (!netN) s += txt(16, (320 - textW(16, S_NONET)) / 2, 110, S_NONET, C_SOFT);
  for (int i = 0; i < NET_PER; i++) {
    int idx = netPage * NET_PER + i;
    if (idx >= netN) break;
    int y = 38 + i * 35;
    bool cur = WiFi.status() == WL_CONNECTED && netSsid[idx] == wifiSsid;
    int bg = cur ? C_SCAN : C_ROW_B;
    zone(4, y, 315, y + 31, Z_ITEM + idx);
    s += cmd("CBOF(4,%d,315,%d,4,%d);SBC(%d);", y, y + 31, bg, bg);
    s += asciiCmd(12, y + 8, dispName(netSsid[idx], 28).c_str(), C_WHITE);
    if (netOpen[idx]) s += txt(12, 252, y + 10, S_OPEN, C_SOFT);
    int bars = netRssi[idx] > -55 ? 4 : netRssi[idx] > -67 ? 3 : netRssi[idx] > -78 ? 2 : 1;
    for (int b = 0; b < 4; b++)
      s += cmd("BOXF(%d,%d,%d,%d,%d);", 286 + b * 6, y + 20 - b * 4, 289 + b * 6, y + 24,
               b < bars ? C_GREEN : C_SLATE);
    if (s.length() > 700) {
      scr(s);
      s = "";
    }
  }
  s += pager(netPage, 1, true);
  scr(s);
}

static void uiWifiScan() {
  ui = UI_WIFI;
  uiFrame(S_PICK_WIFI);
  scr(txt(16, (320 - textW(16, S_SCANNING)) / 2, 110, S_SCANNING, C_AMBER));
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks();
  if (n < 0) {
    // 正在不停重连别的网络时扫不了, 先停下来再扫; 离开设置时会恢复重连
    WiFi.disconnect();
    wifiNeedRestart = true;
    delay(200);
    n = WiFi.scanNetworks();
  }
  netN = 0;
  for (int i = 0; i < n && netN < MAXNET; i++) {
    String ss = WiFi.SSID(i);
    if (!ss.length()) continue;
    bool dup = false;
    for (int j = 0; j < netN; j++) dup = dup || netSsid[j] == ss;
    if (dup) continue;
    netSsid[netN] = ss;
    netRssi[netN] = (int8_t)constrain(WiFi.RSSI(i), -127, 0);
    netOpen[netN] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
    netN++;
  }
  WiFi.scanDelete();
  netPage = 0;
  tpPending = false;  // 搜索期间乱点的不算
  uiWifiList();
}

// ---- 键盘 ----
static String kbPass;
static int kbLayer = 0;  // 0 小写 1 大写 2 数字 3 符号
static const char *const KB_R1[4] = {"qwertyuiop", "QWERTYUIOP", "1234567890", "[]{}#%^*+="};
static const char *const KB_R2[4] = {"asdfghjkl", "ASDFGHJKL", "-/:;()$&@", "_\\|~<>`&@"};
static const char *const KB_R3[4] = {"zxcvbnm", "ZXCVBNM", ".,?!'\"_", ".,?!'\"-"};
#define KB_Y0 68
#define KB_DY 43
#define KB_H 40

static String keyCmd(const Zone &z, bool hot) {
  int id = z.id;
  bool special = id >= 256;
  int bg = hot ? C_AMBER : id == Z_OK ? C_GREEN : (id == Z_SHIFT && kbLayer == 1) ? C_CYAN : special ? C_SLATE : C_ROW_B;
  int fg = (hot || id == Z_OK || (id == Z_SHIFT && kbLayer == 1)) ? C_INK : C_WHITE;
  int x1 = z.x1 + 1, x2 = z.x2 - 1, y1 = z.y1 + 1, y2 = z.y1 + KB_H;
  String s = cmd("CBOF(%d,%d,%d,%d,3,%d);SBC(%d);", x1, y1, x2, y2, bg, bg);
  int cx = (x1 + x2 + 1) / 2, ty = y1 + (KB_H - 16) / 2;
  if (!special) {
    char t[2] = {(char)id, 0};
    s += asciiCmd(cx - 4, ty, t, fg);
  } else if (id == Z_SPACE) {
    s += txt(12, cx - textW(12, S_SPACE) / 2, ty + 2, S_SPACE, C_SOFT);
  } else if (id == Z_OK) {
    s += txt(16, cx - textW(16, S_CONNECT) / 2, ty, S_CONNECT, fg);
  } else {
    const char *t = id == Z_DEL ? "<-" : id == Z_LAYER ? (kbLayer < 2 ? "123" : "abc")
                  : kbLayer < 2 ? "Aa" : kbLayer == 2 ? "#+=" : "123";
    s += txt(16, cx - textW(16, t) / 2, ty, t, fg);
  }
  return s;
}

static void kbField() {
  String s = cmd("BOXF(4,37,315,62,%d);BOX(4,37,315,62,%d);SBC(%d);", C_INK, C_LINE, C_INK);
  int n = kbPass.length();
  if (!n) {
    s += txt(12, 12, 44, S_PWD_HINT, C_DIM);
  } else {
    const char *t = kbPass.c_str() + (n > 36 ? n - 36 : 0);  // 太长只显示末尾
    s += asciiCmd(10, 42, t, C_WHITE);
  }
  int cx = 10 + (n > 36 ? 36 : n) * 8;
  s += cmd("BOXF(%d,57,%d,58,%d);", cx, cx + 6, C_AMBER);
  scr(s);
}

static void kbKeys() {
  nZones = 1;  // 保留"返回"
  for (int r = 0; r < 3; r++) {
    const char *row = (r == 0 ? KB_R1 : r == 1 ? KB_R2 : KB_R3)[kbLayer];
    int x0 = r == 0 ? 0 : r == 1 ? 16 : 48, y = KB_Y0 + r * KB_DY - 1;
    for (int i = 0; row[i]; i++) zone(x0 + i * 32, y, x0 + i * 32 + 31, y + KB_DY - 1, (uint8_t)row[i]);
  }
  int y3 = KB_Y0 + 2 * KB_DY - 1, y4 = KB_Y0 + 3 * KB_DY - 1;
  zone(0, y3, 47, y3 + KB_DY - 1, Z_SHIFT);
  zone(272, y3, 319, y3 + KB_DY - 1, Z_DEL);
  zone(0, y4, 63, y4 + KB_DY - 1, Z_LAYER);
  zone(64, y4, 207, y4 + KB_DY - 1, Z_SPACE);
  zone(208, y4, 319, y4 + KB_DY - 1, Z_OK);
  String s;
  for (int i = 1; i < nZones; i++) {
    s += keyCmd(zones[i], false);
    if (s.length() > 700) {
      scr(s);
      s = "";
    }
  }
  if (s.length()) scr(s);
}

static void uiKeyboard() {
  ui = UI_KEYB;
  uiFrame(dispName(netSsid[netSel], 28).c_str(), true);
  kbField();
  kbKeys();
}

static void pumpWait(uint32_t ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    usbPump();
    delay(10);
  }
}

static void uiConnect() {
  String oldS = wifiSsid, oldP = wifiPass;
  bool open = netOpen[netSel];
  uiFrame(S_CONNECTING);
  String s = asciiCmd((320 - 8 * (int)dispName(netSsid[netSel], 36).length()) / 2, 92,
                      dispName(netSsid[netSel], 36).c_str(), C_WHITE);
  s += cmd("BOXF(60,130,259,133,%d);", C_LINE);
  scr(s);
  wifiSsid = netSsid[netSel];
  wifiPass = open ? String() : kbPass;
  WiFi.disconnect();
  delay(150);
  wifiStart();
  wifiNeedRestart = false;
  uint32_t t0 = millis();
  int step = 0;
  while (millis() - t0 < 15000 && WiFi.status() != WL_CONNECTED) {
    pumpWait(500);
    if (++step <= 30) scr(cmd("BOXF(60,130,%d,133,%d);", 60 + step * 199 / 30, C_AMBER));
  }
  if (WiFi.status() == WL_CONNECTED) {
    prefs.putString("ssid", wifiSsid);
    prefs.putString("pass", wifiPass);
    String ip = WiFi.localIP().toString();
    s = cmd("BOXF(60,130,259,133,%d);", C_GREEN);
    s += txt(16, (320 - textW(16, S_CONN_OK)) / 2, 150, S_CONN_OK, C_GREEN);
    s += txt(16, (320 - 8 * (int)ip.length()) / 2, 176, ip.c_str(), C_SOFT);
    scr(s);
    pumpWait(2200);
    uiClose();
  } else {
    wifiSsid = oldS;
    wifiPass = oldP;
    WiFi.disconnect();
    delay(150);
    wifiStart();
    s = cmd("BOXF(60,130,259,133,%d);", C_RED);
    s += txt(16, (320 - textW(16, S_CONN_FAIL)) / 2, 150, S_CONN_FAIL, C_RED);
    scr(s);
    pumpWait(2500);
    tpPending = false;
    if (open) uiWifiList(); else uiKeyboard();
  }
  uiTouchMs = millis();
}

// 触摸校准: 屏幕自己画四个十字, 点完后从串口回 TPSET
static void uiCalibrate() {
  uiFrame(S_M_CAL);
  scr(txt(16, (320 - textW(16, S_CAL_WAIT)) / 2, 110, S_CAL_WAIT, C_AMBER));
  pumpWait(1200);
  while (Serial1.available()) Serial1.read();
  Serial1.print("TPST(320,240);\r\n");
  String r;
  uint32_t t0 = millis();
  while (millis() - t0 < 60000 && r.indexOf("TPSET") < 0) {
    while (Serial1.available()) {
      r += (char)Serial1.read();
      if (r.length() > 64) r.remove(0, 32);
    }
    usbPump();
    delay(5);
  }
  pumpWait(300);
  tpPending = false;
  uiTouchMs = millis();
  uiMenu();
}

static void kbPress(const Zone &z) {
  int id = z.id;
  scr(keyCmd(z, true));
  int layer = kbLayer;
  if (id < 256) {
    if (kbPass.length() < 63) kbPass += (char)id;
    if (kbLayer == 1) kbLayer = 0;  // 大写只管一个字母
  } else if (id == Z_SPACE) {
    if (kbPass.length() < 63) kbPass += ' ';
  } else if (id == Z_DEL) {
    if (kbPass.length()) kbPass.remove(kbPass.length() - 1);
  } else if (id == Z_SHIFT) {
    kbLayer = kbLayer == 0 ? 1 : kbLayer == 1 ? 0 : kbLayer == 2 ? 3 : 2;
  } else if (id == Z_LAYER) {
    kbLayer = kbLayer < 2 ? 2 : 0;
  }
  if (id != Z_SHIFT && id != Z_LAYER) kbField();
  if (kbLayer != layer) {
    kbKeys();
  } else {
    delay(30);
    scr(keyCmd(z, false));
  }
}

static void uiTouch(int x, int y) {
  uiTouchMs = millis();
  if (ui == UI_BOARD) {
    uiMenu();
    return;
  }
  const Zone *zp = zoneAt(x, y);
  if (!zp) return;
  Zone z = *zp;  // 下面会重建热区表, 先拷一份
  int id = z.id;
  switch (ui) {
    case UI_MENU:
      if (id == Z_BACK) uiClose();
      else if (id == Z_APT) {
        aptPage = 0;
        for (int i = 0; i < APT_N; i++)
          if (!strcmp(APTS[i], airport)) aptPage = i / APT_PER;
        uiAirports();
      } else if (id == Z_WIFI) uiWifiScan();
      else if (id == Z_CAL) uiCalibrate();
      break;
    case UI_APT: {
      int pages = (APT_N + APT_PER - 1) / APT_PER;
      if (id == Z_BACK) uiMenu();
      else if (id == Z_PREV) { aptPage = (aptPage + pages - 1) % pages; uiAirports(); }
      else if (id == Z_NEXT) { aptPage = (aptPage + 1) % pages; uiAirports(); }
      else if (id >= Z_ITEM && id < Z_ITEM + APT_N) {
        setAirport(APTS[id - Z_ITEM]);
        Serial.printf("$AP,%s\n", airport);  // 告诉电脑端跟着换
        uiClose();
      }
      break;
    }
    case UI_WIFI: {
      int pages = max(1, (netN + NET_PER - 1) / NET_PER);
      if (id == Z_BACK) uiMenu();
      else if (id == Z_RESCAN) uiWifiScan();
      else if (id == Z_PREV) { netPage = (netPage + pages - 1) % pages; uiWifiList(); }
      else if (id == Z_NEXT) { netPage = (netPage + 1) % pages; uiWifiList(); }
      else if (id >= Z_ITEM && id < Z_ITEM + netN) {
        netSel = id - Z_ITEM;
        kbPass = "";
        kbLayer = 0;
        if (netOpen[netSel]) uiConnect(); else uiKeyboard();
      }
      break;
    }
    case UI_KEYB:
      if (id == Z_BACK) uiWifiList();
      else if (id == Z_OK) { scr(keyCmd(z, true)); uiConnect(); }
      else kbPress(z);
      break;
    default:
      break;
  }
}

// ---------------- USB 协议 ----------------
// 逗号分隔取下一段; 最后一段(城市名)原样保留
static char *nextField(char **p) {
  if (!*p) return (char *)"";
  char *s = *p;
  char *c = strchr(s, ',');
  if (c) {
    *c = 0;
    *p = c + 1;
  } else {
    *p = nullptr;
  }
  return s;
}

static void handleLine(char *line) {
  if (line[0] != '$') return;
  char c = line[1];
  char *p = line[2] == ',' ? line + 3 : nullptr;

  if (c == '?') {
#ifdef DISPLAY_TFT
    const int tftScreen = 1;
#else
    const int tftScreen = 0;
#endif
    Serial.printf("$ID,flightboard,2,wifi=%d,ip=%s,dep=%d,arr=%d,key=%d,apt=%s,tft=%d,al=%d,ap=%d\n",
                  wifiSsid.length() ? (WiFi.status() == WL_CONNECTED ? 2 : 1) : 0,
                  WiFi.localIP().toString().c_str(), boards[0].n, boards[1].n,
                  alKey.length() ? 1 : 0, airport, tftScreen, alKey.length() ? 1 : 0, apOn ? 1 : 0);
    Serial.printf("$FD,D=%s,A=%s,next=%ld,now=%u\n", fetchDiag[0], fetchDiag[1],
                  (long)((int32_t)(nextFetchMs - millis()) / 1000), (unsigned)nowEpoch());
  } else if (c == 'T') {
    struct timeval tv = {(time_t)strtoul(nextField(&p), nullptr, 10), 0};
    if (tv.tv_sec > 1700000000) settimeofday(&tv, nullptr);
    if (p && *p) {
      int m = atoi(p);
      if (m != tzMin && m >= -720 && m <= 840) {
        applyTz(m);
        prefs.putShort("tz", (int16_t)m);
        needCls = true;  // 所有时间都变了, 整屏重画
        dirty = true;
      }
    }
    if (!lastUsbMs || millis() - lastUsbMs >= USB_FRESH_MS) usbSinceMs = millis() | 1;  // 电脑刚连上
    lastUsbMs = millis() | 1;  // 电脑在线: 取数归它管, 屏幕不再自己走 WiFi 取(省额度)
  } else if (c == 'B') {
    stagingKind = *nextField(&p) == 'A' ? 1 : 0;
    staging.n = 0;
    staging.dataTime = strtoul(nextField(&p), nullptr, 10);
  } else if (c == 'F') {
    int kind = *nextField(&p) == 'A' ? 1 : 0;
    if (kind != stagingKind || staging.n >= MAXF) return;
    Flight &f = staging.f[staging.n];
    memset(&f, 0, sizeof(f));
    f.sched = strtoul(nextField(&p), nullptr, 10);
    f.est = strtoul(nextField(&p), nullptr, 10);
    strlcpy(f.fn, nextField(&p), sizeof(f.fn));
    strlcpy(f.iata, nextField(&p), sizeof(f.iata));
    strlcpy(f.term, nextField(&p), sizeof(f.term));
    f.st = *nextField(&p);
    strlcpy(f.city, p ? p : "", sizeof(f.city));
    clean(f.fn);
    clean(f.city);
    clean(f.term);
    if (f.sched) staging.n++;
  } else if (c == 'E') {
    int kind = *nextField(&p) == 'A' ? 1 : 0;
    if (kind == stagingKind) {
      commit(kind, false);
      lastUsbMs = lastUsbDataMs = millis() | 1;
      Serial.printf("$OK,%c,%d\n", kind ? 'A' : 'D', boards[kind].n);
    }
    stagingKind = -1;
  } else if (c == 'A') {
    if (p) setAirport(p);
  } else if (c == 'C') {
    uint8_t v = (uint8_t)constrain(atoi(p ? p : "0"), 0, 2);
    if (v != nameLang) {
      nameLang = v;
      prefs.putUChar("lang", v);
      needCls = true;  // 地名全变了, 整屏重画
      dirty = true;
    }
  } else if (c == 'L') {
    // 机场的英文城市名(电脑端每次推送都带), 变了才存
    char nm[sizeof(aptName)];
    strlcpy(nm, p ? p : "", sizeof(nm));
    clean(nm);
    if (strcmp(nm, aptName) != 0) {
      strlcpy(aptName, nm, sizeof(aptName));
      prefs.putString("aname", aptName);
      shownKind = -1;  // 顶栏重画
      dirty = true;
    }
  } else if (c == 'N') {
    // 网络诊断: 解析主机名, 再试 443 端口的 TCP 和 TLS 握手
    const char *h = p ? p : "airlabs.co";
    ensureDns();
    IPAddress ip;
    uint32_t t0 = millis();
    int dns = WiFi.hostByName(h, ip);
    uint32_t tDns = millis() - t0;
    WiFiClient tcp;
    t0 = millis();
    int tc = dns == 1 ? tcp.connect(ip, 443, 6000) : -9;
    uint32_t tTcp = millis() - t0;
    tcp.stop();
    WiFiClientSecure tls;
    tls.setInsecure();
    t0 = millis();
    int tl = dns == 1 ? tls.connect(h, 443) : -9;
    uint32_t tTls = millis() - t0;
    char eb[40] = "";
    int le = tls.lastError(eb, sizeof(eb));
    tls.stop();
    Serial.printf("$N,%s,dnsip=%s,dns=%d(%ums),ip=%s,tcp=%d(%ums),tls=%d(%ums),err=%d %s,rssi=%d\n", h,
                  WiFi.dnsIP().toString().c_str(), dns, (unsigned)tDns, ip.toString().c_str(), tc,
                  (unsigned)tTcp, tl, (unsigned)tTls, le, eb, WiFi.RSSI());
  } else if (c == 'X') {
    // 射频试验: $X,p,<功率 0.25dBm> / $X,s,<省电 0|1> / $X,r 重连
    char w = *nextField(&p);
    int v = atoi(nextField(&p));
    if (w == 'p') WiFi.setTxPower((wifi_power_t)v);
    else if (w == 's') WiFi.setSleep(v != 0);
    else if (w == 'r') WiFi.reconnect();
    Serial.printf("$X,%c,%d,pw=%d\n", w, v, (int)WiFi.getTxPower());
  } else if (c == 'Z') {
    ESP.restart();
  } else if (c == 'O') {
    // 屏幕倒着装的话转 180 度: $O,0 / $O,1, 存 flash
    uint8_t r = (uint8_t)(atoi(nextField(&p)) & 1);
    prefs.putUChar("rot", r);
#ifdef DISPLAY_TFT
    tft::setRotation(r);
#else
    scrDir = r ? 2 : 0;
    screenInit();
#endif
    needCls = true;
    dirty = true;
    Serial.printf("$OK,O,%d\n", r);
  } else if (c == 'G') {
    // 立刻走一轮 WiFi 取数(测试用, 扣 2 次额度)
    lastUsbMs = 0;
    nextFetchMs = millis();
  } else if (c == 'P') {
    // 模拟一次触摸(电脑端预览/测试用)
    tpX = atoi(nextField(&p));
    tpY = atoi(nextField(&p));
    tpPending = true;
  } else if (c == 'J') {
    // AirLabs 密钥(电脑每次推送都会带), 板子脱离电脑后自己取数用; 变了才写 flash
    if (p && *p && alKey != p) {
      alKey = p;
      prefs.putString("alkey", alKey);
    }
  } else if (c == 'M') {
    // 镜像开关: 开的同时整屏重画一遍, 电脑端才能拿到完整画面
    mirror = p && *p == '1';
    needCls = true;
    dirty = true;
    Serial.printf("$OK,M,%d\n", mirror ? 1 : 0);
  } else if (c == 'R') {
    // 透传一行原始屏幕命令, 把屏幕的回应原样带回(调试用)
    if (p) {
      while (Serial1.available()) Serial1.read();
      Serial1.print(p);
      Serial1.print("\r\n");
      String r;
      uint32_t t0 = millis();
      while (millis() - t0 < 800) {
        while (Serial1.available()) r += (char)Serial1.read();
        delay(2);
      }
      Serial.print("$R,");
      Serial.print(r);
      Serial.print("\n");
    }
  } else if (c == 'W') {
    if (line[2] == '-') {
      prefs.remove("ssid");
      prefs.remove("pass");
      wifiSsid = wifiPass = "";
      WiFi.disconnect(true);
      Serial.println("$OK,W,cleared");
    } else if (p) {
      char *tab = strchr(p, '\t');
      if (tab) *tab = 0;
      wifiSsid = p;
      wifiPass = tab ? tab + 1 : "";
      prefs.putString("ssid", wifiSsid);
      prefs.putString("pass", wifiPass);
      WiFi.disconnect();
      wifiStart();
      Serial.println("$OK,W,saved");
    }
  }
}

static void pollUsb() {
  static char buf[160];
  static int len = 0;
  usbPump();
  while (usbTail != usbHead) {
    char c = (char)usbRing[usbTail];
    usbTail = (uint16_t)((usbTail + 1) % sizeof(usbRing));
    if (c == '\n' || c == '\r') {
      if (len) {
        buf[len] = 0;
        handleLine(buf);
        len = 0;
      }
    } else if (len < (int)sizeof(buf) - 1) {
      buf[len++] = c;
    }
  }
}

#include "portal.h"

// ---------------- 主循环 ----------------
void setup() {
  Serial.setRxBufferSize(8192);
  Serial.begin(115200);
  applyTz(480);

  prefs.begin("fb", false);
  applyTz(prefs.getShort("tz", 480));
#ifdef DISPLAY_TFT
  tft::begin(prefs.getUChar("rot", 0));
#else
  Serial1.begin(SCR_BAUD, SERIAL_8N1, SCR_RX, SCR_TX);
  if (prefs.isKey("rot")) scrDir = prefs.getUChar("rot", 0) ? 2 : 0;
  delay(1200);  // 屏幕上电比 ESP 慢
#endif
  screenInit();

  wifiSsid = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  prefs.remove("key");  // 旧版固件存的另一个数据源的密钥, 不再用了
  alKey = prefs.getString("alkey", "");
  strlcpy(airport, prefs.getString("apt", DEFAULT_AIRPORT).c_str(), sizeof(airport));
  strlcpy(aptName, prefs.getString("aname", "").c_str(), sizeof(aptName));
  nameLang = prefs.getUChar("lang", 0);
  wifiStart();
  portalBegin();
}

void loop() {
  static int kind = 0, page = 0;
  static uint32_t lastDraw = 0;

  pollUsb();
  portalTask();
  wifiTask();
  while (Serial1.available()) scrRx((char)Serial1.read());
  if (tpPending) {
    tpPending = false;
    uiTouch(tpX, tpY);
  }
  if (ui != UI_BOARD) {
    // 设置界面开着: 航班牌暂停刷新(数据照收), 太久不操作自动回去
    if (millis() - uiTouchMs > UI_IDLE_MS) uiClose();
    return;
  }

  bool have = boards[0].n || boards[1].n;

  if (!have) {
    // 没数据: 等待画面, 每 10 秒刷一次(时钟/WiFi 状态会变)
    if (!waitingShown || dirty || millis() - lastDraw > 10000) {
      drawWaiting();
      waitingShown = true;
      dirty = false;
      lastDraw = millis();
    }
    return;
  }

  bool turn = millis() - lastDraw >= PAGE_MS;
  bool dataOnly = dirty && !turn && !waitingShown;  // 同一页上数据变了: 只扫变化的行, 不重置翻页计时
  if (waitingShown || dirty || turn) {
    computeVis(0);
    computeVis(1);
    if (waitingShown) {
      waitingShown = false;
      needCls = true;
      kind = boards[0].n ? 0 : 1;
      page = 0;
    } else if (turn) {
      // 翻页: 本方向翻完换另一个方向
      if (++page >= pagesOf(kind)) {
        page = 0;
        if (boards[1 - kind].n) kind = 1 - kind;
      }
    }
    if (page >= pagesOf(kind)) page = 0;
    dirty = false;
    if (dataOnly && !needCls && shownKind == kind) {
      drawRows(kind, page);
      drawFooter(kind, page, pagesOf(kind));
    } else {
      drawPage(kind, page);
      lastDraw = millis();
    }
  } else {
    progressTick(kind, millis() - lastDraw);
  }
  if (curMinute() != shownMin) {
    scr(clockCmd());  // 只刷时钟
    shownMin = curMinute();
  }
}
