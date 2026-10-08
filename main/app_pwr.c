/*
 * 拾声 (XianDial) 硬件版 —— 电源 / 关机 / 电量
 *
 * 板型 LCD Wiki ES3C35P。★ 硬件事实见 app_pwr.h 顶部的供电架构图与三条注解。
 *
 * 本文件三件事：
 *   ① IO8(BAT_ADC) 读电池电压 → 顶栏电量 + 低电自动关机
 *   ② deep sleep 关机（BOOT 键 = 开机键）
 *   ③ 供电真值日志（让兰兰插上电池后第一眼就知道读数对不对）
 */

#include "app_pwr.h"
#include "app_audio.h"     /* 只用 app_pins.h 的 PA_EN 常量 */
#include "app_display.h"   /* app_backlight_set */
#include "app_pins.h"
#include "app_radio.h"     /* app_radio_stop */
#include "app_sys.h"       /* app_net_connected / app_net_link_state */

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "XSPWR";

/* ============================================================
 *  ① 电量检测
 * ============================================================ */

#if CONFIG_IDF_TARGET_ESP32S3
/* ESP32-S3：GPIO1~GPIO10 = ADC1_CH0~CH9 ⇒ IO8 = ADC1_CH7。
 * ⚠️ 必须用 ADC1：ADC2 与 WiFi 抢，WiFi 一开 ADC2 就不能用了。*/
#define BAT_ADC_UNIT     ADC_UNIT_1
#define BAT_ADC_CHAN     ADC_CHANNEL_7      /* == IO8 */
#else
#error "这块板是 ESP32-S3，BAT ADC 通道定义不适用于别的目标"
#endif

/* 原理图：BAT+ ─R14(100K)─┬─R15(100K)─ GND，net = BAT_ADC
 * ⇒ 1:2 分压，V_adc = V_batt / 2，反算乘 2。*/
#define BATT_DIV_NUM     2

/* ADC 量程：ESP32-S3 用 12dB 衰减 ≈ 0~3100 mV。
 * 4.2V 电池分压后 2.1V，在范围内且不至于顶到满刻度。*/
#define BAT_ADC_ATTEN    ADC_ATTEN_DB_12

/* 采样节奏：每 5 秒读一次。理由——
 *  · ADC 读数有 ±20mV 抖动，秒级刷新只会看到数字乱跳；
 *  · 电池电压本来就是慢变量，5 秒完全够；
 *  · 深夜待机时少唤醒 12 次/分钟。*/
#define BATT_POLL_MS     5000

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t        s_cali;
static bool                     s_adc_ok;

static int   s_mv_cache;          /* 上次读到的电池 mV */
static int   s_low_mv = 3650;     /* 低电阈值 */
static int   s_tick_cnt;
static bool  s_low_shutdown_done; /* 同一轮低电只关一次 */

/* ★ 10-04：电池在否改为「可手动声明」。
 * 为什么不能只看电压 —— 见 app_pwr_batt_present() 上方那段原理图分析：
 * TP4054 的 CE 脚硬接 GND（常使能），没插电池时它照样往 BAT 线灌电流，
 * 读数 4.15V 落在「满电」区间里，靠电压区分不了。*/
static bool  s_batt_manual;         /* 用户是否手动声明过 */
static bool  s_batt_present_forced; /* 声明：true=有电池 */

int app_pwr_low_mv(void)  { return s_low_mv; }
void app_pwr_set_low_mv(int mv) { s_low_mv = mv; }

/* 锂电放电曲线 → 百分比。
 * ★ 为什么不能 mv/42：锂电的电压-容量曲线非常非线性，
 *   3.7→3.5V 这一小段就放掉了约 40% 的容量（负载越重掉得越快）。
 *   线性算会在「还有一半电」的时候显示 10%，用两天就没电了。
 *   这张表是 3.7V 锂电在中负载下的实测形状。*/
static int mv_to_pct(int mv)
{
    static const struct { int mv; int pct; } tbl[] = {
        { 4200, 100 }, { 4050,  92 }, { 3950,  84 }, { 3880,  76 },
        { 3800,  66 }, { 3750,  58 }, { 3700,  50 }, { 3650,  42 },
        { 3600,  34 }, { 3550,  26 }, { 3500,  18 }, { 3450,  11 },
        { 3400,   5 }, { 3350,   2 }, { 3200,   0 },
    };
    if (mv <= 0) return 0;
    if (mv >= tbl[0].mv) return tbl[0].pct;
    for (size_t i = 0; i + 1 < sizeof(tbl) / sizeof(tbl[0]); i++) {
        if (mv >= tbl[i + 1].mv) {
            int span = tbl[i].mv - tbl[i + 1].mv;
            int up   = mv  - tbl[i + 1].mv;
            return tbl[i + 1].pct + (tbl[i].pct - tbl[i + 1].pct) * up / span;
        }
    }
    return 0;
}

