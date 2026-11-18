// TFT 直驱后端: 把航班牌原本发给 UsartGPU 串口屏的命令行, 在本机解释并画到 ST7789 上。
// 这样版面代码(main.cpp)一行不用改, 串口屏版和 TFT 版共用同一套界面。
//
// 屏: 从蓝牙终端上拆下来的 2.4 寸 ST7789V, 240x320, 横放当 320x240 用。
// 接线: SCL=GPIO2 SDA=GPIO3 DC=GPIO4 RES=GPIO5 CS=GPIO7, 3.3V。
// 3.3 寸 ST7789 模块(同样 320x240)接法相同, 背光 BLK 接 GPIO6; 2.4 寸那块背光常亮, 不接这根。
// 这块屏的脾气: 要开反色(0x21); 硬复位之后不能再发软复位(0x01), 否则后面的命令全不认。
#pragma once
#include <Arduino.h>
#include <SPI.h>

#include "fb_font.h"  // tools/make_font.py 用开源字体生成, 随仓库分发
#include "thai.h"
// 航司标志是各航司的商标, 不进仓库; 想要的话运行 tools/make_logos.py 自己生成。
// 没有的时候固件照样能编译, 航班行里用彩色徽章(航司两字码)代替标志。
#if __has_include("fb_logos.h")
#include "fb_logos.h"
#else
#define FB_LOGO_N 0
#define FB_LOGO_W 22
#define FB_LOGO_H 17
static const char FB_LOGO_CODE[] PROGMEM = "";
static const uint16_t FB_LOGO_PX[1] PROGMEM = {0};
#endif

#define TFT_SCK 2
#define TFT_SDA 3
#define TFT_DC 4
#define TFT_RST 5
#define TFT_CS 7
#define TFT_BL 6
#define TFT_HZ 26000000
#define TFT_W 320
#define TFT_H 240

namespace tft {

static uint16_t pal[64] = {0x0000, 0xF800, 0x07E0, 0x001F, 0xFFE0, 0x07FF, 0xF81F, 0xCE79,
                           0x4208, 0xC208, 0x4608, 0x4218, 0xCE68, 0x4679, 0xCA39, 0xFFFF};
static uint16_t bgColor = 0;  // SBC 设的文字底色
static uint8_t rotation = 0;  // 0 / 1 = 横屏的两个方向
static bool thaiMode = false; // 泰文模式: 文字里 0xA1..0xFB 的字节按 TIS-620 泰文解释(否则按 GBK 汉字)
static uint16_t buf[TFT_W];   // 一行像素, 已换成大端

static inline uint16_t be(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }
static inline void wcmd(uint8_t c) {
  digitalWrite(TFT_DC, LOW);
  SPI.transfer(c);
  digitalWrite(TFT_DC, HIGH);
}

static void window(int x, int y, int w, int h) {
  int x1 = x + w - 1, y1 = y + h - 1;
  uint8_t a[4] = {(uint8_t)(x >> 8), (uint8_t)x, (uint8_t)(x1 >> 8), (uint8_t)x1};
  uint8_t b[4] = {(uint8_t)(y >> 8), (uint8_t)y, (uint8_t)(y1 >> 8), (uint8_t)y1};
  wcmd(0x2A); SPI.writeBytes(a, 4);
  wcmd(0x2B); SPI.writeBytes(b, 4);
  wcmd(0x2C);
}

static void fillRect(int x, int y, int w, int h, uint16_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > TFT_W) w = TFT_W - x;
  if (y + h > TFT_H) h = TFT_H - y;
  if (w <= 0 || h <= 0) return;
  uint16_t s = be(c);
  for (int i = 0; i < w; i++) buf[i] = s;
  window(x, y, w, h);
  for (int r = 0; r < h; r++) SPI.writeBytes((const uint8_t *)buf, w * 2);
}

static void setRotation(uint8_t r) {
  rotation = r & 1;
  wcmd(0x36);
  SPI.transfer(rotation ? 0xA0 : 0x60);  // MV+MY / MV+MX: 横屏的两个朝向
}

