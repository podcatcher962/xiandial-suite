/*
 * 拾声 (XianDial) 硬件版 —— 音频输出（I2S + ES8311 + FM8002E）
 *
 * 依据 10-03 参考对比：小智官方固件（lcdwiki-es3c35p 板级）用的就是
 *   ES8311 + I2C0 + 同一组 I2S 引脚，我们照同一套走，少走弯路。
 *
 * 链路：
 *   ESP32-S3 I2S(主) --MCLK/BCLK/WS/DOUT--> ES8311 --DAC--> FM8002E --> 喇叭
 *   ES8311 寄存器走 I2C0（和触摸共用一条总线，地址 0x18）
 *   PA_EN(IO1) 由本文件直接控制
 *
 * ★ 为什么 PA_EN 自己管：走 codec 驱动的 gpio_if 也能配，但那层是
 *   「传一个 pa_pin 进去、它内部 gpio_set_level」，出问题看不到电平。
 *   自己 gpio_config + set_level，日志里明确打一行，排查快得多。
 */
#include "app_audio.h"

#include "app_display.h"
#include "app_pins.h"

#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"   /* ★ 10-04：codec 互斥锁要用信号量 API */

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"

static const char *TAG = "XSAUD";

#define AUD_SR      44100
#define AUD_CH      2

/* ES8311 的 7 位 I2C 地址：驱动用 8 位形式 0x30，实际线上是 0x18（CE 接地）*/
#define ES8311_I2C_ADDR_7BIT   (ES8311_CODEC_DEFAULT_ADDR >> 1)

/* 写入统计（排查「有链路没声音」用：写入失败会在这里现形）*/
static uint32_t s_wr_bytes = 0;
static uint32_t s_wr_err   = 0;

static i2s_chan_handle_t       s_tx  = NULL;
static esp_codec_dev_handle_t  s_dev = NULL;
static bool                    s_ready = false;
static int                     s_vol = 60;
static int                     s_cur_sr = AUD_SR;   /* 当前 I2S/codec 采样率 */

/* 正弦表：512 点，按频率取步进即可合成任意音高（测试音用） */
#define SINE_N 512
static int16_t s_sine[SINE_N];

static void sine_init(void)
{
    const double pi = 3.14159265358979;
    for (int i = 0; i < SINE_N; i++) {
        double ph = 2.0 * pi * (double)i / (double)SINE_N;
        s_sine[i] = (int16_t)(sin(ph) * 18000.0);
    }
}

/* ============================================================
 *  ★★ 10-04：codec 操作互斥锁（本机第一例「跨任务共享句柄」）
 *
 *  起因：兰兰报「本地播放按倍速就宕机」。
 *  真因：app_audio_set_sample_rate() 要 close codec → 改 I2S 时钟 → open，
 *  而 close/open 期间 s_dev 处于【半死】状态（内部 codec_if 被关、
 *  I2S 通道未使能）。同一个时刻 player_task 正在 app_audio_write() 里
 *  往 s_dev 写 PCM ⇒ 写进一个正在被销毁/重建的对象 ⇒ 野指针 ⇒ 重启。
 *
 *  ★ 这正是速查卡铁律 18 的场景：跨任务共享的句柄，
 *   「句柄归谁」必须写进注释，另一侧只能通过请求变量通信。
 *   这里两个函数是同一文件、同一份句柄，但【调用方在不同任务】
 *   （player_task 写 / UI 任务改倍率），所以仍要互斥。
 * ============================================================ */
static SemaphoreHandle_t s_codec_mtx = NULL;

