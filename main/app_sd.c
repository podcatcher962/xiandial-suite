/*
 * 拾声 (XianDial) 硬件版 —— SD 卡 / 本地音频库
 *
 * 板型：LCD Wiki ES3C35P（ESP32-S3 N16R8）
 * TF 卡座 = **SDIO 四线**，引脚见《集成板-到货准备与操作手册》引脚表：
 *      CLK=IO5  CMD=IO4  DATA0=IO6  DATA1=IO7  DATA2=IO2  DATA3=IO3
 *
 * 注意（10-03 记）：
 *   ① 官方固件【完全没有】sdmmc / sd_card / esp_vfs_fat 的代码 —— 这一段是自补的，
 *      所以任何「SD 不好使」的问题都不会是「厂家 demo 能跑我不能」，得自己查。
 *   ② 这 6 个 GPIO 都不是 S3 的 SDMMC IO_MUX 默认脚，走 GPIO matrix 路由，
 *      速度上限低于 IO_MUX；先用 SDMMC_FREQ_DEFAULT(20MHz) 求稳，验通再提。
 *   ③ FAT32 且要读中文文件名 ⇒ sdkconfig 必须开长文件名：
 *        CONFIG_FATFS_LFN_HEAP=y   （默认是 LFN_NONE，中文名会变 8.3 短名乱码）
 *      ⚠️ 改 sdkconfig 会触发全量重编，所以留到「内存池版验证通过」之后再动。
 *   ④ 挂载失败不能拖垮界面：网络电台不依赖 SD，返回错误码即可，UI 照跑。
 */
#include "app_sd.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "XSSD";

#define MOUNT_POINT "/sdcard"

/* ---- SDIO 四线引脚（来源：集成板引脚表）---- */
#define PIN_SD_CLK  5
#define PIN_SD_CMD  4
#define PIN_SD_D0   6
#define PIN_SD_D1   7
#define PIN_SD_D2   2
#define PIN_SD_D3   3

static sdmmc_card_t *s_card = NULL;

bool app_sd_is_mounted(void) { return s_card != NULL; }

esp_err_t app_sd_mount(void)
{
    if (s_card) return ESP_OK;                 /* 幂等 */

    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        .format_if_mount_failed = false,       /* ★ 绝不格式化用户的卡 */
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_4BIT;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;    /* 20MHz，先求稳 */

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk   = PIN_SD_CLK;
    slot.cmd   = PIN_SD_CMD;
    slot.d0    = PIN_SD_D0;
    slot.d1    = PIN_SD_D1;
    slot.d2    = PIN_SD_D2;
    slot.d3    = PIN_SD_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount FAILED: %s (0x%x)", esp_err_to_name(ret), (unsigned)ret);
        s_card = NULL;
        return ret;
    }

    sdmmc_card_print_info(stdout, s_card);

    uint32_t mb = 0;
    const char *fs = "?";
    app_sd_get_info(&mb, &fs);
    ESP_LOGI(TAG, "SD mounted at %s   fs=%s   cap=%u MB", MOUNT_POINT, fs, (unsigned)mb);
    return ESP_OK;
}

esp_err_t app_sd_unmount(void)
{
    if (!s_card) return ESP_OK;
    esp_err_t ret = esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
    s_card = NULL;
    return ret;
}

void app_sd_get_info(uint32_t *cap_mb, const char **fs_name)
{
    if (!s_card) {
        if (cap_mb)  *cap_mb = 0;
        if (fs_name) *fs_name = "-";
        return;
    }
    if (cap_mb) {
        uint64_t bytes = (uint64_t)s_card->csd.capacity * (uint64_t)s_card->csd.sector_size;
        *cap_mb = (uint32_t)(bytes / (1024ULL * 1024ULL));
    }
    if (fs_name) *fs_name = "FAT32";
}

int app_sd_list(const char *path, app_sd_entry_t *out, int max)
{
    if (!s_card || !out || max <= 0) return -1;

    DIR *d = opendir(path);
    if (!d) {
        ESP_LOGW(TAG, "opendir(%s) failed", path);
        return -1;
    }

    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;             /* 跳过 . / .. 与隐藏 */
        if (n >= max) break;

        app_sd_entry_t *it = &out[n];
        strncpy(it->name, e->d_name, sizeof(it->name) - 1);
        it->name[sizeof(it->name) - 1] = '\0';
        it->is_dir = false;
        it->size = 0;

        char full[320];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            it->size = (uint32_t)st.st_size;
            it->is_dir = S_ISDIR(st.st_mode) ? true : false;
        }
        n++;
    }
    closedir(d);
    return n;
}

static bool is_audio_name(const char *n)
{
    const char *dot = strrchr(n, '.');
    if (!dot) return false;
    return (strcasecmp(dot, ".mp3")  == 0) || (strcasecmp(dot, ".wav") == 0) ||
           (strcasecmp(dot, ".flac") == 0) || (strcasecmp(dot, ".aac") == 0) ||
           (strcasecmp(dot, ".m4a")  == 0) || (strcasecmp(dot, ".ogg") == 0) ||
           (strcasecmp(dot, ".opus") == 0) || (strcasecmp(dot, ".ts")  == 0);
}

