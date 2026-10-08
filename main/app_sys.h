#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* ============================================================
 *  拾声 (XianDial) 硬件版 —— 系统状态查询 + 网络
 * ============================================================
 *  给「关于/状态」页提供一份可刷新的体检数据：固件版本、运行时间、
 *  内存、SD 卡、音频链路、WiFi 连接状态。
 *
 *  10-03 首次实现。WiFi 只做到「初始化 + 扫描周围 AP」，
 *  真正的连接（填 SSID/密码、连上电台）留到网络电台那一步。
 */

typedef struct {
    const char *fw_ver;          /* 固件版本（编译期 ID） */
    const char *build_date;      /* 构建日期时间 */
    uint32_t    uptime_s;        /* 开机至今秒数 */
    uint32_t    heap_internal_kb;
    uint32_t    heap_psram_kb;
    uint32_t    heap_min_kb;     /* 历史最低内部空闲（判断有没有逼近过极限）*/
    bool        sd_mounted;
    uint32_t    sd_cap_mb;
    int         sd_audio_count;
    bool        audio_ready;
    int         volume;          /* 0~100 */
    int         brightness;      /* 0~100 */
    const char *net_state;       /* WiFi 状态文本 */
    const char *net_ip;          /* 已连接时的 IP，未连接为空串 */
    const char *net_ssid;        /* 已配置的 SSID（来自 wifi.txt） */
    int         rssi;            /* 当前连接信号 dBm（0 = 无）*/
    int         ap_count;        /* 上次扫描到的 AP 数 */
    int         cpu_mhz;
} app_status_t;

/* 取一份当前状态快照 */
void app_sys_get(app_status_t *st);

/* 亮度（0~100）：统一入口，内部调 app_backlight_set 并记住当前值 */
void app_sys_set_brightness(int percent);
int  app_sys_get_brightness(void);

/* 开机至今秒数（顶栏在没有 SNTP 时用它假走一个时钟）*/
uint32_t app_sys_uptime_s(void);

/* 网络校时（SNTP）。在拿到 IP 后自动调一次；也可手动再调（内部有去重）。
 * 校上之后 time() 就是本地时间，顶栏显示的是真时间；
 * 没校上时顶栏退回「开机后的小时数」，不会一直显示 --:--。*/
void app_net_sync_time(void);

/* ---- ★★ 铁律：time() 谎报 UTC（10-04 血泪，28831 秒的真正原因）----
 *
 *  on_sntp_sync() 里做了 tv_sec += 8*3600 再 settimeofday()，
 *  也就是把【本地时间】写进了系统时钟。于是：
 *      time()  返回的 =  真实 UTC  + 28800
 *  而 localtime_r() 在没设 TZ 时按 UTC 解读 ⇒ 读回来正好是北京时间，
 *  所以【顶栏和夜间页的时间全对】—— 时间显示没问题，骗人的是 time() 本身。
 *
 *  受害的是任何「time() 与外部 UTC 时间戳相减」的地方：
 *      HLS 分片文件名里的 10 位戳是真 UTC。
 *      真延迟 31 秒 → 算成 31 + 28800 = 28831（兰兰在 CCTV1 上看到的数）
 *  ⇒ 要和外部时间戳比（延迟、过期时间）必须用本函数拿真 UTC。
 *  ⇒ 反过来说：要拿「墙上的时间」（显示给人看）用 time() 没问题。*/
long app_sys_utc_now(void);

/* ---- 网络 ---- */
esp_err_t app_net_init(void);          /* 初始化 STA（不自动连接）*/
void      app_net_scan_start(void);    /* 非阻塞：起一次性任务扫描 AP */
bool      app_net_scan_running(void);
/* 读取上次扫描结果第 idx 条（0-based）：返回 false 表示越界 */
bool      app_net_scan_get(int idx, char *name, int name_len, int *rssi);
const char *app_net_state_str(void);

/* ---- 从 SD 卡上的文本文件读 WiFi 账密并连接 ----
 * 文件格式（纯文本，UTF-8 或 ANSI 都行）：
 *     第一行 = WiFi 名(SSID)
 *     第二行 = 密码（开放网络就留空行）
 *     以 # 开头的行忽略；前后空格、\r、UTF-8 BOM 会自动清掉。
 *
 * ★ 为什么走文件而不是弹窗输入：验证阶段不用把家里的 WiFi 密码发到聊天里，
 *   丢个 txt 进卡最省事也不留痕。发行版的「扫描 + 屏幕键盘」是第二步。
 *
 * 返回：ESP_OK 已发起连接；ESP_ERR_NOT_FOUND 文件不存在；
 *       ESP_ERR_INVALID_ARG 没读到 SSID；ESP_ERR_INVALID_STATE 网络没初始化。 */
esp_err_t app_net_connect_from_file(const char *path);

/* ---- 直接用账密连接（★ WiFi 设置页用这个，不再依赖 SD 卡）----
 * 传进来的 ssid/pass 会被【存进 NVS】，下次开机自动连 ——
 *   所以「从 SD 卡读 wifi.txt」降级成「兜底」：卡里没文件也能联网。
 * 返回：ESP_OK 已发起连接；ESP_ERR_INVALID_ARG SSID 为空；
 *       ESP_ERR_INVALID_STATE 网络没初始化。 */
esp_err_t app_net_connect_creds(const char *ssid, const char *pass);

/* 开机时先试 NVS 里存的账密。返回 true = 有账密并已发起连接。*/
bool      app_net_connect_saved(void);

/* 当前 SSID（已配置的那个，可能还没连上）*/
const char *app_net_ssid(void);

/* 扫描结果排序：按信号从强到弱，且同名只留信号最强的一个。
 * 兰兰要「能连的排前面」——信号 -40 的和 -85 的摆在一起没法用。
 * 必须在 app_net_scan_get() 之前调（它就地重排 s_aps）。*/
void      app_net_scan_sort_by_rssi(void);

bool        app_net_connected(void);   /* 已拿到 IP */
const char *app_net_ip_str(void);      /* 未连接时返回空串 "" */
const char *app_net_ssid(void);        /* 已配置的 SSID，没有则空串 */

/* ---- 网络链路状态（给界面画 WiFi 标志用）----
 * ★ 为什么要有它：光靠 app_net_state_str() 的字符串去 strcmp 判断太脆，
 *   界面需要的是「能一眼区分」的五种态，不是一个句子。*/
typedef enum {
    APP_NET_IDLE = 0,      /* 网络没初始化 */
    APP_NET_NO_CRED,       /* 没读到 wifi.txt（未配置）*/
    APP_NET_CONNECTING,    /* 有账密、正在连（含自动重试中）*/
    APP_NET_OK,            /* 已拿到 IP */
    APP_NET_FAIL,          /* 重试次数用完仍连不上 */
} app_net_link_t;

app_net_link_t app_net_link_state(void);

/* 断线原因码 -> 人话（201 找不到网络 / 202 密码错 …）*/
const char *app_net_reason_str(int reason);
/* 同上但极短，给 320 宽的屏幕上那一行用 */
const char *app_net_reason_short(int reason);
int         app_net_last_reason(void);
