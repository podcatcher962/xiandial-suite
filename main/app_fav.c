#include "app_fav.h"
#include "xs_version_mode.h"   /* XS_FAV_PUBLISH：发布版形态开关，见 xs_version_mode.h */

#include <stdio.h>
#include <string.h>

#include "app_radio.h"
#include "net_stations.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "app_stlist.h"

static const char *TAG = "XSFAV";

#define NVS_NS   "xs_fav"
#define NVS_KEY  "list"
#define NAME_MAX 96          /* 台名上限（按字符截断，不会切开汉字）*/

static char s_name[APP_FAV_MAX][NAME_MAX];
static int  s_idx[APP_FAV_MAX];      /* 当前台单下标；-1 = 没解析出来 */
static int  s_n;

/* ★ 静态缓冲，不放栈上：写 NVS 时是在 LVGL 任务里调的，
 *   LVGL 任务栈就几 KB，一个 6 KB 的局部数组会直接把它打穿。*/
static char s_blob[APP_FAV_MAX * NAME_MAX];

/* ★★ 首次开机的播种名单：**发布版是空的**。
 *
 * 为什么不能把这几个台名硬编码在这里（10-06 差点带进发布包）：
 *   这是【兰兰个人常听的台】。两个问题：
 *     ① 泄漏真实台名 —— 而且是【扫源码扫不出来的那种】：
 *        它不是 URL、不是本机路径、不是凭据，只是一个中文字符串。
 *        _release_gitscan 规则全绿、_release_check 45/45 通过，
 *        最后是靠【直接 grep 编译产物 xiandial-radio.bin】才抓到的。
 *        ⇒ 教训：源码扫描只能证明「没有明显的」，证明不了「没有残留的」。
 *     ② 和「发布版 0 条台单」自相矛盾 —— 台单是空的，这几个名字
 *        必然一个都解析不出来，只是白占 flash 与收藏位。
 *
 * 名单本体由台单生成器产出，两版各一份：
 *   自用版  net_stations_seed.h   （XS_FAV_SEED 宏，10 条）
 *   发布版  【不include 任何名单】—— 由 CMakeLists 定义 XS_FAV_PUBLISH 宏切换
 * app_fav.c 两版共用同一份代码，UI 与行为一致。
 *
 * 播种用的是【关键词】不是完整台名：解析先试「完全相同」再试「包含」，
 * 所以「上海交通」能命中台单里的「上海交通广播」；解析成功后会把
 * 完整台名回写进 NVS，之后就走精确匹配了。
 *
 * ★★ 版本切换用 CMake 传进来的 XS_FAV_PUBLISH 宏，不用 __has_include。
 *   我第一版用 `#if __has_include("net_stations_seed.h")`，看起来更优雅
 *   （发布版仓库里本来就没这个文件），结果【重编后 bin 里照样带着 10 个
 *   真实台名】—— 因为 net_stations_seed.h 是【自用版本地文件】，
 *   我自己机器上有，编「发布版」时它照样在 include 路径里。
 *   ⇒ 判据必须区分「Git 仓库里有什么」和「我这台机器上有什么」，
 *     只有 CMake 里那个【和台单同一个 if/else】的判断才算数
 *     （它本来就是发布形态的唯一真相源）。
 *   ⇒ 这也是为什么 XS_FAV_PUBLISH 要用 target_compile_definitions 传：
 *     idf_component_register() 没有 DEFINES 参数，-D 塞进去会被并进
 *     INCLUDE_DIRS 报「Include directory '.../-DXS_FAV_PUBLISH=1' is not
 *     a directory」（10-06 踩过，exit 2）。*/
#ifndef XS_FAV_PUBLISH
#  define XS_FAV_PUBLISH 0          /* 没传就当自用版（宁可多播种别少） */
#endif

#if XS_FAV_PUBLISH
   /* 发布版：台单是空的，播种名单必然一个都匹配不上 → 不播种。
      k_seed 只为让下面循环里的 k_seed[i] 有定义而存在，
      编译期 SEED_N 就是 0，循环一次都不进。*/
static const char *const k_seed[] = { "" };
#  define SEED_N 0
#else
   /* 自用版：兰兰常听的 10 个台（关键词，解析时先精确后包含匹配） */
#  include "net_stations_seed.h"
static const char *const k_seed[] = { XS_FAV_SEED };
#  define SEED_N XS_FAV_SEED_N
#endif

/* 按【字符】截断复制：按字节截会把汉字切成半个 UTF-8 序列，
 * 存进 NVS 之后再读出来就是乱码，永远也匹配不上台名。*/
static void copy_name(char *dst, size_t dstsz, const char *src)
{
    size_t o = 0;
    while (src && *src) {
        unsigned char c = (unsigned char)*src;
        int len = 1;
        if      ((c & 0x80) == 0x00) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (o + (size_t)len + 1 > dstsz) break;
        for (int i = 0; i < len; i++) {
            if (!src[i]) { dst[o] = '\0'; return; }
            dst[o++] = src[i];
        }
        src += len;
    }
    dst[o] = '\0';
}

/* 在台单里按名字找台：先完全相同，再「台名包含关键词」 */
static int resolve(const char *kw)
{
    if (kw == NULL || kw[0] == '\0') return -1;
    int n = app_radio_station_count();
    for (int i = 0; i < n; i++) {
        if (strcmp(app_st_get(i)->name, kw) == 0) return i;
    }
    for (int i = 0; i < n; i++) {
        if (strstr(app_st_get(i)->name, kw) != NULL) return i;
    }
    return -1;
}

