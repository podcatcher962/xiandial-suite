#pragma once

#include <stdbool.h>

/* ============================================================
 *  拾声 · 全局设置（xs_cfg）  —— 三个产品共用
 * ============================================================
 *  ★ 为什么要有这一层（10-08 兰兰：「设置按键在首页，不是到灵签里去设置」）：
 *    原来音量/亮度/静音这三个量存在 ui_lingqian.c 的 static 里 —— 也就是
 *    "灵签的私有财产"。可它们动的是【整机硬件】：ES8311 的 DAC 增益、
 *    背光 LEDC 占空比。天气和股票一样要用，凭什么归灵签管？
 *    所以提出来做成全局层：谁都能读、谁都能改，改完都是一处生效。
 *
 *  ★★ 默认值（10-08 兰兰指定）：
 *      volume  = 100   （灵签是"念给你听"的机器，恢复音量必须够响）
 *      mute    = true  （兰兰：「观音灵签默认声音关闭」）
 *      backlight = 50  （兰兰：「默认亮度 50%」）
 *    注意 mute=true 与"点朗读要出声"并不矛盾 —— 见 audio_boost()。
 *
 *  ★ 持久化：NVS（命名空间 xs_cfg / 键 cfg），blob 存整个结构体。
 *    与 app_fav.c / app_hist.c 同一套写法，但用独立命名空间，互不干扰。
 * ============================================================ */

typedef struct {
    int  volume;        /* 0~100  —— 解除静音后的音量 */
    int  backlight;     /* 20~100 —— 背光（下限 20，见 ui 侧说明）*/
    bool mute;          /* 声音总开关；默认 true */
} xs_cfg_t;

/* 开机调一次（在 app_display_init() 之后、建界面之前）：
 * 载入 NVS（无存档则用默认值），并【立即】把音量/背光写进硬件。*/
void xs_cfg_init(void);

xs_cfg_t xs_cfg_get(void);
int  xs_cfg_volume(void);
int  xs_cfg_backlight(void);
bool xs_cfg_muted(void);

/* 三个 setter：立即生效（写硬件）+ 标记脏（延时落盘，见 .c 的说明）。
 * volume 传 0~100、backlight 传 20~100，越界自动夹紧。*/
void xs_cfg_set_volume(int v);
void xs_cfg_set_backlight(int v);
void xs_cfg_set_mute(bool m);
void xs_cfg_toggle_mute(void);

/* 立刻落盘（正常不用手动调；退出产品等时机调一次更保险）*/
void xs_cfg_save(void);

/* ---- 「本次出声」旁路 ----
 * ★ 场景：默认静音，但兰兰点了「朗读签文」/「语音播报」，
 *   这一下必须出声，否则按钮看着像坏的。
 *   用法：播放前 audio_boost()，播放结束 audio_restore()。
 *   boost 只是把 DAC 增益临时设到设定音量，不改 mute 标志 ——
 *   播完 restore 回到静音，语义干净（"默认不出声，你点了我才说"）。*/
void xs_cfg_audio_boost(void);
void xs_cfg_audio_restore(void);