/* 真读一次 ADC。返回电池 mV，0 = 读不到。*/
static int batt_read_mv(void)
{
    if (!s_adc_ok) return 0;

    int raw = 0;
    if (adc_oneshot_read(s_adc, BAT_ADC_CHAN, &raw) != ESP_OK) {
        ESP_LOGW(TAG, "adc_oneshot_read failed");
        return 0;
    }

    int mv_adc = 0;
    if (s_cali) {
        adc_cali_raw_to_voltage(s_cali, raw, &mv_adc);
    } else {
        /* 没校准就按 12dB 满量程 3100mV 粗算（会偏，读数只作趋势用）*/
        mv_adc = raw * 3100 / 4095;
    }
    return mv_adc * BATT_DIV_NUM;
}

int app_pwr_batt_mv(void)     { return s_mv_cache; }
int app_pwr_batt_pct(void)    { return mv_to_pct(s_mv_cache); }

/* ★★ 10-04 兰兰实测：「没插电池却显示 94% · 4.15 V」，判定逻辑的根因：
 *
 *   原理图（02_原理图.pdf 实读）给的答案 —— TP4054 的 **CE 脚硬接 GND**
 *   （原理图上 CE 与 GND 短接），CE = 充电使能，接低 = 【常使能】。
 *   于是【没插电池时它照样在充电】：CE 有效 + 电池位空载
 *   ⇒ TP4054 持续往 BAT 线灌充电电流，BAT+ 被拉到接近 4.2V
 *   ⇒ 我们的 1:2 分压读出来就是 4.15V ⇒ 被误判成「满电 94%」。
 *
 *   ⚠️ 这与「USB 延长线」无关（兰兰当时的猜测），换任何 USB 供电都一样；
 *   也不是固件算错 —— 读到的 4.15V 是**真实电平**，只是它代表的是
 *   「TP4054 的空载输出」，不是电池电压。
 *
 * ★ 那么能不能靠「电压」区分「满电」和「没插电池」？——【不能，可靠地区分不了】：
 *     满电锂电 = 4.20V    没插电池（TP4054 空载）= 4.15~4.25V
 *     两者落在同一区间，且这个差距比 ADC 的 ±20mV 噪声还小。
 *     TP4054 的 CHRG1 脚【没有引到 MCU】（原理图上它只接了充电 LED），
 *     所以硬件上根本没有「电池在不在」的电平判据。
 *
 * ⇒ 只能改【判据的方向】：把「电压 > 阈值」反过来当判据是错的
 *   （空载反而电压最高）。正确做法是【让使用者自己声明】——
 *   见 app_pwr_batt_set_present()。默认按「有电池」处理，
 *   这样插上电池后不用做任何事；没插电池时在状态页点一下即可纠正。
 */
bool app_pwr_batt_present(void)
{
    if (!s_batt_manual) return s_mv_cache > 3300;   /* 自动：3300mV 以下当作没插 */
    return s_batt_present_forced;
}

void app_pwr_batt_set_present(bool present)
{
    s_batt_manual     = true;
    s_batt_present_forced = present;
    ESP_LOGI(TAG, "batt: 用户手动声明「%s」", present ? "已接电池" : "未接电池（USB 供电）");
}

bool app_pwr_batt_is_manual(void) { return s_batt_manual; }

/* 没有硬件 CHRG 信号（TP4054 的 CHRG1 脚没引到 MCU，原理图上它只接了 LED）。
 * ⚠️ 所以这里只能给启发式，界面别拿它当"正在充电"的确定依据。*/
bool app_pwr_batt_charging(void)
{
    if (!s_mv_cache) return false;
    return s_mv_cache >= 4020 && app_net_connected();
}

/* 每秒调一次。10-04 铁律：低电自动关机是【页面无关】的，
 * 挂在 stat_timer_cb（全页面每秒都跑）而不是某个页面的刷新里。*/