static void begin(uint8_t rot) {
  pinMode(TFT_CS, OUTPUT); digitalWrite(TFT_CS, HIGH);
  pinMode(TFT_DC, OUTPUT); digitalWrite(TFT_DC, HIGH);
  pinMode(TFT_RST, OUTPUT);
  // 冷上电时屏的电源比 ESP 起得慢, 复位发早了它收不到, 之后整屏不亮; 先等电源稳住
  digitalWrite(TFT_RST, HIGH); delay(600);
  digitalWrite(TFT_RST, LOW); delay(60);
  digitalWrite(TFT_RST, HIGH); delay(200);
  SPI.begin(TFT_SCK, -1, TFT_SDA, -1);
  SPI.beginTransaction(SPISettings(TFT_HZ, MSBFIRST, SPI_MODE0));  // 这条总线只有屏, 一直占着
  digitalWrite(TFT_CS, LOW);
  wcmd(0x11); delay(150);          // 退出睡眠
  wcmd(0x3A); SPI.transfer(0x55);  // 16 位色
  setRotation(rot);
  wcmd(0x21);                      // 反色开
  wcmd(0x13);
  fillRect(0, 0, TFT_W, TFT_H, 0);
  wcmd(0x29);                      // 开显示
  pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH);  // 清完屏再开背光, 不闪花屏
}

// ---------- 图形 ----------
static void line(int x0, int y0, int x1, int y1, uint16_t c) {
  if (y0 == y1) { fillRect(min(x0, x1), y0, abs(x1 - x0) + 1, 1, c); return; }
  if (x0 == x1) { fillRect(x0, min(y0, y1), 1, abs(y1 - y0) + 1, c); return; }
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, e = dx + dy;
  for (;;) {
    fillRect(x0, y0, 1, 1, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * e;
    if (e2 >= dy) { e += dy; x0 += sx; }
    if (e2 <= dx) { e += dx; y0 += sy; }
  }
}

// 圆角矩形: 四角按圆弧每行往里缩
static void roundFill(int x1, int y1, int x2, int y2, int r, uint16_t c) {
  int w = x2 - x1 + 1, h = y2 - y1 + 1;
  if (r * 2 > w) r = w / 2;
  if (r * 2 > h) r = h / 2;
  for (int i = 0; i < r; i++) {
    float d = r - i - 0.5f;
    int inset = r - (int)(sqrtf(r * r - d * d) + 0.5f);
    fillRect(x1 + inset, y1 + i, w - 2 * inset, 1, c);
    fillRect(x1 + inset, y2 - i, w - 2 * inset, 1, c);
  }
  fillRect(x1, y1 + r, w, h - 2 * r, c);
}

static void circle(int cx, int cy, int r, uint16_t c, bool fill) {
  for (int dy = -r; dy <= r; dy++) {
    int dx = (int)(sqrtf((float)(r * r - dy * dy)) + 0.5f);
    if (fill) {
      fillRect(cx - dx, cy + dy, 2 * dx + 1, 1, c);
    } else {
      int prev = abs(dy) == r ? 0 : (int)(sqrtf((float)(r * r - (abs(dy) + 1) * (abs(dy) + 1))) + 0.5f);
      int span = max(1, dx - prev);
      fillRect(cx - dx, cy + dy, span, 1, c);
      fillRect(cx + dx - span + 1, cy + dy, span, 1, c);
    }
  }
}

// ---------- 文字 ----------
// 16 级灰度在底色和字色之间插值, 得到平滑的字边
static void makeLut(uint16_t fg, uint16_t bg, uint16_t *lut) {
  int fr = fg >> 11, fgn = (fg >> 5) & 63, fb = fg & 31;
  int br = bg >> 11, bgn = (bg >> 5) & 63, bb = bg & 31;
  for (int a = 0; a < 16; a++) {
    int r = br + (fr - br) * a / 15, g = bgn + (fgn - bgn) * a / 15, b = bb + (fb - bb) * a / 15;
    lut[a] = be((uint16_t)((r << 11) | (g << 5) | b));
  }
}

// 画一个字模: w x h 的格子, sx/sy 整数倍放大
static void glyph(int x, int y, const uint8_t *g, int w, int h, const uint16_t *lut, int sx, int sy) {
  if (x < 0 || y < 0 || x + w * sx > TFT_W || y + h * sy > TFT_H) return;
  int stride = (w + 1) / 2;
  window(x, y, w * sx, h * sy);
  for (int r = 0; r < h; r++) {
    const uint8_t *row = g + r * stride;
    int n = 0;
    for (int c = 0; c < w; c++) {
      uint8_t v = pgm_read_byte(row + c / 2);
      uint16_t px = lut[(c & 1) ? (v & 15) : (v >> 4)];
      for (int k = 0; k < sx; k++) buf[n++] = px;
    }
    for (int k = 0; k < sy; k++) SPI.writeBytes((const uint8_t *)buf, n * 2);
  }
}

static const uint8_t *hanzi(int size, uint16_t code) {
  int lo = 0, hi = FB_HZ_N - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    uint16_t v = pgm_read_word(&FB_HZ_CODE[mid]);
    if (v == code) return (size == 12 ? FB_HZ12 : FB_HZ16) + (size_t)mid * (size == 12 ? 72 : 128);
    if (v < code) lo = mid + 1; else hi = mid - 1;
  }
  return nullptr;
}

