/*
 * 拾声 (XianDial) 硬件版 —— SD 卡台单导入（stations.tsv）
 * 见app_stlist.h 的契约说明。
 *
 * 【为什么这样写】
 *  ① 回落优先：任何异常都回落到内置台单。依据 v1.40 的教训 ——
 *     LVGL 在 release 下分配失败是静默崩溃，UI 侧任何"数据可能没有"
 *     的路径都必须在这里兜住，界面上只看到"0 台"这种合法状态，
 *     看不到野指针。
 *  ② 内存：8MB PSRAM（OCT）。2000 台的字符串池约 200 KB，
 *     指针数组 8 KB，总约 208 KB，一次 malloc 常驻。
 *  ③ 不在 UI 回调里分配：app_st_load() 由开机流程调用一次。
 *  ④ 解析严格但不苛刻：多一个 TAB、少一个字段都跳过这一行并计数，
 *     不因为一行坏就丢掉整份文件 —— 使用者导出的 M3U 千奇百怪。
 */
#include "app_stlist.h"
#include "xs_version_mode.h"   /* XS_FAV_PUBLISH：发布版形态开关，见 xs_version_mode.h */
#include "app_display.h"       /* ★ 10-07：lq_shot 要 app_display_snapshot() */
#include "app_sd.h"
#include "app_radio.h"
#include "ui_xiandial.h"
#include "ui_lingqian.h"      /* ★ 10-07：lq_demo 跳页（截图调试用） */
#include "ui_shell.h"         /* ★ 10-08：app_open / app_home（进出产品）*/
#include "ui_radio_v.h"       /* ★ 10-08：radio 调试口（竖版电台的分类/滚动/本地）*/
#include "ui_stock.h"         /* ★ 10-08：stk_kline 跳页（截图调试用） */
#include "ui_weather.h"       /* ★ 10-08：ui_demo speak（播报链路调试）*/
#include "app_prov.h"          /* ★ 10-09：wifiscan（配网页下拉的数据源）*/
#include "esp_lvgl_port.h"    /* ★ lgl_port_lock/unlock：跳页必须持 LVGL 锁 */
/* ★ 10-08：lq_shot 要 lv_obj_invalidate / lv_refr_now / lv_screen_active。
 *   不能靠 esp_lvgl_port.h 间接带进来 —— 那是个实现细节，
 *   组件换一版就可能不再透传 lvgl.h。自己显式包含。*/
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <esp_timer.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
/* ★★ 10-06 编译失败踩的坑：这里原来写的是 <driver/uart.h>，报
 *   "fatal error: driver/uart.h: No such file or directory"。
 *   根因不是路径写错，而是【这块板根本没有硬件 UART】：
 *   sdkconfig 里 CONFIG_ESP_CONSOLE_UART_NUM=-1、
 *   CONFIG_ESP_CONSOLE_UART_DEFAULT 未选、控制台走 USB Serial/JTAG。
 *   也就是说 uart_read_bytes() 压根没有设备可读。
 *   正确通道：esp_driver_usb_serial_jtag 组件的 usb_serial_jtag_read_bytes()。
 *   IDF 6.x 已把老 driver 组件拆成 esp_driver_*，但【头文件路径没变】，
 *   所以 app_display.c 里的 driver/i2c_std.h 之类照样能编过 ——
 *   别看到 driver/xxx.h 报缺文件就以为是改名问题，先查有没有这个外设。*/
#include <driver/usb_serial_jtag.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include <lvgl.h>              /* ★ 10-08：lq_mem 用 lv_mem_monitor() 读 LVGL 对象池水位 */

static const char *TAG = "XSTLIST";

/* ---- 常驻状态 ---- */
static net_station_t *s_pool = NULL;      /* 指针数组，PSRAM */
static char         *s_str  = NULL;       /* 字符串池，PSRAM */
static size_t        s_str_used = 0;
static size_t        s_str_cap  = 0;
static int           s_count    = 0;
static app_st_src_t  s_src      = APP_ST_SRC_BUILTIN;
static bool          s_loaded   = false;

/* 名字 → 下标 的小缓存（仅加速收藏/历史查找，不影响正确性） */
#define NAME_CACHE 64
static struct { char name[72]; int idx; } s_ncache[NAME_CACHE];
static int s_ncache_n = 0;

/* ---------- 字符串池 ---------- */
static bool str_reserve(size_t need)
{
    if (s_str_used + need <= s_str_cap) return true;
    if (!s_str) {
        /* 首次：按上限一次性给足，避免反复 realloc 造成碎片 */
        size_t cap = 384 * 1024;                 /* 384 KB，2000 台够用 */
        s_str = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
        if (!s_str) {
            /* 没有 PSRAM 就退到内部 RAM（会紧张，但比不工作强）*/
            ESP_LOGW(TAG, "PSRAM 分配失败，退回内部 RAM %u B", (unsigned)cap);
            cap = 160 * 1024;
            s_str = heap_caps_malloc(cap, MALLOC_CAP_8BIT);
        }
        if (!s_str) { ESP_LOGE(TAG, "字符串池分配失败"); return false; }
        s_str_cap = cap;
        s_str_used = 0;
        return s_str_used + need <= s_str_cap;
    }
    /* 池子不够：扩到 2 倍。old 块留在 PSRAM 里没释放，偶发一次可接受。 */
    size_t ncap = s_str_cap * 2;
    char *n = heap_caps_malloc(ncap, MALLOC_CAP_SPIRAM);
    if (!n) n = heap_caps_malloc(ncap, MALLOC_CAP_8BIT);
    if (!n) { ESP_LOGE(TAG, "字符串池扩容失败 %u->%u", (unsigned)s_str_cap, (unsigned)ncap); return false; }
    memcpy(n, s_str, s_str_used);
    s_str = n; s_str_cap = ncap;
    return s_str_used + need <= s_str_cap;
}

static const char *str_put(const char *src, size_t n)
{
    if (n == 0) return "";
    if (!str_reserve(n + 1)) return NULL;
    char *p = s_str + s_str_used;
    memcpy(p, src, n);
    p[n] = '\0';
    s_str_used += n + 1;
    return p;
}

/* ---------- 名字 → 下标 映射 ---------- */
/* 栏目/地区对不上时的兜底下标。名字必须与 net_stations.h 的
 * NET_CAT_N=13 顺序一致；找不到就用 0（第一个，界面仍能显示）。*/
static unsigned char cat_index(const char *s)
{
    if (!s || !*s) return 0;
    for (int i = 0; i < NET_CAT_N; i++)
        if (strcmp(s, g_cat_name[i]) == 0) return (unsigned char)i;
    return 0;      /* 对不上→ 第一个（界面显示的是真名，不影响播放） */
}

static unsigned char prov_index(const char *s)
{
    if (!s || !*s) return NET_PROV_NONE;
    /* 「全国」「其他」按契约→ NET_PROV_NONE。
     * ⚠️ 这与g_prov_name 第 35 项「其他华语」不是一回事，别混。 */
    if (strcmp(s, "全国") == 0 || strcmp(s, "其他") == 0)
        return NET_PROV_NONE;
    for (int i = 0; i < NET_PROV_N; i++)
        if (strcmp(s, g_prov_name[i]) == 0) return (unsigned char)i;
    return NET_PROV_NONE;
}

/* 去掉首尾空白 + 去掉 Windows 风格尾部 CR（★ CRLF 会把URL 尾部挂 \r，
 * 症状是「别的台都好，就这几台不行」，极难查，所以这里必须剥）*/
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' ||
                     s[n - 1] == ' '  || s[n - 1] == '\t'))
        s[--n] = '\0';
    return s;
}

/* 把 TSV 的一行拆成 4 段，就地打 \0，返回段数（<4 视为坏行） */
static int split4(char *line, char *f[4])
{
    int k = 0;
    f[0] = line;
    for (char *p = line; *p; p++) {
        if (*p == '\t') {
            *p = '\0';
            if (k < 3) f[++k] = p + 1;
            else return 4;            /* 第 5 个 TAB 起，后面全部并入第 4 段 */
        }
    }
    return k + 1;
}

/* ---------- 内部台单也走缓存池，让上层代码只有一条路径 ----------
 * 只在「没找到 TSV」时调用：把 g_stations 复制到池里。
 * 好处：app_st_get() 永远是同一套逻辑，不会出现
 *「走了 TSV 路径」和「走了内置路径」两种行为。 */
static void load_builtin(void)
{
    int n = g_station_count;
    if (n <= 0) { s_count = 0; return; }
    if (n > APP_ST_MAX) n = APP_ST_MAX;

    s_pool = heap_caps_calloc(n, sizeof(net_station_t), MALLOC_CAP_SPIRAM);
    if (!s_pool) s_pool = calloc(n, sizeof(net_station_t));
    if (!s_pool) { ESP_LOGE(TAG, "内置台单 %d 条分配失败", n); s_count = 0; return; }

    for (int i = 0; i < n; i++) {
        const char *nm = g_stations[i].name ? g_stations[i].name : "";
        const char *ur = g_stations[i].url  ? g_stations[i].url  : "";
        const char *pn = str_put(nm, strlen(nm));
        const char *pu = str_put(ur, strlen(ur));
        if (!pn || !pu) {            /* 池子不够就到此为止，能放多少放多少 */
            s_count = i; break;
        }
        s_pool[i].name = pn;
        s_pool[i].url  = pu;
        s_pool[i].cat  = g_stations[i].cat;
        s_pool[i].prov = g_stations[i].prov;
    }
    if (s_count == 0) s_count = n;
    s_src = APP_ST_SRC_BUILTIN;
    ESP_LOGI(TAG, "内置台单 %d 条", s_count);
}