esp_err_t app_audio_write(const void *pcm, size_t bytes)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    /* ★ 拿不到锁就宁可丢这一帧也不要崩：DMA 欠载会自动补零 = 静音，
     *   而写进正在被 close/open 的 codec 是野指针。*/
    if (s_codec_mtx && xSemaphoreTake(s_codec_mtx, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    int ret = esp_codec_dev_write(s_dev, (void *)pcm, (int)bytes);
    if (s_codec_mtx) xSemaphoreGive(s_codec_mtx);
    if (ret == ESP_CODEC_DEV_OK) {
        s_wr_bytes += (uint32_t)bytes;
    } else {
        if (s_wr_err < 3) ESP_LOGW(TAG, "write FAILED ret=%d (bytes=%u)", ret, (unsigned)bytes);
        s_wr_err++;
    }
    return (ret == ESP_CODEC_DEV_OK) ? ESP_OK : ESP_FAIL;
}

bool app_audio_ready(void)      { return s_ready; }
int  app_audio_sample_rate(void){ return s_cur_sr; }

esp_err_t app_audio_set_sample_rate(int hz)
{
    if (!s_dev || !s_tx)  return ESP_ERR_INVALID_STATE;
    if (hz < 8000 || hz > 192000) return ESP_ERR_INVALID_ARG;
    if (hz == s_cur_sr)   return ESP_OK;

    /* ★★★ 10-04：与 app_audio_write 用【同一把锁】—— close/open 期间
     *   必须没有别的任务在往 s_dev 写 PCM，否则写进一个正在销毁/重建的
     *   codec 对象 = 野指针 = 重启（兰兰报「本地按倍速就宕机」）。
     *   超时给 1.5 秒：写 512 帧只要几毫秒，拿不到说明有别的异常，
     *   宁可这次倍率不生效也不能崩。*/
    if (s_codec_mtx && xSemaphoreTake(s_codec_mtx, pdMS_TO_TICKS(1500)) != pdTRUE) {
        ESP_LOGE(TAG, "改采样率 %d Hz 时拿不到 codec 锁，本次倍率不生效", hz);
        return ESP_ERR_TIMEOUT;
    }

    /* ★ 顺序不能反：i2s_channel_reconfig_std_clock 开头有守卫
     *   （要求通道处于【未使能】状态），所以必须先 close codec 让它把 I2S 关掉，
     *   再改时钟，最后 open 重新使能。*/
    esp_codec_dev_close(s_dev);

    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)hz);
    esp_err_t err = i2s_channel_reconfig_std_clock(s_tx, &clk);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reconfig clock -> %d Hz failed: %s", hz, esp_err_to_name(err));
        /* 尽量退回原采样率，别把音频链路留在半死状态。
         * ★ 必须用【复合字面量】强转：I2S_STD_CLK_DEFAULT_CONFIG() 展开是
         *   `{ ... }` 初始化列表，只能用于初始化；直接 `clk = I2S_STD_...` 赋值
         *   会报 `error: expected expression before '{' token`（10-03 实锤）。*/
        clk = (i2s_std_clk_config_t)I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)s_cur_sr);
        i2s_channel_reconfig_std_clock(s_tx, &clk);
        esp_codec_dev_sample_info_t fs0 = {
            .bits_per_sample = 16, .channel = AUD_CH, .sample_rate = s_cur_sr,
        };
        esp_codec_dev_open(s_dev, &fs0);
        esp_codec_dev_set_out_vol(s_dev, s_vol);
        goto out;              /* ★ 统一出口放锁，别漏一条 return */
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = AUD_CH,
        .sample_rate     = hz,
    };
    int ret = esp_codec_dev_open(s_dev, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "reopen codec @%d Hz failed ret=%d", hz, ret);
        err = ESP_FAIL;
        goto out;
    }
    /* open 会把音量重置成默认，必须重新设回用户调的值 */
    esp_codec_dev_set_out_vol(s_dev, s_vol);
    s_cur_sr = hz;
    ESP_LOGI(TAG, "sample rate -> %d Hz", hz);
    err = ESP_OK;
out:
    /* ★★★ 所有退出路径都在这里放锁。close/open 期间不放 = 其他任务
     *   写进半死的 codec = 宕机。漏任何一条 return 都会重现这个 bug。*/
    if (s_codec_mtx) xSemaphoreGive(s_codec_mtx);
    return err;
}
int  app_audio_get_volume(void) { return s_vol; }

/* ============================================================
 *  倍速播放
 * ============================================================
 * ★ 原理见 app_audio.h 的说明：把 I2S/codec 的时钟拉高到
 *   「源采样率 × 倍率」，解码器输出的采样点数不变、频率不变，
 *   于是整段音频在更短的时间内播完 ⇒ 音调不变、速度变快。
 *
 * ⚠️ 这里存的是【源采样率】（s_src_rate），不是当前时钟。
 *   因为每次换台/换流都要把时钟对齐到那个源的新采样率，
 *   届时要重新乘一次倍率（见 app_audio_apply_rate）。
 */
static int s_speed_pct = 100;      /* 100 / 120 / 140 */
static int s_src_rate  = AUD_SR;   /* 解码器输出的采样率（倍率的基准）*/

/* 把「源采样率 × 倍率」真正配到硬件上 */
static esp_err_t apply_rate(int src_rate)
{
    int hz = src_rate * s_speed_pct / 100;
    return app_audio_set_sample_rate(hz);
}