// ---------- 泰文 ----------
// 排版在 thai.h(和串口屏共用)。泰文比汉字高, 往上多占 3 行, 多出来的行没有笔画就不画(不会盖掉上面的东西)。
static void thaiText(int size, int x, int y, const char *s, int n, uint16_t fg) {
  int h = size + 4;
  int pen = thaiCompose(size, s, n);
  if (pen > TFT_W - x) pen = TFT_W - x;
  if (pen <= 0) return;
  uint16_t lut[16];
  makeLut(fg, bgColor, lut);
  for (int r = 0; r < h; r++) {
    int Y = y - 3 + r;
    bool any = false;
    for (int c = 0; c < pen && !any; c++) any = thLine[r][c] != 0;
    // 字号以内的行照常画(带底色, 和汉字一致); 多出来的上下几行只画有笔画的
    if ((r < 3 || r >= 3 + size) && !any) continue;
    if (Y < 0 || Y >= TFT_H) continue;
    for (int c = 0; c < pen; c++) buf[c] = lut[thLine[r][c]];
    window(x, Y, pen, 1);
    SPI.writeBytes((const uint8_t *)buf, pen * 2);
  }
}

// ---------- 英文单词 ----------
// 纯英文、而且有小写字母的(地名、状态、表头), 用窄体按字宽排; 时间、航班号、代码这类仍用等宽字。
static bool wordy(const char *s, int n) {
  bool lower = false;
  for (int i = 0; i < n; i++) {
    uint8_t b = (uint8_t)s[i];
    if (b >= 0x80) return false;
    if (b >= 'a' && b <= 'z') lower = true;
  }
  return lower;
}

static int latinWidth(int size, const char *s, int n) {
  const uint8_t *adv = size == 16 ? FB_LAT16_ADV : FB_LAT12_ADV;
  int w = 0;
  for (int i = 0; i < n; i++) {
    uint8_t c = (uint8_t)s[i];
    w += pgm_read_byte(&adv[(c < 0x20 || c > 0x7E ? '?' : c) - 0x20]);
  }
  return w;
}

