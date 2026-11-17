# 航班牌 · ESP32-C3 机场航班信息屏

把真实机场的出发/到达航班显示在一块小屏上，像缩小版的机场航显。全球四千多个机场可选，界面支持中文、英文、泰文。

- **屏幕**：2.4 寸 ST7789（240×320，横放）彩屏；也支持 UsartGPU 串口屏
- **主控**：ESP32-C3（Super Mini 之类的小板都行）
- **数据**：[AirLabs](https://airlabs.co)（全球机场）
- **两种用法**：插着 Mac 由电脑取数推送；或者配好 WiFi 后板子自己联网取数，完全不用电脑

显示内容：航班时间、航班号、航司标志、目的地/出发地、航站楼、状态（计划 / 预计 / 延误 / 登机 / 取消……）。延误分级显示：晚 15 分钟以内按计划，15–29 分钟标「预计」，30 分钟以上整格标橙色「延误」。时间按机场当地时间显示，时区自动从航班数据里算。

---

## ⚠️ 数据源密钥：请自己申请

本仓库**不含任何 API 密钥**。航班数据来自 [AirLabs](https://airlabs.co)，请自己到 <https://airlabs.co> 注册申请一个密钥（免费档每月 1000 次）。没有密钥就取不到数据。

免费档每月 1000 次，程序已经按这个额度控制刷新频率：每轮取 3 次（出发 1 次 + 到达 2 次），约 140 分钟一轮。**同一个密钥不要同时给多块板子或多台电脑用**，额度会不够。

密钥只放在本地文件里（已在 `.gitignore` 中，不会被提交）：

```
flightboard_host/airlabs_key.txt     # AirLabs 密钥, 一行
```

也可以用环境变量 `AIRLABS_KEY`。板子脱离电脑使用时，密钥可以由电脑推送一次存进板子，也可以在板子的手机设置页里填写（页面只显示「已填写」，不会回显密钥本身）。

---

## 硬件与接线

### 2.4 寸 ST7789 彩屏（推荐）

| 屏幕 | ESP32-C3 |
|---|---|
| VCC | 3V3（不要接 5V） |
| GND | GND |
| SCL | GPIO2 |
| SDA | GPIO3 |
| DC | GPIO4 |
| RES | GPIO5 |
| CS | GPIO7 |
| BLK（背光） | 3V3 或常亮 |

这类拆机屏的小坑（已在固件里处理）：要开反色（0x21）；硬件复位之后**不要**再发软件复位（0x01），否则屏幕不响应；SPI 26 MHz 稳定。

### UsartGPU 串口屏（可选）

屏 RX ← GPIO21，屏 TX → GPIO20，5V 供电，38400 8N1。这一版支持触摸：点屏幕可以换机场、连 WiFi。

### 外壳

`case/` 里是一个站牌造型的 3D 打印外壳（OpenSCAD 源文件 + 导出的 STL）：正面开口留一圈黑边，顶上两根 T 形挂柱可以粘在显示器下沿，ESP32 贴在后盖内侧，USB-C 从侧面开孔接出。屏幕尺寸等参数都在 `flightboard_case.scad` 开头，改完重新导出即可：

```bash
openscad -D 'part="shell"' -o shell.stl case/flightboard_case.scad
openscad -D 'part="lid"'   -o lid.stl   case/flightboard_case.scad
```

---

## 编译和烧录

需要 [PlatformIO](https://platformio.org)。

```bash
python3 -m venv .venv
.venv/bin/pip install platformio pyserial pillow
```

### 1. 生成字库（仅彩屏版需要，只能在 Mac 上做）

彩屏没有内置字库，固件里的中文/英文/泰文点阵是用 macOS 自带字体（冬青黑体、Menlo、DIN Condensed、Thonburi）渲染出来的。这些字体不能随仓库分发，所以 `include/fb_font.h` 不在仓库里，第一次编译前在 Mac 上生成一次：

```bash
.venv/bin/python tools/make_font.py
```

### 2. 航司标志（可选）

航司标志是各航司的商标，不随仓库分发。没有标志时固件照样能编译，航班行里用彩色徽章（航司两字码）代替。想要标志的话：

```bash
git clone --depth 1 https://github.com/spydogenesis/airlines-logo.git assets/airlines-logo
git clone --depth 1 https://github.com/Jxck-S/airline-logos.git assets/jxck
.venv/bin/python tools/make_logos.py      # 生成 include/fb_logos.h
```

### 3. 烧录

```bash
.venv/bin/pio run -e flightboard_tft -t upload    # 2.4 寸彩屏版
.venv/bin/pio run -e flightboard -t upload        # 串口屏版
```

---

## 用法一：插着 Mac

电脑端的推送程序负责取数、算时区、按所选语言准备地名，再通过 USB 推给屏幕。

```bash
.venv/bin/pip install pyserial
python3 flightboard_host/host.py               # 前台运行, 自动找到航班牌
bash tools/install_autostart.sh                # 或者装成开机自启(LaunchAgent)
```

运行后打开 <http://127.0.0.1:8377> 选机场、切换语言。也可以编译一个独立的 Mac App：

```bash
bash tools/build_app.sh                        # 生成 ~/Applications/航班牌.app
```

## 用法二：只用 WiFi，不接电脑

1. 板子第一次上电（或者 WiFi 连不上超过 45 秒）会自己开一个热点 **FlightBoard-XXXX**，屏幕上有提示。
2. 手机连上这个热点，一般会自动弹出设置页；没弹的话浏览器打开 `192.168.4.1`。
3. 选 WiFi、输入密码，填机场三字码、屏幕语言和 AirLabs 密钥，保存。

之后板子通电就自己联网取数。插着电脑时由电脑负责取数，板子不重复取（省额度）；电脑超过 150 分钟没推来新数据，板子会自己接手。同一 WiFi 下用浏览器打开板子的 IP 也能进设置页。

---

## 目录

```
src/flightboard/      固件: main.cpp(航班牌逻辑), tft.h(ST7789 驱动和绘图), portal.h(手机设置页)
src/uartbridge/       调试用: USB <-> 串口透传
include/fb_gbk.h      生成的: 界面文字(三种语言)和机场名表
flightboard_host/     电脑端推送程序(host.py)和控制页面(ui.html), 机场列表 airports.json
tools/                生成数据/字库/标志的脚本, 预览模拟器 preview.py, Mac App 源码
case/                 3D 打印外壳
```

改了界面文字或机场名之后，依次重新生成：

```bash
.venv/bin/pip install opencc-python-reimplemented
python3 tools/make_airports.py   # 需要 assets/ 里的原始数据, 见脚本开头说明
python3 tools/make_fb_gbk.py
python3 tools/make_font.py
```

---

## 数据来源与致谢

- 航班数据：[AirLabs](https://airlabs.co)（其服务条款适用，请自行申请密钥）
- 机场列表：[OurAirports](https://ourairports.com/data/)（公有领域）
- 机场多语言名称：[Wikidata](https://www.wikidata.org)（CC0）
- 航司标志（可选，自行生成）：[spydogenesis/airlines-logo](https://github.com/spydogenesis/airlines-logo)、[Jxck-S/airline-logos](https://github.com/Jxck-S/airline-logos)、Wikimedia Commons。标志是各航司的商标，仅用于辨认航司。

## 许可证

代码以 [MIT](LICENSE) 许可证开源。航司标志、字体和第三方数据不在此许可证范围内，以各自的条款为准。