void app_audio_set_speed(int pct)
{
    /* 只认三档 —— 界面上也只有这三档，别的值一律夹到最近的档。
     * 理由：非整数倍率会让 4/6 的时钟分频算不出整除，
     * IDF 会退回近似值，听起来是「忽快忽慢」。不如不给。*/
    if (pct < 110)      pct = 100;
    else if (pct < 130) pct = 120;
    else                pct = 140;
    if (pct == s_speed_pct) return;

    s_speed_pct = pct;
    int hz = s_src_rate * s_speed_pct / 100;
    esp_err_t r = apply_rate(s_src_rate);
    if (r != ESP_OK) {
        /* 失败就把倍率退回去 —— 宁可还是 1.0X，也不要停在「界面说 1.4X
         * 实际声音是乱的」这种状态。*/
        s_speed_pct = (pct == 140) ? 120 : 100;
        apply_rate(s_src_rate);
        ESP_LOGE(TAG, "speed %d%% 失败，退回 %d%%", pct, s_speed_pct);
    } else {
        ESP_LOGI(TAG, "speed -> %d%%  (源 %d Hz → I2S %d Hz)",
                 s_speed_pct, s_src_rate, hz);
    }
}

int app_audio_get_speed(void) { return s_speed_pct; }

/* 播放器拿到流的真实采样率时调它：既对齐时钟，又记住倍率基准 */
esp_err_t app_audio_set_src_rate(int hz)
{
    if (hz < 8000 || hz > 192000) return ESP_ERR_INVALID_ARG;
    s_src_rate = hz;
    return apply_rate(hz);
}

void app_audio_set_volume(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    s_vol = percent;
    if (s_dev) esp_codec_dev_set_out_vol(s_dev, s_vol);
}

