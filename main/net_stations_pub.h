#pragma once

/* ============================================================
 *  拾声 (XianDial) 硬件版 ---- 内置网络电台清单【发布版 · 空台单】
 * ============================================================
 *  ★ 本文件由 firmware/_gen_stations_all.py --publish 自动生成，不要手改。
 *
 *  这是【发布版】：内置台单为 0 条。
 *
 *  使用者怎么加台：
 *    ① 用配套工具 XianForge.html 把自己的 M3U / TXT 转成 stations.tsv
 *    ② 把 stations.tsv 放到 TF 卡根目录，插卡开机
 *    ③ 机器自动读卡；没有卡或文件坏了就保持 0 台（不白屏、不崩）
 *
 *  栏目 / 地区的取值范围与自用版完全一致，导入的台源填进同一套结构。
 */

/* 栏目数（见 g_cat_name）与地区数（见 g_prov_name）*/
#define NET_CAT_N  13
#define NET_PROV_N 42
#define NET_PROV_CN_N 31  /* 前 31 个是国内省级行政区，其余是港澳台/海外 */
#define NET_PROV_NONE 255         /* 全国台 / 无地区归属 */

typedef struct {
    const char *name;    /* 台名 */
    const char *url;     /* 直播流地址 */
    unsigned char cat;   /* 栏目下标，见 g_cat_name */
    unsigned char prov;  /* 地区下标，见 g_prov_name；NET_PROV_NONE = 全国 */
} net_station_t;

/* ★ 台数 0。app_stlist.c 读不到 stations.tsv 时回落到这里。 */
#define NET_STATION_COUNT 0

extern const net_station_t g_stations[];
extern const int           g_station_count;   /* ★ UI 与 app_stlist.c 都要用 */
extern const char *const g_cat_name[NET_CAT_N];
extern const char *const g_prov_name[NET_PROV_N];
