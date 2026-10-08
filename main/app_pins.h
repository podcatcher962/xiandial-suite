/*
 * 拾声 (XianDial) 硬件版 —— 引脚定义
 *
 * 依据：官方《01_IO资源分配表.xlsx》＋ xiaozhi-esp32/boards/lcdwiki-es3c35p/config.h
 * 板型：LCD Wiki ES3C35P  (ESP32-S3 N16R8 / 3.5" ST77922 QSPI / FT6336G / ES8311+FM8002E)
 */
#pragma once

/* ---- QSPI 屏 ---- */
#define PIN_LCD_CS      10
#define PIN_LCD_CLK     12
#define PIN_LCD_D0      11
#define PIN_LCD_D1      13
#define PIN_LCD_D2      14
#define PIN_LCD_D3      9
/* 屏无 RST 引脚（由 0x11 软复位序列唤醒）*/
#define PIN_LCD_BL      41      /* 背光，可 PWM */

/* ---- 触摸 FT6336G（与 ES8311 共用 I2C）---- */
#define PIN_TOUCH_SDA   38
#define PIN_TOUCH_SCL   39
#define PIN_TOUCH_RST   48
#define PIN_TOUCH_INT   47
#define TOUCH_I2C_ADDR  0x55

/* ---- 音频（ES8311 + FM8002E）---- */
#define PIN_PA_EN       1       /* 功放使能脚 */
/* ★★★ 功放极性：低电平使能 ★★★
 * 证据（10-03 查证，别再改反）：官方板级 xz_lcdwiki-es3c35p.cc 里是
 *   Es8311AudioCodec(..., AUDIO_CODEC_PA_PIN, ES8311_CODEC_DEFAULT_ADDR, true, true)
 * 最后那个 true = pa_reverted，而驱动头文件写明语义：
 *   device/include/es8311_codec.h:  false: enable PA when pin set to 1,
 *                                   true : enable PA when pin set to 0
 * 即官方板 GPIO1 输出【0】时功放才工作。
 * ⚠️ 10-03 曾按「高电平使能」写，结果功放全程关死 -> 整机完全无声（连底噪都没有），
 *    而日志一切正常、极易误判成软件问题。 */
#define PA_EN_ACTIVE    0       /* 功放开：输出低 */
#define PA_EN_IDLE      1       /* 功放关：输出高（静音） */
#define PIN_I2S_MCLK    17
#define PIN_I2S_BCLK    18
#define PIN_I2S_WS      21
#define PIN_I2S_DOUT    15      /* ESP32 -> codec */
#define PIN_I2S_DIN     16      /* codec -> ESP32 */
#define I2C_NUM_CODEC   0

/* ---- 其他 ---- */
#define PIN_BAT_ADC     8
#define PIN_LED_RGB     40
#define PIN_BOOT_BTN    0

/* ---- 面板物理分辨率（厂家 config.h：DISPLAY_WIDTH=320 / DISPLAY_HEIGHT=480）---- */
#define LCD_PANEL_W     320
#define LCD_PANEL_H     480

/* ---- LVGL 逻辑分辨率 ----
 * ★★★ 10-07 灵签：改成【竖屏 320x480】，与面板原生尺寸一致 ★★★
 *
 * 拾声是横屏（逻辑 480x320），靠 xs_flush_cb() 软件旋转 90° 实现 ——
 * 代价是写屏要逐像素重排、触摸要反解一次坐标（lx = 479 - ry, ly = rx），
 * 而且「旋转只做一次」这条约束贯穿整个触摸链路（早前两边各转一次，
 * 点击全飞、现象是"点屏幕完全没反应"）。
 *
 * 灵签是竖排签诗 + 竖直签筒，天生该竖着看 ⇒ 逻辑分辨率直接取面板原生
 * 320x480：
 *   · display 的旋转恒为 0，flush 里【零旋转】，只是换字节序 + 贴出去；
 *   · 触摸【零换算】，rx/ry 就是逻辑坐标；
 *   · 少了一整类"转了两次 / 方向差 180°"的坑。
 *
 * ⚠ 唯一要确认的是「上下有没有颠倒」（面板原生朝向 vs 手持方向）。
 *   这由 app_pins.h 的 LQ_PORTRAIT_FLIP 一个宏控制，
 *   显示与触摸【同时】跟着它翻，所以翻错了也只需改这一处。 */