esp_err_t app_audio_init(void)
{
    /* ---- 1. 功放使能脚：先配好并置于【关】电平，等 codec 就绪再开功放 ----
     *   ★ 本板功放是【低电平使能】（证据见 app_pins.h 的 PA_EN_ACTIVE 注释）。
     *   一开始就开功放的话，ES8311 还没配好时 DAC 输出是浮动的，
     *   经 FM8002E 放大就是一记「啪」的爆音（开机音还没放就先吓一跳）。*/
    gpio_config_t pa = {
        .pin_bit_mask = 1ULL << PIN_PA_EN,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&pa), TAG, "pa gpio");
    gpio_set_level(PIN_PA_EN, PA_EN_IDLE);   /* 拉高 = 功放关（静音）*/

    /* ---- 2. I2S 标准模式（主），44100/16bit/立体声 ---- */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    /* ★★★ 2026-10-03 第十七次：显式加大 DMA 缓冲（兰兰报「海外台一开始都听不到，
     *   法国国际等一会儿才有，还是卡，卡得比以前更严重」）。
     *
     *   真因不是「网速慢」，是【读网是阻塞的】：
     *     player_task 里 `esp_http_client_read(http, buf, IN_CHUNK - in_len)`
     *     一次要凑满 64 KB 才返回。法国国际实测 124.9 kbps
     *     ⇒ 64 KB ÷ 15.6 KB/s ≈ 【4.2 秒】。
     *     这 4.2 秒里 player_task 卡在 read 上，【一帧 PCM 都没往 I2S 送】
     *     ⇒ 喇叭断音 4.2 秒 ⇒ 听到的就是「等一会儿才有」＋「一顿一顿」。
     *   IN_CHUNK 从 16 KB 涨到 64 KB 反而让间隔变成 8.4 秒 —— 这就是
     *   「卡得比以前更严重」的原因：**大缓冲在这套单任务结构里是负优化。**
     *
     *   两处一起改，才是真解：
     *     ① 这里：DMA 缓冲 6 个描述符 × 1024 帧 = 24 ms 实时余量，
     *        读网抖动时不会立刻断音。
     *     ② app_radio.c：IN_CHUNK 回到 16 KB，并且【只要求读满剩余空间的 1/4】
     *        就返回去解码（READ_MIN 门槛），把阻塞上限压到 ~1 秒。
     */
    chan_cfg.dma_desc_num  = 4;          /* 默认 6 → 4 */
    chan_cfg.dma_frame_num = 512;        /* 默认 256 帧（1 KB/描述符）→ 4 KB，合计 16 KB 内部 DMA */
    chan_cfg.auto_clear    = true;       /* 没数据时输出 0，防止残留噪声 */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "i2s new chan");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUD_SR),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws   = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din  = PIN_I2S_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "i2s std init");
    /* ★ 绝对不要在这里 i2s_channel_enable()！
     *   esp_codec_dev_open() 内部顺序是：
     *     data_if->set_fmt()  -> i2s_channel_reconfig_std_slot()/reconfig_std_clock()
     *     data_if->enable(true)
     *   而 IDF 的 reconfig_std_slot/clock 开头就有守卫：
     *     ESP_GOTO_ON_FALSE(handle->state == I2S_CHAN_STATE_READY, ESP_ERR_INVALID_STATE,
     *                       ..., "I2S should be disabled before reconfiguring the slot");
     *   ⇒ 提前 enable 会让 set_fmt 直接失败（还是静默失败：esp_codec_dev_open 不检查
     *     set_fmt 的返回值），时钟/位宽按旧值跑，很容易就变成「界面正常但没声音」。
     *   正确做法：只「建通道 + 配 std 模式」，enable 交给 esp_codec_dev_open()。
     *   （10-03 实锤：i2s_std.c:395 / i2s_std.c:reconfig_std_slot 都有这条 READY 守卫）*/

    /* ---- 3. ES8311 ----
     *   先做一次 I2C 在线探测：这一步能干净地把「芯片不在总线上 / 地址错 /
     *   触摸那条 I2C0 冲突」和「codec 寄存器配错」区分开。
     *   探测失败不一定拦下来（codec 可能仍能跑），但日志里必须留痕。*/
    esp_err_t pr = i2c_master_probe(app_i2c_bus_get(), ES8311_I2C_ADDR_7BIT, 200);
    ESP_LOGI(TAG, "ES8311 probe @0x%02X -> %s", ES8311_I2C_ADDR_7BIT,
             pr == ESP_OK            ? "ACK (在线)" :
             pr == ESP_ERR_NOT_FOUND ? "NACK (芯片不在总线)" : "TIMEOUT (总线异常)");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr       = ES8311_CODEC_DEFAULT_ADDR,   /* 0x30(8bit) -> 内部 >>1 = 0x18 */
        .bus_handle = app_i2c_bus_get(),           /* 复用触摸那条 I2C0 */
    };
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctrl != NULL, ESP_FAIL, TAG, "i2c ctrl if");

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if != NULL, ESP_FAIL, TAG, "gpio if");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if        = ctrl,
        .gpio_if        = gpio_if,
        .codec_mode     = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin         = -1,          /* ★ 不用它的 PA 控制，见文件头注释 */
        .pa_reverted    = false,
        .master_mode    = false,       /* ESP32 是 I2S 主机 */
        .use_mclk       = true,
        .digital_mic    = false,
        .invert_mclk    = false,
        .invert_sclk    = false,
        .hw_gain        = { .pa_voltage = 5.0f, .codec_dac_voltage = 3.3f, .pa_gain = 0.0f },
        .mclk_div       = 256,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec_if != NULL, ESP_FAIL, TAG, "es8311 new");

    audio_codec_i2s_cfg_t i2s_data_cfg = {
        .port      = I2S_NUM_0,
        .tx_handle = s_tx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
    ESP_RETURN_ON_FALSE(data_if != NULL, ESP_FAIL, TAG, "i2s data if");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if  = data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_dev != NULL, ESP_FAIL, TAG, "codec dev new");

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = AUD_CH,
        .sample_rate     = AUD_SR,
    };
    int ret = esp_codec_dev_open(s_dev, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec open FAILED ret=%d", ret);
        s_dev = NULL;
        return ESP_FAIL;
    }

    esp_codec_dev_set_out_vol(s_dev, s_vol);
    sine_init();

    /* ★★ 10-04：codec 互斥锁必须在对外开写【之前】建好。
     *   player_task 从这一刻起就可能调 app_audio_write，
     *   而 UI 任务随时可能调 set_sample_rate（换台对齐采样率 / 改倍速）
     *   ⇒ 两者共用 s_dev，必须串行化。*/
    if (!s_codec_mtx) {
        s_codec_mtx = xSemaphoreCreateMutex();
        if (!s_codec_mtx) {
            /* 建不出来就干脆不让别人用 codec：宁可「没声音」也不要「写野指针」*/
            ESP_LOGE(TAG, "codec 互斥锁创建失败 —— 音频写入将被拒绝");
        } else {
            ESP_LOGI(TAG, "codec 互斥锁已建（write / set_sample_rate 串行）");
        }
    }

    /* ---- 4. codec 就绪后才开功放 ----
     *   ★ 本板功放【低电平使能】，所以这里是写 PA_EN_ACTIVE(=0)，
     *     不是写 1！10-03 写反过一次，症状是「日志全绿但一点声音都没有」。*/
    gpio_set_level(PIN_PA_EN, PA_EN_ACTIVE);
    ESP_LOGI(TAG, "PA_EN(IO%d) = %d (功放使能 / 低电平有效)", PIN_PA_EN, PA_EN_ACTIVE);

    s_ready = true;

    ESP_LOGI(TAG, "audio ready: I2S0 %d Hz / 16bit / stereo, ES8311 @I2C0 0x18, vol=%d",
             AUD_SR, s_vol);
    return ESP_OK;
}