static void free_all(void)
{
    if (s_pool) { free(s_pool); s_pool = NULL; }
    /* s_str 刻意不释放：常驻，释放了立刻还要再分配，碎片更糟 */
    s_count = 0; s_ncache_n = 0;
}

static void ncache_add(const char *name, int idx)
{
    if (s_ncache_n >= NAME_CACHE) {
        /* 简单覆盖第0 项（收藏一般只有十几个，够用） */
        snprintf(s_ncache[0].name, sizeof(s_ncache[0].name), "%s", name);
        s_ncache[0].idx = idx;
        return;
    }
    snprintf(s_ncache[s_ncache_n].name, sizeof(s_ncache[s_ncache_n].name), "%s", name);
    s_ncache[s_ncache_n].idx = idx;
    s_ncache_n++;
}

/* ---------- 主流程：解析 stations.tsv ---------- */
static app_st_src_t load_tsv(void)
{
    if (!app_sd_is_mounted()) {
        ESP_LOGI(TAG, "SD 未挂载，用内置台单");
        load_builtin();
        return APP_ST_SRC_BUILTIN;
    }

    FILE *fp = fopen(APP_ST_FILE, "rb");
    if (!fp) {
        ESP_LOGI(TAG, "没有 %s，用内置台单", APP_ST_FILE);
        load_builtin();
        return APP_ST_SRC_BUILTIN;
    }

    /* 先数行，据此定池大小，避免反复扩容 */
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsz <= 0) { fclose(fp); load_builtin(); return APP_ST_SRC_BUILTIN; }

    int cap = (int)(fsz / 64);          /* 估算：一条台单最短约 64 字节 */
    if (cap < 32)   cap = 32;
    if (cap > APP_ST_MAX) cap = APP_ST_MAX;

    s_pool = heap_caps_calloc(cap, sizeof(net_station_t), MALLOC_CAP_SPIRAM);
    if (!s_pool) s_pool = calloc(cap, sizeof(net_station_t));
    if (!s_pool) {
        ESP_LOGE(TAG, "台单数组分配失败（%d 条）", cap);
        fclose(fp); load_builtin(); return APP_ST_SRC_BUILTIN;
    }

    char line[APP_ST_MAX_LINE];
    int  bad = 0, over = 0;
    /* ① UTF-8 BOM：'\xEF\xBB\xBF'。带 BOM 会让第一个台名变成
     *    "□中央人民广播电台"，且收藏按名匹配永远存不上。 */
    int  first = 1;

    while (fgets(line, sizeof(line), fp)) {
        if (first) {
            first = 0;
            size_t l = strlen(line);
            if (l >= 3 && (unsigned char)line[0] == 0xEF &&
                (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) {
                memmove(line, line + 3, l - 2);
                ESP_LOGW(TAG, "★ 文件带 BOM，已剥离（否则第一个台名会多一个方块）");
            }
        }
        /* 注释行与空行 */
        char *t = trim(line);
        if (*t == '\0' || *t == '#') continue;

        char *f[4];
        if (split4(t, f) < 4) { bad++; continue; }

        char *name = trim(f[0]);
        char *cat  = trim(f[1]);
        char *prov = trim(f[2]);
        char *url  = trim(f[3]);
        if (!*name || !*url) { bad++; continue; }

        if (s_count >= APP_ST_MAX) { over++; continue; }
        if (s_count >= cap) break;             /* 池满，停止 */

        const char *pn = str_put(name, strlen(name));
        const char *pu = str_put(url,  strlen(url));
        if (!pn || !pu) {
            ESP_LOGE(TAG, "字符串池在第 %d 行耗尽，截断到 %d 条", s_count + 1, s_count);
            break;
        }
        s_pool[s_count].name = pn;
        s_pool[s_count].url  = pu;
        s_pool[s_count].cat  = cat_index(cat);
        s_pool[s_count].prov = prov_index(prov);
        ncache_add(pn, s_count);
        s_count++;
    }
    fclose(fp);

    if (s_count <= 0) {
        ESP_LOGW(TAG, "TSV 解析到 0 条（坏行 %d），回落到内置台单", bad);
        free_all();
        load_builtin();
        return APP_ST_SRC_BUILTIN;
    }

    s_src = APP_ST_SRC_TSV;
    ESP_LOGI(TAG, "★ 台单来自 %s：%d 条（坏行 %d，超限丢弃 %d）",
             APP_ST_FILE, s_count, bad, over);
    return APP_ST_SRC_TSV;
}

/* ---------- 对外接口 ---------- */
app_st_src_t app_st_load(void)
{
    if (s_loaded) return s_src;             /* 幂等 */
    s_loaded = true;
    free_all();
    s_ncache_n = 0;
    app_st_src_t r = load_tsv();
    ESP_LOGI(TAG, "载入完成：来源=%s 台数=%d PSRAM free=%u",
             r == APP_ST_SRC_TSV ? "TSV" : "内置", s_count,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return r;
}

app_st_src_t app_st_source(void) { return s_src; }
int         app_st_count(void)   { return s_count; }

const net_station_t *app_st_get(int i)
{
    if (i < 0 || i >= s_count) return NULL;
    return &s_pool[i];
}

int app_st_find_by_name(const char *name)
{
    if (!name || !*name) return -1;
    for (int k = 0; k < s_ncache_n; k++)
        if (strcmp(s_ncache[k].name, name) == 0) return s_ncache[k].idx;
    for (int i = 0; i < s_count; i++)
        if (strcmp(s_pool[i].name, name) == 0) return i;
    return -1;
}

/* ---------- 串口命令通道（10-06）----------
 * 用途：插了卡、换了 stations.tsv 之后不用重启机器。
 * 串口敲 st_reload 即可重新读卡；st_dump 打印当前台单。
 *
 * ★ 为什么需要它：板载TF 卡座走SDIO，Windows 看不到这张卡
 *   （板子不是 USB 读卡器），所以开发时没法像 U 盘那样往里拷文件。
 *   有了这条命令，改完文件在串口敲一下就能验证，不必反复断电。
 * ★ 长期有用：使用者自己换了台单同样能热重载，不用重启。
 */
/* ---------- 卡上文件的读 / 写 / 删（10-06）----------
 * 存在的理由：板载 TF 走 SDIO，Windows 看不到这张卡；板载 Type-C 的
 * PHY 又被 Serial-JTAG 占着、板子不能当 USB 主机。⇒ 改卡上文件以前
 * 只能拔卡插读卡器。有了这一组命令，改台单/改 WiFi 账密都在串口里做。
 *
 * ★ 写协议刻意做成「一行一条命令」而不是发裸字节：串口是字符设备，
 *   裸字节协议要自己处理转义、半包、粘包，调试时肉眼看不出来。
 *   一行一条虽然慢（13139 字节的台单要发 144 行），但每次都能看见。
 */

/* 串口命令任务的栈（字节）。★ 这个数【只允许写一处】：
 * app_st_start_serial_cmd() 用它建任务，lq_mem 用它印"栈共多少" ——
 * 两处各写一个数字，改了这处忘那处，报出来的余量就是假的。
 * 每次上调的原因见 app_st_start_serial_cmd() 里的注释。*/
#define ST_CMD_STACK  10240

#define ST_PATH_MAX 64

/* 正在写入的目标文件名（st_put 用）*/
static char s_wfp_name[ST_PATH_MAX];

/* 只允许操作 SD 卡根目录下的文件，且文件名里不许出现 '/' 或 ".."，
 * 免得一条 st_cat ../../../ 之类把别的分区也读了。
 * 只读卡是别人的数据，这层检查不能省。 */
static bool st_safe_name(const char *name)
{
    if (!name || !*name) return false;
    size_t n = strlen(name);
    if (n >= ST_PATH_MAX) return false;
    if (strchr(name, '/') || strchr(name, '\\')) return false;
    if (n >= 2 && name[0] == '.' && name[1] == '.') return false;
    return true;
}

static bool st_path(char *out, size_t cap, const char *name)
{
    if (!st_safe_name(name)) return false;
    snprintf(out, cap, "/sdcard/%s", name);
    return true;
}

/* ---- st_ls：列根目录 ---- */
static void st_ls(void)
{
    if (!app_sd_is_mounted()) { ESP_LOGW(TAG, "st_ls: SD 没挂载"); return; }
    DIR *d = opendir("/sdcard");
    if (!d) { ESP_LOGW(TAG, "st_ls: 打不开根目录"); return; }
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        /* ★ d_name 最长 255 字节，路径缓冲必须留够 8+255+1，否则
         *   编译报 format-truncation（-Werror 直接挂）。曾经写 160 被抓出来。*/
        char p[300];
        snprintf(p, sizeof(p), "/sdcard/%s", e->d_name);
        struct stat stt;
        if (stat(p, &stt) == 0)
            ESP_LOGI(TAG, "  %-16s %8lld B", e->d_name, (long long)stt.st_size);
        else
            ESP_LOGI(TAG, "  %-16s <?>", e->d_name);
        n++;
    }
    closedir(d);
    ESP_LOGI(TAG, "st_ls: 共 %d 项", n);
}