void app_pwr_tick(void)
{
    if (++s_tick_cnt < 5) return;      /* 5 秒一次 */
    s_tick_cnt = 0;

    int mv = batt_read_mv();
    if (mv <= 0) {
        ESP_LOGW(TAG, "batt: ADC 读不到（没电池？）");
        return;
    }

    /* 只在变化超过 50mV 时打日志，否则 5 秒一行会刷爆串口 */
    static int last_log = -1;
    if (last_log < 0 || mv - last_log > 50 || last_log - mv > 50) {
        ESP_LOGI(TAG, "batt: %dmV (%d%%)  %s", mv, mv_to_pct(mv),
                 mv < s_low_mv ? "LOW" : "ok");
        last_log = mv;
    }
    s_mv_cache = mv;

    /* ---- 低电自动关机 ----
     * ★ 必须先判「有电池」：没插电池时（手动声明为未接，或自动读到 0）
     *   电压数字要么是 TP4054 的空载假读数、要么根本无意义，
     *   拿它跟 3650mV 比只会做出「无电池却在低电」这种荒唐决策。*/
    if (app_pwr_batt_present() && mv < s_low_mv && !s_low_shutdown_done) {
        s_low_shutdown_done = true;
        ESP_LOGW(TAG, "batt: %dmV < %dmV —— 低电自动关机", mv, s_low_mv);
        app_pwr_shutdown();
    }
}

/* ============================================================
 *  ② 关机 / 开机
 * ============================================================ */

bool app_pwr_woke_from_shutdown(void)
{
    /* ⚠️ IDF 6.x：老的 esp_sleep_get_wakeup_cause() 已 deprecated
     *   （本工程 -Werror ⇒ 用它直接编译失败）。
     *   新接口 esp_sleep_get_wakeup_causes() 返回的是【位图】，
     *   所以要用 (1 << ESP_SLEEP_WAKEUP_EXT1) 去掩，
     *   不能拿它和枚举直接比大小。*/
    uint32_t causes = esp_sleep_get_wakeup_causes();
    return (causes & (1ULL << (uint32_t)ESP_SLEEP_WAKEUP_EXT1)) != 0;
}

void app_pwr_shutdown(void)
{
    ESP_LOGI(TAG, "shutdown: 开始");

    /* 1. 停播。不停的话 I2S 还在写，deep sleep 里 DMA 可能留在活动态。*/
    app_radio_stop();
    vTaskDelay(pdMS_TO_TICKS(120));

    /* 2. 关功放（PA_EN 是低电平使能 ⇒ 写 1 关）。
     *    顺序理由：先停播再关功放，反过来会有一声「啪」的爆音。*/
    gpio_set_level(PIN_PA_EN, PA_EN_IDLE);

    /* 3. 关背光。深睡时屏还在供电，不关的话有残影 + 白耗一点电。*/
    app_backlight_set(0);
    vTaskDelay(pdMS_TO_TICKS(60));

    /* 4. 关 WiFi：省掉 RF 部分的待机电流，也避免 deinit 时机不稳。*/
    esp_wifi_disconnect();
    esp_wifi_stop();

    /* 5. ★★ 唤醒源 = IO0（板载 BOOT 键）配 ANY_LOW，也就是【按住才唤醒】。
     *
     *  ★★★ 10-04 兰兰实测：「关机后黑屏，但马上又自己开机了」———
     *     这是 v1.32 的原理性错误，不是他操作不当。根因在 ESP-IDF 头文件：
     *       esp_sleep.h:365  "Once selected pins go into the state given by
     *                         level_mode argument, the chip will be woken up."
     *     ⇒ EXT1 判的是【当前电平】，不是「有没有变化」。
     *     我上一版配 ANY_HIGH 指望「按住时 IO0 是低、不满足；松开变高才触发」
     *     ——【错】，那是边沿的思路。实际是：BOOT 键松开时 IO0 已经被内部上拉
     *     到高，进 deep sleep 的那一瞬间电平就满足 ANY_HIGH
     *     ⇒ 芯片还没睡稳就自己醒回来了，看起来就是「关机后自动开机」。
     *
     *  ⇒ ANY_LOW 才是对的：常态高（不满足，静静睡着），
     *    用户【按住 BOOT 键】才把它拉低 ⇒ 唤醒。
     *
     *  ★★ 随之而来的代价，必须讲清楚（10-04 已向兰兰说明）：
     *    ESP32 复位时会采样 IO0 电平决定是否进下载模式。
     *    ANY_LOW 唤醒时用户【正按着】⇒ IO0 是低 ⇒ 采样为「进下载模式」。
     *    ⇒ 开机必须【按住 BOOT 键，等屏幕亮起再松开】；
     *      若刷不出来，松开再按一下 RESET 键即可恢复。
     *    这是「用 BOOT 键当开关」的固有限制：它本来就是下载模式的引脚，
     *    没有第二个可作开机键的 RTC 按键（板上 RESET 走的是 EN 脚，
     *    EN 不在 RTC 域、也不参与 ext1）。
     */
    esp_sleep_enable_ext1_wakeup_io(1ULL << PIN_BOOT_BTN,
                                     ESP_EXT1_WAKEUP_ANY_LOW);

    ESP_LOGI(TAG, "shutdown: 进 deep sleep —— 【按住】BOOT 键开机，"
                  "屏幕亮起后再松开");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));   /* 让最后几行日志真的发出去 */

    esp_deep_sleep_start();           /* 不返回 */
}

