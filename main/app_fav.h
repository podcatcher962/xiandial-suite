#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* ============================================================
 *  拾声 (XianDial) 硬件版 —— 收藏电台（掉电不丢）
 * ============================================================
 *  兰兰 10-03：「如果能在硬件版也能做收藏设置最好了」——
 *  所以收藏不是编译期写死的名单，而是**机器上能随时加/取消**，
 *  存进 NVS，拔电不丢。
 *
 *  ★ 为什么存【台名】而不是台单下标（重要）：
 *    台单（net_stations.c）是 _gen_stations.py 生成的，
 *    以后每补一批台，所有下标都会平移 —— 存下标的话，
 *    兰兰今天的「上海交通」明天可能变成「某县戏曲台」。
 *    存名字 + 开机时按名字重新解析，台单怎么变都不会错位。
 *    （代价：NVS 里多占几 KB，换掉的是「收藏悄悄指错台」这种硬伤。）
 * ============================================================ */

#define APP_FAV_MAX 64      /* 最多收藏几个台 */

/* 载入收藏（首次开机则用内置名单播种）。
 * ★ 要在 app_radio_init() 之后调用：解析名字要用到台单。 */
esp_err_t app_fav_init(void);

bool app_fav_has(int station_idx);

/* 加/取消收藏（会自动写 NVS）。返回 ESP_OK / ESP_ERR_NO_MEM（满了）。*/
esp_err_t app_fav_toggle(int station_idx);
esp_err_t app_fav_add(int station_idx);
esp_err_t app_fav_remove(int station_idx);

int  app_fav_count(void);

/* 把收藏的台单下标写进 out（最多 max 个），返回实际个数。
 * ★ out 里的每个下标都保证是**当前台单里真实存在的**（解析失败的会带上日志但不上榜）。*/
int  app_fav_list(int *out, int max);

/* 把收藏清空（给「恢复出厂」用，暂时只有串口能触发）*/
esp_err_t app_fav_clear(void);