/* ---- st_cat <file>：回读卡上文本文件 ----
 * 用途：写完立刻核对。写入成功返回码不代表内容对，
 * 只有把字节读回来看到才对（★ 铁律：自写 check 先用已知答案验证再信结论）。*/
static void st_cat(const char *name)
{
    char p[ST_PATH_MAX + 16];
    if (!st_path(p, sizeof(p), name)) { ESP_LOGW(TAG, "st_cat: 文件名不合法"); return; }
    FILE *fp = fopen(p, "rb");
    if (!fp) { ESP_LOGW(TAG, "st_cat: 打不开 %s", p); return; }
    char line[APP_ST_MAX_LINE];
    int n = 0;
    size_t total = 0;
    while (fgets(line, sizeof(line), fp)) {
        total += strlen(line);
        /* 只打前后各若干行，中间省掉，免得刷屏 */
        if (n < 5 || n >= 1000) ESP_LOGI(TAG, "  |%s", line);
        else if (n == 5) ESP_LOGI(TAG, "  ...（中间省略）...");
        n++;
    }
    fclose(fp);
    ESP_LOGI(TAG, "st_cat: %s 共 %d 行 / %u 字节", name, n, (unsigned)total);
}

/* ---- st_rm <file> ---- */
static void st_rm(const char *name)
{
    char p[ST_PATH_MAX + 16];
    if (!st_path(p, sizeof(p), name)) { ESP_LOGW(TAG, "st_rm: 文件名不合法"); return; }
    if (remove(p) == 0) ESP_LOGW(TAG, "st_rm: 已删除 %s", name);
    else            ESP_LOGW(TAG, "st_rm: 删不掉 %s", name);
}

/* ---- st_put <file>：进入写入模式 ----
 * 后续每行原样写入（自动补 LF），单独一行 "." 结束并回读校验。
 * 返回 true 表示已进写入模式，调用方要把后续行喂给 st_put_line()。 */
static bool st_put_begin(const char *name, FILE **out_fp)
{
    char p[ST_PATH_MAX + 16];
    if (!app_sd_is_mounted()) { ESP_LOGW(TAG, "st_put: SD 没挂载"); return false; }
    if (!st_path(p, sizeof(p), name)) { ESP_LOGW(TAG, "st_put: 文件名不合法"); return false; }

    /* ★ 先写 .tmp 再改名：中途断电/断线不会留下半个文件，
     *   而半个 stations.tsv 会被解析成「一堆坏行」甚至 0 台。 */
    char tmp[ST_PATH_MAX + 24];
    snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    FILE *fp = fopen(tmp, "wb");
    if (!fp) { ESP_LOGW(TAG, "st_put: 建不了 %s", tmp); return false; }
    *out_fp = fp;
    ESP_LOGW(TAG, "st_put: 开始写 %s（写到 %s.tmp，收到单独一行 . 结束）", name, name);
    return true;
}

static void st_put_line(FILE *fp, const char *line)
{
    fputs(line, fp);
    fputc('\n', fp);
}

static void st_put_end(FILE *fp, const char *name)
{
    fflush(fp);
    fclose(fp);
    char p[ST_PATH_MAX + 16], tmp[ST_PATH_MAX + 24];
    st_path(p, sizeof(p), name);
    snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    /* ★ 同 st_putraw：FatFs 的 f_rename 不覆盖已存在的目标，
     *   不先删就是"第一次能写、以后都写不进去"。*/
    remove(p);
    if (rename(tmp, p) != 0) {
        ESP_LOGE(TAG, "st_put:改名失败 %s -> %s", tmp, p);
        return;
    }
    ESP_LOGW(TAG, "st_put: 写完，已改名为 %s，下面回读核对", name);
    st_cat(name);
}

/* ---- st_putraw <file> <字节数> <CRC32> ：整块【二进制】写入（10-07 加）----
 *
 * ★ 为什么必须有这一条（而不是拿 st_put 凑）：
 *   灵签的签文语音是 100 个 mp3（合计约 16 MB），要放进 SD 卡。
 *   但——
 *     ① 板载 TF 走 SDIO，Windows 看不到这张卡；板子的 Type-C 又被
 *        Serial-JTAG 占着，当不了 USB 主机 ⇒ 插卡器是唯一"拔卡"方案；
 *     ② 板子在 TP_Guest 访客网络上，访客网与有线内网【互相不可达】
 *        （实测：电脑 ping 不到板子、板子 st_net 第二步 TCP 恒挂、
 *         ARP 表里板子是 Unreachable）⇒ 走 WiFi 也传不了；
 *   剩下唯一的路就是这根 USB 线，所以必须让它能搬二进制。
 *
 *   st_put 是【按行】写的协议：每行 fputs + 自动补 LF —— 拿它传 mp3
 *   会被插入 LF、并且 0x0A/0x0D 字节会被当成行结束，数据必坏。
 *   再包一层 base64 又白白多 33% 的传输量（16 MB → 21 MB）。
 *   ⇒ 干脆另开一条【按字节数】的通道：不解析行、不过滤、
 *     收到 nbytes 个字节就收工，最后用 CRC32 判成败。
 *
 * ★★ 判据为什么是「字节数 + CRC32」而不是「文件建出来了」：
 *   串口是有可能丢字节的（RX 环只有 256 字节，且这台机器同时在
 *   往同一个口打日志）。少了字节也会建出一个文件 —— 看着像成功，
 *   播出来是半截噪音。CRC 对不上就【删掉 .tmp】，绝不留半成品。
 *   （铁律：读得回来的才算写成。这里等价于"校验对得上的才算写成"。）
 *
 * ★ 协议（PC 侧 tools/_sd_push_raw.py）：
 *     st_putraw lq001.mp3 161712 1a2b3c4d
 *     ← 设备打一行 STRAW_READY 表示"我在这儿等着收"
 *     → PC 灌一块（默认 4 KB）
 *     ← 设备【写进卡以后】回一个 0x06（ACK）
 *     → PC 再灌下一块 … 直到灌完
 *     ← 设备打 "st_putraw: 完成 …" 或 "校验失败 …"
 *   ⚠ 中间【绝不能】发别的行 —— 那些字节会被当成文件内容。
 *
 * ★★ 为什么一定要 ACK（10-07 实测逼出来的）：
 *   RX 环只有 256 字节，而设备读一块之后要【写 SD 卡】（几毫秒到几十
 *   毫秒）。没有流控时 PC 在这期间照发不误 —— 环一满就【静默丢字节】。
 *   实测：一个 158 KB 的 mp3 少了 1024 字节。
 *   加 ACK 之后 PC 只在收到"上一块已落盘"才发下一块，
 *   设备的读取窗口永远只有一块数据那么大，环不可能溢出。
 *   ⇒ 这不是"保险起见"，是把一个【偶发丢字节】变成【不可能丢字节】。
 */
#define ST_RAW_CHUNK 4096
#define ST_RAW_ACK   0x06
static uint8_t s_rawbuf[ST_RAW_CHUNK];

/* 标准 CRC32（多项式 0xEDB88320，初值 0xFFFFFFFF，末尾取反）
 * —— 与 Python zlib.crc32() 完全一致，PC 侧不用自己实现。
 * 逐位算不用查表：16 MB 多花不到一秒，换 1 KB 常驻 RAM 不值得。*/
static uint32_t st_crc32_inc(uint32_t crc, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return crc;
}

