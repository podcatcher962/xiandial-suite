#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* ============================================================
 *  拾声 (XianDial) 硬件版 —— 播放器
 * ============================================================
 *  两个音源，共用同一套解码核心（esp_audio_simple_dec -> I2S -> ES8311）：
 *    ① SD 卡本地文件（app_radio_play_file）
 *    ② 网络电台 HTTP 直播流（app_radio_play_station / app_radio_play_url）
 *
 *  ★★ 10-03 血泪：解码器要注册【两套】，少一套就「点文件没反应」★★
 *    esp_audio_codec 把注册拆成两层：
 *      · esp_audio_dec_register_default()        → MP3 / AAC / FLAC / OPUS …（纯帧解码器）
 *      · esp_audio_simple_dec_register_default() → WAV / M4A / TS / OGG（带容器 parser 的）
 *    simple_dec 只是个「调度壳」，它内部要转手给上面那些帧解码器。
 *    所以只调后者 = MP3 压根没注册 = 打开/解码直接 NOT_SUPPORT，
 *    症状是「按下文件没声音、也没报错弹窗」。
 *    验证办法：esp_audio_dec_get_ops(ESP_AUDIO_TYPE_MP3) 非 NULL 才算注册上了。
 *    ⚠️ 别用 esp_audio_dec_get_avail_type() 判断 —— 它返回的是「下一个可用的
 *       自定义类型号」，不是已注册解码器的位图（10-03 更正，之前据此下过错的结论）。
 */

/* 注册解码器。必须在 app_audio_init() 之后、播放之前调用一次。*/
esp_err_t app_radio_init(void);

/* ---- 音源一：本地文件 ---- */

/* 播放一个本地文件（SD 卡上的绝对路径，如 /sdcard/xx/a.mp3）。
 * 会先停掉当前正在播的内容。非阻塞：内部起播放任务。*/
esp_err_t app_radio_play_file(const char *path);

/* 在 dir 里换曲：step=+1 下一个音频文件、-1 上一个（到目录末尾会回卷）。
 * 成功后内部直接开始播放，并把新文件名写进 out_name（可为 NULL）。
 * 返回 ESP_OK / ESP_ERR_NOT_FOUND（目录里没有别的音频了）。*/
esp_err_t app_radio_play_next(const char *dir, int step, char *out_name, int out_name_sz);

/* ---- 音源二：网络电台 ---- */

/* 播放内置台单（main/net_stations.c，由 _gen_stations.py 生成）第 idx 个。
 * 全部是【无 TLS 的 http】地址，且都是「裸请求就 200」的台 —— 省掉证书包
 * 与 30~40 KB 的 TLS 握手堆（本机内部 RAM 只剩 80 KB，这是决定性的）。*/
esp_err_t app_radio_play_station(int idx);

/* 播放任意流地址（带显示名）。*/
esp_err_t app_radio_play_url(const char *name, const char *url);

/* 上一个 / 下一个台（在整份内置台单里循环）。返回新台下标，-1 表示失败。*/
int app_radio_station_step(int step);

/* 内置台单总数 */
int app_radio_station_count(void);

/* 当前在放的是第几个台；-1 = 不是网络电台（本地文件或没在放）*/
int app_radio_station_current(void);

/* 当前在放的是网络直播流？直播没有总时长，界面要据此换成「LIVE + 电平表」。*/
bool app_radio_is_stream(void);

/* 0~100 的瞬时电平（由解码后的 PCM 峰值算出），给直播态当表头动画用。
 * ★ 用它而不是假动画：屏幕上跳的就是真的在响的音量。*/
int app_radio_level(void);

/* ---- 动态频谱（兰兰 10-03：「播放页如果是动态频谱更好」）----
 * 真频谱，不是装饰动画：内部对解码后的 PCM 做 Hann 窗 + 12 点 Goertzel，
 * 频点按对数分布在 50 Hz ~ 12 kHz。开销约每秒 3.6 万次乘加，可忽略。
 *
 * 用法：
 *   int n = app_radio_spectrum_bins();
 *   for (int i = 0; i < n; i++) { int v = app_radio_spectrum(i); ... }   // 0~100
 * 停播后全部返回 0。返回的是快照，不阻塞播放任务。*/
int app_radio_spectrum_bins(void);
int app_radio_spectrum(int i);

/* ---- 通用控制 ---- */

/* 停止播放（非阻塞：置标志，任务自己收尾）*/
void app_radio_stop(void);

/* 是否正在播放（含暂停中）*/
bool app_radio_is_playing(void);

/* 暂停 / 继续。暂停时不再往 I2S 写数据，DMA 欠载由 auto_clear 补零 = 静音，
 * 但位置保留，继续时接着放。*/
void app_radio_set_paused(bool paused);
bool app_radio_is_paused(void);
void app_radio_toggle_pause(void);

/* 当前播放的显示名（本地取文件名；电台取台名；没在播时为空串）*/
const char *app_radio_now_playing(void);

/* 已播放秒数 / 估算总秒数（0 = 未知，直播恒为 0）*/
int app_radio_elapsed_s(void);
int app_radio_total_s(void);

/* ★ 拖动进度条跳到第 sec 秒（10-03 第十四次加）。
 *   只对【本地文件】有效 —— 直播没法跳（返回 ESP_ERR_NOT_SUPPORTED）。
 *
 *   为什么不做成「解码器原地 seek」：esp_audio 这一套**没有提供 seek 函数**
 *   （把 managed_components 的头文件都翻过，只有 simple_dec.h:206 的一句注释
 *   "Seek when use frame decoder"，没有对应的 API）。
 *   所以做法是：fseek 到位 → 重建解码器（parser 状态机必须从头开始），
 *   代价约 200 ms。一首本地音频 3~5 分钟，等一下完全能接受。
 *   注意往回退 4 KB，避免落在两帧中间。
 *
 *   用法：界面在 EV_VALUE_CHANGED 里调，参数是「总秒数 × 拖动比例」。*/
esp_err_t app_radio_seek(int sec);
/* 当前能不能拖（本地文件且已知总时长）。界面据此决定进度条要不要响应触摸。*/
bool        app_radio_can_seek(void);

/* 上一帧解出来的流信息（诊断用）*/
int app_radio_stream_rate(void);
int app_radio_stream_channel(void);
/* ★★ 直播落后几秒（HLS 直播才有意义；0 = 不是直播或判断不了）。
 *  从分片文件名里的 unix 时间戳算出来。用来区分「重播老片」和
 *  「源本身有延迟」—— 这两者的症状在耳朵里一模一样。*/
long app_radio_hls_lag_s(void);

/* 最近一次失败原因（中文短句；成功播放时为空串）。
 * 界面直接显示这个，比抛一个 esp_err 名字有用人得多。*/
const char *app_radio_last_error(void);

/* ---- 倍速播放（第二十二次）----
 * 倍率乘 100：100 = 1.0X，120 = 1.2X，140 = 1.4X。
 * 非阻塞：内部改 I2S 时钟 + 重开 codec（约几十毫秒，期间一声极短静默）。
 * 切下一首/下一个台时会自动沿用当前倍率。
 * 传入其它值会自动夹到最近的一档（见 app_audio_set_speed）。*/
void app_radio_set_speed(int pct);
int  app_radio_speed(void);