#define LCD_H_RES       320
#define LCD_V_RES       480

/* ★★ 竖屏朝向翻转（1 = 上下颠倒 180°）★★
 * 显示侧：xs_flush_cb() 按此反向取源像素、并把列带写到面板对称位置；
 * 触摸侧：touch_read_cb() 同时把 rx/ry 各自镜像一次。
 * 两边【必须同时翻】，否则屏是正的、点在对面。
 * 判定方法：真机看一眼文字是否正立；或点屏幕四角看串口 raw 值。 */
#define LQ_PORTRAIT_FLIP    0

/*
 * 面板 MADCTL —— ★ 必须保持厂家原值 0x00 ★
 *
 * 10-02 定案：ST77922 这块屏【不支持 XY 轴交换】。厂家板级源码里写着
 *     static_assert(!DISPLAY_SWAP_XY, "ST77922 does not support swapping the X and Y axes");
 * 驱动 esp_lcd_st77922 的 swap_xy() 也是空实现（直接返回 NOT_SUPPORTED）。
 * 之前设 0x60(MX|MV) 想硬件横屏 ⇒ MV 位被忽略，面板仍是 320 列地址空间，
 * 480 宽的帧只写进左边 320 列，右边 160 列保留旧画面（这就是"右边还是
 * 厂家 demo 的 22fps/15%CPU"的原因）。
 *
 * ★ 10-07 灵签：我们要的就是竖屏 ⇒ 与 0x00 天然一致，连软件旋转都省了。
 *   面板保持 0x00 不动；朝向万一需要 180°，也不去动 MADCTL（这块屏的
 *   镜像位同样不可靠），而是走 LQ_PORTRAIT_FLIP 在 flush 里自己做 ——
 *   "能在自己手里控制的事，别交给一块已经被证明不可靠的寄存器"。
 */
#define LCD_MADCTL_PANEL   0x00
#define LCD_MADCTL_LANDSCAPE   0x00     /* 兼容旧引用；实际值同 LCD_MADCTL_PANEL */

/*
 * 触摸换算 —— ★ 10-07 灵签：竖屏下【不需要换算】★
 *
 *   面板物理坐标（竖屏 320x480）  (rx, ry)：触摸控制器直接给出，与面板同坐标系
 *   逻辑坐标（LVGL 320x480）      (lx, ly)
 *
 *       无翻转： lx = rx            ly = ry
 *       翻转时： lx = 319 - rx      ly = 479 - ry
 *
 * 竖屏就是面板原生朝向 ⇒ 两者同一个坐标系，直接相等。
 *
 * 对照（拾声横屏那套，留作参考，别照抄到这个工程）：
 *       lx = (LCD_H_RES - 1) - ry     即 479 - ry
 *       ly = rx
 *   那是"逻辑 480x320 用软件旋转 90° 映射到面板 320x480"的逆变换。
 *   灵签的逻辑分辨率已经等于面板分辨率，旋转不存在，逆变换也就成了恒等。
 *
 * 注意：这个换算必须与「显示朝向」一致。两边差 180° 的现象是
 *   「点屏幕全落到对面」。真机四角若对不上，翻 LQ_PORTRAIT_FLIP
 *   （★ 它是显示与触摸共用的那一个开关，不要只改一边）。
 *
 * （拾声那两个 TOUCH_MIRROR_LX/LY 开关已废弃：竖屏下不需要"再镜像一次"
 *   这种纠偏手段，朝向问题统一由 LQ_PORTRAIT_FLIP 一次解决。）
 */
