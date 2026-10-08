/*
 * 拾声 (XianDial) 硬件版 —— 系统状态查询 + 网络
 *
 * 10-03 首次实现。要点：
 *   ① 状态一律「现查现取」：内存、SD、音频都是读实时句柄，不做缓存，
 *      这样状态页刷新一次就是真数据。
 *   ② WiFi 只做到初始化 + 扫描。扫描用【阻塞式】API，但跑在独立一次性任务里，
 *      绝不占用 LVGL 任务（否则界面会卡住 2~3 秒）。
 *   ③ 扫描结果拷进静态数组，UI 定时器随时读，无锁竞争（UI 只读、任务只写）。
 */
#include "app_sys.h"

#include "app_audio.h"
#include "app_display.h"
#include "app_sd.h"
#include "app_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>        /* time()/time_t：app_sys_utc_now() 要 */

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "XSSYS";

static int  s_brightness = 85;
static bool s_net_inited = false;
static volatile bool s_net_connected = false;
static int  s_rssi = 0;

/* ---- WiFi 账密（来自 /sdcard/wifi.txt）---- */
static char          s_ssid[33]  = "";
static char          s_ip[20]    = "";
static volatile bool s_has_cred  = false;
static volatile int  s_retry     = 0;
static volatile bool s_conn_fail = false;    /* 重试次数用完仍连不上 */
static volatile int  s_last_reason = 0;      /* 最后一次断线原因码 */
static bool s_sntp_started = false;          /* 校时只发起一次 */

#define NET_RETRY_MAX  8

/* 断线原因码 -> 人话。
 * ★ 只看到 reason=201 等于没信息；201 是 NO_AP_FOUND，能直接指路。*/
const char *app_net_reason_str(int reason)
{
    switch (reason) {
    case 0:   return "正常关闭";
    case 2:   return "认证过期";
    case 15:  return "四次握手超时（多半密码错）";
    case 200: return "信号丢失（beacon 超时）";
    case 201: return "找不到这个 WiFi（名字错 / 只开 5G / 太远）";
    case 202: return "密码错误（认证失败）";
    case 203: return "路由器拒绝关联";
    case 204: return "握手超时（多半密码错）";
    case 205: return "连接中断";
    default:  return "未知原因";
    }
}

int app_net_last_reason(void) { return s_last_reason; }

/* 极短版：给屏幕那一行用（完整版太长，会顶出卡片）*/
const char *app_net_reason_short(int reason)
{
    switch (reason) {
    case 0:   return "正常";
    case 2:   return "认证过期";
    case 15:  return "密码错?";
    case 200: return "信号丢失";
    case 201: return "找不到该网络";
    case 202: return "密码错";
    case 203: return "被拒绝";
    case 204: return "密码错?";
    case 205: return "连接中断";
    default:  return "未知";
    }
}

/* ---- 扫描结果（最多 AP_MAX 个）----
 * ★ AP_MAX 从 10 提到 16（10-04）：兰兰要「能连的排前面」，
 *   而家里/楼里通常同时能扫到十几个 —— 只留 10 个的话，
 *   排完序他自己的那个网络可能直接掉出列表了。*/
#define AP_MAX 16
typedef struct {
    char ssid[33];
    int  rssi;
} ap_item_t;
static ap_item_t        s_aps[AP_MAX];
static volatile int     s_ap_count = 0;
static volatile bool    s_scan_running = false;

/* ============================================================
 *  亮度
 * ============================================================ */
void app_sys_set_brightness(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    s_brightness = percent;
    app_backlight_set(percent);
}

int app_sys_get_brightness(void) { return s_brightness; }

uint32_t app_sys_uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000LL);
}