bool app_sd_is_audio(const char *name) { return name && is_audio_name(name); }

/* ★ 10-03：递归层数由 2 层放宽到 5 层。
 *   原因：真机日志 SD root listing 只有两项 ——「System Volume Information」
 *   与「小宇宙播客 64K」，而音频实际在 小宇宙播客 64K/<节目名>/xxx.mp3，
 *   也就是第 3 层。原来 depth<1 只下钻一层，所以数出来恒为 0。
 *
 *   同时加一个「最多看多少条目录项」的预算：32 GB 的卡塞满音频时，
 *   逐条 stat 会拖慢界面（本函数在进 SD 页时会被调用）。
 *   预算用完就返回，宁可数字偏小也不卡死 —— 界面标注的是「约」。
 */
#define SD_SCAN_BUDGET 4000

static int count_audio_rec(const char *path, int depth, int *budget)
{
    if (*budget <= 0) return 0;
    DIR *d = opendir(path);
    if (!d) return 0;

    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (--(*budget) <= 0) break;

        char full[600];
        /* ★ 缓冲区必须够大且精度上限要留足：FATFS 长文件名最长 255 字节，
         *   递归路径再叠几层。写成 %.32s 会把长中文名截断成打不开的路径。
         *   （gcc 的 -Wformat-truncation 会按数组声明尺寸算最坏情况，
         *     所以目标是 600，精度给 280+255=535，静态可证不截断。）*/
        snprintf(full, sizeof(full), "%.280s/%.255s", path, e->d_name);

        struct stat st;
        if (stat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (depth < 4) n += count_audio_rec(full, depth + 1, budget);
        } else if (is_audio_name(e->d_name)) {
            n++;
        }
    }
    closedir(d);
    return n;
}

int app_sd_count_audio(const char *path)
{
    if (!s_card) return 0;
    int budget = SD_SCAN_BUDGET;
    return count_audio_rec(path, 0, &budget);
}

/* ---- 带缓存的全卡音频计数 ----
 * ★ 为什么需要：状态页定时器 1 秒刷一次、SD 页每切一次目录刷一次，
 *   如果每次都真去递归 stat 几千条目录项，界面会明显卡顿。
 *   所以统一走这里：rescan=false 直接给上次结果，只有用户主动动作才重扫。
 *   -1 表示「还没数过」→ 第一次调用自动真扫一次。*/
#define SD_AUDIO_CACHE_PATH "/sdcard"
static int s_audio_total = -1;

int app_sd_audio_total(bool rescan)
{
    if (!s_card) { s_audio_total = -1; return 0; }
    if (rescan || s_audio_total < 0) {
        s_audio_total = app_sd_count_audio(SD_AUDIO_CACHE_PATH);
        ESP_LOGI(TAG, "audio total (rescan=%d) = %d", (int)rescan, s_audio_total);
    }
    return s_audio_total;
}

/* ============================================================
 *  同目录换曲（上一个 / 下一个）
 * ============================================================
 *  两遍扫描，不缓存播放列表 —— 理由见 app_sd.h 的注释。
 *  cur_name 传【纯文件名】，因为 app_radio_now_playing() 给的就是基名。*/
int app_sd_dir_audio_nav(const char *dir, const char *cur_name, int step,
                         char *out_full, int out_full_sz)
{
    if (!s_card || !dir || !dir[0] || !out_full || out_full_sz <= 1) return -1;
    if (step == 0) step = 1;

    DIR *d = opendir(dir);
    if (!d) {
        ESP_LOGW(TAG, "nav: opendir(%s) failed", dir);
        return -1;
    }

    /* ---- 第一遍：数音频总数 + 定位当前曲 ---- */
    int total = 0;
    int cur   = -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (!is_audio_name(e->d_name)) continue;
        if (cur_name && cur_name[0] && strcmp(e->d_name, cur_name) == 0) cur = total;
        total++;
    }
    if (total == 0) {
        closedir(d);
        return -1;
    }

    int want;
    if (cur < 0) {
        /* 当前曲不在这个目录（比如从别处切过来的）→ 从头开始 */
        want = (step > 0) ? 0 : total - 1;
    } else {
        want = ((cur + step) % total + total) % total;   /* 回卷 */
    }

    /* ---- 第二遍：走到目标下标 ---- */
    rewinddir(d);
    int idx = 0, ok = -1;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (!is_audio_name(e->d_name)) continue;
        if (idx == want) {
            /* 精度上限给足 250+255，静态可证不会截断（同 count_audio_rec 的理由）*/
            snprintf(out_full, (size_t)out_full_sz, "%.250s/%.255s", dir, e->d_name);
            ok = 0;
            break;
        }
        idx++;
    }
    closedir(d);

    if (ok == 0) {
        ESP_LOGI(TAG, "nav %s step=%d -> [%d/%d] %s", dir, step, want, total, out_full);
    }
    return ok;
}
