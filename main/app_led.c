/*
 * 拾声 (XianDial) 硬件版 —— 板载 RGB 状态灯
 *
 * 硬件：XL-5050RGBC-WS2812B（单 IO 串行 WS2812 协议），数据脚 = IO40。
 * 原理图上 LED2 的 DIN ← RGB_LED ← IO40，VDD 挂 +5V。
 *
 * ★★★ 为什么必须用 RMT 而不是 delay 微秒 ★★★
 *   WS2812 的 T0H 只有 **400 ns**、周期 1.25 us。
 *   软件忙等在 WiFi 跑着的时候会被调度抢占，
 *   一旦抖一下，24 bit 就整体错位 —— 现象是「颜色随机乱串」，
 *   而且因为发 24 bit 只要 30 us，抢占概率其实不低。
 *   RMT 用硬件时钟（80 MHz ⇒ 1 tick = 12.5 ns）生成时序，
 *   天然不受 FreeRTOS 调度影响。
 *
 * ★★★ IDF 6.x 的 API 与老教程完全不同（这版踩了一轮）★★★
 *   老教程（IDF 4/5）：rmt_item32_t[] + rmt_write_items()。
 *   IDF 6.x：【encoder 架构】——
 *       rmt_new_tx_channel()      → 通道
 *       rmt_new_copy_encoder()    → 拷贝型编码器（数据已是符号格式）
 *       rmt_transmit(chan, enc, payload, payload_bytes, cfg)
 *   payload 的单位是【字节】，所以要传 `sizeof(符号数组)` 不是元素个数。
 *   类型名是 `rmt_symbol_word_t`（老教程那个 rmt_item32_t 已不存在）。
 *   通道配置字段是 `trans_queue_depth`（不是 transmit_queue_depth）。
 *
 * ★ 10-04 之前固件里从没点亮过它（PIN_LED_RGB 只在 app_pins.h 出现过），
 *   所以「这颗灯的 GRB 顺序与颜色是否与假设一致」是未知的 ——
 *   接上实物若颜色不对，只需调下面 GRB 的位序，不用动其它逻辑。
 */

#include "app_pwr.h"
#include "app_pins.h"
#include "app_sys.h"   /* app_net_link_state */

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>              /* memset（空结构体初始化）*/

/* ★★★★ 10-04 兰兰：「灯这个功能暂时不用」 ⇒ 整块编译期关掉 ★★★★
 *
 *  为什么是「编译期」而不是注释掉调用：
 *    关掉之后 RMT 通道、定时器、符号数组（26×8=208 B）一个都不存在，
 *    既省内部 RAM 又少一个可能出问题的地方 ——
 *    v1.31 的无限重启就出在这一带，先让它彻底离开固件。
 *
 *  ★ 复活的办法：把下面这行改成 1，其余代码原样可用。
 *    （真要复现时注意 mem_block_symbols 必须偶数且 >=48）*/
#define XS_LED_ENABLE  0

#if XS_LED_ENABLE

/* ⚠️⚠️ 这三个 rmt 头【必须写在这里，不能提到文件头上】⚠️⚠️
 *   IDF 的 CMake 组件扫描（component_requirements.py）是【纯文本正则】，
 *   它不认识 #if —— 只要文件里出现 "driver/rmt_tx.h"，就判定 main 依赖
 *   esp_driver_rmt。而兰兰 10-04 叫停了灯、CMakeLists 的 REQUIRES 里
 *   已把 esp_driver_rmt 删掉 ⇒ 两边对不上，cmake 直接崩：
 *     BUG: component_requirements.py: driver/rmt_encoder.h found in
 *          component esp_driver_rmt which is already in the requirements list
 *   所以「停用一个组件」的正确做法是：连它的 #include 一起关进条件块，
 *   光把 REQUIRES 删了是删不干净的。*/
#include "driver/rmt_encoder.h"
#include "driver/rmt_common.h"   /* rmt_del_channel */
#include "driver/rmt_tx.h"

static const char *TAG = "XSLED";

/* ---- WS2812 时序（RMT 记数，1 tick = 1/80MHz = 12.5 ns）----
 * 手册允许范围：T0H 350~450ns、T1H 600~900ns、周期 1.25us。取中间值。*/
#define WS_T0H   32      /* 400 ns */
#define WS_T1H   64      /* 800 ns */
#define WS_T0L   68      /* 850 ns  ⇒ T0H+T0L = 1.25 us */
#define WS_T1L   36      /* 450 ns  ⇒ T1H+T1L = 1.25 us */
#define WS_RESET_TICKS  (400 * 80)  /* 复位低电平 ≥300us ⇒ 取 400us */

#define WS_BITS      24               /* GRB 三色 × 8 位 */
#define WS_SYMS      (WS_BITS + 2)    /* 24 个数据符号 + 1 个空(补偶)+ 1 个复位 */
#define WS_BYTES     (WS_SYMS * sizeof(rmt_symbol_word_t))

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;
static esp_timer_handle_t  s_timer;
static int   s_bright = 30;           /* 0~100 */
static bool  s_enabled = true;
static app_led_mode_t s_mode = APP_LED_OFF;
static int   s_last_level = -1;       /* 上次发出的亮度，-1 = 没发过 */

