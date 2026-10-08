#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "net_stations.h"
#include "esp_err.h"

/* ============================================================
 *  拾声 (XianDial) 硬件版 —— SD 卡台单导入（stations.tsv）
 * ============================================================
 *  目的：让【使用者自己】的电台源进机器，而不是把兰兰的 1254 台烧进去。
 *
 *  文件：/sdcard/stations.tsv（根目录，文件名固定）
 *  格式（★ 契约，改动必须同步 XianForge.html 与《台源文件格式规格.md》）:
 *      UTF-8 无 BOM / LF 换行（绝不能 CRLF）/ 单个 TAB 分隔 / 4 列
 *      台名 \t 栏目 \t 地区 \t URL
 *      栏目与地区必须落在 net_stations.h 的 13 / 42 个标准名里，
 *      对不上→ 栏目归「综合」、地区归 NET_PROV_NONE(255)。
 *
 *  ★ 设计要点（照 v1.40 血泪教训来的）：
 *    ① 回落而非报错：没有卡 / 没有文件 / 解析失败，一律回落到内置
 *       g_stations，界面照跑，**绝不白屏**。
 *    ② 一次分配、常驻：解析时在 PSRAM 上只 malloc 一次，之后只读。
 *       不在 UI 回调里 malloc/free。
 *    ③ 上限2000 台；超了截断并记日志，不崩。
 *    ④ 字符串池 + 指针数组（不是定长数组）：2000 台大约 200 KB，
 *       定长 net_station_t[2000] 只要 40 KB，但字符串必须另存，
 *       池子能省掉逐条对齐的浪费。
 * ============================================================ */

#define APP_ST_FILE "/sdcard/stations.tsv"
#define APP_ST_MAX      2000        /* 与契约一致 */
#define APP_ST_MAX_LINE 512         /* 一行最长：台名71+URL199+栏目/地区+3个TAB+CRLF 余量 */

/* 载入结果 */
typedef enum {
    APP_ST_SRC_BUILTIN = 0,   /* 用内置台单（没卡 / 没文件 / 解析失败）*/
    APP_ST_SRC_TSV     = 1,   /* 用 SD 卡的 stations.tsv */
} app_st_src_t;

/* ★ 必须在显示任何台单界面前调用一次（内部幂等）。
 * 成功载入 TSV 返回 APP_ST_SRC_TSV。
 * ★ 必须在 app_sd_mount() 之后调用。 */
app_st_src_t app_st_load(void);

/* 载入来源（未调用 app_st_load 时恒为 BUILTIN）*/
app_st_src_t app_st_source(void);

/* 台数（内置或 TSV）*/
int app_st_count(void);

/* 取第 i 个（0 起）。越界返回 NULL。*/
const net_station_t *app_st_get(int i);

/* 按台名查找（收藏、历史用；比对逻辑与 app_fav/app_hist 一致）。
 * 找到返回下标，找不到返回 -1。 */
int app_st_find_by_name(const char *name);

/* ★ 兼容层：现有 36 处调用点仍写g_stations / g_station_count，
 * 靠下面两个宏重定向到访问器，**调用点一行不用改**。
 * 用法：把 `g_stations[idx]` 换成 `ST_AT(idx)`，
 *       把 `g_station_count` 换成 `ST_COUNT()`。
 * ⚠️ 保留原符号是为了让旧代码仍能编译（不报 undefined），
 *    但真正取数据一律走 ST_AT / ST_COUNT，否则拿到的还是内置那份。*/
#define ST_AT(i)      app_st_get(i)
#define ST_COUNT()    app_st_count()

/* 调试用：把载入诊断打到串口（状态页/串口日志用）*/
void app_st_dump_info(void);

/* ★ 10-06：启动串口命令通道。不只是调试用 —— 使用者自己换了
 * stations.tsv / wifi.txt 也能热重载，不用重启、不用拔卡。
 *
 * 命令一览（串口监视器里敲，CR 结尾）：
 *   st_dump              打印当前台单来源、条数、构成、前 3 条
 *   st_reload            重新读卡上的 stations.tsv 并重建台单网格
 *   st_ls                列出 SD 卡根目录（看文件在不在、多大）
 *   st_cat wifi.txt      打印卡上某个文本文件（★ 写完必须用它回读核对）
 *   st_put wifi.txt      进入写入模式：之后每行内容原样写进该文件，
 *                        单独一行 . 结束，然后自动 st_cat 回读校验
 *   st_rm <文件名>       删除卡上某个文件
 *
 * ★ 为什么需要写通道：板载 TF 走 SDIO，Windows 看不到这张卡；
 *   板载 Type-C 的 PHY 被 Serial-JTAG 占着，板子也不能当 USB 主机。
 *   ⇒ 以前改卡上文件唯一的办法是拔卡插读卡器，改一次拔一次。
 *   有这条通道后，改台单/改 WiFi 账密都在串口里完成。
 */
void app_st_start_serial_cmd(void);
