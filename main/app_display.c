/*
 * 灵签 (LingQian) —— 显示 / 触摸 驱动层
 *   （底座来自拾声 XianDial，★ 10-07 把「横屏 + 软件旋转 90°」改成「竖屏」）
 *
 * 屏：ST77922 QSPI —— 面板原生竖屏 320x480
 *      ★★ 逻辑分辨率【就等于】面板原生 320x480 ⇒ 本文件里【没有任何旋转】。
 *
 *      拾声原来是横屏：逻辑 480x320，靠 xs_flush_cb() 逐像素旋转 90° 写到面板。
 *      灵签要竖排签诗、竖直签筒，竖着看才对，于是逻辑直接取原生尺寸：
 *        · flush 只需换 RGB565 字节序，不重排（快、也不可能转错方向）；
 *        · 触摸 rx/ry 就是逻辑坐标（除了一个可能需要的 180° 翻转）。
 *      代价是 UI 的坐标要按 320x480 重新排 —— 那本来就该重排。
 *
 * 触摸：FT6336G (I2C 0x55)，直接读寄存器（不依赖 esp_lcd_touch）
 */
#include <string.h>
#include <stdio.h>

#include "app_display.h"
#include "app_pins.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77922.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "XSDISP";

#define LCD_CMD_MADCTL   0x36          /* ST77922 内存访问控制寄存器 */

/* ---------- 背光 (LEDC PWM) ---------- */
#define BL_LEDC_TIMER    LEDC_TIMER_0
#define BL_LEDC_CHANNEL  LEDC_CHANNEL_0
#define BL_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define BL_LEDC_DUTY_RES LEDC_TIMER_10_BIT
#define BL_FREQ_HZ       5000

/* ---------- ST77922 初始化命令表（来源：lcdwiki 官方 / xiaozhi-esp32 的 es3c35p 板级）----------
 * ★ 与厂家原表完全一致（含 0x36 = 0x00 竖屏）。
 *   横屏【不】靠 MADCTL —— 这块屏不支持 XY 轴交换，硬设 MV 位会被忽略，
 *   并导致面板仍是 320 列地址空间、480 宽画面只写进左半屏。
 *   横屏由本文件的 xs_flush_cb() 在写屏时旋转 90°（也不用组件的 sw_rotate）。
 */