static rmt_symbol_word_t s_sym[WS_SYMS];   /* 24 bit + 复位，提前编好，发送时只改亮度 */

/* 颜色（RGB 语义，内部按 GRB 顺序打包）*/
static const uint8_t C_GREEN[] = {  40, 255,   0 };
static const uint8_t C_AMBER[] = { 255, 140,  60 };
static const uint8_t C_CYAN[]  = { 255, 255, 200 };
static const uint8_t C_GOLD[]  = { 255, 170,  20 };
static const uint8_t C_RED[]   = { 255,   0,   0 };

void app_led_set_enabled(bool on) { s_enabled = on; }

void app_led_set_bright(int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    s_bright = pct;
}

int app_led_get_bright(void) { return s_bright; }

void app_led_set_mode(app_led_mode_t m) { s_mode = m; }

/* 三角波 0..100（呼吸曲线）。period_ms 一个来回。*/
static int breathe(int t_ms, int period_ms)
{
    int half = period_ms / 2;
    if (half <= 0) half = 1;
    int p = t_ms % period_ms;
    int up = (p < half) ? p : (period_ms - p);
    return up * 100 / half;
}

/* 把 rgb 打进符号数组（GRB 顺序），复位符号预先写好在末尾。*/
static void led_pack(const uint8_t *rgb)
{
    for (int i = 0; i < WS_BITS; i++) {
        int bit;
        /* WS2812 的位序是 G(高位先) → B → R */
        if      (i <  8) bit = (rgb[1] >> (7 - i))       & 1;   /* 绿 */
        else if (i < 16) bit = (rgb[2] >> (15 - i))      & 1;   /* 蓝 */
        else             bit = (rgb[0] >> (23 - i))      & 1;   /* 红 */

        s_sym[i].level0    = 1;
        s_sym[i].duration0 = bit ? WS_T1H : WS_T0H;
        s_sym[i].level1    = 0;
        s_sym[i].duration1 = bit ? WS_T1L : WS_T0L;
    }
    /* 末尾：先补一个静止电平，再给 400us 的复位低电平。
     * 复位必须有 —— 没有它 WS2812 不会锁存这一帧，颜色会停在上一帧。*/
    s_sym[WS_BITS].level0    = 0;
    s_sym[WS_BITS].duration0 = 1;
    s_sym[WS_BITS].level1    = 0;
    s_sym[WS_BITS].duration1 = 1;

    s_sym[WS_BITS + 1].level0    = 0;
    s_sym[WS_BITS + 1].duration0 = WS_RESET_TICKS;
    s_sym[WS_BITS + 1].level1    = 0;
    s_sym[WS_BITS + 1].duration1 = 0;
}

/* 发一帧。level 0~255（已含亮度）。rgb 传 NULL = 只发复位（灭灯）。*/
static void led_send(const uint8_t *rgb, int level)
{
    if (!s_chan || !s_enc) return;

    if (rgb == NULL || level <= 0) {
        /* 灭灯：靠一个长复位告诉 WS2812「全灭」，
         * 不用重新打包 24 bit（RMT 也不必发那么长的数据）。*/
        rmt_symbol_word_t off[2] = {
            { .level0 = 0, .duration0 = 1, .level1 = 0, .duration1 = 1 },
            { .level0 = 0, .duration0 = WS_RESET_TICKS, .level1 = 0, .duration1 = 0 },
        };
        rmt_transmit_config_t tc = { .loop_count = 1 };
        rmt_transmit(s_chan, s_enc, off, sizeof(off), &tc);
        return;
    }

    uint8_t dim[3];
    for (int i = 0; i < 3; i++) {
        int v = (rgb[i] * level) / 255;
        dim[i] = (uint8_t)(v > 255 ? 255 : v);
    }
    led_pack(dim);

    rmt_transmit_config_t tc = { .loop_count = 1 };
    rmt_transmit(s_chan, s_enc, s_sym, WS_BYTES, &tc);
}

/* 80ms 一步的呼吸定时器。
 * ★ 用 esp_timer 而不是 LVGL 定时器：灯的状态来自 app_net/app_radio，
 *   与界面无关。挂 LVGL 会绑到当前页面上（关掉页面灯就不亮了）。*/
