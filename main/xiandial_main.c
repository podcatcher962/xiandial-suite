/*
 * 拾声 (XianDial) 硬件版 —— 主程序
 * 板型：LCD Wiki ES3C35P（ESP32-S3 N16R8 / ST77922 QSPI 3.5" / FT6336G / ES8311+FM8002E）
 *
 * 里程碑：
 *   1  屏 + 触摸 + 界面骨架                      ✅
 *   2  交互全接通 + SD 挂载 + 音频链路 + 状态查询  ✅
 *   3  城市声音集本地点播（SD 里的音频）           ✅ 10-03（修「解码器要注册两套」）
 *   4  网络电台出声（WiFi + HTTP 流 + MP3 解码）   ✅ 10-03 傍晚
 *   5  电台库扩容 + 栏目/地区分类 + 机上收藏       ← 当前（10-03 晚）
 *      内置 687 台（10 栏目 / 31 省份，栏目与省份直接照抄网页版台单的 c/g 字段）
 *      收藏存 NVS，机器上可随时加/取消（app_fav.c）
 */
#include "app_audio.h"
#include "app_display.h"
#include "app_fav.h"
#include "app_hist.h"
#include "app_pwr.h"
#include "app_radio.h"
#include "app_sd.h"
#include "app_settings.h"   /* ★ 10-08 全局设置（音量/背光/静音，三产品共用）*/
#include "app_sleep.h"   /* 睡眠定时（10-04 新增） */
#include "app_sys.h"
#include "app_version.h"
#include "ui_shell.h"        /* ★ 10-08 多产品外壳：开机进【主菜单】，
                              *   灵签/天气/股票都是它下面的产品。
                              *   （10-07 是直接调 ui_lingqian.h；那一层现在由外壳代理）*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "app_stlist.h"

static const char *TAG = "XSDIAL";

/* ============================================================
 *  调试用：开机自动试播
 * ============================================================
 *  ★ 为什么需要它：这块板子验证「有没有出声」本来只能靠耳朵 ——
 *    固件没有截图通道，人也不可能一边听一边看串口。
 *    而调试网络电台时最需要的是【不用手点】也能在日志里看到：
 *      有没有连上 WiFi → HTTP 状态码对不对 → 解出多少 Hz → 往 I2S 写了多少字节。
 *    所以留这个口子：往 SD 卡根目录丢一个 autoplay.txt，第一行写
 *       3                     → 开机自动播内置台单第 3 台（0 起算）
 *       http://host/x.mp3     → 开机自动播这个流地址（试新源用）
 *    没有这个文件 = 什么都不做（发行版就是这种情况）。
 *  ⚠️ 它只是「自证工具」，不参与界面逻辑：播起来之后首页大卡会
 *    自己跟着更新（stat_timer_cb 里比对 app_radio_station_current()）。
 *
 *  ★ XS_AUTOPLAY_FORCE：卡拿不出来（在读卡器上抠卡麻烦）时用的临时开关，
 *    打开 = 就算没有 autoplay.txt 也照样试播一次，好让日志能证明「真出声了」。
 *    验完必须改回 0 再编一版 —— 发行版不能自己开播。
 * ============================================================ */
#define XS_AUTOPLAY_FORCE   0      /* 0 = 只有卡里有 autoplay.txt 才自动试播 */
#define XS_AUTOPLAY_IDX     42     /* 北京交通广播 http://lhttp.qingting.fm/... */