static const st77922_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xF1, (uint8_t[]){0x00}, 1, 0},
    {0x60, (uint8_t[]){0x00, 0x00, 0x00}, 3, 0},
    {0x65, (uint8_t[]){0x80}, 1, 0},
    {0x79, (uint8_t[]){0x06}, 1, 0},
    {0x7B, (uint8_t[]){0x00, 0x08, 0x08}, 3, 0},
    {0x80, (uint8_t[]){0x55, 0x62, 0x2F, 0x17, 0xF0, 0x52, 0x70, 0xD2, 0x52, 0x62, 0xEA}, 11, 0},
    {0x81, (uint8_t[]){0x26, 0x52, 0x72, 0x27}, 4, 0},
    {0x84, (uint8_t[]){0x92, 0x25}, 2, 0},
    {0x87, (uint8_t[]){0x10, 0x10, 0x58, 0x00, 0x02, 0x3A}, 6, 0},
    {0x88, (uint8_t[]){0x00, 0x00, 0x2C, 0x10, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01,
                       0x00, 0x06}, 15, 0},
    {0x89, (uint8_t[]){0x00, 0x00, 0x00}, 3, 0},
    {0x8A, (uint8_t[]){0x13, 0x00, 0x2C, 0x00, 0x00, 0x2C, 0x10, 0x10, 0x00, 0x3E, 0x19}, 11, 0},
    {0x8B, (uint8_t[]){0x15, 0xB1, 0xB1, 0x44, 0x96, 0x2C, 0x10, 0x97, 0x8E}, 9, 0},
    {0x8C, (uint8_t[]){0x1D, 0xB1, 0xB1, 0x44, 0x96, 0x2C, 0x10, 0x50, 0x0F, 0x01, 0xC5, 0x12, 0x09},
     13, 0},
    {0x8D, (uint8_t[]){0x0C}, 1, 0},
    {0x8E, (uint8_t[]){0x33, 0x01, 0x0C, 0x13, 0x01, 0x01}, 6, 0},
    {0xB3, (uint8_t[]){0x00, 0x30}, 2, 0},
    {0xF1, (uint8_t[]){0x00}, 1, 0},
    {0x71, (uint8_t[]){0xD0}, 1, 0},
    {0x66, (uint8_t[]){0x02, 0x3F}, 2, 0},
    {0xBE, (uint8_t[]){0x26, 0x00, 0x9D}, 3, 0},
    {0x70, (uint8_t[]){0x01, 0xA0, 0x11, 0x40, 0xE0, 0x00, 0x11, 0x69, 0x11, 0x00, 0x00, 0x1A}, 12, 0},
    {0x90, (uint8_t[]){0x04, 0x04, 0x55, 0x74, 0x00, 0x40, 0x43, 0x27, 0x27}, 9, 0},
    {0x91, (uint8_t[]){0x04, 0x04, 0x55, 0x75, 0x00, 0x40, 0x42, 0x27, 0x27}, 9, 0},
    {0x92, (uint8_t[]){0x04, 0x44, 0x55, 0xC0, 0x06, 0x00, 0x07, 0x05, 0x90, 0x27}, 10, 0},
    {0x93, (uint8_t[]){0x04, 0x43, 0x11, 0x00, 0x00, 0x00, 0x00, 0x05, 0x90, 0x27}, 10, 0},
    {0x94, (uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 6, 0},
    {0x95, (uint8_t[]){0x96, 0x16, 0x00, 0x00, 0xFF}, 5, 0},
    {0x96, (uint8_t[]){0x44, 0x53, 0x03, 0x12, 0x23, 0x24, 0x06, 0x05, 0x94, 0x27, 0x00, 0x44}, 12, 0},
    {0x97, (uint8_t[]){0x44, 0x53, 0x47, 0x56, 0x20, 0x20, 0x02, 0x01, 0x94, 0x27, 0x00, 0x44}, 12, 0},
    {0xBA, (uint8_t[]){0x55, 0x94, 0x2D, 0x94, 0x27}, 5, 0},
    {0x9A, (uint8_t[]){0x40, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00}, 7, 0},
    {0x9B, (uint8_t[]){0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00}, 7, 0},
    {0x9C, (uint8_t[]){0x5C, 0x12, 0x00, 0x00, 0x10, 0x12, 0x00, 0x00, 0x10, 0x02, 0x00, 0x00, 0x00}, 13, 0},
    {0x9D, (uint8_t[]){0x8A, 0x51, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x01}, 8, 0},
    {0x9E, (uint8_t[]){0x51, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x01}, 7, 0},
    {0xB4, (uint8_t[]){0x1D, 0x1C, 0x1E, 0x0B, 0x14, 0x02, 0x13, 0x09, 0x1E, 0x00, 0x1E, 0x10}, 12, 0},
    {0xB5, (uint8_t[]){0x1D, 0x1C, 0x1E, 0x0A, 0x15, 0x03, 0x11, 0x08, 0x1E, 0x01, 0x1E, 0x12}, 12, 0},
    {0xB6, (uint8_t[]){0x77, 0x77, 0x00, 0x0A, 0xFF, 0x0A, 0xFF}, 7, 0},
    {0x86, (uint8_t[]){0xCD, 0x04, 0xB1, 0x02, 0x58, 0x12, 0x58, 0x0C, 0x13, 0x01, 0xA5, 0x00, 0xA5,
                       0xA5}, 14, 0},
    {0xB7, (uint8_t[]){0x07, 0x0A, 0x0E, 0x06, 0x05, 0x03, 0x2B, 0x03, 0x03, 0x42, 0x07, 0x10, 0x10,
                       0x2E, 0x3F, 0x0D}, 16, 0},
    {0xB8, (uint8_t[]){0x07, 0x0A, 0x0D, 0x05, 0x05, 0x02, 0x2B, 0x02, 0x03, 0x42, 0x06, 0x10, 0x0F,
                       0x2E, 0x3F, 0x0D}, 16, 0},
    {0xB9, (uint8_t[]){0x23, 0x23}, 2, 0},
    {0xBF, (uint8_t[]){0x10, 0x14, 0x14, 0x0B, 0x0B, 0x0B}, 6, 0},
    {0xF2, (uint8_t[]){0x00}, 1, 0},
    {0x73, (uint8_t[]){0x04, 0xDA, 0x12, 0x54, 0x47}, 5, 0},
    {0x77, (uint8_t[]){0x6B, 0x5B, 0xFD, 0xC3, 0xC5}, 5, 0},
    {0x7A, (uint8_t[]){0x15, 0x27}, 2, 0},
    {0x7B, (uint8_t[]){0x04, 0x57}, 2, 0},
    {0x7E, (uint8_t[]){0x01, 0x0E}, 2, 0},
    {0xBF, (uint8_t[]){0x36}, 1, 0},
    {0xE3, (uint8_t[]){0x40, 0x40}, 2, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xD0, (uint8_t[]){0x00}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x3F}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x21, (uint8_t[]){0x00}, 0, 0},
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x29, (uint8_t[]){0x00}, 0, 0},
    {0x2C, (uint8_t[]){0x00}, 0, 0},
    {0x3A, (uint8_t[]){0x01}, 1, 0},
    {0x36, (uint8_t[]){LCD_MADCTL_PANEL}, 1, 0},
    /*   ↑ 保持厂家原值 0x00（竖屏）。横屏不靠 MADCTL —— 这块屏不支持
     *     XY 轴交换，硬设 MV 位会被忽略并导致画面只写左半屏。
     *     横屏由 xs_flush_cb() 旋转，见 app_display_init() 第 5.5 步。 */
    {0x35, (uint8_t[]){0x01}, 1, 20},
};