static esp_err_t save(void)
{
    size_t o = 0;
    for (int i = 0; i < s_n; i++) {
        size_t l = strlen(s_name[i]) + 1;
        if (o + l > sizeof(s_blob)) break;
        memcpy(s_blob + o, s_name[i], l);
        o += l;
    }
    nvs_handle_t h;
    esp_err_t r = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open 失败: %s", esp_err_to_name(r));
        return r;
    }
    r = nvs_set_blob(h, NVS_KEY, s_blob, o);
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    if (r != ESP_OK) ESP_LOGW(TAG, "保存收藏失败: %s", esp_err_to_name(r));
    return r;
}

static void load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_blob);
    esp_err_t r = nvs_get_blob(h, NVS_KEY, s_blob, &len);
    nvs_close(h);
    if (r != ESP_OK) return;

    size_t o = 0;
    while (o < len && s_n < APP_FAV_MAX) {
        size_t l = strnlen(s_blob + o, len - o);
        if (l == 0) break;                       /* 空串 = 名单结束 */
        if (l > NAME_MAX - 1) l = NAME_MAX - 1;
        memcpy(s_name[s_n], s_blob + o, l);
        s_name[s_n][l] = '\0';
        s_n++;
        o += l + 1;
    }
}

esp_err_t app_fav_init(void)
{
    s_n = 0;
    for (int i = 0; i < APP_FAV_MAX; i++) s_idx[i] = -1;

    load();
    bool first_time = (s_n == 0);

    if (first_time) {
        for (int i = 0; i < SEED_N && s_n < APP_FAV_MAX; i++) {
            copy_name(s_name[s_n], sizeof(s_name[0]), k_seed[i]);
            s_n++;
        }
        ESP_LOGI(TAG, "首次开机：播种 %d 个收藏", s_n);
        if (s_n == 0)
            ESP_LOGW(TAG, "播种 0 个 —— 发布版台单为空，属预期");
    } else {
        ESP_LOGI(TAG, "从 NVS 读到 %d 个收藏", s_n);
    }

    int ok = 0, bad = 0;
    for (int i = 0; i < s_n; i++) {
        int idx = resolve(s_name[i]);
        s_idx[i] = idx;
        if (idx >= 0) {
            ok++;
            /* 播种时写的是关键词，这里换成台单里的完整台名，之后就是精确匹配 */
            if (first_time) copy_name(s_name[i], sizeof(s_name[0]), app_st_get(idx)->name);
        } else {
            bad++;
            ESP_LOGW(TAG, "收藏「%s」在当前台单里找不到（跳过）", s_name[i]);
        }
    }
    ESP_LOGI(TAG, "收藏就绪：%d 个（解析成功 %d，找不到 %d）", s_n, ok, bad);
    if (first_time) save();
    return ESP_OK;
}

static int find_pos(int station_idx)
{
    for (int i = 0; i < s_n; i++) {
        if (s_idx[i] == station_idx) return i;
    }
    return -1;
}

bool app_fav_has(int station_idx)
{
    return station_idx >= 0 && find_pos(station_idx) >= 0;
}

int app_fav_count(void)
{
    int c = 0;
    for (int i = 0; i < s_n; i++) {
        if (s_idx[i] >= 0) c++;
    }
    return c;
}

esp_err_t app_fav_add(int station_idx)
{
    if (station_idx < 0 || station_idx >= app_radio_station_count()) {
        return ESP_ERR_INVALID_ARG;
    }
    if (find_pos(station_idx) >= 0) return ESP_OK;      /* 已经在里面了 */
    if (s_n >= APP_FAV_MAX) {
        ESP_LOGW(TAG, "收藏已满（%d 个）", APP_FAV_MAX);
        return ESP_ERR_NO_MEM;
    }
    copy_name(s_name[s_n], sizeof(s_name[0]), app_st_get(station_idx)->name);
    s_idx[s_n] = station_idx;
    s_n++;
    ESP_LOGI(TAG, "+ 收藏「%s」（共 %d 个）", app_st_get(station_idx)->name, app_fav_count());
    return save();
}

esp_err_t app_fav_remove(int station_idx)
{
    int p = find_pos(station_idx);
    if (p < 0) return ESP_OK;
    ESP_LOGI(TAG, "- 取消收藏「%s」（共 %d 个）", app_st_get(station_idx)->name,
             app_fav_count() - 1);
    for (int i = p; i < s_n - 1; i++) {
        memcpy(s_name[i], s_name[i + 1], NAME_MAX);
        s_idx[i] = s_idx[i + 1];
    }
    s_n--;
    return save();
}

esp_err_t app_fav_toggle(int station_idx)
{
    if (app_fav_has(station_idx)) return app_fav_remove(station_idx);
    return app_fav_add(station_idx);
}

int app_fav_list(int *out, int max)
{
    if (out == NULL || max <= 0) return 0;
    int c = 0;
    for (int i = 0; i < s_n && c < max; i++) {
        if (s_idx[i] >= 0) out[c++] = s_idx[i];
    }
    return c;
}

esp_err_t app_fav_clear(void)
{
    s_n = 0;
    ESP_LOGW(TAG, "已清空全部收藏");
    return save();
}
