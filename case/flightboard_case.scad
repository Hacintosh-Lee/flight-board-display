// 航班牌外壳 —— 2.4 寸 ST7789 屏(横放) + ESP32-C3, 像站牌一样挂在显示器下沿
//
// 两个零件, 都不用支撑:
//   part = "shell" 前壳(正面朝下打印): 正面边框 + 四周侧壁 + 顶上两根挂柱
//   part = "lid"   后盖(平放打印): 带一圈卡边, 塞进前壳后面
//   part = "both"  装好的样子(只看效果)
// 命令行导出: openscad -D 'part="shell"' -o shell.stl flightboard_case.scad
//
// 坐标: X 左右(宽), Y 前后(0 = 正面, 往后为正), Z 上下(0 = 底面)。单位 mm。
// ★ 标星的是我没量过、按 2.4 寸 240x320 屏的常见尺寸估的, 打印前请用卡尺核对。

part = "both";

// ---------- 屏幕 ----------
// 实测: 外框 63 x 41, 左右各 5 mm 非显示边, 点亮时显示区外一圈 1 mm 黑边
panel_w = 63;       // 屏幕外框宽(横放时左右)
panel_h = 41;       // 屏幕外框高
panel_t = 2.4;      // ★ 屏幕厚(玻璃 + 背光), 只影响定位框高度
aa_w    = 51;       // 显示区宽: 63 - 2x5 - 2x1
aa_h    = 38.25;    // 显示区高: 按 320x240 的 4:3 算
border_fpc = 6;     // 左右一样宽: 5 mm 边 + 1 mm 黑边
fpc_side = "left";  // 从正面看排线在哪边: "left" / "right"
fpc_gap  = 24;      // 定位框在排线那一侧留的缺口宽(让排线弯到背后)
trim_left = 1.0;    // 开口左边再往里收(从正面看)
trim_top  = 0.5;    // 开口上边再往里收
black_margin = 0.8;// 开口比显示区每边大多少; 实际黑边只有 1 mm, 开口压住 0.2 mm, 不露银边

// ---------- 外形 ----------
frame   = 8;        // 正面开口四周的边框宽
wall    = 2.0;      // 侧壁厚
front_t = 1.6;      // 正面板厚
inner_depth = 9.2;  // 正面板后面的内部深度(整体厚度比上一版减 1/3: 18.6 -> 12.4)
corner_r = 2.5;     // 外壳四角圆角
fit     = 0.3;      // 屏幕和定位框之间的间隙

// ---------- 后盖 ----------
lid_t   = 1.6;      // 后盖板厚
lip_h   = 3.0;      // 卡边伸进去的深度
lip_t   = 1.6;      // 卡边厚
lid_fit = 0.2;      // 卡边和侧壁之间的间隙(太松就改小, 太紧改大)

// ---------- ESP32 (贴在后盖内侧) 和 USB-C 开孔 ----------
// ESP32-C3 Super Mini 平贴在后盖里面, USB-C 朝左; 后盖上有一圈定位槽。USB-C 开孔的位置由板子位置算出来。
esp_l   = 22.6;     // 板长(USB-C 那头到另一头)
esp_w   = 18.0;     // 板宽
esp_pcb = 1.0;      // 板厚
usbc_t  = 3.2;      // USB-C 插座高(板子上面那部分)
esp_z0  = 4;        // 板子下边离外壳底面多高(内壁往上一点)
esp_gap = 0.3;      // 定位槽比板子大一圈
usb_open_w = 12.5;  // 开孔宽(上下方向): 线头塑料外壳大约 12 mm
usb_open_h = 7.5;   // 开孔高(前后方向): 线头外壳大约 6.5 mm
usb_side = "left";

// ---------- 挂柱(顶上两根, 粘在显示器下沿) ----------
post_w   = 8;       // 柱子宽
post_h   = 12;      // 柱子高(外壳顶面到粘贴面)
pad_w    = 26;      // 粘贴面宽
pad_t    = 2.5;     // 粘贴面厚
post_pos = [0.22, 0.78];  // 两根柱子在宽度上的位置(比例)
// 柱子和粘贴面前后贯穿整个外壳深度: 正面朝下打印时不悬空, 粘贴面也够大(约 26 x 20)

// ================= 以下由上面的参数算出 =================
win_w = aa_w + 2 * black_margin;
win_h = aa_h + 2 * black_margin;
border_other = (panel_w - aa_w - border_fpc);       // 另一侧的边
border_tb = (panel_h - aa_h) / 2;
// 开口外每边的距离: 至少 frame, 也要保证屏幕外框之外还有间隙 + 侧壁
need = function(b) b - black_margin + fit + wall;
fl = max(frame, need(fpc_side == "left" ? border_fpc : border_other));
fr = max(frame, need(fpc_side == "left" ? border_other : border_fpc));
fb = max(frame, need(border_tb));
ft = fb;
outer_w = fl + win_w + fr;
outer_h = fb + win_h + ft;
depth   = front_t + inner_depth;
usb_cy  = depth - esp_pcb - usbc_t / 2;           // USB-C 中心离正面多远: 后盖内表面往前 板厚 + 半个插座
usb_cz  = esp_z0 + esp_gap + esp_w / 2;           // USB-C 中心离底面多高: 板子的正中
// 屏幕外框在正面坐标里的位置
panel_x0 = fl + black_margin - (fpc_side == "left" ? border_fpc : border_other);
panel_z0 = fb + black_margin - border_tb;

echo(str("外壳正面 ", outer_w, " x ", outer_h, " mm, 深 ", depth + lid_t, " mm (含后盖); 开口 ",
         win_w, " x ", win_h, "; 左右边框 ", fl, " / ", fr, ", 上下 ", fb));