static void led_timer_cb(void *arg)
{
    (void)arg;
    if (!s_enabled) return;

    const uint8_t *rgb = NULL;
    int  level  = s_bright * 255 / 100;
    bool breath  = false;
    int  period  = 1600;
    int  t       = (int)(esp_timer_get_time() / 1000);

    switch (s_mode) {
    case APP_LED_WIFI:
        rgb = C_GREEN;                       /* 联网成功：绿常亮 */
        break;
    case APP_LED_LINKING:
        rgb = C_AMBER;  breath = true;       /* 正在连接：琥珀呼吸 */
        break;
    case APP_LED_PLAYING:
        rgb = C_CYAN;   breath = true; period = 1100;   /* 放音：青呼吸 */
        break;
    case APP_LED_FAV:
        /* 收藏：一秒内快速闪三下 —— 要的是「事件感」，不是呼吸 */
        if (t % 1000 < 550) rgb = C_GOLD;
        break;
    case APP_LED_LOW_BATT:
        /* 低电：250ms 亮 / 1750ms 灭 —— 慢闪不刺眼，快闪像坏了 */
        if (t % 2000 < 250) rgb = C_RED;
        break;
    case APP_LED_OFF:
    default:
        rgb = NULL;
        break;
    }

    if (breath) level = level * breathe(t, period) / 100;

    if (rgb == NULL || level <= 0) {
        if (s_last_level != 0) { led_send(NULL, 0); s_last_level = 0; }
        return;
    }

    /* 亮度变化小于 4/255 就跳过：肉眼无感，但能明显减少 RMT 占用与功耗 */
    if (s_last_level > 0 && (s_last_level - level < 4 && level - s_last_level < 4)) {
        return;
    }
    s_last_level = level;
    led_send(rgb, level);
}

void app_led_init(void)
{
    if (s_chan) return;

    rmt_tx_channel_config_t cc = {
        .gpio_num   = PIN_LED_RGB,
        .clk_src    = RMT_CLK_SRC_DEFAULT,
        /* ★ 分辨率必须 80MHz：WS2812 的 T0H 只有 400ns。
         *   1 tick = 12.5ns 时，32 tick = 400ns 正好。
         *   ⚠️ 别为了省 RAM 降分辨率 —— 100kHz（1 tick = 10us）
         *      根本表达不出 400ns，灯会变成单色常亮。*/
        .resolution_hz    = 80000000,
        /* ⚠️⚠️ mem_block_symbols 的约束是【偶数且 >= 48】，不是"够用就行"。
         *   我第一次写 `WS_SYMS + 8` = 34 ⇒ rmt_new_tx_channel 直接返回
         *   ESP_ERR_INVALID_ARG，灯压根没建起来（日志里
         *   「rmt: mem_block_symbols must be even and at least 48」）。
         *   这个下限来自硬件：RMT 一个符号在内存里占 2 个 entry，
         *   通道硬件每次至少要能取 48 个。
         *   ⇒ 取 48 正好（34→48，多 14 个符号 ≈ 28 B，可忽略）。*/
        .mem_block_symbols = 48,
        .trans_queue_depth = 4,        /* ⚠️ 字段名是 trans_queue_depth */
        .flags = {
            .invert_out   = 0,
            .with_dma     = 0,
            .allow_pd     = 0,
            .init_level   = 0,
        },
    };
    esp_err_t r = rmt_new_tx_channel(&cc, &s_chan);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "rmt_new_tx_channel: %s —— 状态灯不工作，不影响其他功能",
                 esp_err_to_name(r));
        s_chan = NULL;
        return;
    }

    /* 拷贝型编码器：payload 已经是符号格式，直接搬进 RMT 内存。
     * ⚠️ rmt_copy_encoder_config_t 是【空结构体】（零个字段），
     *   写 `{ 0 }` 会被 -Werror=excess-elements 打死 ⇒ 只能是 `= {NULL}`，
     *   或者按 C 标准用 `{ 0 }` 的等价空初始化。*/
    rmt_copy_encoder_config_t ec;
    memset(&ec, 0, sizeof(ec));
    r = rmt_new_copy_encoder(&ec, &s_enc);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "rmt_new_copy_encoder: %s —— 状态灯不工作",
                 esp_err_to_name(r));
        rmt_del_channel(s_chan);
        s_chan = NULL;
        return;
    }

    ESP_LOGI(TAG, "RGB 灯就绪: IO%d, RMT %d Hz, %d 个符号/帧",
             PIN_LED_RGB, cc.resolution_hz, WS_SYMS);

    esp_timer_create_args_t ta = {
        .callback        = led_timer_cb,
        .name            = "xs_led",
        .dispatch_method = ESP_TIMER_TASK,
    };
    if (esp_timer_create(&ta, &s_timer) == ESP_OK) {
        esp_timer_start_periodic(s_timer, 80 * 1000);
    } else {
        ESP_LOGW(TAG, "esp_timer_create 失败 —— 灯不会呼吸（常亮状态仍正常）");
    }
}

#else   /* ============ XS_LED_ENABLE == 0 ============ */

/* 整块功能已停用。这几个函数必须仍有定义，
 * 否则 ui_xiandial.c / app_pwr.c 里的调用会变成链接错误。*/
void app_led_init(void)           { }
void app_led_set_mode(app_led_mode_t m) { (void)m; }
void app_led_set_enabled(bool on) { (void)on; }
void app_led_set_bright(int pct)  { (void)pct; }
int  app_led_get_bright(void)     { return 0; }

#endif  /* XS_LED_ENABLE */
