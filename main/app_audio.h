#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* ============================================================
 *  拾声 (XianDial) 硬件版 —— 音频（ES8311 + FM8002E 功放）
 * ============================================================
 *  板型 LCD Wiki ES3C35P：
 *    ESP32-S3 --I2S--> ES8311 --模拟--> FM8002E(PA) --> 喇叭
 *    I2S : MCLK=IO17 BCLK=IO18 WS=IO21 DOUT=IO15 DIN=IO16
 *    控制: I2C0（与触摸共用）= SDA IO38 / SCL IO39，地址 0x18(7bit)
 *    功放: PA_EN = IO1（★ 低电平使能 —— 见 app_pins.h 的 PA_EN_ACTIVE）
 *
 *  10-03 首次接通。设计取舍：
 *    · PA_EN 由本模块直接 GPIO 控制（不走 codec 驱动的 gpio_if），
 *      少一层间接、日志能直接看到电平，排查最省事。
 *    · 采样率固定 44100 / 16bit / 立体声 —— 网络电台与本地 MP3 的主流格式。
 */

/* 初始化 I2S + ES8311 + 功放。失败返回错误码，界面照跑（只是没声音）。*/
esp_err_t app_audio_init(void);

/* 音频链路是否就绪 */
bool app_audio_ready(void);

/* 音量 0~100（对数曲线由 codec 内部处理）*/
void app_audio_set_volume(int percent);
int  app_audio_get_volume(void);

/* 往 I2S 写一段 PCM（阻塞到写完）。格式：16bit 交错立体声。*/
esp_err_t app_audio_write(const void *pcm, size_t bytes);

/* 播放开机提示音（非阻塞：内部起一次性任务，播完自动退出）。
 * 用来在【不接任何音源】的情况下证明「喇叭真的会响」。*/
void app_audio_beep_async(void);

/* 当前采样率（Hz） */
int app_audio_sample_rate(void);

/* 切换采样率（Hz）：重配 I2S 时钟 + 重开 codec。
 * ★ 为什么需要：MP3 流不一定是 44100 —— 播客（如卡里的「小宇宙播客 64K」）
 *   常见 24000 / 32000。若不管它，按 44100 播放会变调、音长也对不上。
 *   播放器解出第一帧、拿到真实采样率后调本函数对齐，之后再开始写 PCM。
 * 返回 ESP_OK 表示已切到目标采样率。*/
esp_err_t app_audio_set_sample_rate(int hz);

/* ---- 倍速播放（10-04 兰兰：「增加倍速播放按键1.2X 或1.4X」）----
 *
 *  ★★ 为什么能做到「音调不变」——
 *   播 1.2 倍速有两种做法：
 *     ① 丢掉 1/5 的采样点直接送 I2S（最简单）
 *        ⇒ 声音【变调变尖】，像卡带快放。电台/播客听着非常难受。
 *     ② 让 I2S/codec 以【源采样率 × 倍率】运行，解码器照旧输出源采样率的 PCM
 *        ⇒ 同样的采样点数在【更短的时间里】被硬件取走 ⇒ 时间轴压缩 1.2 倍，
 *          而每个采样点代表的频率没变 ⇒ **音调完全不变**，只是读得快了。
 *   ②才是「倍速」该有的样子，实现也简单：只是把 I2S 时钟拉高 20%。
 *
 *  ★ 代价：codec 要重开一次（约几十毫秒，期间会有一声极短的爆音/静默）。
 *   所以切换是在【切下一段 PCM 之前】做的，且只切一次，不是每帧都切。
 *
 *  ★ 为什么不用软件重采样：44100→52920 的插值重采样要动 PCM 数据本身，
 *   在解码任务里加一层重采样器 = 多几百 KB 缓冲 + 每样本一次乘加，
 *   而收益（避免几十毫秒爆音）远不抵。ESP32-S3 上 I2S 时钟随便改。
 *
 *  用法：
 *      app_radio_set_speed(120);   // 1.0x / 1.2x / 1.4x，倍率乘 100
 *      app_radio_speed();           // 读当前倍率（100/120/140）*/
void      app_audio_set_speed(int pct);   /* 100 = 正常；内部换算成时钟倍率 */
int       app_audio_get_speed(void);

/* 播放器拿到流的真实采样率时调它（app_radio.c 内部用）。
 * 它做两件事：把 I2S 对齐到「源采样率 × 当前倍率」，并记住倍率基准，
 * 这样切倍速时不用再问播放器要采样率。*/
esp_err_t app_audio_set_src_rate(int hz);