module rounded_box(w, d, h, r) {
  // 四角(从正面看)圆角的长方体, 前后方向是直的
  hull() for (x = [r, w - r], z = [r, h - r]) translate([x, 0, z]) rotate([-90, 0, 0]) cylinder(r = r, h = d, $fn = 40);
}

module usb_hole(extra = 0) {
  // 侧壁上的开孔; 贴着后缘, 所以是个开口朝后的缺口。extra: 往里多挖多少(后盖的卡边要让开)
  x = usb_side == "left" ? -1 : outer_w - wall - 1 - extra;
  translate([x, usb_cy - usb_open_h / 2, usb_cz - usb_open_w / 2]) cube([wall + 2 + extra, usb_open_h + 5, usb_open_w]);
}

module shell() {
  difference() {
    union() {
      rounded_box(outer_w, depth, outer_h, corner_r);
      // 挂柱 + 粘贴面
      for (p = post_pos) {
        cx = outer_w * p;
        translate([cx - post_w / 2, 0, outer_h - 1]) cube([post_w, depth, post_h + 1]);
        translate([cx - pad_w / 2, 0, outer_h + post_h - pad_t]) cube([pad_w, depth, pad_t]);
      }
    }
    // 内腔
    translate([wall, front_t, wall]) cube([outer_w - 2 * wall, depth, outer_h - 2 * wall]);
    // 正面开口, 前沿倒一个小角; 左边和上边按实装效果再往里收一点(屏幕位置不动)
    ow = win_w - trim_left; oh = win_h - trim_top;
    translate([fl + trim_left, -1, fb]) cube([ow, front_t + 2, oh]);
    translate([fl + trim_left, -0.01, fb]) hull() {
      translate([-0.6, 0, -0.6]) cube([ow + 1.2, 0.01, oh + 1.2]);
      translate([0, 0.6, 0]) cube([ow, 0.01, oh]);
    }
    usb_hole();
  }
  // 屏幕定位框: 贴着正面板背面, 把屏幕框在开口后面; 排线那一侧留缺口
  rib = 1.2;
  difference() {
    translate([panel_x0 - fit - rib, front_t, panel_z0 - fit - rib])
      cube([panel_w + 2 * (fit + rib), panel_t + 0.4, panel_h + 2 * (fit + rib)]);
    translate([panel_x0 - fit, front_t - 1, panel_z0 - fit]) cube([panel_w + 2 * fit, panel_t + 2, panel_h + 2 * fit]);
    gx = fpc_side == "left" ? panel_x0 - fit - rib - 1 : panel_x0 + panel_w + fit - 1;
    translate([gx, front_t - 1, panel_z0 + panel_h / 2 - fpc_gap / 2]) cube([rib + 2, panel_t + 2, fpc_gap]);
  }
}

module esp_guide() {
  // 后盖内侧的定位槽: 上、下、远端三面挡边, USB-C 那头敞开; 挡边比板子高一点
  rib = 1.0; rh = esp_pcb + 1.2;
  x0 = usb_side == "left" ? wall + 0.8 : outer_w - wall - 0.8 - esp_l - 2 * esp_gap;
  L = esp_l + 2 * esp_gap; Wd = esp_w + 2 * esp_gap;
  translate([0, depth - rh, 0]) {
    translate([x0, 0, esp_z0 - rib]) cube([L, rh, rib]);                 // 下
    translate([x0, 0, esp_z0 + Wd]) cube([L, rh, rib]);                  // 上
    xe = usb_side == "left" ? x0 + L : x0 - rib;
    translate([xe, 0, esp_z0 - rib]) cube([rib, rh, Wd + 2 * rib]);      // 远端
  }
}

module lid() {
  esp_guide();
  // 后盖: 外板 + 伸进内腔的一圈卡边(在装好的位置画)
  difference() {
    union() {
      translate([0, depth, 0]) rounded_box(outer_w, lid_t, outer_h, corner_r);
      iw = outer_w - 2 * wall - 2 * lid_fit;
      ih = outer_h - 2 * wall - 2 * lid_fit;
      translate([wall + lid_fit, depth - lip_h, wall + lid_fit]) difference() {
        cube([iw, lip_h, ih]);
        translate([lip_t, -1, lip_t]) cube([iw - 2 * lip_t, lip_h + 2, ih - 2 * lip_t]);
      }
    }
    usb_hole(lid_fit + lip_t + 1);
    // 卡边在板子那一段整段让开, 板子的 USB 头才能顶到侧壁
    ex = usb_side == "left" ? wall - 0.5 : outer_w - wall - lid_fit - lip_t - 1;
    translate([ex, depth - lip_h - 1, esp_z0 - 1]) cube([lid_fit + lip_t + 1.5, lip_h + 1.01, esp_w + 2 * esp_gap + 2]);
    // 底边一个撬口, 指甲伸进去就能把后盖拔出来
    translate([outer_w / 2 - 6, depth + lid_t - 1, -1]) cube([12, 2, 2.2]);
  }
}

if (part == "shell") rotate([90, 0, 0]) shell();                                   // 正面朝下
else if (part == "lid") translate([0, 0, depth + lid_t]) rotate([-90, 0, 0]) lid(); // 外表面朝下
else {
  color("#2b2f36") shell();
  color("#3a3f48") lid();
  // 示意: 屏幕(黑框) + 显示区(亮)
  ax = panel_x0 + (fpc_side == "left" ? border_fpc : border_other);
  color("#050505") translate([panel_x0, front_t, panel_z0]) cube([panel_w, panel_t, panel_h]);
  color("#1d4f8f") translate([ax, front_t - 0.05, panel_z0 + border_tb]) cube([aa_w, 0.1, aa_h]);
}