static void autoplay_task(void *arg)
{
    (void)arg;

    /* 等 WiFi（最多 20 秒）—— app_main 里那次连接是异步的 */
    for (int i = 0; i < 40 && !app_net_connected(); i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!app_net_connected()) {
        ESP_LOGW(TAG, "autoplay: 20 秒内没连上 WiFi，放弃（先解决网络再说）");
        vTaskDelete(NULL);
        return;
    }

    char line[300] = {0};
    FILE *fp = fopen("/sdcard/autoplay.txt", "rb");
    if (fp) {
        if (fgets(line, sizeof(line), fp) == NULL) line[0] = '\0';
        fclose(fp);
    } else if (XS_AUTOPLAY_FORCE) {
        snprintf(line, sizeof(line), "%d", XS_AUTOPLAY_IDX);
        ESP_LOGW(TAG, "autoplay: 卡里没 autoplay.txt，但自证开关开着 → 试播第 %d 台",
                 XS_AUTOPLAY_IDX);
    } else {
        ESP_LOGI(TAG, "autoplay: 卡里没有 autoplay.txt，按正常流程等你点屏幕");
        vTaskDelete(NULL);
        return;
    }

    /* 清 BOM / \r\n / 首尾空白 */
    char *s = line;
    if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF) s += 3;
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ')) s[--n] = '\0';
    if (n == 0) {
        ESP_LOGW(TAG, "autoplay: 第一行是空的");
        vTaskDelete(NULL);
        return;
    }

    esp_err_t r;
    if (strncmp(s, "http", 4) == 0) {
        ESP_LOGI(TAG, "autoplay: 直接播 URL -> %s", s);
        r = app_radio_play_url("autoplay", s);
    } else {
        int idx = atoi(s);
        ESP_LOGI(TAG, "autoplay: 播内置台单第 %d 台", idx);
        r = app_radio_play_station(idx);
    }
    ESP_LOGI(TAG, "autoplay: play -> %s", esp_err_to_name(r));

    /* 盯 12 秒，把「到底出没出声」用数字写进日志 */
    for (int i = 0; i < 12; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "autoplay[%2ds]: playing=%d paused=%d stream=%d level=%3d "
                      "rate=%d ch=%d elapsed=%ds err=\"%s\"",
                 i + 1,
                 (int)app_radio_is_playing(), (int)app_radio_is_paused(),
                 (int)app_radio_is_stream(), app_radio_level(),
                 app_radio_stream_rate(), app_radio_stream_channel(),
                 app_radio_elapsed_s(), app_radio_last_error());
    }
    ESP_LOGI(TAG, "autoplay: 观察结束（电平一直 0 = 没出声）");
    vTaskDelete(NULL);
}

