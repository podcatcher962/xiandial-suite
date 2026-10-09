/*
 * ============================================================
 *  app_hist.c —— 播放历史（最近听过什么）
 * ============================================================ */
#include "app_hist.h"

#include <stdio.h>
#include <string.h>

#include "app_radio.h"
#include "net_stations.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "app_stlist.h"

static const char *TAG = "XSHIST";

#define NVS_NS   "xs_hist"
#define NVS_KEY  "list"
#define NAME_MAX 96          /* 台名上限（按字符截断，不切开汉字）*/

/* s_name[0] = 最近听过的那台，依次往后。
 * ★ 静态缓冲，不放栈上：写 NVS 是在 LVGL 任务里调的，
 *   LVGL 任务栈就几 KB，一个 1.5 KB 的局部数组也够它喝一壶。*/
static char s_name[APP_HIST_MAX][NAME_MAX];
static int  s_n;
static bool s_dirty;         /* 有改动还没落盘 */

/* 按【字符】截断复制：按字节截会把汉字切成半个 UTF-8 序列，
 * 存进 NVS 再读出来就是乱码，永远匹配不上台名。*/
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

/* 在台单里按名字找台：先完全相同，再「台名包含」。
 * ★ 与 app_fav 的 resolve() 逐字相同 —— 两份都要有，
 *   不要为了「复用」去抽公共函数：app_fav 那份已经过真机验证，
 *   抽公共反而会让两处耦合，以后改一处忘另一处。*/
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

static char s_blob[APP_HIST_MAX * NAME_MAX];

/* ★ 前向声明：note() 在上面、schedule_flush 的定义在它下面。
 *   缺这一句 gcc 报 implicit declaration —— 但更坑的是 IDF 默认
 *   -Werror=implicit-function-declaration，编译直接失败（这次就撞上了）。
 *   正确做法是显式声明，而不是把定义往上挪：
 *   挪了以后 schedule_flush 又要调 app_hist_flush，顺序会绕回来。*/
static void schedule_flush(void);

static void save(void)
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
        return;
    }
    r = nvs_set_blob(h, NVS_KEY, s_blob, o);
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    s_dirty = false;
    if (r != ESP_OK) ESP_LOGW(TAG, "保存历史失败: %s", esp_err_to_name(r));
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
    while (o < len && s_n < APP_HIST_MAX) {
        size_t l = strnlen(s_blob + o, len - o);
        if (l == 0) break;                       /* 空串 = 名单结束 */
        if (l > NAME_MAX - 1) l = NAME_MAX - 1;
        memcpy(s_name[s_n], s_blob + o, l);
        s_name[s_n][l] = '\0';
        s_n++;
        o += l + 1;
    }
}

esp_err_t app_hist_init(void)
{
    s_n = 0;
    s_dirty = false;
    load();
    ESP_LOGI(TAG, "历史恢复 %d 条", s_n);
    return ESP_OK;
}

void app_hist_note(const char *name)
{
    if (name == NULL || name[0] == '\0') return;

    /* 台名过长先截 —— copy_name 自己会按字符截，
     * 但这里要确认截完还非空，否则会存进一条空记录。*/
    char buf[NAME_MAX];
    copy_name(buf, sizeof(buf), name);
    if (buf[0] == '\0') return;

    /* 已经在历史里 ⇒ 移到最前（不是重复添加）。
     * memmove 而不是逐个赋值：条目少，但写成循环更啰嗦。*/
    for (int i = 0; i < s_n; i++) {
        if (strcmp(s_name[i], buf) == 0) {
            if (i == 0) return;                /* 已经是最近的了 */
            char tmp[NAME_MAX];
            memcpy(tmp, s_name[i], NAME_MAX);
            memmove(&s_name[1], &s_name[0], (size_t)i * NAME_MAX);
            memcpy(s_name[0], tmp, NAME_MAX);
            s_dirty = true;
            ESP_LOGI(TAG, "移到最前：%s", buf);
            schedule_flush();
            return;
        }
    }

    /* 新的：插到最前，满了丢最后一条。*/
    int keep = s_n;
    if (keep >= APP_HIST_MAX) keep = APP_HIST_MAX - 1;
    if (keep > 0) memmove(&s_name[1], &s_name[0], (size_t)keep * NAME_MAX);
    copy_name(s_name[0], NAME_MAX, buf);
    s_n = keep + 1;
    s_dirty = true;
    ESP_LOGI(TAG, "新增历史：%s（共 %d 条）", buf, s_n);
    schedule_flush();
}

int app_hist_top(int *out, int max)
{
    if (out == NULL || max <= 0) return 0;
    int w = 0;
    for (int i = 0; i < s_n && w < max; i++) {
        int idx = resolve(s_name[i]);
        if (idx < 0) continue;                 /* 台单里已删，跳过 */
        /* ★ 顺手去重：台单里若有同名台，resolve 会两次返同一个下标，
         *   直接写进去会让首页出现两个一模一样的格子。*/
        bool dup = false;
        for (int k = 0; k < w; k++) {
            if (out[k] == idx) { dup = true; break; }
        }
        if (dup) continue;
        out[w++] = idx;
    }
    return w;
}

int app_hist_count(void)
{
    return s_n;
}

/* 开机后延迟落盘：切台是高频动作，若每次都同步写 Flash ——
 * ① 一次切台要擦写一页 NOR Flash，天天切台会慢慢磨穿；
 * ② 那 3~5 ms 的阻塞发生在调用者（UI 任务 / 切台路径）上，
 *    正好顶在音频链路上，表现为「切台时声音卡一下」。
 * ⇒ 改成：note() 只改内存并置脏，起一个 20 秒的一次性定时器；
 *   到期才真写。期间再 note 只是把定时器重置，不额外写。*/
static esp_timer_handle_t s_flush_timer;

static void flush_cb(void *arg)
{
    (void)arg;
    app_hist_flush();
    s_flush_timer = NULL;
}

void app_hist_flush(void)
{
    if (!s_dirty) return;
    save();
}

static void schedule_flush(void)
{
    if (s_flush_timer) {
        esp_timer_stop(s_flush_timer);          /* 重置，不新建 */
    } else {
        esp_timer_create_args_t a = {
            .callback = flush_cb,
            .name = "xs_hist_flush",
        };
        if (esp_timer_create(&a, &s_flush_timer) != ESP_OK) {
            /* 建不出定时器就退化成同步写：宁可慢，也不能丢历史。*/
            ESP_LOGW(TAG, "建不出 flush 定时器，改为直接写");
            save();
            return;
        }
    }
    esp_timer_start_once(s_flush_timer, 20 * 1000 * 1000LL);
}