/* ============================================================
 *  网络校时（SNTP）
 * ============================================================
 *  顶栏要显示「当前时间」，而 ESP32 复位后 RTC 是 1970-01-01 —— 不校时
 *  顶栏只能写死一个假时间。SNTP 依赖 DNS 与 UDP，都在 lwip 里（已启用）。
 *
 *  ★ 为什么用一次性 esp_sntp_set_time_sync_notification_cb 而不是常驻：
 *    收音机不联网时显示的是「开机后的小时数」，联网成功后校一次就够；
 *    常驻 SNTP 每小时发一次包，纯粹浪费电。
 *
 *  ★★ 时区：这是本节最容易踩的坑（10-03 第一次烧完就发现）★★
 *   SNTP 协议传的是【UTC】。本机 sdkconfig 没有设 TZ、也没有本地时区数据
 *   （LWIP_SNTP_LOCAL_TIME_OFFSET 之类没开），所以回调拿到的 tv_sec 就是 UTC。
 *   10-03 实测：unix=1791025004 = UTC 10:56，而兰兰本地是 18:56 ——
 *   差整整 8 小时，顶栏会显示「10:56」。
 *   两条路可选：
 *     a) 改 sdkconfig 加时区 → 触发 LVGL + wpa_supplicant 全量重编 25~30 分钟
 *     b) 回调里自己把 tv_sec 加上偏移，再 settimeofday 写回去（秒级）
 *   选 b。★ 只适用于「固定在一个时区」的机器 —— 这台机在国内，够用；
 *   真要跨时区用，得改成按配置查偏移表。
 */
#define XS_TZ_OFFSET_SEC  (8 * 3600)      /* 中国标准时间 UTC+8，无夏令时 */

long app_sys_utc_now(void)
{
    /* ★★ 10-04：time() 已经被上面的 +8h 污染成「本地时间」了，
     *   拿它去减外部的 UTC 时间戳会凭空多出 28800 秒
     *   （HLS 直播延迟「28831s」的真正原因，不是分片取老）。
     *   这里减回真 UTC。没校过时 time() 本身就是垃圾值，减了也一样垃圾，
     *   调用方（HLS）本来就只在联网后才有意义。*/
    time_t t = 0;
    time(&t);
    return (long)t - XS_TZ_OFFSET_SEC;
}

static void on_sntp_sync(struct timeval *tv)
{
    /* tv->tv_sec 是 UTC；界面要本地时间，所以先加偏移再写回系统时钟。
     * 这样后面 localtime_r() 拿到的就是本地时间，顶栏/夜间页都不用再各自处理。*/
    tv->tv_sec += XS_TZ_OFFSET_SEC;
    settimeofday(tv, NULL);
    ESP_LOGI(TAG, "SNTP synced, local unix time = %ld (UTC%+d)",
             (long)tv->tv_sec, XS_TZ_OFFSET_SEC / 3600);
}

void app_net_sync_time(void)
{
    if (s_sntp_started) return;
    s_sntp_started = true;

    /* ★ IDF 6.1 的 esp_sntp_config_t 用 servers[] 数组（不是旧版的单个 server
     *   字段，写 cfg.server 会直接编译报错）。数组长度由 sdkconfig 的
     *   CONFIG_LWIP_SNTP_MAX_SERVERS 决定 —— 本项目 = 1，所以只能挂一个。
     *   选 ntp.aliyun.com：国内通畅率高；公共 pool.ntp.org 在国内经常超时。
     *   ★ 想加备选（如 ntp.ntsc.ac.cn）要先在 sdkconfig 里把 MAX_SERVERS 改成 2，
     *   那是全量重编，别为了一个备份服务器折腾。*/
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    cfg.sync_cb = on_sntp_sync;
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "sntp init failed（不影响收听）");
        return;
    }
    ESP_LOGI(TAG, "SNTP 校时已启动：ntp.aliyun.com");
}

/* ============================================================
 *  状态快照
 * ============================================================ */