void app_main(void)
{
    /* ★★ 10-08 夜：整机名定为「拾声集 / XianDial Suite」（兰兰定名，见
     *   `2026-09-15-21-50-21/四合一-命名报告.md` 第五版）。
     *   ⚠️ 与子产品名的关系：拾声集（整机）⊃ 拾声（电台子产品）。
     *      所以【只有指代整机的地方】改成"拾声集"，
     *      首页那张卡与电台页标题仍叫"拾声电台"——那是子产品，不动。
     *   ⚠️ 这行的中文会进字库（扫源码、剥注释后仍在字符串里）；
     *      "拾/声/集"三字都已在主字库四档里，不用重跑生成器。*/
    ESP_LOGI(TAG, "============ 拾声集 XianDial Suite ============");
    ESP_LOGI(TAG, "board: LCD Wiki ES3C35P  ESP32-S3 N16R8");
    ESP_LOGI(TAG, "固件版本: %s (%s)  %s", XS_FW_VERSION, XS_FW_DATE, XS_FW_NOTE);

    /* ★★ 0. 先判断这次是不是「关机后唤醒」起来的。
     *   放在最前面，因为后面的初始化（屏、WiFi、SD）都要好几秒，
     *   而深睡唤醒最怕的就是【看起来像没开机】。
     *   ★ 深睡后 esp_timer / NVS 之外的运行时状态全部重置，
     *     所以 app_sleep 的定时器必须在下面重新初始化（app_sleep_init 里做了）。*/
    bool woke = app_pwr_woke_from_shutdown();
    if (woke) {
        ESP_LOGI(TAG, "wakeup: EXT1 —— 由「关机 + 松开 BOOT 键」启动");
    }

    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        r = nvs_flash_init();
    }
    ESP_ERROR_CHECK(r);

    /* 1. 屏 + 触摸 + LVGL */
    ESP_ERROR_CHECK(app_display_init());

    /* 2. 音频（放在界面之前：这样 splash 结束时的开机提示音一定已就绪）
     *    失败不致命 —— 界面照跑，只是没声音。*/
    esp_err_t ar = app_audio_init();
    if (ar != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed (%s) - UI keeps running",
                 esp_err_to_name(ar));
    }

    /* 2.4★ 10-08 新增：全局设置（音量/背光/静音）—— 三个产品共用一份。
     *   ★ 必须在 app_audio_init() 之后（要写 ES8311 的 DAC 增益），
     *     且在建界面之前（否则会看到背光从 display 默认的 85 跳到 50 的那一下）。
     *   ★ 默认值：音量 100 / 背光 50 / 声音【关】（兰兰 10-08 指定）。*/
    xs_cfg_init();

    /* 2.45★ 10-06新增：SD 卡挂载【必须提前到台单加载之前】。
     *     原来挂载在第 4 步（L208），但台单在 2.6 收藏初始化时就要用到，
     *     顺序反了会导致「导入了stations.tsv 却读不到」。
     *     挂载本身很轻（SDIO 初始化），放在音频之后不影响开机速度。
     *     失败不致命 —— 台单会回落到内置，见下。*/
    static app_sd_entry_t s_ents[24];
    esp_err_t sd = app_sd_mount();
    if (sd == ESP_OK) {
        int n = app_sd_list("/sdcard", s_ents, 24);
        ESP_LOGI(TAG, "SD root listing: %d entries", n);
    } else {
        ESP_LOGW(TAG, "SD not available (%s) - 台单用内置", esp_err_to_name(sd));
    }

    /* 2.5 播放器：注册 MP3/AAC/WAV/FLAC/M4A/TS 解码器（注册≠占内存，很轻）。
     *     失败只影响本地点播，界面照跑。*/
    if (app_radio_init() != ESP_OK) {
        ESP_LOGW(TAG, "radio init failed - local playback unavailable");
    }

    /* 2.55★ 10-06新增：载入台单。读 /sdcard/stations.tsv（使用者自己的），
     *     没有卡或没有文件则回落到内置 g_stations。
     *     ★ 必须在 app_fav_init() 之前 —— 收藏按【台名】匹配台单，
     *       台单还不确定就初始化收藏，会把常听台判成"找不到"。
     *     ★ 必须在 app_radio_init() 之后无要求，但放这里最省事。*/
    app_st_src_t stsrc = app_st_load();
    app_st_dump_info();
    /* 串口命令通道：st_reload（重读卡） / st_dump（打印台单）。
     * 开发时板载 TF 走 SDIO、Windows 看不到卡，靠它免得反复断电；
     * 使用者换了台单也能用。*/
    app_st_start_serial_cmd();
    ESP_LOGI(TAG, "台单来源: %s（%d 条）",
             stsrc == APP_ST_SRC_TSV ? "SD 卡 stations.tsv" : "内置（自用版）",
             app_st_count());

    /* 2.6 收藏：必须在 app_radio_init() 之后（解析台名要用台单），
     *     且在 ui_init() 之前 —— 界面第一次画「收藏电台」卡就要知道有几个。
     *     首次开机自动播种兰兰常听的台，之后从 NVS 读，机器上可随时加减。*/
    if (app_fav_init() != ESP_OK) {
        ESP_LOGW(TAG, "fav init failed - 收藏页会是空的");
    }
    if (app_hist_init() != ESP_OK) {
        ESP_LOGW(TAG, "hist init failed - 首页不会顶历史台");
    }

    /* 2.7 睡眠定时（10-04 新增）：读 NVS 恢复上次设定。
     *     必须在 nvs_flash_init 之后（已经过了），
     *     且在 ui_init 之前 —— 夜间页建胶囊时要知道该选中哪一颗。
     *     ★ 深睡唤醒后 esp_timer 是全新的，这里调的 app_sleep_init()
     *     会重新注册定时器 —— 不重挂的话睡眠定时会静默失效。*/
    app_sleep_init();

    /* 2.8 电源 / 电量（10-04 新增，读 IO8 的 BAT_ADC）。
     *     日志里那行「batt: xxxxmV」就是兰兰插上电池后的第一个验收点。*/
    app_pwr_init();

    /* 2.9 板载 RGB 状态灯（IO40，WS2812 via RMT）。
     *     失败不致命 —— 只是灯不亮。*/
    app_led_init();

    /* 3. 界面（★ 10-08 多产品外壳：建【主菜单】，不再直接进灵签）
     *    ★ 为什么改成先过主菜单：这块板要装三个产品（灵签/天气/股票），
     *      直接进灵签，另外两个就没地方挂。分工是：
     *        外壳 ui_shell  = 决定"进哪个产品"
     *        产品自己       = 建屏 / 拆屏（ui_lingqian_init / _leave）
     *    ★ 只插一块板时也安全：外壳不碰任何硬件，只是个界面调度层。*/
    if (lvgl_port_lock(0)) {
        ui_shell_init();
        lvgl_port_unlock();
    } else {
        ESP_LOGE(TAG, "lvgl lock failed");
    }

    ESP_LOGI(TAG, "boot done: heap free=%u  internal free=%u  psram free=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* 4. SD 卡 / 本地音频库。
     *    ★ 10-06：挂载与列目录已提前到 2.45（台单要读它），
     *      这里只做音频计数与更详细的日志，不再重复挂载。
     *      app_sd_mount() 本身幂等，但少做一次是一次。*/
    if (sd == ESP_OK) {
        for (int i = 0; i < 24; i++) {
            if (s_ents[i].name[0] == '\0') break;
            ESP_LOGI(TAG, "   [%d] %s%s  %u B", i,
                     s_ents[i].name, s_ents[i].is_dir ? "  <DIR>" : "",
                     (unsigned)s_ents[i].size);
        }
        /* 递归最多 5 层（10-03 由 2 层放宽：卡里是「小宇宙播客 64K/<节目名>/x.mp3」）*/
        ESP_LOGI(TAG, "SD audio count (recursive, <=5 levels) = %d",
                 app_sd_count_audio("/sdcard"));
    }

    /* 5. 网络（STA 初始化）*/
    esp_err_t nr = app_net_init();
    if (nr != ESP_OK) {
        ESP_LOGW(TAG, "net init failed (%s)", esp_err_to_name(nr));
    }

    /* 6. WiFi 账密：★ 优先用 NVS 里存的（用户在「状态 → WiFi 设置」页里输的），
     *    没有才回退到 SD 卡根目录的 wifi.txt。
     *    —— 顺序反过来是因为发行版用户不可能先去电脑上造一个 txt 插卡；
     *       wifi.txt 保留是为了「卡片插着就能连」的老用法与应急兜底。*/
    if (nr == ESP_OK) {
        if (!app_net_connect_saved() && sd == ESP_OK) {
            esp_err_t wr = app_net_connect_from_file("/sdcard/wifi.txt");
            if (wr != ESP_OK) {
                ESP_LOGW(TAG, "wifi.txt connect: %s", esp_err_to_name(wr));
                ESP_LOGW(TAG, "两处都没有可用账密 —— 到「状态」页点「WiFi 设置」在机器上输");
            }
        }
    }

    ESP_LOGI(TAG, "after all init: internal free=%u  psram free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* 7. 调试钩子：卡里有 autoplay.txt 就自动试播一次（见函数上的说明）。
     *    发行版卡里没这个文件，等于不存在。*/
    xTaskCreate(autoplay_task, "xs_autoplay", 4096, NULL, 4, NULL);
}