/* ---------- 内部状态 ---------- */
static lv_display_t *s_disp = NULL;
static lv_indev_t *s_indev = NULL;
static esp_lcd_panel_handle_t    s_panel = NULL;
static uint8_t  *s_buf1;        /* LVGL 整屏渲染缓冲（PSRAM，307200 B）*/
static uint8_t  *s_prev;        /* 上一帧（PSRAM）—— 跳过没变的列带；可失败
                                 * ★ 10-07 竖屏：布局是【按带序】
                                 *   [band][面板行 py][带内列 0..19]，
                                 *   所以每个 band 在 s_prev 里占的
                                 *   band*XS_BAND_BYTES 起是连续 19200 B，
                                 *   比对只需一次 memcmp。
                                 *   （横屏时带子沿逻辑行切、在原帧里是连续的，
                                 *     竖屏不是 —— 所以不能沿用"直接 memcmp 源帧"那套。）*/
static uint8_t  *s_rot;         /* 输出（内部 DMA RAM，19200 B = 一条列带）*/
static SemaphoreHandle_t s_dma_done;   /* 「上一笔 DMA 传完」令牌：ISR 给、flush 回调里等 */
static volatile uint32_t s_isr_cnt;    /* DMA 完成中断计数（诊断：ISR 到底有没有被调用）*/
static uint32_t s_frame_cnt;    /* 已出图帧数（日志配额用）*/
static uint32_t s_timeout_warn; /* DMA 等待超时告警配额 */
static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_touch_dev = NULL;
static uint8_t s_touch_points = 1;

/* 最近一次触摸数据（供调试界面显示）*/
static volatile int s_raw_x, s_raw_y, s_log_x, s_log_y;
static volatile bool s_touched;

/* ---------- 背光 ---------- */
static void backlight_init(void)
{
    ledc_timer_config_t tcfg = {
        .speed_mode = BL_LEDC_MODE,
        .duty_resolution = BL_LEDC_DUTY_RES,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = BL_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tcfg));

    ledc_channel_config_t ccfg = {
        .gpio_num = PIN_LCD_BL,
        .speed_mode = BL_LEDC_MODE,
        .channel = BL_LEDC_CHANNEL,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ccfg));
}