static void st_putraw(const char *name, long nbytes, uint32_t want_crc)
{
    char p[ST_PATH_MAX + 16], tmp[ST_PATH_MAX + 24];
    if (!app_sd_is_mounted()) { ESP_LOGW(TAG, "st_putraw: SD 没挂载"); return; }
    if (!st_path(p, sizeof(p), name)) { ESP_LOGW(TAG, "st_putraw: 文件名不合法"); return; }
    if (nbytes <= 0 || nbytes > 32 * 1024 * 1024) {
        ESP_LOGW(TAG, "st_putraw: 字节数不合法 %ld", nbytes);
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    FILE *fp = fopen(tmp, "wb");
    if (!fp) { ESP_LOGW(TAG, "st_putraw: 建不了 %s", tmp); return; }

    /* ★★★ 关键一步：先把行尾残留的 CR/LF 吃掉，再声明 READY。
     *
     *   为什么：命令派发发生在读到【第一个】\r 时（见 st_serial_task），
     *   而 PC 发的是 "st_putraw …\r\n" ⇒ 那个 \n 还没被读走，
     *   就静静躺在 RX 环里，成为文件的【第一个字节】。
     *
     *   现象极具误导性（10-07 第一次实测就是它）：
     *     收到的字节数【一个不差】= 161712/161712，
     *     只有 CRC 对不上。若不校验内容，就会满心以为写成功了，
     *     实际整个文件后移一个字节、末尾少一个字节 —— 播出来是噪音。
     *   事后用 Python 反推印证：zlib.crc32(b"\x0a" + data[:-1]) 与
     *   设备算出的 EEF8E395 完全一致，就是这个 \n。
     *
     *   等待用 50ms 而不是 0：\n 紧跟 \r 发出，但 USB 分包有延迟，
     *   立即读可能读不到。此时 PC 还【没开始灌内容】（它在等
     *   STRAW_READY，而那行在本函数下面才打）⇒ 这 50ms 里除了
     *   换行不可能读到别的字节，不会误吃内容。*/
    for (int k = 0; k < 4; k++) {
        uint8_t junk;
        int r = usb_serial_jtag_read_bytes(&junk, 1, pdMS_TO_TICKS(50));
        if (r != 1) break;                       /* 安静了，可以开始收 */
        if (junk == '\r' || junk == '\n') continue;
        /* 读到了非换行字节：只可能是 PC 违反协议提前开灌。
         * 不猜、不凑 —— 直接作废重来。硬凑出来的文件是最坏的结果：
         * 大小可能还对，内容已经错位。*/
        ESP_LOGE(TAG, "st_putraw: PC 提前开灌（多余字节 0x%02X），本次作废", junk);
        fclose(fp);
        remove(tmp);
        return;
    }

    /* ★ PC 侧必须等到这一行才开始灌字节，否则前面那些字节会在
     *   命令还没解析完的时候就涌进 RX 环，被当命令丢掉。*/
    ESP_LOGW(TAG, "STRAW_READY %s %ld", name, nbytes);

    uint32_t crc = 0xFFFFFFFFu;
    long got = 0;
    int stalls = 0;
    while (got < nbytes) {
        size_t want = (size_t)(nbytes - got);
        if (want > ST_RAW_CHUNK) want = ST_RAW_CHUNK;
        size_t rd = 0;
        while (rd < want) {
            int r = usb_serial_jtag_read_bytes(s_rawbuf + rd, (uint32_t)(want - rd),
                                               pdMS_TO_TICKS(1000));
            if (r <= 0) break;
            rd += (size_t)r;
        }
        if (rd == 0) {                       /* 断流：PC 那边掉线/没在发 */
            if (++stalls >= 2) break;
            continue;
        }
        stalls = 0;
        crc = st_crc32_inc(crc, s_rawbuf, rd);
        if (fwrite(s_rawbuf, 1, rd, fp) != rd) {
            ESP_LOGE(TAG, "st_putraw: 写卡失败（卡满了？）已写 %ld 字节", got);
            fclose(fp);
            remove(tmp);
            return;
        }
        got += (long)rd;

        /* ★ 流控：这一块已经【落盘】了才回 ACK 让 PC 发下一块。
         *   顺序不能颠倒 —— 先 ack 后 fwrite 就等于没流控。*/
        uint8_t ack = ST_RAW_ACK;
        usb_serial_jtag_write_bytes(&ack, 1, pdMS_TO_TICKS(1000));
    }
    crc = ~crc;
    fflush(fp);
    fclose(fp);

    if (got != nbytes || crc != want_crc) {
        ESP_LOGE(TAG, "st_putraw: 校验失败 —— 收到 %ld/%ld 字节, CRC %08X/%08X，已删 .tmp",
                 got, nbytes, (unsigned)crc, (unsigned)want_crc);
        remove(tmp);
        return;
    }
    /* ★★ 目标文件已存在时必须【先删掉】再改名。
     *   FatFs 的 f_rename 不覆盖已存在的目标（返回 FR_EXIST），
     *   于是：第一次推成功，第二次起每次都在最后一步失败。
     *   而且失败发生在【校验通过之后】—— 日志看着像"传输出错"，
     *   实际数据一个字节都没错。10-07 被这个坑掉了一轮排查。
     *   ⚠ st_put_end() 那条文本通道有同样的问题，已经一起修。*/
    remove(p);
    if (rename(tmp, p) != 0) {
        ESP_LOGE(TAG, "st_putraw: 改名失败 %s -> %s（目标已删除还是失败？卡只读？）", tmp, p);
        return;
    }
    ESP_LOGW(TAG, "st_putraw: 完成 %s %ld 字节 CRC OK %08X", name, got, (unsigned)crc);
}

/* ---- lq_shot ：把当前屏幕整帧（320x480 RGB565）从串口吐给 PC ----
 * 协议（PC 侧 tools/_lq_shot.py）：
 *   发  "lq_shot\r\n"
 *   ←   "SHOT 320 480 307200 <crc32>\r\n"   纯 ASCII 行，PC 靠它定位
 *   ←   307200 个原始字节（LVGL 行主序 RGB565，小端）
 *   ←   "\r\nSHOT_END\r\n"
 *
 * ★ 方向与 st_putraw 相反：那条是"PC 发设备收"，这条是"设备发 PC 收"。
 * ★ 为什么要这个东西：改 UI 时若只能靠人说"哪儿不好看"，一轮就得
 *   烧一次固件、等一次反馈。有了截图，改完自己先看一眼 ——
 *   把"明显不对"的那几轮挡在自己这边，别拿人去当验证工具。
 * ★ 发送期间不能插日志：控制台与这条二进制走的是同一个 USB-JTAG，
 *   插进去一行，PC 收到的那一帧就整体错位（表现为颜色全乱）。
 */
static void st_shot(void)
{
    const int W = 320, H = 480;
    const int nbytes = W * H * 2;
    static uint8_t *buf = NULL;

    if (buf == NULL) {
        buf = heap_caps_malloc((size_t)nbytes, MALLOC_CAP_SPIRAM);
        if (buf == NULL) {
            ESP_LOGE(TAG, "lq_shot: PSRAM 分配 %d B 失败", nbytes);
            return;
        }
    }
    /* ★★★ 10-08：取快照前先【强制同步重绘一帧】。
     *   为什么（这次被它坑掉整整一轮排查）：
     *     · 本工程显示是 FULL 模式 + 【两块】整屏缓冲，LVGL 每帧在
     *       s_buf1 / s_buf2 之间轮换；
     *     · 而 app_display_snapshot() 只拷 s_buf1；
     *     · 于是"截到哪一帧"取决于"发 lq_shot 那一刻 LVGL 最近一次渲染
     *       写的是哪块缓冲"。
     *   症状：同一个界面，有时截得到、有时截到的是【上一屏】——
     *     实测「刚用 ui_demo cfg 打开设置面板，截出来却是主菜单，而且
     *     CRC 与主菜单那一张逐字节相同」。看图会得出"面板根本没建"的
     *     结论，于是去查对象树、查父对象是不是活动屏、查 HIDDEN 标志，
     *     全是白费（这些都已经被 ui_demo dump 证明是对的）。
     *   ⇒ 教训：**"看起来没生效"要先怀疑观测手段，再怀疑功能。**
     *      截图工具本身也会骗人，而且骗得比功能更隐蔽。
     *
     *   修法：在 LVGL 锁里 invalidate 整屏 + lv_refr_now() 同步渲染一次，
     *   这样 s_buf1 必定是【本命令发出瞬间】的真实画面，与定时器、
     *   与哪块缓冲、与上一帧有没有脏区都无关。*/
    bool snap_ok = false;
    if (lvgl_port_lock(0)) {
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(NULL);
        snap_ok = app_display_snapshot(buf);
        lvgl_port_unlock();
    } else {
        ESP_LOGW(TAG, "lq_shot: 拿不到 LVGL 锁，跳过这次截图");
    }
    if (!snap_ok) {
        ESP_LOGE(TAG, "lq_shot: 还没出过帧（display 还没 ready？）");
        return;
    }

    uint32_t crc = ~st_crc32_inc(0xFFFFFFFFu, buf, (size_t)nbytes);
    char hdr[80];
    int n = snprintf(hdr, sizeof(hdr), "\r\nSHOT %d %d %d %08x\r\n",
                     W, H, nbytes, (unsigned)crc);
    usb_serial_jtag_write_bytes(hdr, (size_t)n, pdMS_TO_TICKS(2000));

    int off = 0, stalls = 0;
    /* ★★ 块大小不是随便定的：USB Serial/JTAG 的 TX 环形缓冲默认只有 64 B，
     *   usb_serial_jtag_write_bytes() 在"装不下"时【直接返回 0】，不阻塞、
     *   也不报错。第一版按 2048 发，结果整帧实发 0 字节 —— 而头尾两行
     *   （都 <64 B）照常送达，于是 PC 侧看到的是"头有、尾有、正文全无"。
     *   ⇒ 块必须 ≤ TX 缓冲；留余量取 48。
     *   另：每次发送后要读一次 RX 丢掉回显，否则 PC 写入的命令字节会
     *   在环里堆积（这条通道是命令通道，回显会污染下一次 lq_shot）。*/
    while (off < nbytes) {
        int chunk = nbytes - off;
        if (chunk > 48) chunk = 48;
        int w = usb_serial_jtag_write_bytes(buf + off, (size_t)chunk,
                                            pdMS_TO_TICKS(200));
        if (w <= 0) {
            if (++stalls >= 40) break;      /* 约 8 秒还没进展 ⇒ 放弃 */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        stalls = 0;
        off += w;
        uint8_t junk[64];
        usb_serial_jtag_read_bytes(junk, sizeof(junk), 0);   /* 非阻塞，丢回显 */
    }
    usb_serial_jtag_write_bytes("\r\nSHOT_END\r\n", 13, pdMS_TO_TICKS(2000));
    ESP_LOGW(TAG, "lq_shot: 整帧 %d B 已发（实发 %d）CRC %08x",
             nbytes, off, (unsigned)crc);
}

/* ---- st_net <url>[?insecure] ：分层测网络（10-06）----
 * 把「连不上」拆成四层，每层单独报，才能知道该修哪：
 *   ① DNS   —— getaddrinfo 解析出 IP
 *   ② TCP   —— socket + connect 裸握手，打印 errno / 目标 IP / 耗时
 *   ③ HTTP  —— esp_http_client_open + fetch_headers，打印状态码
 *   ④ 内容  —— 拉第一行（正常时应该是 #EXTM3U）
 *
 * ★ 实测结论（10-06 台单播不出声的现场）：
 *   https 源：① 通 ② 通（68 ms）③ 失败 TLS err=0x008D
 *   http  源：① 通 ② 通（17 ms）③ 200 ④ #EXTM3U
 *   ⇒ 断点在 TLS 这一层，DNS 与 TCP 都没问题，源也没死。
 *
 * ★ 加 ?insecure 后缀可关掉证书校验再试一次，这是区分
 *   「内存不够」与「证书链不认」的唯一办法 —— 两者都走 HTTPS、
 *   都吃内部 RAM，光看错误码分不出来。
 *
 * ★★★ 10-06 隐私审计：`?insecure` 在【发布版里编译期关掉】
 *   （XS_FAV_PUBLISH=1）。理由：
 *     它是全工程唯一一条能人为关闭 TLS 加密的代码路径。虽然要物理
 *     串口输入才触发、不是自动行为，但「发布版本里带一个关证书校验的
 *     开关」在任何合规审查里都是扣分项 —— 而它的唯一用途是【排障】，
 *     排障只需要自用版。这是不损失任何发布功能的纯减法。
 *   ⇒ 使用者拿到的是发布版时，`st_net <地址>?insecure` 会被忽略并打
 *     一行提示，要用这个开关请自编译（README 已说明）。
 */
static void st_net_probe(const char *url_in)
{
/*★★ 10-06 踩坑：这几个数组原本是函数里的局部变量，结果
 *   ***ERROR*** A stack overflow in task xs_stcmd has been detected.
 *   板子直接复位 —— 而且因为复位发生在 printf 中间，串口日志被截成半行
 *   （「=== 分层测网络: https://<某个流地址>」），看着像丢日志，
 *   实际是崩了。**教训：加了大的局部数组后，必须同步调大任务栈。**
     *   这里选【static】而不是把栈调大：串口任务只在有人敲命令时跑，
     *   320×2 字节常驻内部 RAM 无所谓；而调栈会影响常驻内存，
     *   在只剩 23~30 KB 内部 RAM 的机器上是更坏的选择。
     *   ⚠ 这几个变量绝不能递归/重入使用 —— 目前只在这一个任务里用，安全。*/
    static char url[320];
    static char clean[320];
    static char host[128];

    /* 先把 ?insecure 后缀摘掉，剩下才是真正的 URL */
    snprintf(url, sizeof(url), "%.300s", url_in ? url_in : "");
    char *qmark = strchr(url, '?');
    if (qmark) *qmark = '\0';

    ESP_LOGW(TAG, "=== 分层测网络: %s ===", url);

    /* ⚠ url 已经是上面的局部数组（不再是入参指针），所以不能判 !url，
     *   判了 gcc 会报 "address of 'url' will always evaluate as true"。
     *   入参为空的情形在 snprintf 那一步就把数组填成空串了。*/
    if (url[0] == '\0' || strncmp(url, "http", 4) != 0) {
        ESP_LOGE(TAG, "不是 http(s) 开头的地址");
        return;
    }
    int is_https = (strncmp(url, "https", 5) == 0);
    int port = is_https ? 443 : 80;

    /* 拆出 host 与 path */
    const char *hs = strstr(url, "://");
    hs = hs ? hs + 3 : url;
    const char *he = strpbrk(hs, "/?#");
    size_t hn = he ? (size_t)(he - hs) : strlen(hs);
    if (hn >= sizeof(host)) hn = sizeof(host) - 1;
    memcpy(host, hs, hn);
    host[hn] = '\0';
    /* 带端口的写法 a.b:80 */
    char *colon = strrchr(host, ':');
    if (colon) { *colon = '\0'; port = atoi(colon + 1); }

    ESP_LOGW(TAG, "① host = %s  port = %d (%s)", host, port, is_https ? "TLS" : "明文");
    ESP_LOGW(TAG, "   内存起点：internal free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    /* ① DNS */
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0 || !res) {
        /* ★ 不用 gai_strerror：新版 lwIP 的 netdb.h 不导出它，
         *   用了会报 implicit declaration（-Werror 直接挂）。
         *   打错误码 + 主机名就够了，读者要的是「解析不出」这件事。*/
        ESP_LOGE(TAG, "① DNS 失败 rc=%d —— 机器解析不了 %s", rc, host);
        return;
    }
    struct in_addr ip4;
    memcpy(&ip4, &((struct sockaddr_in *)res->ai_addr)->sin_addr, sizeof(ip4));
    ESP_LOGW(TAG, "① DNS OK → %d.%d.%d.%d",
             (int)((uint8_t *)&ip4)[0], (int)((uint8_t *)&ip4)[1],
             (int)((uint8_t *)&ip4)[2], (int)((uint8_t *)&ip4)[3]);
    freeaddrinfo(res);

    /* ② TCP：这一步是判定重点。esp_http_client 只会笼统报
     *   ESP_ERR_HTTP_CONNECT，分不出是解析失败还是握手被拒。*/
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) { ESP_LOGE(TAG, "② socket() 失败 errno=%d", errno); return; }
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    sa.sin_addr   = ip4;
    int64_t t0 = esp_timer_get_time();
    int cr = connect(s, (struct sockaddr *)&sa, sizeof(sa));
    int64_t dt = (esp_timer_get_time() - t0) / 1000;
    if (cr == 0) {
        ESP_LOGW(TAG, "② TCP OK —— 握手成功，%lld ms", (long long)dt);
    } else {
        ESP_LOGE(TAG, "② TCP 失败 errno=%d (%s) 目标 %s:%d 耗时 %lld ms",
                 errno, strerror(errno), host, port, (long long)dt);
        close(s);
        return;      /* 握手都过不去，后面几层不用试了 */
    }
    close(s);

    /* ③+④ HTTP：走 esp_http_client，状态码与 m3u8 首行一并打出来。
     * 故意用最简配置（带 CA 证书包），和 hls_fetch_list 保持一致，
     * 免得「这里通、那里不通」是配置差异造成的假象。
     *
     * ★★ st_net insecure：URL 带 ?insecure 就关掉证书校验再试一次。
     *   这是【区分「内存不够」与「证书链不认」的唯一办法】：
     *   ① 证书校验要额外分配 X509 解析缓冲，很吃内部 RAM；
     *   ② 校验失败返回的是 X509 段错误码（0x2xxx/0x3xxx）。
     *   两个都走 HTTPS、都吃 RAM，错误码不同，靠猜分不出来 ——
     *   只能让机器自己跑一遍两种配置对比。*/
    /* ★ url 顶层的 ?insecure 已经在函数开头摘掉了（连同 query 一起），
     *   所以这里只要判断标记还在不在原始入参里。
     *   ⚠ clean 必须开 320：url 是 320 字节数组，用 256 会被
     *   -Werror=format-truncation 拦下（319 > 256）。*/
    /* ★ 发布版（XS_FAV_PUBLISH=1）这个开关在【编译期就不存在】——
       隐私审计要求，见上面函数头注释。*/
#if XS_FAV_PUBLISH
    bool insecure = false;
    if (url_in && strstr(url_in, "?insecure"))
        ESP_LOGW(TAG, "   发布版不支持 ?insecure（关证书校验的开关只留自用版）");
#else
    bool insecure = (strstr(url_in ? url_in : "", "?insecure") != NULL);
#endif
    snprintf(clean, sizeof(clean), "%.300s", url);
    char *q = strchr(clean, '?');
    if (q) *q = '\0';

    esp_http_client_config_t cfg = {
        .url = clean,
        .timeout_ms = 8000,
        .buffer_size = 1024,
        .crt_bundle_attach = insecure ? NULL : esp_crt_bundle_attach,
    };
    /* insecure 时用 mbedtls 自带的 skip_verify（不挂证书包即可）*/
    if (insecure) {
        ESP_LOGW(TAG, "   ⚠ 已关掉证书校验（仅用于定位问题，不可用于发布）");
        cfg.crt_bundle_attach = NULL;
    }
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { ESP_LOGE(TAG, "③ http 句柄建不了（可能是内部 RAM 不够）"); return; }
    esp_http_client_set_header(c, "User-Agent", "Mozilla/5.0 (XianDial)");
    esp_err_t oe = esp_http_client_open(c, 0);
    if (oe != ESP_OK) {
        ESP_LOGE(TAG, "③ open 失败 err=%d (%s)", (int)oe, esp_err_to_name(oe));
        if (is_https) {
            int tc = 0, tf = 0;
            esp_http_client_get_and_clear_last_tls_error(c, &tc, &tf);
            ESP_LOGE(TAG, "   TLS err=0x%04X flags=0x%04X", (unsigned)tc, (unsigned)tf);
            ESP_LOGE(TAG, "   内部 RAM 此刻 free=%u largest=%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            if (insecure)
                ESP_LOGE(TAG, "   ⇒ 不挂证书包也失败 ⇒ 不是证书链问题，是内存/协议问题");
            else
                ESP_LOGE(TAG, "   ⇒ 用 st_net <同一个地址>?insecure 再试一次即可定性");
        }
        esp_http_client_cleanup(c);
        return;
    }
    int total = esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    ESP_LOGW(TAG, "③ HTTP 状态 = %d（fetch_headers 返回 %d）", status, total);
    if (status == 200 || status == 206) {
        char buf[256];
        int n = esp_http_client_read(c, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            char *nl = strpbrk(buf, "\r\n");
            if (nl) *nl = '\0';
            ESP_LOGW(TAG, "④ 首行 = %s", buf);
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    ESP_LOGW(TAG, "=== 测完 ===");
}

/* ---- lq_mem：内存体检（10-08）------------------------------
 * 为「灵签 + 天气 + 股票 三合一」打地基。三合一要新增网络栈与页面，
 * 而运行时内部 RAM 的【最大连续块】只剩几 KB —— TLS 握手要 16~40 KB
 * 连续内存，所以这本账必须算到字节。
 *
 * ★ 只读、不分配，可作为长期监控命令反复跑。
 * ★ 为什么连编译期配置一起打：以后改了 CONFIG_* 要能一眼对照同一行看
 *   「腾出了多少」，否则全靠记忆（铁律 42/44：判据要在真产物上验）。
 *   —— 实测基线（10-08 未优化时）：INTERNAL free≈19.5 KB / largest≈7.7 KB。
 */
static void lq_mem_dump(void)
{
    static const struct { const char *name; uint32_t cap; } H[] = {
        { "INTERNAL", MALLOC_CAP_INTERNAL },
        { "DMA",      MALLOC_CAP_DMA      },
        { "SPIRAM",   MALLOC_CAP_SPIRAM   },
    };

    ESP_LOGW(TAG, "=== 内存体检 ===");
    for (size_t i = 0; i < sizeof(H) / sizeof(H[0]); i++) {
        ESP_LOGW(TAG, "  %-8s free=%7u  largest=%7u  min=%7u",
                 H[i].name,
                 (unsigned)heap_caps_get_free_size(H[i].cap),
                 (unsigned)heap_caps_get_largest_free_block(H[i].cap),
                 (unsigned)heap_caps_get_minimum_free_size(H[i].cap));
    }

    /* LVGL 对象池：内部 RAM 里最大的那块【静态】占用（CONFIG_LV_MEM_SIZE）。
     * 池见底 ⇒ lv_malloc 返回 NULL，而 release 构建下 LV_ASSERT_MALLOC 是
     * 空操作 ⇒ 往 NULL 写 ⇒ 无限重启。所以 used%/峰值必须一直看得见。*/
    lv_mem_monitor_t m;
    memset(&m, 0, sizeof(m));
    bool got = false;
    if (lvgl_port_lock(0)) {
        lv_mem_monitor(&m);
        lvgl_port_unlock();
        got = true;
    }
    if (got) {
        ESP_LOGW(TAG, "  LVGL池   size=%u  free=%u  used=%u%%  frag=%u%%  峰值=%u",
                 (unsigned)m.total_size, (unsigned)m.free_size,
                 (unsigned)m.used_pct, (unsigned)m.frag_pct,
                 (unsigned)m.max_used);
    } else {
        ESP_LOGW(TAG, "  LVGL池   拿不到 LVGL 锁，跳过");
    }

    /* ★★★ 单位是【字节】，不是 word —— 这条判据 10-08 才纠正过来。
     *   原来这里写的是「（FreeRTOS 单位 word）」，那是查 FreeRTOS 手册
     *   得来的印象；但 **ESP-IDF 把 portSTACK_TYPE 定义成 uint8_t**，
     *   于是"栈里还剩多少个 StackType_t"= 还剩多少个字节。
     *   ⇒ 判据写错的代价：本任务栈 8192 时若按 word 读，1840 会被当成
     *     "只剩 1.8 KB" 而白白恐慌，或者反过来把 6304 当成"只用了 1.8 KB"
     *     而误判余量充足。数字一样，结论相反 —— 和"码率 vs 下载速率"同类坑。
     *   自检办法：数值必须 ≤ 任务栈字节数（xTaskCreate 的第三个参数）。*/
    ESP_LOGW(TAG, "  本任务栈余量=%u 字节（高水位；栈共 %d 字节，越小越险）",
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             ST_CMD_STACK);

    ESP_LOGW(TAG, "  编译期配置 ALWAYSINTERNAL=%d  RESERVE_INTERNAL=%d  LV_MEM_SIZE=%d",
             (int)CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL,
             (int)CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL,
             (int)CONFIG_LV_MEM_SIZE);

    /* 每个 heap region 的 free / allocated / largest 分桶明细 */
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    ESP_LOGW(TAG, "=== 体检完 ===");
}

static void st_serial_task(void *arg)
{
    char    line[APP_ST_MAX_LINE];
    size_t  n = 0;
    uint8_t ch;
    FILE   *wfp = NULL;            /* 非空 = 正在写入模式 */

    ESP_LOGI(TAG, "串口通道就绪：st_dump / st_reload / st_ls / st_cat <f> / "
                  "st_put <f> (结束=单行.) / st_putraw <f> <字节> <CRC32> / "
                  "st_rm <f> / st_play <下标> / lq_play <n> / lq_shot / "
                  "lq_demo <page|set> [签号] / lq_mem / "
                  "app_open <n> / app_home / radio <view|tap|scroll|filter|cattab|sd> / "
                  "stk_kline <idx> / wifiscan / "
                  "ui_demo <cfg|cfg <y>|cfgclose|prov|provclose|dump|speak|add|power>");

    /* ★ 驱动已被控制台装好了（usb_serial_jtag_vfs_dev_port_init 在启动时
     *   调了 driver_install + vfs_use_driver），这里只管读。
     *   万一没装上就自己装一次，失败就退出任务，不能让机器重启。*/
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        cfg.rx_buffer_size = 1024;
        if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
            ESP_LOGE(TAG, "USB-JTAG 驱动装不上，热重载通道关闭（不影响收音）");
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGW(TAG, "USB-JTAG 驱动由本任务补装");
    }

    for (;;) {
        if (usb_serial_jtag_read_bytes(&ch, 1, portMAX_DELAY) != 1)
            continue;
        if (ch == '\r' || ch == '\n') {
            line[n] = '\0';
            char *t = trim(line);
            if (*t) {
                if (wfp) {                          /* 写入模式：吃掉一切 */
                    if (strcmp(t, ".") == 0) { st_put_end(wfp, s_wfp_name); wfp = NULL; }
                    else                            st_put_line(wfp, t);
                } else if (strcmp(t, "st_reload") == 0) {
                    ESP_LOGW(TAG, "收到 st_reload —— 重新读卡");
                    free_all();
                    s_ncache_n = 0;
                    app_st_src_t r = load_tsv();
                    ESP_LOGI(TAG, "重读完成：来源=%s 台数=%d",
                             r == APP_ST_SRC_TSV ? "TSV" : "内置", s_count);
                    app_st_dump_info();
                    /* UI 需要重建才会看到新台单 */
                    ui_reload_stations();
                } else if (strcmp(t, "st_dump") == 0) {
                    app_st_dump_info();
                } else if (strcmp(t, "st_ls") == 0) {
                    st_ls();
                } else if (strncmp(t, "st_play", 7) == 0 && t[7] == ' ') {
                    int idx = atoi(trim(t + 8));
                    if (idx < 0 || idx >= s_count) {
                        ESP_LOGW(TAG, "st_play: 下标 %d 越界（共 %d 台）", idx, s_count);
                    } else {
                        ESP_LOGW(TAG, "st_play: 播第 %d 台 = %s", idx, s_pool[idx].name);
                        ESP_LOGW(TAG, "st_play: url = %s", s_pool[idx].url);
                        esp_err_t r = app_radio_play_url(s_pool[idx].name, s_pool[idx].url);
                        ESP_LOGW(TAG, "st_play: 返回 %s，3 秒后看错误与状态",
                                 esp_err_to_name(r));
                    }
                } else if (strncmp(t, "st_cat", 6) == 0 && t[6] == ' ') {
                    st_cat(trim(t + 7));
                } else if (strncmp(t, "st_rm", 5) == 0 && t[5] == ' ') {
                    st_rm(trim(t + 6));
                } else if (strncmp(t, "st_putraw", 9) == 0 && t[9] == ' ') {
                    /* ★ 必须排在 st_put 之前判（虽然 t[6] 一个是 'r' 一个是 ' '，
                     *   本来就不会撞，但显式放在前面更经得起以后改命令名）*/
                    char nm[ST_PATH_MAX];
                    long nb = 0;
                    unsigned crc = 0;
                    if (sscanf(t + 10, "%63s %ld %x", nm, &nb, &crc) == 3) {
                        st_putraw(nm, nb, (uint32_t)crc);
                    } else {
                        ESP_LOGW(TAG, "st_putraw: 用法 st_putraw <文件名> <字节数> <CRC32>");
                    }
                } else if (strncmp(t, "st_put", 6) == 0 && t[6] == ' ') {
                    char *nm = trim(t + 7);
                    snprintf(s_wfp_name, sizeof(s_wfp_name), "%s", nm);
                    wfp = NULL;
                    if (st_put_begin(nm, &wfp)) { /* 已进入写入模式 */ }
                } else if (strncmp(t, "app_open", 8) == 0 &&
                           (t[8] == ' ' || t[8] == 0)) {
                    /* ★ 10-08：直接进第 n 个产品。
                     *   ⚠️ 索引 = ui_shell.c 里 APPS[] 的【数组下标】，不是固定值：
                     *      10-08 晚「拾声电台」被置顶到第 0 位之后，
                     *      全表变成 0=拾声电台 1=观音灵签 2=天气时钟 3=股票行情。
                     *      后续再调顺序，这里的注释【必须】跟着改 ——
                     *      旧注释写成"0=灵签"，照着它调会打到别的产品上。
                     *   为什么需要它：开机停在主菜单，而这板子【没有物理手指】——
                     *   要验证"主菜单 → 产品"这条跳转只能靠命令，
                     *   否则每轮改完都得找人点一下屏幕，还转述不准。
                     *   ★ 必须持 LVGL 锁：产品 enter() 会整屏重建。
                     *   ★ 无参数（t[8]==0）时 atoi("") = 0 ⇒ 进拾声电台。*/
                    int n = atoi(t + 8);
                    if (lvgl_port_lock(0)) {
                        ui_shell_launch(n);
                        lvgl_port_unlock();
                    } else {
                        ESP_LOGW(TAG, "app_open: 拿不到 LVGL 锁");
                    }
                } else if (strcmp(t, "app_home") == 0) {
                    /* 回主菜单：等价于点产品设置面板里的「主 页」。*/
                    if (lvgl_port_lock(0)) {
                        ui_shell_back();
                        lvgl_port_unlock();
                    } else {
                        ESP_LOGW(TAG, "app_home: 拿不到 LVGL 锁");
                    }
                } else if (strncmp(t, "radio", 5) == 0 &&
                           (t[5] == ' ' || t[5] == 0)) {
                    /* ★★ 10-08：竖版电台 App 的调试口。
                     *   为什么必须有它：这块板子【没有物理手指】，串口也
                     *   注入不了触摸 —— 而分类筛选、列表滚动（行对象复用
                     *   是这一屏最容易写错的一段）、本地音频点播，
                     *   全都只能靠"点屏幕"触发。没有这条命令，那些改动
                     *   就只能靠人点 + 转述，而转述恰恰最不靠谱。
                     *   ★ 全部走的是与手指完全相同的回调路径，不是另抄
                     *     一份动作序列（抄一份只能证明"抄的那份对"）。
                     *   ★ 必须持 LVGL 锁：会改对象树。
                     *   ★ 没参数时只打用法，不动界面。*/
                    char sub[16] = {0};
                    char arg[256] = {0};
                    int  a = -1, b = -1;
                    /* ⚠ 偏移必须是 +5 不是 +6：t 正好是 "radio"（长度 5）时，
                     *   t+6 已经跨过结尾的 '\0' 了 —— 那是越界读，
                     *   而 sscanf 会跳过前导空白，所以 +5 一样好使。*/
                    if (sscanf(t + 5, "%15s", sub) != 1) {
                        ESP_LOGW(TAG,
                            "radio 用法：view <0列表1播放2分类3本地> | "
                            "tap <n> | scroll <y> | filter <栏目> <地区> | "
                            "cattab <0栏目|1地区> | sd [路径]");
                    } else if (strcmp(sub, "sd") == 0) {
                        sscanf(t + 5, "%*s %255s", arg);
                        if (lvgl_port_lock(0)) {
                            ui_radio_v_demo_sd(arg);
                            lvgl_port_unlock();
                        } else {
                            ESP_LOGW(TAG, "radio sd: 拿不到 LVGL 锁");
                        }
                    } else if (sscanf(t + 5, "%*s %d %d", &a, &b) >= 1) {
                        if (lvgl_port_lock(0)) {
                            if      (strcmp(sub, "view")   == 0) ui_radio_v_demo_view(a);
                            else if (strcmp(sub, "tap")    == 0) ui_radio_v_demo_tap(a);
                            else if (strcmp(sub, "scroll") == 0) ui_radio_v_demo_scroll(a);
                            else if (strcmp(sub, "filter") == 0) ui_radio_v_demo_filter(a, b);
                            else if (strcmp(sub, "cattab") == 0) ui_radio_v_demo_cat_tab(a);
                            else ESP_LOGW(TAG, "radio: 不认识的子命令 %s", sub);
                            lvgl_port_unlock();
                        } else {
                            ESP_LOGW(TAG, "radio %s: 拿不到 LVGL 锁", sub);
                        }
                    } else {
                        ESP_LOGW(TAG, "radio: %s 的参数不对", sub);
                    }
                } else if (strncmp(t, "stk_kline", 9) == 0 &&
                           (t[9] == ' ' || t[9] == 0)) {
                    /* ★ 10-08：直接落到「第 idx 个自选的日K屏」。
                     *   为什么必须有它：这块板子【没有物理手指】——
                     *   K线屏原本只能靠"点列表里的卡片"进入，而串口没法注入
                     *   触摸。没有这条命令，K线画得对不对就【只能靠猜】，
                     *   而 K线是这次改动里对象最多、最容易画歪的一屏。
                     *   与 lq_demo 同一个套路（那边是灵签的跳页命令）。
                     *   ⚠ 前置：先 app_open 3 进股票（否则 s_code[] 还是空的）。
                     *   ★ 必须持 LVGL 锁：会重建整屏 + lv_screen_load。*/
                    int n = atoi(t + 9);
                    if (lvgl_port_lock(0)) {
                        ui_stock_open_kline(n);
                        lvgl_port_unlock();
                    } else {
                        ESP_LOGW(TAG, "stk_kline: 拿不到 LVGL 锁");
                    }
                } else if (strncmp(t, "ui_demo", 7) == 0 &&
                           (t[7] == ' ' || t[7] == 0)) {
                    /* ★ 10-08：把"只能靠手指点的三个新界面"开出一条命令口。
                     *   起因：这次加了 ①首页设置面板 ②天气页播报 ③股票添加面板，
                     *   三者的入口都是按钮，而这块板【没有物理手指】——
                     *   串口也注入不了触摸。没有这条命令，这三个新面板
                     *   【就只能靠读代码相信它对】，而排版/豆腐字/叠字
                     *   恰恰是读代码看不出来的那类问题。
                     *   ★ 走的是和按钮【同一个回调】，不是另抄一份动作序列
                     *     （抄一份只能证明"抄的那份对"）。
                     *   ★ 三者都必须在 LVGL 锁内调用（会建对象）。*/
                    char what[16] = {0};
                    int arg = 0;
                    if (sscanf(t + 7, "%15s %d", what, &arg) >= 1) {
                        if (lvgl_port_lock(0)) {
                            if (strcmp(what, "cfg") == 0) {
                                if (arg > 0) {
                                    /* ui_demo cfg <y>：把面板内容层滚到 y。
                                     * 用于看【下半屏】的设备信息（板子没有手指）。*/
                                    ui_shell_demo_cfg_scroll(arg);
                                } else {
                                    ui_shell_demo_cfg();        /* 需先 app_home */
                                }
                            } else if (strcmp(what, "cfgclose") == 0)
                                ui_shell_demo_cfg_close();    /* 代按 ✕ */
                            else if (strcmp(what, "dump") == 0)
                                ui_shell_demo_dump();        /* 只读，不改状态 */
                            else if (strcmp(what, "prov") == 0)
                                ui_shell_demo_prov();       /* 需先 ui_demo cfg */
                            else if (strcmp(what, "provclose") == 0)
                                ui_shell_demo_prov_close(); /* 代按引导层底部按钮 */
                            else if (strcmp(what, "speak") == 0)
                                ui_weather_demo_speak();    /* 需先 app_open 2（见 APPS 表）*/
                            else if (strcmp(what, "add") == 0)
                                ui_stock_demo_add();        /* 需先 app_open 3（见 APPS 表）*/
                            else if (strcmp(what, "power") == 0)
                                ui_shell_demo_power();      /* ★ 只「上膛」，不真关 */
                            else
                                ESP_LOGW(TAG, "ui_demo: 未知的 %s", what);
                            lvgl_port_unlock();
                        } else {
                            ESP_LOGW(TAG, "ui_demo: 拿不到 LVGL 锁");
                        }
                    } else {
                        ESP_LOGW(TAG, "ui_demo: 用法 ui_demo <cfg|cfg <y>|cfgclose|prov|provclose|dump|speak|add|power>");
                    }
                } else if (strcmp(t, "wifiscan") == 0) {
                    /* ★ 10-09：把配网页「WiFi 名称」下拉的数据源打出来。
                     *   起因：兰兰报「配网 WiFi 连接后，下拉是空的」——
                     *   下拉读的是 s_aps[]，而填它的路径从未被调用过。
                     *   这条命令【不持 LVGL 锁】（里面要等扫描数秒），
                     *   所以单独成一条，不塞进 ui_demo（那个在锁内跑）。*/
                    app_prov_scan_dump();
                } else if (strcmp(t, "lq_shot") == 0) {
                    /* ★ 10-07：把当前屏幕整帧吐给 PC（见 st_shot 的注释）*/
                    st_shot();
                } else if (strncmp(t, "lq_demo", 7) == 0 &&
                           (t[7] == ' ' || t[7] == 0)) {
                    /* ★ 10-07：跳页 + 截图，两件套。
                     *   lq_demo <page|set> [签号] → 直接落到那一页，
                     *   随后 lq_shot 就能看到那页长什么样。
                     *   ★ 必须持 LVGL 锁：ui_lingqian_demo() 会重建整个页面。*/
                    char pg[24];
                    int pk = 0;
                    int nf = sscanf(t + 7, "%23s %d", pg, &pk);
                    if (nf >= 1) {
                        if (lvgl_port_lock(0)) {
                            ui_lingqian_demo(pg, pk);
                            lvgl_port_unlock();
                        } else {
                            ESP_LOGW(TAG, "lq_demo: 拿不到 LVGL 锁");
                        }
                    } else {
                            ESP_LOGW(TAG, "lq_demo: 用法 lq_demo <cover|sutra|wish|draw|poem|read|set> [签号]");
                    }
                } else if (strcmp(t, "lq_mem") == 0) {
                    /* ★ 10-08：内存体检（只读）。见 lq_mem_dump 的头注释。*/
                    lq_mem_dump();
                } else if (strncmp(t, "lq_play", 7) == 0 && t[7] == ' ') {
                    /* ★ 灵签专用：放 /sdcard/lqNNN.mp3。
                     *   为什么不能用 st_play —— 那条是"放台单第 N 台"，
                     *   而灵签编的是发布版台单（0 条），下标全都越界。
                     *   有了这条，不开屏幕、不用手点就能确认
                     *   「SD 上的语音到底能不能放、解码链路通不通」。
                     *   判据不只是返回值：0.8 秒后再看 is_playing 与
                     *   total_s —— total_s>0 说明 MP3 头已经被解析出来
                     *   时长，那才是"真的在放"。*/
                    int n = atoi(trim(t + 8));
                    char vp[48];
                    snprintf(vp, sizeof(vp), "/sdcard/lq%03d.mp3", n);
                    esp_err_t r = app_radio_play_file(vp);
                    vTaskDelay(pdMS_TO_TICKS(800));
                    ESP_LOGW(TAG, "lq_play: %s 返回=%s 在放=%d 总时长=%ds err=%s",
                             vp, esp_err_to_name(r), (int)app_radio_is_playing(),
                             app_radio_total_s(), app_radio_last_error());
                } else if (strncmp(t, "st_net", 6) == 0 && t[6] == ' ') {
                    /* ★ 10-06 加这条：分层测网络 —— DNS / TCP / HTTP 拆开。
                     *   起因：143 个台全部 ESP_ERR_HTTP_CONNECT（TCP 连不上），
                     *   而【同一台电脑、同一 WiFi、同一源全部 200】。
                     *   同一个源一边通一边不通 ⇒ 一定要知道断在哪一层，
                     *   否则只能猜。分层测完才知道是 DNS 解析、TCP 握手、
                     *   还是 TLS 证书 —— 三者的修法完全不同。*/
                    st_net_probe(trim(t + 7));
                } else {
                    ESP_LOGI(TAG, "未知命令「%s」", t);
                }
            }
            n = 0;
            continue;
        }
        if (ch == 0x08 || ch == 0x7F) { if (n) n--; continue; }   /* 退格 */
        if (n < sizeof(line) - 1) line[n++] = (char)ch;
    }
}

void app_st_start_serial_cmd(void)
{
    static TaskHandle_t t = NULL;
    if (t) return;
    /* ★★ 栈 3072 → 5120。10-06 实测 3072 会
     *   ***ERROR*** A stack overflow in task xs_stcmd has been detected***
     *   —— 触发者是 st_net 里 esp_http_client_open() 走 TLS 握手那条路
     *   （mbedTLS 自己还要用几百字节栈）。诊断命令把板子搞复位，
     *   一次白等 5 分钟。留足余量，别再省这点内存。
     *
     * ★★ 优先级 3 → 12（10-07 加 st_putraw 时改的，是被丢包逼出来的）：
     *   USB-Serial-JTAG 的 RX 环只有 256 字节，控制台侧写死了，
     *   没有 Kconfig 可调（查过 esp_driver_usb_serial_jtag，装驱动时
     *   用的是 USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT，rx=256）。
     *   优先级 3 时，只要 LVGL 在刷动画（首页光斑 20ms 一帧）或 FatFs
     *   在写卡，这个任务就会被抢占；抢占超过 256 字节的到达时间
     *   （约 0.2ms @12Mbps）就【静默丢字节】。
     *   实测症状：16 MB 推上去少了 1024 字节，文件照样建出来、
     *   大小还不一定对得上 —— 只有 CRC 能抓。
     *   提到 12 之后它压过 LVGL，快读快走；空闲时是 portMAX_DELAY
     *   阻塞读，不占 CPU，不会饿死别的任务。*/
    /* ★★ 栈 3072 → 5120 → 8192 → 10240：每一次都是被真机的 stack overflow
     *   或实测余量逼出来的，不是"预防性留余量"：
     *   · 10-06 3072 → 5120：爆在 st_net 走 TLS 握手（mbedTLS 自己要几百字节栈）。
     *   · 10-08 5120 → 8192：爆在 ui_demo cfg —— 这条命令会【在命令任务上
     *        直接建整套设置面板】，调用链深、建对象次数多。当时面板里还有个
     *        char b[560] 的局部缓冲 + newlib snprintf（已改成
     *        lv_label_set_text_fmt 走 LVGL 池，省掉约 3 KB 栈）。
     *   · 10-08 8192 → 10240：跑完「ui_demo cfg + ui_demo prov（起 SoftAP
     *        + httpd）」再发 lq_mem，高水位只剩 1296 字节（已用 6896/8192 = 84%）。
     *        对还在长功能的调试口太薄 —— 它崩起来是复位，现象恰好是
     *        "命令像没生效"（本轮已经在这上面白查过一圈）。加 2 KB 后余量约 3.4 KB。
     *
     *   ★ 教训一：**"能建界面"的调试命令 = 这个任务上最重的活**。
     *     以后再加同类命令，按 10240 估，别按 st_ls / lq_mem 那种轻命令估。
     *   ★ 教训二：真正吃栈的是 app_prov_start()（esp_netif + softAP +
     *     esp_http_server_start，约 4~5 KB）。**手指点「配网」时它跑在
     *     LVGL 任务（8192）上** —— 拾声早就这么用且真机验证过，所以这次
     *     没动 LVGL 任务的栈；但以后要往那条路上再加重的调用，第一件事
     *     就是先量 LVGL 任务的余量。
     *   ★ 够不够【不靠猜】：跑完 ui_demo cfg / ui_demo prov 紧接着发 lq_mem，
     *     看「本任务栈余量=NNNN 字节」那一行（高水位，越小越险）。
     *     数值必须 ≤ ST_CMD_STACK，否则就是判据写错。
     *   ★ 宏挪到文件顶部：app_st_dump_info() 在文件里【排在本函数之前】，
     *     宏写在函数体内它对上面那些函数不可见（编译直接报 undeclared）。*/
    xTaskCreate(st_serial_task, "xs_stcmd", ST_CMD_STACK, NULL, 12, &t);
}

void app_st_dump_info(void)
{
    if (s_src == APP_ST_SRC_TSV)
        ESP_LOGI(TAG, "台单源=TSV(%s) 条数=%d", APP_ST_FILE, s_count);
    else
        ESP_LOGI(TAG, "台单源=内置 条数=%d", s_count);

    /* ★ 台单构成统计（10-06 从 app_radio.c 搬来）。
     *   必须分开报 http/https：10-04 那次把 281 个 https HLS 改写成 http
     *   （省 TLS 握手堆），但文案还写着「HLS https 353」，看日志会以为没生效。
     *   ⇒ 四个数都打：直连 http / 直连 https / HLS http / HLS https。
     *   排查「某个台连不上」时先看它落在哪一格。
     *   ★ 放在这里而不是 app_radio_init() 里，是因为那时台单还没载入。*/
    {
        int n_dir_http = 0, n_dir_https = 0, n_hls_http = 0, n_hls_https = 0;
        for (int i = 0; i < s_count; i++) {
            const char *u = s_pool[i].url;
            int https = (strncmp(u, "https://", 8) == 0);
            if (strstr(u, ".m3u8")) { if (https) n_hls_https++; else n_hls_http++; }
            else                      { if (https) n_dir_https++; else n_dir_http++; }
        }
        ESP_LOGI(TAG, "台单构成: %d 台 = 直连 %d(http %d + https %d) + HLS %d(http %d + https %d)",
                 s_count,
                 n_dir_http + n_dir_https, n_dir_http, n_dir_https,
                 n_hls_http + n_hls_https, n_hls_http, n_hls_https);
    }

    for (int i = 0; i < 3 && i < s_count; i++)
        ESP_LOGI(TAG, "  [%d] %s | cat=%u prov=%u | %s",
                 i, s_pool[i].name, s_pool[i].cat, s_pool[i].prov, s_pool[i].url);
}