void app_sys_get(app_status_t *st)
{
    if (!st) return;
    memset(st, 0, sizeof(*st));

    const esp_app_desc_t *app = esp_app_get_description();
    /* ★ 状态页显示的固件版本改用 app_version.h 的 XS_FW_VERSION，
     *   不再用 esp_app_desc_t.version。
     *   原因：后者来自 ESP-IDF 的 PROJECT_VER，本项目从没设过它，
     *   状态页那一行一直显示「1.0 · Jan  1 1970」—— 等于没有版本号，
     *   出问题时无法判断板上跑的是哪一版。
     *   app->date 仍保留（它带编译时刻，排障时有用），格式化成 mm-dd hh:mm。*/
    static char ver_buf[32];
    static char date_buf[24];
    snprintf(ver_buf, sizeof(ver_buf), "%s (%s)", XS_FW_VERSION, XS_FW_DATE);
    st->fw_ver     = ver_buf;
    /* ⚠️ esp_app_desc_t.date 是 char[16] 数组不是指针 ——
     * 写 `app->date &&` 会被 gcc 判成「地址永不为 NULL」(-Werror=address)。*/
    if (app && app->date[0]) {
        /* IDF 的 date 形如 "Oct  4 2026 08:15:30"，太宽；只取我们用得上的部分 */
        const char *p = strchr(app->date, ' ');
        const char *q = p ? strchr(p + 1, ' ') : NULL;
        snprintf(date_buf, sizeof(date_buf), "%s",
                 q ? (q + 1) : app->date);
    } else {
        snprintf(date_buf, sizeof(date_buf), "?");
    }
    st->build_date = date_buf;

    st->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000LL);

    st->heap_internal_kb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
    st->heap_psram_kb    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
    st->heap_min_kb      = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024;

    st->sd_mounted = app_sd_is_mounted();
    if (st->sd_mounted) {
        uint32_t mb = 0;
        const char *fs = "?";
        app_sd_get_info(&mb, &fs);
        st->sd_cap_mb = mb;
        /* ★ 用带缓存的计数：本函数被状态页定时器每秒调用一次，
         *   直接 app_sd_count_audio() 会每次递归 stat 几千条目录项。*/
        st->sd_audio_count = app_sd_audio_total(false);
    }

    st->audio_ready = app_audio_ready();
    st->volume      = app_audio_get_volume();
    st->brightness  = s_brightness;

    st->net_state = app_net_state_str();
    st->net_ip    = app_net_ip_str();
    st->net_ssid  = app_net_ssid();
    st->rssi      = s_rssi;
    st->ap_count  = s_ap_count;
    st->cpu_mhz   = 240;
}

/* ============================================================
 *  网络
 * ============================================================ */
/* 断线重连：不能在事件回调里直接 esp_wifi_connect()（会变成忙等），
 * 单开一个小任务睡 3 秒再连，顺便把重试次数限住，免得密码错时无限刷屏。*/
static void reconnect_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));
    if (s_has_cred && !s_net_connected) {
        ESP_LOGI(TAG, "reconnect attempt %d ...", s_retry);
        esp_wifi_connect();
    }
    vTaskDelete(NULL);
}

static void net_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "wifi sta started");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        s_net_connected = false;
        s_rssi = 0;
        s_ip[0] = '\0';
        s_last_reason = d ? d->reason : -1;
        ESP_LOGW(TAG, "disconnected (reason=%d: %s)", s_last_reason,
                 app_net_reason_str(s_last_reason));
        if (s_has_cred && s_retry < NET_RETRY_MAX) {
            s_retry++;
            xTaskCreate(reconnect_task, "xs_reconn", 3072, NULL, 4, NULL);
        } else if (s_has_cred) {
            /* ★ 这里必须把状态切成「连不上」，否则界面会永远停在「连接中」——
             *   兰兰 10-03 问过这个：一直显示连接中，其实早就放弃重试了。*/
            s_conn_fail = true;
            ESP_LOGE(TAG, "WiFi 连接失败：自动重试 %d 次都没连上（最后 reason=%d: %s）",
                     s_retry, s_last_reason, app_net_reason_str(s_last_reason));
            ESP_LOGE(TAG, "  请检查 wifi.txt 的 SSID/密码；改好后到「状态」页点「连 wifi.txt」重来");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_net_connected = true;
        s_conn_fail     = false;
        s_retry         = 0;
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) s_rssi = ap.rssi;
        ESP_LOGI(TAG, "got ip: %s  (ssid=\"%s\", %d dBm)", s_ip, s_ssid, s_rssi);
        /* 拿到 IP 才能做 DNS ⇒ 这时才是校时的时机（顶栏要显示当前时间）*/
        app_net_sync_time();
    }
}

esp_err_t app_net_init(void)
{
    if (s_net_inited) return ESP_OK;

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    esp_err_t r = esp_event_loop_create_default();
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(r));
        return r;
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, net_event_handler, NULL, NULL),
        TAG, "wifi evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, net_event_handler, NULL, NULL),
        TAG, "ip evt");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    /* 省电模式关掉：扫描/连接都响应快些，收音机不在乎那点功耗 */
    esp_wifi_set_ps(WIFI_PS_NONE);

    s_net_inited = true;
    ESP_LOGI(TAG, "net ready (STA, not connected)");
    return ESP_OK;
}