void app_backlight_set(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint32_t max = (1u << BL_LEDC_DUTY_RES) - 1u;
    uint32_t duty = max * percent / 100;
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

/* I2C0 主总线（触摸 + ES8311 共用）—— 音频模块初始化 codec 时要复用这条 */
i2c_master_bus_handle_t app_i2c_bus_get(void) { return s_i2c_bus; }

/* ---------- 触摸（FT6336G 兼容，★厂家 16 位寄存器映射）----------
 * ⚠️ 大坑：这颗屏的触摸**不是**标准 FT6336G 的单字节寄存器映射！
 *    厂家实现用的是 **16 位寄存器地址（大端两字节）**，实测寄存器：
 *      0x0010 INFO         bit3=1 → 有按下
 *      0x0009 MAX_TOUCHES  最大触点数（1~5）
 *      0x0014 POINT0       每点 7 字节：
 *                          [0] bit7=本点有效, bit5:0 与 [1] 拼成 X
 *                          [2] (bit5:0) 与 [3] 拼成 Y
 *    用标准映射（0x02/0x03）读 → 恒返回垃圾，触摸完全没反应。
 */
#define FT_REG_INFO        0x0010
#define FT_REG_MAX_TOUCHES 0x0009
#define FT_REG_POINT0      0x0014
#define FT_PT_BYTES        7

static esp_err_t ft_read(uint16_t reg, uint8_t *buf, size_t len)
{
    if (!s_touch_dev) return ESP_FAIL;
    uint8_t wbuf[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    return i2c_master_transmit_receive(s_touch_dev, wbuf, 2, buf, len, 100);
}

static void ft_reset(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_TOUCH_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level(PIN_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(12));
    gpio_set_level(PIN_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static int16_t last_x = LCD_H_RES / 2, last_y = LCD_V_RES / 2;
    data->point.x = last_x;
    data->point.y = last_y;
    data->state = LV_INDEV_STATE_RELEASED;
    s_touched = false;

    uint8_t status = 0;
    if (ft_read(FT_REG_INFO, &status, 1) != ESP_OK) return;
    if (!(status & 0x08)) return;          /* bit3 = 有触摸 */

    uint8_t buf[FT_PT_BYTES * 5] = {0};
    if (ft_read(FT_REG_POINT0, buf, FT_PT_BYTES * (size_t)s_touch_points) != ESP_OK) return;
    if (!(buf[0] & 0x80)) return;          /* bit7 = 本点有效 */

    int rx = ((buf[0] & 0x3F) << 8) | buf[1];
    int ry = ((buf[2] & 0x3F) << 8) | buf[3];

    s_raw_x = rx;
    s_raw_y = ry;

    /* 竖屏逻辑坐标 = 面板原生坐标（本工程无旋转）。
     * ★ 180° 翻转时显示侧也翻，两边必须一致，开关是同一个 LQ_PORTRAIT_FLIP。 */
#if LQ_PORTRAIT_FLIP
    int lx = (LCD_H_RES - 1) - rx;
    int ly = (LCD_V_RES - 1) - ry;
#else
    int lx = rx;
    int ly = ry;
#endif
    if (lx < 0) lx = 0;
    if (ly < 0) ly = 0;
    if (lx >= LCD_H_RES) lx = LCD_H_RES - 1;
    if (ly >= LCD_V_RES) ly = LCD_V_RES - 1;

    /* ★ 调试：串口打印原始坐标（节流 200ms）。竖屏下 raw 与 lv 应【完全相同】，
     *   不一致说明 LQ_PORTRAIT_FLIP 与显示侧没对齐。 */
    static TickType_t t_last = 0;
    TickType_t t_now = xTaskGetTickCount();
    if (t_now - t_last > pdMS_TO_TICKS(200)) {
        t_last = t_now;
        ESP_LOGI(TAG, "TOUCH raw=(%d,%d) -> lv=(%d,%d)", rx, ry, lx, ly);
    }

    s_log_x = lx;
    s_log_y = ly;
    s_touched = true;

    last_x = lx;
    last_y = ly;
    data->point.x = lx;
    data->point.y = ly;
    data->state = LV_INDEV_STATE_PRESSED;
}

void app_touch_get_last(int *rx, int *ry, int *lx, int *ly, bool *pressed)
{
    if (rx) *rx = s_raw_x;
    if (ry) *ry = s_raw_y;
    if (lx) *lx = s_log_x;
    if (ly) *ly = s_log_y;
    if (pressed) *pressed = s_touched;
}

lv_display_t *app_display_get(void) { return s_disp; }

/* ============================================================
 *  显示：自建 LVGL display（★ 不走 lvgl_port_add_disp）
 * ============================================================
 * 为什么不用组件的显示路径（10-02 四个真机实测 Bug 逼出来的）：
 *   1. 组件 sw_rotate 分支对「窄区域」刷新会算错源/目标行距，
 *      画出一道错位噪点带。组件会在「按当前刷新区域 reshape 缓冲」之后，
 *      仍按整屏宽去读 —— 只有恰好整屏宽的块碰巧对，窄块全斜拉成乱码。
 *   2. 组件会按 display 的 rotation 自动执行 lv_display_rotate_point()；
 *      如果我们在触摸回调里再手算一次横屏换算，就等于转了两次，
 *      点击全落到别处 —— 现象：触摸完全没反应（日志里 39 次触摸、0 次点击）。
 *   3. 即使自建 flush，只要还用 PARTIAL 模式，窄区域一样会错：LVGL 内部
 *      reshape 缓冲时会改写 w/h/stride，而这个改写是静默失败的。
 *      ⇒ 最终定案改用 FULL 模式，见 xs_flush_cb() 顶上的长注释。
 *   4. 窄区域刷新在面板一侧也不可靠：唯一稳定正确的窗口形状是
 *      「40 列 × 480 行（整个面板高）」，非满高窗口会画出亮白彩条。
 *
 * 自建之后 display 的 rotation 恒为 0：
 *   · LVGL 逻辑坐标系就是 480x320（UI 代码一行不用改）；
 *   · LVGL 不再动触摸点，横屏换算由 touch_read_cb 自己负责；
 *   · 旋转在 flush 里做，源/目标行距全部自己算死，不留歧义。
 */
/* ★★★ 面板写入的「列带」宽度（= 面板 X 方向一次写多少列）★★★
 * 10-02 真机实测：唯一稳定正确的窗口形状是「N 列 × 480 行（整个面板高）」；
 * 非满高窗口（如 18 列 × 141 行）会画出亮白彩条。
 * 所以整帧永远切成这种形状写，绝不出现别的窗口大小。
 *
 * ★ 10-07 竖屏改造：形状没变（仍是「20 列 × 480 行」），变的是「哪一边是列」。
 *   横屏时带子沿【逻辑行】切（逻辑行 = 面板列）；竖屏下逻辑列就是面板列，
 *   所以带子沿【逻辑列】切，20 列 × 480 行 = 切 16 带铺满 320 列。
 *   每条带仍是 19200 B，帧总字节 307200 不变。
 *
 * ★ 10-03 由 40 列改 20 列：宽度只影响「一次 DMA 写多少列」，只要【行数满高】
 *   形状就依然合法。40 列需要 38400 B 内部 DMA RAM，改成 20 列只要 19200 B ——
 *   正好把 LVGL 内存池从 64 KB 提到 192 KB 后吃掉的那块内部 RAM 让出来。
 *   必须取 320(=LCD_H_RES) 的约数（10/16/20/32/40/64），否则 XS_BAND_CNT
 *   会截断、最右几列永远不刷。*/
#define XS_BAND_COLS   20
#define XS_BAND_CNT    (LCD_H_RES / XS_BAND_COLS)                          /* 16 带 */
#define XS_BAND_BYTES  (XS_BAND_COLS * LCD_V_RES * (int)sizeof(uint16_t))  /* 19200 B */

/* 整屏帧缓冲（逻辑 320x480，RGB565）—— LVGL 的渲染目标，放 PSRAM。
 * ⚠️ 它只被 CPU 读写（LVGL 渲染 + 我们读），【不做 DMA 源】，
 *    所以放 PSRAM 是安全的；真正的 DMA 源是 s_rot（内部 DMA RAM）。*/
#define XS_FRAME_BYTES (LCD_H_RES * LCD_V_RES * (int)sizeof(uint16_t))     /* 307200 B */

/* 颜色数据传输完成（ISR 上下文）：只做最轻的两件事 —— 计数 + 归还令牌。
 * ★ 不在这里调 lv_display_flush_ready()：LVGL 的推进改由 xs_flush_cb() 同步等待后自己做。
 *   原因见 xs_flush_cb() 末尾注释（ISR 里推 LVGL 时界面停在第一帧不动）。*/
static bool panel_color_trans_done_cb(esp_lcd_panel_io_handle_t io,
                                      esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)io;
    (void)edata;
    (void)user_ctx;
    BaseType_t hp = pdFALSE;
    s_isr_cnt++;
    if (s_dma_done) xSemaphoreGiveFromISR(s_dma_done, &hp);
    return hp == pdTRUE;
}

/* ============================================================================
 *  整帧写屏（LV_DISPLAY_RENDER_MODE_FULL）—— 10-07 竖屏版
 * ============================================================================
 *  ★★ 与拾声最大的不同：这里【没有旋转】★★
 *  逻辑分辨率 = 面板原生 320x480 ⇒ 逻辑像素 (lx,ly) 直接落在面板 (lx,ly)，
 *  只剩一件必须做的事：RGB565 的【字节序】（面板要高字节在前）。
 *
 *  拾声那版要做「逻辑 (lx,ly) -> 面板 (ly, 479-lx)」的 90° 重排，是为了把
 *  480x320 的横屏画面塞进 320x480 的面板。灵签不横屏，那一步整个不存在 ——
 *  也就不存在"旋转方向写反 180°"这种错法（那是横屏两套常见翻车方式之一）。
 *
 *  列的切分方式随之改变，但【形状】没变，仍然是「20 列 × 480 行整高」：
 *    横屏：带子沿逻辑行切（逻辑行 0..319 ＝ 面板列 0..319）
 *    竖屏：带子沿逻辑列切（逻辑列 0..319 ＝ 面板列 0..319）  ← 这个工程
 *  每带 20 列 × 480 行 × 2 B = 19200 B，16 带铺满 307200 B。
 *
 *  已知的坑（全部继承拾声的结论，不要重新踩）：
 *   1. 渲染模式必须是 FULL，不能改回 PARTIAL：PARTIAL 会在窄区域上 reshape
 *      缓冲行距、且该 reshape 在 release 构建里是【静默失败】的，读出来就是
 *      错像素（现象：整宽的块对、窄块斜拉成乱码）。
 *   2. 面板写入窗口只能「N 列 × 480 行」，非满高窗口会画出亮白彩条。
 *   3. flush_ready 必须在【同步等到 DMA 传完】之后再调，不能在 ISR 里调。
 * ============================================================================*/
static void xs_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;   /* FULL 模式恒为整屏，见上面说明 */

    /* 防御：万一 LVGL 内部又拆出多次调用，非最后一次就不要白发 16 笔 QSPI。
     * （FULL 模式下每帧只有一次，这道门正常永远放行。）*/
    if (!lv_display_flush_is_last(disp)) {
        lv_display_flush_ready(disp);
        return;
    }

    const uint16_t *frame = (const uint16_t *)px_map;   /* 320x480，行距 320 px */
    uint16_t *rot = (uint16_t *)s_rot;                  /* 一条列带的输出 */

    TickType_t t0 = xTaskGetTickCount();
    int wrote = 0;

    for (int band = 0; band < XS_BAND_CNT; band++) {
        const int32_t x0 = band * XS_BAND_COLS;          /* 逻辑起始列 */
#if LQ_PORTRAIT_FLIP
        /* 翻转 180°：这条带的像素来自逻辑帧的【对称位置】，
         * 落到面板上的列区间也镜像。px = px0 + i 对应逻辑列 (319 - px)。*/
        const int32_t px0 = LCD_PANEL_W - x0 - XS_BAND_COLS;
#else
        const int32_t px0 = x0;
#endif

        /* 1) 先把这条带按面板行主序摆好，顺便换字节序。
         *    目标下标 = 面板行 py * 20 + 带内列 i（与 draw_bitmap 的窗口一致）。*/
        for (int32_t py = 0; py < LCD_PANEL_H; py++) {
#if LQ_PORTRAIT_FLIP
            const uint16_t *srow = frame + (size_t)(LCD_PANEL_H - 1 - py) * LCD_H_RES;
#else
            const uint16_t *srow = frame + (size_t)py * LCD_H_RES;
#endif
            uint16_t *drow = rot + (size_t)py * XS_BAND_COLS;
            for (int i = 0; i < XS_BAND_COLS; i++) {
#if LQ_PORTRAIT_FLIP
                uint16_t c = srow[LCD_H_RES - 1 - (px0 + i)];
#else
                uint16_t c = srow[x0 + i];
#endif
                drow[i] = (uint16_t)((c >> 8) | (c << 8));   /* 面板要高字节在前 */
            }
        }

        /* 2) 这条带和上一帧一模一样，面板上还是对的，省掉一笔 19.2 KB 的 QSPI。
         *    ★ 10-07 的对比方式变了：s_prev 改成【按带序】存上一帧（见 s_prev 注释），
         *      所以这里就是一次干净的 memcmp —— 不需要再逐行去原帧里挑 20 列。
         *      （竖屏下一条带在原帧里是【跨行不连续】的，逐行挑也可以，但没必要。）*/
        if (s_prev && memcmp(rot, s_prev + (size_t)band * XS_BAND_BYTES,
                             XS_BAND_BYTES) == 0) {
            continue;
        }

        /* 3) 清掉可能残留的令牌，再发这一笔；然后【同步等它传完】。
         *    不能靠轮询打转：vTaskDelay(pdMS_TO_TICKS(1)) 在 100Hz 的 tick 下
         *    等于睡 10ms，16 带就是 160ms 白等。信号量由 ISR 给，微秒级唤醒。
         *    19.2 KB 在 80MHz QIO 上约 0.5ms，100ms 的超时纯属看门狗。*/
        xSemaphoreTake(s_dma_done, 0);
        esp_lcd_panel_draw_bitmap(s_panel, px0, 0, px0 + XS_BAND_COLS, LCD_PANEL_H, s_rot);
        bool ok = xSemaphoreTake(s_dma_done, pdMS_TO_TICKS(100)) == pdTRUE;
        if (!ok && s_timeout_warn < 5) {
            ESP_LOGE(TAG, "band %d dma timeout", band);
            s_timeout_warn++;
        }
        if (ok) {
            wrote++;
            /* 更新参考帧。超时的带不更新，下一帧会重画它 —— 这样即使令牌链
             * 出问题，屏上也只是多刷几次，不会永久停在半张画面上。*/
            if (s_prev) memcpy(s_prev + (size_t)band * XS_BAND_BYTES, rot, XS_BAND_BYTES);
        }
    }

    uint32_t ms = (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);
    if (s_frame_cnt <= 20 || ms > 60) {
        ESP_LOGI(TAG, "frame#%u  bands %d/%d  %ums  isr=%u",
                 (unsigned)s_frame_cnt, wrote, XS_BAND_CNT,
                 (unsigned)ms, (unsigned)s_isr_cnt);
    }
    s_frame_cnt++;

    lv_display_flush_ready(disp);
}

/* ---------- 主初始化 ---------- */
/* ★★ 10-07 截图：整帧拷出（见 app_display.h 里这段函数为什么存在）。
 *   不返回 s_buf1 本身 —— LVGL 随时会拿它渲染下一帧。*/
bool app_display_snapshot(void *dst)
{
    if (s_buf1 == NULL || dst == NULL) return false;
    memcpy(dst, s_buf1,
           (size_t)LCD_H_RES * LCD_V_RES * (size_t)sizeof(uint16_t));
    return true;
}

esp_err_t app_display_init(void)
{
    /* 1. 背光先关着（防止上电花屏闪白）*/
    backlight_init();
    app_backlight_set(0);

    /* 2. I2C 总线（触摸 + ES8311 共用）*/
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_CODEC,
        .sda_io_num = PIN_TOUCH_SDA,
        .scl_io_num = PIN_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c_bus), TAG, "i2c bus");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_touch_dev), TAG, "touch dev");

    ft_reset();
    uint8_t maxpts = 0;
    if (ft_read(FT_REG_MAX_TOUCHES, &maxpts, 1) == ESP_OK && maxpts > 0 && maxpts <= 5) {
        s_touch_points = maxpts;
    }
    ESP_LOGI(TAG, "touch controller ready (16-bit reg map), max points = %d", s_touch_points);

    /* 3. QSPI 总线 */
    ESP_LOGI(TAG, "init QSPI bus");
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_CLK,
        .data0_io_num = PIN_LCD_D0,
        .data1_io_num = PIN_LCD_D1,
        .data2_io_num = PIN_LCD_D2,
        .data3_io_num = PIN_LCD_D3,
        .max_transfer_sz = LCD_H_RES * 64 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "qspi bus");

    /* 4. 面板 IO + ST77922 */
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = 80 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = { .quad_mode = true },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &io), TAG, "panel io");

    st77922_vendor_config_t vendor_cfg = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(st77922_lcd_init_cmd_t),
        .flags = { .use_qspi_interface = 1 },
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st77922(io, &panel_cfg, &panel), TAG, "st77922");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init");
    /* ★★★ 反色：必须 true（10-02 真机取样定案）★★★
     * 现象：整屏颜色互补 —— 深底 #0B0E12 显示成近白 #F4F1ED，
     *       主色绿 #34D399 显示成品红 #CB2C66（照片取样：左蓝/右黄/上品红/下青）。
     * 原因：初始化表里本来就有 {0x21}(INVON)，但紧接着这一行把它覆盖成 INVOFF。
     *       驱动 invert_color(false) 发 0x20，把表里的 0x21 抹掉了。
     * 修法：传 true ⇒ 面板停在 INVON，与初始化表的意图一致、也与厂家 demo 一致。
     */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel, true), TAG, "panel invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "panel on");
    ESP_LOGI(TAG, "panel ready  %dx%d (panel native, MADCTL=0x%02X)",
             LCD_PANEL_W, LCD_PANEL_H, LCD_MADCTL_PANEL);

    /* ★ 颜色传完回调必须在【起 LVGL 任务之前】注册好。
     * 否则第一帧就有可能在回调还没挂上时发出，xs_flush_cb 里的信号量等待
     * 会一路超时（8 带 × 100ms），画面卡在第一帧。*/
    const esp_lcd_panel_io_callbacks_t io_cbs = {
        .on_color_trans_done = panel_color_trans_done_cb,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(io, &io_cbs, NULL),
                        TAG, "io callbacks");

    /* 5. LVGL 端口 */
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 3;
    port_cfg.task_stack = 8192;
    port_cfg.task_affinity = 0;
    port_cfg.task_max_sleep_ms = 50;
    port_cfg.timer_period_ms = 5;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

    /* 5.5 自建显示：一块 LVGL 整屏缓冲（PSRAM）+ 一块旋转输出（内部 DMA RAM）*/
    s_panel = panel;

    s_dma_done = xSemaphoreCreateBinary();   /* 「上一笔 DMA 传完」令牌，初值为空 */
    ESP_RETURN_ON_FALSE(s_dma_done != NULL, ESP_ERR_NO_MEM, TAG, "dma sem");

    /* LVGL 的渲染目标：整屏 480x320 RGB565 = 307200 B，只有 PSRAM 放得下。
     * 它【不做 DMA 源】，所以放 PSRAM 没问题。*/
    ESP_LOGI(TAG, "heap before draw bufs: internal/DMA free=%u largest=%u B, psram free=%u B"
                  " (need band=%d B, frame=%d B)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (int)XS_BAND_BYTES, (int)XS_FRAME_BYTES);

    s_buf1 = heap_caps_malloc(XS_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    /* 上一帧参考（用于「列带没变就跳过」）。分配失败也能跑，只是每帧全写。
     * ★ 竖屏下它按「带序」存（band 在前、带内行主序），与帧缓冲的排布不同，
     *   但【字节数相同】：XS_BAND_BYTES(19200) x XS_BAND_CNT(16) = 307200
     *   = XS_FRAME_BYTES。这里用 _Static_assert 把这条账钉死，
     *   以后谁改了带宽/分辨率，编译期就会红，而不是等到屏上花掉。*/
    _Static_assert(XS_BAND_BYTES * XS_BAND_CNT == XS_FRAME_BYTES,
                   "s_prev 按带序存放，容量必须与整帧一致");
    s_prev = heap_caps_malloc(XS_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    /* 输出 = 一条列带（20 列 x 480 行 = 9600 px）。必须是内部 DMA RAM。*/
    s_rot  = heap_caps_malloc(XS_BAND_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    /* ★ 分开报，别用一句 "draw buffers" —— 上次就是这句把 PSRAM 与内部 RAM 混在一起，
     *   排查时无法判断到底是哪一块不够。*/
    if (s_buf1 == NULL) ESP_LOGE(TAG, "FAILED: s_buf1 (psram, %d B)", (int)XS_FRAME_BYTES);
    if (s_rot  == NULL) ESP_LOGE(TAG, "FAILED: s_rot (internal DMA, %d B) - internal free now=%u",
                                 (int)XS_BAND_BYTES,
                                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_RETURN_ON_FALSE(s_buf1 && s_rot, ESP_ERR_NO_MEM, TAG, "draw buffers");
    if (s_prev == NULL) ESP_LOGW(TAG, "no s_prev (psram), will rewrite all bands every frame");

    /* ★★★ 竖屏方案（10-07 灵签）★★★
     * 面板原生就是竖屏 320x480，且【不支持 XY 轴交换】：
     *   厂家板级源码  static_assert(!DISPLAY_SWAP_XY, "ST77922 does not support swapping the X and Y axes");
     *   驱动实现      panel_st77922_swap_xy() 直接 return ESP_ERR_NOT_SUPPORTED（空实现）
     * 拾声要横屏，只能保持 MADCTL=0x00 并在 flush 里软件旋转 90°。
     * 灵签要竖屏 ⇒ 逻辑分辨率直接等于面板原生 ⇒ MADCTL 仍然 0x00，
     * 而 flush 里【旋转这一步不需要了】，只剩换字节序。
     *
     * ★ 渲染模式 = FULL（不是 PARTIAL）：理由见 xs_flush_cb() 顶上的长注释。
     *   一句话：PARTIAL 会在窄区域上改写缓冲行距、且该改写是静默失败的，
     *   我们拿到的就是错像素（整宽块对、窄块全花）。
     *   FULL 每帧只有一次整屏 flush，拿到的永远是干净完整的 320x480 帧。*/
    if (lvgl_port_lock(0)) {
        s_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
        lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
        lv_display_set_buffers(s_disp, s_buf1, NULL, XS_FRAME_BYTES,
                               LV_DISPLAY_RENDER_MODE_FULL);
        lv_display_set_flush_cb(s_disp, xs_flush_cb);

        /* 6. 触摸 → LVGL 输入设备
         * display 的 rotation 恒为 0 ⇒ LVGL 不会自动旋转触摸点。
         * 竖屏下 rx/ry 就是逻辑坐标（只在 LQ_PORTRAIT_FLIP 时镜像一次），
         * 由 touch_read_cb 自己完成（见 app_pins.h 注释）。 */
        s_indev = lv_indev_create();
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, touch_read_cb);
        lv_indev_set_display(s_indev, s_disp);
        /* ★★ 2026-10-04 第二十二次：把 indev 的读取周期从默认 33 ms 压到 10 ms。
         *
         *   兰兰报「本地拖动开始有作用，但是不灵敏」。这一条链路上有两处延迟：
         *     ① LVGL 的 indev 读取定时器 —— 默认 LV_DEF_REFR_PERIOD = 33 ms
         *        （sdkconfig 里的 CONFIG_LV_DEF_REFR_PERIOD 就是 33）。
         *        也就是说即便我的拖动轮询跑得再快，【读到的坐标】最多也只有
         *        33 ms 的粒度 —— 手指划过 100 px，坐标要分 3 帧才更新完。
         *     ② 我自己的 seek_drag_poll —— 挂在 100 ms 频谱定时器上。
         *   两处叠加 ⇒ 最坏 130 ms 才更新一次位置，手感就是「一顿一顿」。
         *
         *   为什么不用 sdkconfig 改 LV_DEF_REFR_PERIOD：
         *     它同时也是【显示刷新】周期，改它 = 改 sdkconfig
         *     = LVGL 877 文件 + wpa_supplicant 全量重编 25~30 分钟，
         *     而且会把整屏重绘频率一起拉高（480×320 全屏 FULL 模式下更费）。
         *   ⇒ 只改 indev 这一个定时器：它只做「读一次 FT6336 寄存器」，
         *     10 ms 一次的开销可以忽略（I2C 一共 6 字节，约 100 µs）。
         *     兰兰说的「可能是硬件速度造成的」—— 硬件没问题，
         *     480×320 的屏上 33 ms 只走 16 px，10 ms 才跟得上手指。*/
        lv_timer_t *rd = lv_indev_get_read_timer(s_indev);
        if (rd) {
            lv_timer_set_period(rd, 10);
            /* ⚠️ LVGL v9 没有 lv_timer_get_period()（只有 set），别顺手去读旧值 */
            ESP_LOGI(TAG, "indev read period: %d ms -> 10 ms (拖动跟手)", LV_DEF_REFR_PERIOD);
        }
        lvgl_port_unlock();
    }
    ESP_RETURN_ON_FALSE(s_disp != NULL, ESP_FAIL, TAG, "create lvgl display");

    ESP_LOGI(TAG, "display ready: 竖屏 logical %dx%d == panel %dx%d, FULL mode, %d bands x %d cols"
                  " (no rotation%s)",
             LCD_H_RES, LCD_V_RES, LCD_PANEL_W, LCD_PANEL_H, XS_BAND_CNT, XS_BAND_COLS,
             LQ_PORTRAIT_FLIP ? ", FLIPPED 180" : "");

    /* 7. 点屏 */
    app_backlight_set(85);
    return ESP_OK;
}
