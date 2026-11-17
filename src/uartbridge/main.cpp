// USB <-> 串口屏透传: 电脑直接跟串口屏对话, 用来探测协议/波特率。
// 屏 RX 接 GPIO21(TX), 屏 TX 接 GPIO20(RX), 5V 供电。
// 在 USB 侧发一行 "@@BAUD <n>\n" 切换屏幕串口波特率, 其余字节原样转发。
// "@@RAW <n>\n": 接下来 n 个字节不看转义, 原样转发(下载二进制程序时用, 程序里可能恰好有 "@@")。
// 也拿来给 DA14585 价签板做 UART 下载: GPIO21 接板子 URX, GPIO20 接 UTX。
// "@@BOOT <n>\n" + n 字节程序: 先存下来, 然后等芯片发 STX, 由这块板子当场应答并下载。
//   芯片只给 60 ms 应答时间, 电脑经 USB 来回赶不上, 所以握手放在这里做。结果用 "@@BOOT ..." 行报给电脑。
#include <Arduino.h>

#define SCR_TX 21
#define SCR_RX 20

static uint32_t g_baud = 115200;
static char esc[32];
static int escLen = -1;  // -1: 不在转义行中
static uint32_t rawLeft = 0;  // 还剩多少字节要原样转发
static uint8_t *img = nullptr;  // @@BOOT 收下的程序
static uint32_t imgLen = 0, imgGot = 0;
static bool bootArmed = false;

// 等 STX -> SOH+长度 -> 等 ACK -> 发程序 -> 收校验 -> 回 ACK
static void runBoot() {
  uint8_t crc = 0;
  for (uint32_t i = 0; i < imgLen; i++) crc ^= img[i];
  uint32_t t0 = millis();
  while (millis() - t0 < 120000) {
    if (!Serial1.available()) {
      delay(1);  // 让出 CPU, 不然看门狗会把板子复位; 1 ms 远小于芯片给的 60 ms
      continue;
    }
    if (Serial1.read() != 0x02) continue;
    uint8_t hdr[5] = {0x01, (uint8_t)(imgLen & 0xFF), (uint8_t)((imgLen >> 8) & 0xFF), 0, 0};
    int hl = 3;
    if (imgLen >= 65536) {  // 64 KB 以上用扩展长度
      hdr[1] = hdr[2] = 0;
      hdr[3] = (uint8_t)((imgLen - 65536) & 0xFF);
      hdr[4] = (uint8_t)((imgLen - 65536) >> 8);
      hl = 5;
    }
    Serial1.write(hdr, hl);
    uint32_t t1 = millis();
    int r = -1;
    while (millis() - t1 < 100) {
      if (Serial1.available()) {
        r = Serial1.read();
        if (r != 0x02) break;  // 跳过还在路上的 STX
      }
    }
    if (r != 0x06) {
      Serial.printf("\n@@BOOT NOACK %d\n", r);
      continue;  // 下一个 STX 再试
    }
    Serial1.write(img, imgLen);
    Serial1.flush();
    t1 = millis();
    r = -1;
    while (millis() - t1 < 1000 && r < 0) {
      if (Serial1.available()) r = Serial1.read();
      else delay(1);
    }
    if (r != crc) {
      Serial.printf("\n@@BOOT BADCRC %d want %d\n", r, crc);
      return;
    }
    Serial1.write((uint8_t)0x06);
    Serial.printf("\n@@BOOT OK %u\n", (unsigned)imgLen);
    return;
  }
  Serial.println("\n@@BOOT TIMEOUT");
}

static void setBaud(uint32_t b) {
  Serial1.end();
  Serial1.begin(b, SERIAL_8N1, SCR_RX, SCR_TX);
  g_baud = b;
  Serial.printf("\n@@OK %u\n", (unsigned)b);
}

void setup() {
  Serial.begin(115200);
  Serial1.setRxBufferSize(2048);
  Serial1.begin(g_baud, SERIAL_8N1, SCR_RX, SCR_TX);
}

void loop() {
  while (Serial.available()) {
    int c = Serial.read();
    if (imgGot < imgLen) {
      img[imgGot++] = (uint8_t)c;
      if (imgGot == imgLen) bootArmed = true;
      continue;
    }
    if (rawLeft) {
      rawLeft--;
      Serial1.write((uint8_t)c);
      continue;
    }
    if (escLen < 0 && c == '@' && Serial.peek() == '@') {
      escLen = 0;
      continue;
    }
    if (escLen >= 0) {
      if (c == '\n' || escLen >= (int)sizeof(esc) - 1) {
        esc[escLen] = 0;
        unsigned b;
        if (sscanf(esc, "@BAUD %u", &b) == 1 && b >= 1200) setBaud(b);
        else if (sscanf(esc, "@RAW %u", &b) == 1) rawLeft = b, Serial.printf("\n@@OK RAW %u\n", b);
        else if (sscanf(esc, "@BOOT %u", &b) == 1 && b > 0 && b <= 131072) {
          free(img);
          img = (uint8_t *)malloc(b);
          imgLen = img ? b : 0;
          imgGot = 0;
          Serial.printf("\n@@OK BOOT %u\n", b);
        }
        escLen = -1;
      } else {
        esc[escLen++] = (char)c;
      }
      continue;
    }
    Serial1.write((uint8_t)c);
  }
  if (bootArmed) {
    bootArmed = false;
    Serial.println("\n@@BOOT ARMED");
    while (Serial1.available()) Serial1.read();
    runBoot();
  }
  uint8_t buf[256];
  int n = Serial1.available();
  if (n > 0) {
    n = Serial1.readBytes(buf, min(n, 256));
    Serial.write(buf, n);
  }
}