const char *app_net_state_str(void)
{
    if (!s_net_inited)  return "未初始化";
    if (s_net_connected) return "已连接";
    if (!s_has_cred)    return "未配置";
    if (s_conn_fail)    return "连不上";
    return "连接中";
}

/* 给界面用的「能一眼区分」的五态（别拿字符串去 strcmp）*/
app_net_link_t app_net_link_state(void)
{
    if (!s_net_inited)  return APP_NET_IDLE;
    if (s_net_connected) return APP_NET_OK;
    if (!s_has_cred)    return APP_NET_NO_CRED;
    if (s_conn_fail)    return APP_NET_FAIL;
    return APP_NET_CONNECTING;
}

bool        app_net_connected(void) { return s_net_connected; }
const char *app_net_ip_str(void)    { return s_ip; }
const char *app_net_ssid(void)      { return s_ssid; }

/* ============================================================
 *  NVS 里的 WiFi 账密
 * ----------------------------------------------------------
 * ★ 为什么要有这一份（兰兰 10-04：「状态页的扫描WiFi还要有连接设置页，
 *   有键盘输入」）：
 *   原来唯一的配置途径是 SD 卡根目录的 wifi.txt。那在验证期很省事，
 *   但发行版不行 —— 用户拿到机器不可能先去电脑上造一个 txt 插卡。
 *   ⇒ 机器上直接输账密、存 NVS、开机自动连。wifi.txt 降级为兜底。
 *
 * ⚠️ 存 NVS 有个安全边界：NVS 不加密，串口能读到。
 *   这是「家里的路由器」量级的取舍，不是保存银行卡密码 —— 可以接受，
 *   但别拿它存任何真正敏感的东西。
 */
#define WIFI_NS    "xs_wifi"
#define WIFI_K_SSID "ssid"
#define WIFI_K_PASS "pass"

static void wifi_save_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(%s) 失败", WIFI_NS);
        return;
    }
    esp_err_t r = nvs_set_str(h, WIFI_K_SSID, ssid);
    if (r == ESP_OK) r = nvs_set_str(h, WIFI_K_PASS, pass ? pass : "");
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    if (r == ESP_OK) {
        ESP_LOGI(TAG, "账密已存进 NVS（下次开机自动连）");
    } else {
        ESP_LOGW(TAG, "存 NVS 失败: %s", esp_err_to_name(r));
    }
}

/* 前向声明：下面 app_net_connect_creds() 会用它做「差一个下划线」的
 * 模糊匹配，但它的定义在文件更靠后的位置（和 wifi.txt 那条路共用）。*/
static void scan_and_check_ssid(char *ssid, size_t ssid_sz);