static void latinText(int size, int x, int y, const char *s, int n, const uint16_t *lut) {
  int gw = size == 16 ? FB_LAT_GW16 : FB_LAT_GW12, stride = (gw + 1) / 2;
  const uint8_t *font = size == 16 ? FB_LAT16 : FB_LAT12, *adv = size == 16 ? FB_LAT16_ADV : FB_LAT12_ADV;
  int w = latinWidth(size, s, n);
  if (x + w > TFT_W) w = TFT_W - x;
  if (w <= 0 || y < 0 || y + size > TFT_H) return;
  // 一行一行合成: 字模比字宽宽一点, 相邻字母的笔画取最大值叠起来
  static uint8_t row[TFT_W];
  window(x, y, w, size);
  for (int r = 0; r < size; r++) {
    memset(row, 0, w);
    int pen = 0;
    for (int i = 0; i < n && pen < w; i++) {
      uint8_t c = (uint8_t)s[i];
      int idx = (c < 0x20 || c > 0x7E ? '?' : c) - 0x20;
      const uint8_t *g = font + (size_t)idx * stride * size + r * stride;
      for (int k = 0; k < gw && pen + k < w; k++) {
        uint8_t v = pgm_read_byte(g + k / 2);
        uint8_t a = (k & 1) ? (v & 15) : (v >> 4);
        if (a > row[pen + k]) row[pen + k] = a;
      }
      pen += pgm_read_byte(&adv[idx]);
    }
    for (int k = 0; k < w; k++) buf[k] = lut[row[k]];
    SPI.writeBytes((const uint8_t *)buf, w * 2);
  }
}

// 屏幕上一段文字的宽度(和 text() 的画法一致)
static int textWidth(int size, const char *s) {
  int n = strlen(s);
  if (thaiMode) return thaiWidth(size, s);
  if (wordy(s, n)) return latinWidth(size, s, n);
  int w = 0;
  for (int i = 0; i < n; i++) {
    if ((uint8_t)s[i] >= 0x81 && i + 1 < n) { w += size; i++; } else w += size / 2;
  }
  return w;
}

// GBK 文字。size 12 或 16; 放大 2x2 的 16 号 ASCII 改用真正的 32 号字模, 不出锯齿。
static void text(int size, int x, int y, const char *s, int n, uint16_t fg, int sx, int sy) {
  if (thaiMode && !(size == 16 && sx == 2)) {  // 顶栏的大时钟永远是数字, 不走泰文
    thaiText(size, x, y, s, n, fg);
    return;
  }
  uint16_t lut[16];
  makeLut(fg, bgColor, lut);
  if (sx == 1 && sy == 1 && wordy(s, n)) {
    latinText(size, x, y, s, n, lut);
    return;
  }
  bool big = size == 16 && sx == 2 && sy == 2;
  for (int i = 0; i < n; i++) {
    uint8_t c = (uint8_t)s[i];
    if (c >= 0x81 && i + 1 < n) {
      const uint8_t *g = hanzi(size, (uint16_t)((c << 8) | (uint8_t)s[i + 1]));
      if (g) glyph(x, y, g, size, size, lut, sx, sy);
      else fillRect(x + 2, y + 2, size * sx - 4, size * sy - 4, fg);  // 字库里没有的字画个方块
      x += size * sx;
      i++;
    } else {
      if (c < 0x20 || c > 0x7E) c = '?';
      if (big) {
        glyph(x, y, FB_ASC32 + (c - 0x20) * 8 * 32, 16, 32, lut, 1, 1);
      } else {
        const uint8_t *g = size == 12 ? FB_ASC12 + (c - 0x20) * 3 * 12 : FB_ASC16 + (c - 0x20) * 4 * 16;
        glyph(x, y, g, size / 2, size, lut, sx, sy);
      }
      x += size / 2 * sx;
    }
  }
}

// ---------- 航司图标 ----------
// 按两字码找图标, 没有返回 -1
static int findLogo(const char *code) {
  for (int i = 0; i < FB_LOGO_N; i++)
    if (pgm_read_byte(&FB_LOGO_CODE[i * 2]) == code[0] && pgm_read_byte(&FB_LOGO_CODE[i * 2 + 1]) == code[1])
      return i;
  return -1;
}

