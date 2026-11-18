// 泰文排版: 彩屏(tft.h)和串口屏(main.cpp)共用。字模在 fb_font.h(tools/make_font.py 生成)。
// 规则和 tools/make_font.py 的说明一致: 字母按字宽排开; 上下符号不占宽度, 右边缘对齐前一个字母
// (ป ฝ ฟ ฬ 再往左让 2 像素); 声调跟在上方元音后面时用抬高的那套。整行先在内存里合成,
// 泰文比汉字高, 往上多占 3 行。
#pragma once
#include <Arduino.h>
#include "fb_font.h"

#define TH_W 320
static inline bool thMark(uint16_t cp) { return cp == 0x0E31 || (cp >= 0x0E34 && cp <= 0x0E3A) || (cp >= 0x0E47 && cp <= 0x0E4E); }
static inline bool thUpper(uint16_t cp) { return cp == 0x0E31 || (cp >= 0x0E34 && cp <= 0x0E37) || cp == 0x0E47 || cp == 0x0E4D; }
static inline bool thTall(uint16_t cp) { return cp == 0x0E1B || cp == 0x0E1D || cp == 0x0E1F || cp == 0x0E2C; }

static int thaiWidth(int size, const char *s, int n = -1) {
  const uint8_t *adv = size == 16 ? FB_TH16_ADV : FB_TH12_ADV;
  int w = 0;
  for (int i = 0; n < 0 ? s[i] != 0 : i < n; i++) {
    uint8_t b = (uint8_t)s[i];
    if (b < 0x80) w += size / 2;
    else if (b >= 0xA1 && b <= 0xFB) w += pgm_read_byte(&adv[b == 0xD3 ? 0x0E32 - 0x0E01 : b - 0xA1]);
  }
  return w;
}

static uint8_t thLine[20][TH_W];  // 一行泰文的灰度(0..15)

static void thPut(int size, int idx, int gx) {
  int gw = size == 16 ? FB_TH_GW16 : FB_TH_GW12, h = size + 4, stride = (gw + 1) / 2;
  const uint8_t *g = (size == 16 ? FB_TH16 : FB_TH12) + (size_t)idx * stride * h;
  for (int r = 0; r < h; r++)
    for (int c = 0; c < gw; c++) {
      int X = gx + c;
      if (X < 0 || X >= TH_W) continue;
      uint8_t v = pgm_read_byte(g + r * stride + c / 2);
      uint8_t a = (c & 1) ? (v & 15) : (v >> 4);
      if (a > thLine[r][X]) thLine[r][X] = a;
    }
}

// 把一段泰文排进 thLine(灰度 0..15), 返回宽度
static int thaiCompose(int size, const char *s, int n) {
  const uint8_t *adv = size == 16 ? FB_TH16_ADV : FB_TH12_ADV, *ink = size == 16 ? FB_TH16_INK : FB_TH12_INK;
  memset(thLine, 0, sizeof(thLine));
  int pen = 0, baseX = -1, baseInk = 0;
  uint16_t baseCp = 0;
  bool upper = false;
  auto mark = [&](int idx) {
    if (baseX < 0) return;
    thPut(size, idx, baseX + baseInk - pgm_read_byte(&ink[idx]) - (thTall(baseCp) ? 2 : 0));
  };
  for (int i = 0; i < n && pen < TH_W; i++) {
    uint8_t b = (uint8_t)s[i];
    if (b < 0x80) {  // 夹在泰文里的数字、空格、点: 用英文字模
      uint8_t c = (b < 0x20 || b > 0x7E) ? '?' : b;
      int cw = size / 2, stride = (cw + 1) / 2;
      const uint8_t *g = size == 12 ? FB_ASC12 + (c - 0x20) * 3 * 12 : FB_ASC16 + (c - 0x20) * 4 * 16;
      for (int r = 0; r < size; r++)
        for (int k = 0; k < cw && pen + k < TH_W; k++) {
          uint8_t v = pgm_read_byte(g + r * stride + k / 2);
          uint8_t a = (k & 1) ? (v & 15) : (v >> 4);
          if (a > thLine[r + 3][pen + k]) thLine[r + 3][pen + k] = a;
        }
      pen += cw;
      baseX = -1;
      continue;
    }
    if (b < 0xA1 || b > 0xFB) continue;
    uint16_t cp = b - 0xA1 + 0x0E01;
    int idx = cp - 0x0E01;
    if (cp == 0x0E33) {  // ำ = ํ 叠在前一个字母上 + า
      mark(0x0E4D - 0x0E01);
      cp = 0x0E32;
      idx = cp - 0x0E01;
    }
    if (thMark(cp)) {
      if (upper && cp >= 0x0E48 && cp <= 0x0E4C) idx = 91 + (cp - 0x0E48);
      mark(idx);
      upper = upper || thUpper(cp);
    } else {
      thPut(size, idx, pen);
      baseX = pen;
      baseInk = pgm_read_byte(&ink[idx]);
      baseCp = cp;
      upper = false;
      pen += pgm_read_byte(&adv[idx]);
    }
  }
  return pen;
}