/* 用给定的账密连（并顺手存 NVS）。SSID 为空 = 断开，不动配置。*/
esp_err_t app_net_connect_creds(const char *ssid, const char *pass)
{
    if (!s_net_inited) {
        ESP_LOGW(TAG, "net not inited");
        return ESP_ERR_INVALID_STATE;
    }
    if (ssid == NULL || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;

    char s[64] = "";
    char p[72] = "";
    snprintf(s, sizeof(s), "%.32s", ssid);
    if (pass) snprintf(p, sizeof(p), "%.63s", pass);

    /* ★★ 10-04 关键补漏：网页配网这条路也必须做【归一化模糊匹配】。
     *   开机走 wifi.txt 的路径一直有 scan_and_check_ssid() 兜底，
     *   所以「名字少一个下划线」这种手误能被自动纠正；
     *   但网页提交走的是本函数，原来【一次扫描都不做】直接 esp_wifi_connect。
     *   真机实测就是这么翻车的：
     *       XSSYS: got ip 192.168.0.105 (ssid="MyGuest")   ← 开机连上了
     *       XSPROV: 网页提交 ssid="MyGuest"               ← 网页提交的
     *       W: disconnected (reason=201: 找不到这个 WiFi)     ← 连不上
     *   802.11 把这两个视为两个不同的网络。
     *   ⇒ 发布版把真实 SSID 换成占位名：教学价值在「差一个下划线会被当成
     *     两个网络」这件事本身，不在具体那串字符。
     *   顺带：这次多花的 2~3 秒扫描还会把附近所有 AP 打进日志，
     *   「名字错 / 只开 5G / 太远」三种原因一眼可辨。*/
    scan_and_check_ssid(s, sizeof(s));

    /* ★ 密码只打长度，绝不打明文 */
    ESP_LOGI(TAG, "connect_creds: ssid=\"%s\", password %s (%u chars)", s,
             p[0] ? "已设置" : "为空(开放网络)", (unsigned)strlen(p));

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, s, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, p, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;   /* 开放/WPA/WPA2/WPA3 全兼容 */
    wc.sta.pmf_cfg.capable    = true;
    wc.sta.pmf_cfg.required   = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "set wifi cfg");

    strncpy(s_ssid, s, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    s_has_cred  = true;
    s_retry     = 0;
    s_conn_fail = false;

    esp_wifi_disconnect();
    esp_err_t cr = esp_wifi_connect();
    ESP_LOGI(TAG, "wifi connect -> %s", esp_err_to_name(cr));
    wifi_save_creds(s, p);
    return cr;
}

bool app_net_connect_saved(void)
{
    if (!s_net_inited) return false;
    nvs_handle_t h;
    if (nvs_open(WIFI_NS, NVS_READONLY, &h) != ESP_OK) return false;
    char ssid[64] = "", pass[72] = "";
    /* ⚠️ nvs_get_str 的第 4 参是 size_t*（长度指针）不是 size_t。
     * 传 sizeof() 会被判「integer to pointer」直接编译失败。*/
    size_t n1 = sizeof(ssid), n2 = sizeof(pass);
    esp_err_t r1 = nvs_get_str(h, WIFI_K_SSID, ssid, &n1);
    esp_err_t r2 = nvs_get_str(h, WIFI_K_PASS, pass, &n2);
    nvs_close(h);
    if (r1 != ESP_OK || ssid[0] == '\0') return false;
    if (r2 != ESP_OK) pass[0] = '\0';
    ESP_LOGI(TAG, "NVS 里有账密：ssid=\"%s\"，直接连", ssid);
    return app_net_connect_creds(ssid, pass) == ESP_OK;
}

/* ============================================================
 *  WiFi 账密文件 -> 连接
 * ============================================================ */

/* 清掉一行里的 BOM / \r / \n / 首尾空格与制表符 */
static void trim_line(char *s)
{
    if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF) {
        memmove(s, s + 3, strlen(s + 3) + 1);     /* Windows 记事本存 UTF-8 会带 BOM */
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' ||
                     s[n - 1] == ' '  || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (i > 0) memmove(s, s + i, strlen(s + i) + 1);
}

/* SSID 归一化：只保留字母（转小写）、数字与非 ASCII 字节，丢掉 _ - 空格 . 等。
 * 目的：把 TPGuest / TP_Guest / tp-guest / TP Guest 当成同一个名字。
 * ★ 10-03 实锤：兰兰的 wifi.txt 写 TPGuest，路由器实际叫 TP_Guest，
 *   差一个下划线，机器一直 reason=201 —— 这种手误太常见了。*/
static void norm_ssid(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    if (!in || !out || outsz == 0) return;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < outsz; p++) {
        unsigned char c = *p;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80) {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

/* ---- 连接前先扫一遍，把所有热点打进日志，并做一次名字核对 ----
 * ★ 为什么值得多花 2~3 秒：WiFi 连不上无非三种原因 ——
 *     ① 名字/密码写错   ② 那个网络只开 5 GHz（ESP32-S3 只有 2.4 G，永远扫不到）
 *     ③ 太远 / 是隐藏网络。
 *   只看到一句 "disconnected (reason=201)" 等于没有信息；把扫到的 SSID 全列出来，
 *   一眼就能定位是哪一种。顺带还能把「差一个下划线」这种手误自动纠正掉。*/
static void scan_and_check_ssid(char *ssid, size_t ssid_sz)
{
    wifi_scan_config_t sc = { 0 };
    sc.show_hidden = true;                        /* 隐藏 SSID 的 AP 也要现形 */
    esp_err_t sr = esp_wifi_scan_start(&sc, true);   /* true = 阻塞到扫完 */
    if (sr != ESP_OK) {
        ESP_LOGW(TAG, "pre-connect scan failed: %s", esp_err_to_name(sr));
        return;
    }

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    uint16_t want = num > 24 ? 24 : num;
    wifi_ap_record_t *recs = heap_caps_calloc(want ? want : 1, sizeof(wifi_ap_record_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!recs) recs = calloc(want ? want : 1, sizeof(wifi_ap_record_t));
    if (!recs) return;

    char want_norm[48];
    norm_ssid(ssid, want_norm, sizeof(want_norm));

    bool found = false;
    int  cand  = -1, n_cand = 0;

    if (esp_wifi_scan_get_ap_records(&want, recs) == ESP_OK) {
        for (int i = 0; i < (int)want; i++) {
            const char *ap = (const char *)recs[i].ssid;
            bool hit = (strcmp(ap, ssid) == 0);
            if (hit) found = true;

            char ap_norm[48];
            norm_ssid(ap, ap_norm, sizeof(ap_norm));
            bool fuzzy = (!hit && ap_norm[0] && want_norm[0] &&
                          strcmp(ap_norm, want_norm) == 0);
            if (fuzzy) { n_cand++; cand = i; }

            ESP_LOGI(TAG, "AP[%02d] %-26s %4d dBm  ch%02u  %s", i,
                     ap[0] ? ap : "(隐藏)",
                     recs[i].rssi, (unsigned)recs[i].primary,
                     hit ? "<== 目标" : (fuzzy ? "<== 归一化后同名" : ""));
        }
    }

    if (!found && n_cand == 1) {
        /* 唯一候选才敢自动采用：两个都像就说明没把握，宁可不连 */
        ESP_LOGW(TAG, "精确名字 \"%s\" 没找到，但归一化后唯一命中 \"%s\" → 自动采用",
                 ssid, (const char *)recs[cand].ssid);
        ESP_LOGW(TAG, "（填的名字和路由器差在大小写/下划线/空格，已自动纠正）");
        snprintf(ssid, ssid_sz, "%.32s", (const char *)recs[cand].ssid);
        found = true;
    } else if (!found && n_cand > 1) {
        ESP_LOGW(TAG, "归一化后有 %d 个像的 AP，不敢替你猜，请把名字写准", n_cand);
    }

    free(recs);
    ESP_LOGI(TAG, "scan: %u 个热点，目标 \"%s\" %s", (unsigned)num, ssid,
             found ? "已定位" : "不在列表里");   /* 不用 ✔✘：两套 TTF 都没这字形 */
    if (!found) {
        ESP_LOGW(TAG, "找不到这个 SSID。常见原因：① 名字拼错/多空格 "
                      "② 该网络只开 5 GHz（本机只能 2.4 G）③ 太远或是隐藏网络");
    }
}

esp_err_t app_net_connect_from_file(const char *path)
{
    if (!s_net_inited) {
        ESP_LOGW(TAG, "net not inited, cannot connect");
        return ESP_ERR_INVALID_STATE;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGW(TAG, "no wifi config file at %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    char ssid[64] = "";
    char pass[96] = "";
    char raw[192];
    int  got = 0;
    while (got < 2 && fgets(raw, sizeof(raw), fp) != NULL) {
        char t[192];
        snprintf(t, sizeof(t), "%s", raw);
        trim_line(t);
        if (t[0] == '#') continue;                       /* 允许 # 注释行 */
        if (t[0] == '\0' && got == 0) continue;          /* SSID 之前的空行 */
        /* ★ 必须给 %s 加精度：t 有 192 字节，而 ssid 最多 32、密码最多 63，
         *   不加精度 gcc 会按最坏情况算「可能截断」，IDF 默认 -Werror → 编不过。*/
        if (got == 0) snprintf(ssid, sizeof(ssid), "%.32s", t);
        else          snprintf(pass, sizeof(pass), "%.63s", t);
        got++;
    }
    fclose(fp);

    if (ssid[0] == '\0') {
        ESP_LOGW(TAG, "%s 里没读到 SSID（第一行应为 WiFi 名）", path);
        return ESP_ERR_INVALID_ARG;
    }

    /* ★ 密码只打长度，绝不打明文 —— 串口日志会被截图、会被贴出来 */
    ESP_LOGI(TAG, "wifi.txt -> ssid=\"%s\", password %s (%u chars)", ssid,
             pass[0] ? "已设置" : "为空(开放网络)", (unsigned)strlen(pass));

    /* SSID 含非 ASCII 字节时多打一行十六进制。
     * 中文 SSID 如果被记事本存成「ANSI(GBK)」，按 UTF-8 解出来就是乱码、连接必失败；
     * 有这行就能一眼分清「是编码问题」还是「密码打错了」。*/
    bool non_ascii = false;
    for (const char *q = ssid; *q; q++) {
        if ((unsigned char)*q >= 0x80) { non_ascii = true; break; }
    }
    if (non_ascii) {
        static const char *HEXD = "0123456789ABCDEF";
        char hx[3 * 34];
        int  o = 0;
        for (int i = 0; ssid[i] && i < 32; i++) {
            unsigned char c = (unsigned char)ssid[i];
            hx[o++] = HEXD[c >> 4];
            hx[o++] = HEXD[c & 0x0F];
            hx[o++] = ' ';
        }
        hx[o] = '\0';
        ESP_LOGW(TAG, "ssid hex: %s", hx);
        ESP_LOGW(TAG, "（若这串和路由器上的名字对不上，说明 wifi.txt 存成了 ANSI/GBK，"
                      "请用记事本「另存为 UTF-8」再试）");
    }

    /* ---- 连接前先扫一遍：打印所有热点 + 核对名字（差下划线这种手误自动纠）---- */
    scan_and_check_ssid(ssid, sizeof(ssid));

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;   /* 开放/WPA/WPA2/WPA3 全兼容 */
    wc.sta.pmf_cfg.capable    = true;             /* WPA3 过渡模式：能力有、不强制 */
    wc.sta.pmf_cfg.required   = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "set wifi cfg");

    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    s_has_cred  = true;
    s_retry     = 0;
    s_conn_fail = false;          /* 重新发起连接 → 把「连不上」清掉 */

    esp_wifi_disconnect();          /* 先断开旧的，避免 "already connected" */
    esp_err_t cr = esp_wifi_connect();
    ESP_LOGI(TAG, "wifi connect -> %s", esp_err_to_name(cr));
    return cr;
}

/* ---- 扫描：一次性任务，阻塞 2~3 秒 ---- */
static void scan_task(void *arg)
{
    (void)arg;
    wifi_scan_config_t sc = {
        .ssid = NULL, .bssid = NULL, .channel = 0,
        .show_hidden = false,
    };
    ESP_LOGI(TAG, "wifi scan start");
    esp_err_t r = esp_wifi_scan_start(&sc, true);   /* true = 阻塞到扫完 */
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(r));
        s_ap_count = 0;
        s_scan_running = false;
        vTaskDelete(NULL);
        return;
    }

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    uint16_t want = num;
    if (want > AP_MAX) want = AP_MAX;

    wifi_ap_record_t *recs = heap_caps_calloc(want ? want : 1, sizeof(wifi_ap_record_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!recs) recs = calloc(want ? want : 1, sizeof(wifi_ap_record_t));
    if (recs) {
        if (esp_wifi_scan_get_ap_records(&want, recs) == ESP_OK) {
            /* ★ 10-04 顺带修：隐藏网络的 ssid 是空串。之前直接收进来，
             *   网页下拉框里就会出现一个【空白选项】，用户点了必然连不上。
             *   日志里能看见：AP[01] (隐藏) -32 dBm。空名的一律跳过。*/
            int k = 0;
            for (int i = 0; i < (int)want; i++) {
                if (recs[i].ssid[0] == '\0') continue;
                strncpy(s_aps[k].ssid, (const char *)recs[i].ssid, sizeof(s_aps[k].ssid) - 1);
                s_aps[k].ssid[sizeof(s_aps[k].ssid) - 1] = '\0';
                s_aps[k].rssi = recs[i].rssi;
                k++;
            }
            s_ap_count = k;
        }
        free(recs);
    }
    ESP_LOGI(TAG, "wifi scan done: %u APs found (kept %d)", (unsigned)num, s_ap_count);
    s_scan_running = false;
    vTaskDelete(NULL);
}

void app_net_scan_start(void)
{
    if (!s_net_inited || s_scan_running) return;
    s_scan_running = true;
    xTaskCreate(scan_task, "xs_scan", 4096, NULL, 4, NULL);
}

bool app_net_scan_running(void) { return s_scan_running; }

/* 按信号排序：同名（归一化后）只留信号最强的那条，其余丢掉。
 * ★ 为什么必须做这一步（兰兰要「能连的排前面」）：
 *   esp_wifi_scan_get_ap_records 的返回顺序是信道顺序，不是强弱顺序；
 *   而且同一个路由器会被邻区同名 AP 或自己的多个天线重复收录。
 *   不去重的话列表里连着出现三条「TPGuest」，用户根本不知道该点哪个。
 *
 * ⚠️ 这里的临时数组全用 static —— 本函数由 UI（LVGL 任务）调用，
 *   那个任务的栈只有几 KB，局部数组（key 16×48 + tmp 16×40 ≈ 1.4 KB）
 *   足以把它打穿。铁律：LVGL 任务里不许放大数组。*/
static int ap_cmp_rssi(const void *pa, const void *pb)
{
    const ap_item_t *a = (const ap_item_t *)pa;
    const ap_item_t *b = (const ap_item_t *)pb;
    /* ★★ 10-04 修正：原来写成 (b->rssi > a->rssi) - (b->rssi > a->rssi)，
     *   两边是同一个表达式，差值恒为 0 ⇒ qsort 认为所有元素相等 ⇒
     *   压根没排序！而下面「踢掉太弱的」又依赖「强的在前面」，
     *   于是数组还是扫描返回的原始顺序，第一个稍弱就 keep=0，
     *   整份列表被清空（真机日志：scan list 16->15，随后「丢掉 15 个」，
     *   紧接着 GET /ssids -> 0 条）。
     *   降序 = rssi 大的排前面：a 比 b 弱则 a 排后面（返回正数）。*/
    return (a->rssi < b->rssi) - (a->rssi > b->rssi);   /* 降序 */
}

void app_net_scan_sort_by_rssi(void)
{
    int n = s_ap_count;
    if (n <= 1) return;

    static char    key[AP_MAX][48];
    static int     best[AP_MAX];
    static ap_item_t tmp[AP_MAX];

    /* 1) 同名去重：归一化名相同就只留 rssi 最大的 */
    int nb = 0;
    for (int i = 0; i < n; i++) {
        norm_ssid(s_aps[i].ssid, key[i], sizeof(key[i]));
        int hit = -1;
        for (int j = 0; j < nb; j++) {
            if (strcmp(key[best[j]], key[i]) == 0) { hit = j; break; }
        }
        if (hit < 0) {
            best[nb++] = i;
        } else if (s_aps[i].rssi > s_aps[best[hit]].rssi) {
            best[hit] = i;
        }
    }

    /* 2) 收集到临时区再排序（key[] 是按原下标建的，不能就地 qsort）*/
    for (int j = 0; j < nb; j++) tmp[j] = s_aps[best[j]];
    qsort(tmp, (size_t)nb, sizeof(tmp[0]), ap_cmp_rssi);

    int old = n;
    for (int j = 0; j < nb; j++) s_aps[j] = tmp[j];
    s_ap_count = nb;
    if (nb != old) {
        ESP_LOGI(TAG, "scan list: %d -> %d 条（同名去重后按信号排序）", old, nb);
    }
    /* ★ 极弱的直接踢掉：-85 dBm 基本连不上，摆在列表里只会浪费兰兰的时间。
     * ★★ 10-04 改写：原来是
     *       int keep = 0;
     *       while (keep < s_ap_count && s_aps[keep].rssi >= -85) keep++;
     *   那个写法【依赖「已按信号降序」】：一旦排序没生效（比较函数写错、
     *   或者将来有人改了 qsort），第一个稍弱的就提前退出，keep=0，
     *   整份列表被清空 —— 而且不报任何错，界面上只表现为「扫不到网络」。
     *   过滤本身不该依赖顺序，改成【遍历全部、命中的搬到前面】。*/
    int keep = 0;
    for (int j = 0; j < s_ap_count; j++) {
        if (s_aps[j].rssi >= -85) {
            if (keep != j) s_aps[keep] = s_aps[j];
            keep++;
        }
    }
    if (keep < s_ap_count) {
        ESP_LOGI(TAG, "丢掉 %d 个太弱的（< -85 dBm），剩 %d 条",
                 s_ap_count - keep, keep);
        s_ap_count = keep;
    }
}

bool app_net_scan_get(int idx, char *name, int name_len, int *rssi)
{
    if (idx < 0 || idx >= s_ap_count || !name || name_len <= 0) return false;
    strncpy(name, s_aps[idx].ssid, (size_t)name_len - 1);
    name[name_len - 1] = '\0';
    if (rssi) *rssi = s_aps[idx].rssi;
    return true;
}
