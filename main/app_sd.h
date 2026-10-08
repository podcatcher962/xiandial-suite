#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* ============================================================
 *  拾声 (XianDial) 硬件版 —— SD 卡 / 本地音频库
 * ============================================================
 *  板型 LCD Wiki ES3C35P：TF 卡座走 **SDIO 四线**（不是 SPI）。
 *  官方固件没驱动 SD，这一段是自己补的。
 *
 *  用途：
 *    ① 开机/进「城市声音集」页时挂载，列出 /sdcard 下的音频文件；
 *    ② 二期做本地音频点播（MP3 → esp_audio_codec → ES8311）。
 */

/* 单条目录项（够放中文长文件名：FATFS LFN_HEAP，最长 255 字节 UTF-8） */
typedef struct {
    char     name[260];
    uint32_t size;      /* 字节 */
    bool     is_dir;
} app_sd_entry_t;

/* 挂载 SD 卡到 /sdcard（幂等：重复调用直接返回 ESP_OK）。
 * 失败不致命——返回错误码，界面照样能跑（网络电台不依赖 SD）。*/
esp_err_t app_sd_mount(void);

/* 已挂载？ */
bool app_sd_is_mounted(void);

/* 卡信息（挂载成功后有效）：容量 MB 与文件系统名 */
void app_sd_get_info(uint32_t *cap_mb, const char **fs_name);

/* 列目录：把 path（如 "/sdcard" 或 "/sdcard/城市声音志"）下的条目写进 out，
 * 最多 max 条；返回实际条数（负数表示出错）。
 * ★ 调用前请确保已 mount。 */
int app_sd_list(const char *path, app_sd_entry_t *out, int max);

/* 只数音频文件（.mp3/.wav/.flac/.aac/.m4a，大小写不敏感），递归最多 5 层
 * （10-03 由 2 层放宽：卡里是「小宇宙播客 64K/<节目名>/xxx.mp3」共第 3 层，
 *  原来的 2 层数出来恒为 0）。带扫描预算上限，宁可数字偏小也不拖慢界面。
 * 用于界面显示「本地 N 首」。 */
int app_sd_count_audio(const char *path);

/* 带缓存的全卡（/sdcard）音频计数。
 * rescan=true 强制重扫；false 直接用上次结果（没数过则自动数一次）。
 * ★ 状态页每秒刷新、SD 页每次切目录刷新，都必须走这个而不是 app_sd_count_audio()，
 *   否则每刷一次就递归 stat 几千条目录项，界面会明显卡顿。 */
int app_sd_audio_total(bool rescan);

/* 判断文件名是不是音频（供界面做「能不能点」的判定，也可以直接用）*/
bool app_sd_is_audio(const char *name);

/* 在同一目录里换曲：以 cur_name（纯文件名，不含目录）为基准，
 * step=+1 取下一个音频文件、-1 取上一个，**到末尾会回卷到另一端**。
 * 成功后把【完整路径】写进 out_full（缓冲区请给 ≥ 512 字节）。返回 0 成功、
 * -1 失败（无音频/没找到）。
 *
 * ★ 为什么不做成「返回整个播放列表」：一个节目目录几十上百首，
 *   每个文件名最长 255 字节，缓存整张列表要几十 KB 内存。
 *   这里用「两遍扫描」代替缓存：第一遍数总数并定位当前曲，
 *   第二遍直接走到目标下标 —— 零额外内存，代价是多读一次目录。*/
int app_sd_dir_audio_nav(const char *dir, const char *cur_name, int step,
                         char *out_full, int out_full_sz);

/* 卸载（调试用） */
esp_err_t app_sd_unmount(void);
