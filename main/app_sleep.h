/*
 * app_sleep.h —— 睡眠定时（到点自动停播）
 *
 * 兰兰 10-04：「能不能设置睡眠定时，时间可以自己定」
 *   ⇒ 固定档 关闭/30/60/90/120/150/180 + 「关闭」，
 *     设定值存 NVS（掉电不丢），到点停止播放。
 *   兰兰明确不要「真·定时开关机」，本模块只管「到点停播」。
 *
 * ★ 剩余时间的算法用 uptime（esp_timer），【不用系统时间】：
 *   系统时间会被 SNTP 改（还会 +8h，见 app_sys.c 的铁律），
 *   拿它算倒计时，改一次时间定时就乱。
 *
 * ★★ 10-04 第二十七次：原来的「步长 + 自定义值」两个概念【整套删掉】。
 *   兰兰原话「定时自由按键还是取消了比较好，把时间固定按键改好了
 *   多几个选择即可」。理由记在 ui_xiandial.c 的 ev_sleep_pick 上方。
 *   连带删掉的 NVS 键（step / cus）留在分区里不清 —— 删 key 省不了
 *   多少空间，反而多一次 nvs_erase_page 的写风险。旧值读不出来
 *   就当没存过（load() 只认 mins 一个键），无害。*/
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 开机初始化（读 NVS 恢复上次设定）。在 nvs_flash_init 之后调。*/
esp_err_t app_sleep_init(void);

/* 设定时：mins > 0 开启（上限 240 分钟），mins <= 0 关闭。*/
void      app_sleep_set_minutes(int mins);

/* 剩余秒数；0 = 没在跑或已到点 */
int       app_sleep_remaining_s(void);
bool      app_sleep_is_on(void);
int       app_sleep_minutes(void);   /* 设定值（0 = 关闭），用于恢复选中态 */
bool      app_sleep_fired(void);     /* 已经到点停过一次 */

/* 由 1 秒定时器驱动；到点时返回 true（那一秒返回一次）。*/
bool      app_sleep_tick(void);

#ifdef __cplusplus
}
#endif
