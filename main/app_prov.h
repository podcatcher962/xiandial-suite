#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* ============================================================
 *  拾声 (XianDial) 硬件版 —— 扫码/网页配网（SoftAP + Captive Portal）
 * ============================================================
 *  兰兰 10-04：「不用手动输入 WiFi 又要键盘操作也不方便，
 *              还是市场上最主流的方式吧，比如扫码链接或者你觉得什么连接最好」
 *
 *  ★ 为什么是「热点 + 网页」而不是二维码：
 *
 *    ❶ `WIFI:S:xxx;T:WPA;P:yyy;` 这个格式来自 ZXing，写进 Wi-Fi Alliance 的
 *       WPA3 规范，语义是「【已经知道密码的设备】把网络分享出去」。
 *       手机扫到之后是【手机】去连那个 WiFi，设备端全程不参与 ——
 *       所以它【不能】把用户家的账密送进我们的机器。方向是反的。
 *
 *    ❷ BLE 配网（乐鑫官方 esp_provisioning）文档明说 BLE 传输运行时吃
 *       ~110 KB RAM。本机 LVGL 池总共 114 KB、内部 RAM 也吃紧，出局。
 *
 *    ❸ DPP / Easy Connect 只支持「部分 Android 10+」，iPhone 完全不支持。
 *
 *    ❹ 真正在用的业界主流（ESPHome / 米家 / 各类打印机音箱）就是本文件这套：
 *       设备自己开热点 → 手机连上 → 系统自动弹出配网页 → 网页里选 WiFi 填密码。
 *       「自动弹页」的原理是：设备里跑一个极小的 DNS server，对**所有**
 *       DNS 查询一律回自己的 IP，于是系统对 captive.apple.com /
 *       connectivitycheck.gstatic.com 的连通性探测被截获，
 *       系统就认为「这个网络需要登录」，自动弹出网页。
 *
 *  ★ 两条入口（缺一不可）：
 *      主 —— 网页下拉选 SSID（消灭「中文 SSID 打错」这个最大失败源）
 *      兜 —— 网页里可手输 SSID（扫不到 / 被 16 条上限截断时用；
 *           注意这是【手机上的键盘】，不是 480×320 屏上那个）
 *    兜底里绝不再出现屏幕键盘 —— 兰兰已经明确否决。
 * ============================================================ */

/* AP 网段固定 192.168.4.1/24（IDF 默认），DNS 与 HTTP 都在这个地址上 */
#define APP_PROV_IP_STR   "192.168.4.1"
#define APP_PROV_PORT      80

/* 热点名。开机时按 STA MAC 后两字节生成，形如 XianDial-9F3A */
#define APP_PROV_SSID_MAX  20

/* 开启配网：起 SoftAP（APSTA，STA 仍可连家里 WiFi）+ HTTP + DNS 劫持。
 * 返回 ESP_OK = 热点已开；其它值看 esp_err_to_name()。 */
esp_err_t app_prov_start(void);

/* 关掉配网（AP + HTTP + DNS 一起收）。已连上家里 WiFi 时也能安全调用。*/
void      app_prov_stop(void);

bool      app_prov_is_active(void);

/* 当前热点名（形如 XianDial-9F3A），没开时返回空串。给屏幕显示用。*/
const char *app_prov_ssid(void);

/* 已连上来的手机数（lwip 的 AP 站点表）。只用来打日志自证。*/
int       app_prov_clients(void);

/* 手机提交账密后的结果（给屏幕显示）：
 *   0 还没提交 / 1 正在连 / 2 连上了 / 3 密码错或找不到 / 4 参数不对 */
int       app_prov_result(void);
void      app_prov_clear_result(void);

/* 连上之后延迟自动关热点的秒数（页面已经拿到「成功」提示了，
 * 再留着热点没意义，也白占内存）。 */
#define APP_PROV_AUTOCLOSE_S   20