/* ============================================================
 *  初始化
 * ============================================================ */

esp_err_t app_pwr_init(void)
{
    /* ---- 电量检测 ---- */
    /* ⚠️ 三个字段必须全写：IDF 的头文件没给默认值，
     *   少写任何一个 -Wextra 就会报 missing-field-initializers
     *   （本工程 -Werror ⇒ 直接编译失败）。*/
    adc_oneshot_unit_init_cfg_t u = {
        .unit_id   = BAT_ADC_UNIT,
        .clk_src   = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode  = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t r = adc_oneshot_new_unit(&u, &s_adc);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "adc_oneshot_new_unit: %s（电量显示不可用，不影响其他功能）",
                 esp_err_to_name(r));
        s_adc = NULL;
    } else {
        adc_oneshot_chan_cfg_t cc = {
            .atten = BAT_ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        r = adc_oneshot_config_channel(s_adc, BAT_ADC_CHAN, &cc);
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "config_channel: %s", esp_err_to_name(r));
        } else {
            /* 校准：ESP32-S3 只支持 curve fitting。
             * ⚠️ line fitting 那套在头文件里被
             *   `#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED` 包着，
             *   S3 走不到那个分支 ⇒ 直接引用它的类型会报
             *   「unknown type name」。所以这里用条件编译，而不是
             *   「先试 curve、失败再退 line」——
             *   那个写法在 S3 上第一次编译就过不去。*/
            adc_cali_curve_fitting_config_t cf = {
                .unit_id  = BAT_ADC_UNIT,
                .chan     = BAT_ADC_CHAN,
                .atten    = BAT_ADC_ATTEN,
                .bitwidth = ADC_BITWIDTH_DEFAULT,
            };
            if (adc_cali_create_scheme_curve_fitting(&cf, &s_cali) != ESP_OK) {
                ESP_LOGW(TAG, "ADC 曲线校准失败，读数会偏（只作趋势参考）");
            }
            s_adc_ok = true;
        }
    }

    /* ---- 开机先读一次，让日志里立刻有供电真值 ----
     * 兰兰插上电池后第一件事就是看这两行：
     *   「batt: 4100mV (92%)」  = 读数正常，可以继续
     *   「batt: 0」            = 没电池或分压网络不对，去查 JP1 极性*/
    if (s_adc_ok) {
        int mv = batt_read_mv();
        s_mv_cache = mv;
        ESP_LOGI(TAG, "batt: %dmV (%d%%)  present=%d", mv, mv_to_pct(mv), mv > 3300);
        /* ★ 这行是给兰兰自己看的，别删（10-04 他就是靠它判断读数可不可信）。
         * 原理图结论：TP4054 的 CE 脚硬接 GND = 常使能 ⇒ 没插电池时
         * 它照样往 BAT 线灌电流，BAT+ 被顶到 4.2V 附近
         * ⇒ 读数会显示成「满电」。这个假读数【无法】靠电压识别
         *（满电真电池 4.20V vs 空载 4.15V，差距比噪声还小），
         * CHRG1 脚也没引到 MCU ⇒ 只能手动声明，界面上是「电池」那一行点一下。*/
        if (mv > 4000) {
            ESP_LOGI(TAG, "batt: 读数 > 4.0V —— 若现在没插电池，这是 TP4054 的"
                          "空载假读数，不是电池电压（CE 脚常使能，原理图已确认）");
        }
    }

    ESP_LOGI(TAG, "pwr init done: low_mv=%d, wakeup=按住 BOOT 键（深睡时 IO0 须为低）",
             s_low_mv);
    return ESP_OK;
}