/* ============================================================
 *  开机提示音：三声上行「叮-咚-叮」，非阻塞
 * ============================================================ */
/* ★★ 10-04 第二十九次：并发保护。
 *   原来 app_audio_beep_async() 每次调用都 xTaskCreate 一个新任务，
 *   没有任何互斥。两个任务同时往同一条 I2S 写 PCM ⇒ 数据交错
 *   ⇒ 听起来是「提示音卡顿/断断续续」。
 *   触发路径不止开机那一次（状态页有「提示音」键，播放页空台时也响），
 *   连按几下就会撞上。⇒ 用一个句柄做「已在响就拒绝新请求」。*/
static TaskHandle_t s_beep_task = NULL;

static void beep_task(void *arg)
{
    (void)arg;
    if (!s_ready) { s_beep_task = NULL; vTaskDelete(NULL); return; }

    static int16_t buf[SINE_N * AUD_CH];
    const int freqs[3] = { 784, 988, 1319 };   /* G5 / B5 / E6 */
    const int n_frames  = AUD_SR * 170 / 1000;
    const int gap_frames = AUD_SR * 80 / 1000;

    uint32_t b0 = s_wr_bytes, e0 = s_wr_err;

    ESP_LOGI(TAG, "beep: 3 tones (784/988/1319 Hz)");
    for (int k = 0; k < 3; k++) {
        for (int done = 0; done < n_frames; ) {
            int n = n_frames - done;
            if (n > SINE_N) n = SINE_N;
            /* 包络：起止各 15% 做淡入淡出，避免"啪"的爆音 */
            for (int i = 0; i < n; i++) {
                int idx = (int)(((uint32_t)(done + i) * (uint32_t)freqs[k] * SINE_N
                                 / (uint32_t)AUD_SR) & (SINE_N - 1));
                int32_t v = s_sine[idx];
                int pos = done + i;
                int ramp = n_frames / 6;
                if (ramp > 0) {
                    if (pos < ramp)             v = v * pos / ramp;
                    else if (pos > n_frames - ramp) v = v * (n_frames - pos) / ramp;
                }
                int16_t s16 = (int16_t)v;
                buf[i * 2]     = s16;
                buf[i * 2 + 1] = s16;
            }
            app_audio_write(buf, (size_t)n * AUD_CH * sizeof(int16_t));
            done += n;
        }
        /* 间隔静音（每两音之间） */
        if (k < 2) {
            memset(buf, 0, sizeof(int16_t) * SINE_N * AUD_CH);
            for (int done = 0; done < gap_frames; ) {
                int n = gap_frames - done;
                if (n > SINE_N) n = SINE_N;
                app_audio_write(buf, (size_t)n * AUD_CH * sizeof(int16_t));
                done += n;
            }
        }
    }
    /* 收尾静音，把 DMA 里最后一点余量推出去 */
    memset(buf, 0, sizeof(int16_t) * SINE_N * AUD_CH);
    app_audio_write(buf, (size_t)SINE_N * AUD_CH * sizeof(int16_t));

    ESP_LOGI(TAG, "beep done: wrote %u B, err=%u (expect %u B)",
             (unsigned)(s_wr_bytes - b0), (unsigned)(s_wr_err - e0),
             (unsigned)((3 * n_frames + 2 * gap_frames + SINE_N) * AUD_CH * sizeof(int16_t)));
    s_beep_task = NULL;              /* 先清句柄，再删任务：反过来的话
                                     * 中间那一瞬新请求会以为「没人占用」*/
    vTaskDelete(NULL);
}

void app_audio_beep_async(void)
{
    if (!s_ready) return;
    if (s_beep_task != NULL) {
        ESP_LOGW(TAG, "beep 已在响，忽略这一次请求（别让两个任务抢 I2S）");
        return;
    }
    if (xTaskCreate(beep_task, "xs_beep", 4096, NULL, 5, &s_beep_task) != pdPASS) {
        s_beep_task = NULL;
        ESP_LOGE(TAG, "beep 任务创建失败");
    }
}