// 白底小牌。第一行和最后一行两端各少画 1 像素, 四角就是圆的。
static void logo(int x, int y, int idx) {
  if (idx < 0 || idx >= FB_LOGO_N) return;
  const uint16_t *px = FB_LOGO_PX + (size_t)idx * FB_LOGO_W * FB_LOGO_H;
  for (int r = 0; r < FB_LOGO_H; r++) {
    int inset = (r == 0 || r == FB_LOGO_H - 1) ? 1 : 0, n = FB_LOGO_W - 2 * inset;
    for (int c = 0; c < n; c++) buf[c] = be(pgm_read_word(px + r * FB_LOGO_W + inset + c));
    window(x + inset, y + r, n, 1);
    SPI.writeBytes((const uint8_t *)buf, n * 2);
  }
}

// ---------- 命令解释 ----------
// 语法: NAME(arg,arg,'文字',...);  文字用单引号, GBK 的第二个字节不会撞上 ' , ; 这些符号
static void run(const char *p) {
  while (*p) {
    char name[8];
    int nl = 0;
    while (*p && *p != '(' && *p != ';') {
      if (nl < 7 && *p > ' ') name[nl++] = *p;
      p++;
    }
    name[nl] = 0;
    long a[8] = {0};
    const char *str = nullptr;
    int strLen = 0, na = 0;
    if (*p == '(') {
      p++;
      while (*p && *p != ')') {
        if (*p == '\'') {
          str = ++p;
          while (*p && *p != '\'') p++;
          strLen = (int)(p - str);
          if (*p) p++;
          if (na < 8) a[na++] = 0;  // 文字占一个参数位
        } else if (*p == ',' || *p == ' ') {
          p++;
        } else {
          char *e;
          long v = strtol(p, &e, 10);
          if (e == p) { p++; continue; }
          if (na < 8) a[na++] = v;
          p = e;
        }
      }
      if (*p == ')') p++;
    }
    if (*p == ';') p++;
    if (!nl) continue;
#define COL(i) pal[a[i] & 63]
    if (!strcmp(name, "SCC")) pal[a[0] & 63] = (uint16_t)a[1];
    else if (!strcmp(name, "SBC")) bgColor = COL(0);
    else if (!strcmp(name, "CLS")) fillRect(0, 0, TFT_W, TFT_H, COL(0));
    else if (!strcmp(name, "BOXF")) fillRect(a[0], a[1], a[2] - a[0] + 1, a[3] - a[1] + 1, COL(4));
    else if (!strcmp(name, "BOX")) {
      fillRect(a[0], a[1], a[2] - a[0] + 1, 1, COL(4));
      fillRect(a[0], a[3], a[2] - a[0] + 1, 1, COL(4));
      fillRect(a[0], a[1], 1, a[3] - a[1] + 1, COL(4));
      fillRect(a[2], a[1], 1, a[3] - a[1] + 1, COL(4));
    } else if (!strcmp(name, "CBOF")) roundFill(a[0], a[1], a[2], a[3], a[4], COL(5));
    else if (!strcmp(name, "PL")) line(a[0], a[1], a[2], a[3], COL(4));
    else if (!strcmp(name, "PS")) fillRect(a[0], a[1], 1, 1, COL(2));
    else if (!strcmp(name, "CIR")) circle(a[0], a[1], a[2], COL(3), false);
    else if (!strcmp(name, "CIRF")) circle(a[0], a[1], a[2], COL(3), true);
    else if (!strcmp(name, "DS12") && str) text(12, a[0], a[1], str, strLen, COL(3), 1, 1);
    else if (!strcmp(name, "DS16") && str) text(16, a[0], a[1], str, strLen, COL(3), 1, 1);
    else if (!strcmp(name, "DSD") && str) text(a[0] == 12 ? 12 : 16, a[1], a[2], str, strLen, COL(6), a[4], a[5]);
    else if (!strcmp(name, "LOGO")) logo(a[0], a[1], a[2]);
    else if (!strcmp(name, "LANG")) thaiMode = a[0] == 2;   // TFT 版自己加的命令: 0 中 1 英 2 泰  // TFT 版自己加的命令, 串口屏没有
    // 其余(TPN 等串口屏专用命令)忽略
#undef COL
  }
}

}  // namespace tft
