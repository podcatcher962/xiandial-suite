/* ============================================================
 *  app_hls.c —— HLS（m3u8）直播流拉流器
 * ============================================================
 *  为什么需要它（10-03 第十五次）：
 *   网页版 1120 个台里有 387 个是 m3u8，其中【63 个是央视/卫视】
 *   （CCTV-1~17、CETV-1~4、各省卫视、东方/东南/深圳卫视…）。
 *   固件原来只认直连流，于是「电视伴音」栏目只剩 2 个台。
 *
 * ★ 为什么这些 m3u8 好做（_probe_hls2.py 实测，5 个源逐个验过）：
 *    ① 全部是 media list，【没有一个是 master list】 ⇒ 不用取变体流
 *    ② 切片全是 .ts ⇒ esp_audio_simple_dec 的 TS 容器直接认
 *    ③ 全部无加密（无 EXT-X-KEY）
 *    ④ 分片数少（3~6 个），每片 TARGETDURATION 5~11 秒
 *   ⇒ 只需实现「拉列表 → 按序下 .ts → 喂进已有解码器」这一条路。
 *
 * ★ 但分片 URL 的【相对路径规则有三家三种写法】，都真机存在：
 *    清单 = https://<cdn 域名>/audio/<频道>_2.m3u8
 *    片   = cctv1_audio/1791032737_14548999.ts      ← 相对目录（拼 /audio/）
 *    片   = /wstvcpud/udrmbtv7_1md/xxx.ts            ← 绝对路径（拼域名）
 *    片   = a1live.livecdn.yicai.com_radio_tv-xxx.ts ← 裸文件名（拼目录）
 *   ⇒ 见 hls_resolve()。这三种在同一次调试里各撞过一次，别删。
 *   ★ 发布版刻意不写真实域名（避免把第三方地址带进公开仓库），
 *     格式说明保留 —— 这三种是 HLS 标准的合法写法，换域名照样成立。
 *
 * 用法（app_radio.c 里）：
 *    esp_http_client_handle_t c; char buf[...];
 *    hls_state_t h; hls_start(&h, m3u8_url);
 *    while (播放中) { hls_pull(&h, buf, want, &got); ... }
 *    hls_stop(&h);
 *
 * 内存：state 约 2.2 KB，【放 PSRAM】。内部 RAM 只剩 48 KB，别碰。
 *   ⚠️ 10-04 纠正一处自相矛盾的注释：原来这里写「片内缓冲由调用方
 *   （app_radio 的 inbuf）复用，本模块不额外分配大块」——【假的】，
 *   本模块的 hls_get_one_segment() 自己 malloc 了 384 KB 的 seg_buf。
 *   内部 RAM 只剩 45~48 KB ⇒ 那次 malloc 必然失败 ⇒ init 返 NULL ⇒
 *   「播 20 秒就哑」。现已改成 heap_caps_malloc(…, MALLOC_CAP_SPIRAM)。
 * ============================================================ */
#include "app_hls.h"
#include "app_sys.h"     /* app_sys_utc_now()：time() 被时区污染过，见 app_sys.h 铁律 */

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>     /* hls_lag_seconds() 要 time()/time_t */

static const char *TAG = "XSHLS";

/* ---------------- URL 工具 ---------------- */

/* 把 base（一个目录，形如 http://host/a/b/）和 ref 拼成完整 URL。
 * 规则（三种都见过，见文件头）：
 *   ref 以 "http" 开头  → 原样用
 *   ref 以 '/'  开头  → scheme://host + ref
 *   否则                → base + ref（base 必须以 '/' 结尾）*/
static void hls_resolve(const char *base, const char *ref, char *out, size_t n)
{
    if (ref == NULL || ref[0] == '\0') { if (n) out[0] = '\0'; return; }

    if (strncmp(ref, "http://", 7) == 0 || strncmp(ref, "https://", 8) == 0) {
        snprintf(out, n, "%.500s", ref);
        return;
    }
    if (ref[0] == '/') {
        /* 绝对路径：从 base 里抠出 scheme://host */
        const char *host = base;
        const char *p = strstr(base, "://");
        if (p) {
            host = p + 3;
            while (*host && *host != '/') host++;
        }
        snprintf(out, n, "%.*s%.500s", (int)(host - base), base, ref);
        return;
    }
    /* 相对路径：确保 base 以 '/' 结尾 */
    size_t bl = strlen(base);
    if (bl > 0 && base[bl - 1] == '/') {
        snprintf(out, n, "%.400s%.500s", base, ref);
    } else {
        snprintf(out, n, "%.400s/%.500s", base, ref);
    }
}

/* ---------------- 拉播放列表 ---------------- */

static void hls_set_err(hls_state_t *h, const char *msg)
{
    snprintf(h->err, sizeof(h->err), "%s", msg);
}

/* 拉 m3u8 文本到 h->list（PSRAM 里的大缓冲）*/
static bool hls_fetch_list(hls_state_t *h)
{
    h->list_len = 0;
    esp_http_client_config_t cfg = {
        .url = h->url,
        .timeout_ms = 8000,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        /* ★★★ 10-03 第十六次：央视台全是 https，不挂 CA 证书包握手必失败
         *   （兰兰报「电视伴音央视的都不能播放」）。证书包在 flash 里，不占 RAM。*/
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { hls_set_err(h, "HLS: 句柄建不了"); return false; }

    esp_http_client_set_method(c, HTTP_METHOD_GET);   /* 铁律 17：绝不用 HEAD */
    esp_http_client_set_header(c, "User-Agent",
                               "Mozilla/5.0 (XianDial)");
    esp_err_t oe = esp_http_client_open(c, 0);
    if (oe != ESP_OK) {
        /* ★ 第十八次加详细日志：兰兰报「央视还是不能播」。
         *   分开打：open 失败 = DNS/TCP/TLS 握手（https 的话八成是证书）；
         *   open 成功但 fetch 失败 = HTTP 层（状态码/超时）。
         *   之前只有一个笼统的「连不上播放列表」，没法下手。*/
        ESP_LOGE(TAG, "HLS: open failed err=%d (%s) url=%s", (int)oe,
                 esp_err_to_name(oe), h->url);
        if (strncmp(h->url, "https", 6) == 0) {
            int tcode = 0, tflags = 0;
            esp_http_client_get_and_clear_last_tls_error(c, &tcode, &tflags);
            ESP_LOGE(TAG, "HLS: TLS err=0x%04X (-0x%04X) flags=0x%04X -- cert/handshake?",
                     (unsigned)tcode, (unsigned)(-tcode), (unsigned)tflags);
        }
        esp_http_client_cleanup(c);
        hls_set_err(h, "HLS: 连不上播放列表");
        return false;
    }
    int total = esp_http_client_fetch_headers(c);
    if (total <= 0) {
        ESP_LOGE(TAG, "HLS: fetch_headers failed ret=%d", total);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        hls_set_err(h, "HLS: 播放列表无响应");
        return false;
    }
    /* ★ fetch_headers 返回的是【HTTP 状态码】不是长度！写日志时别混。*/
    int status = esp_http_client_get_status_code(c);
    ESP_LOGI(TAG, "m3u8 status=%d (raw=%d) cap=%d", status, total, (int)h->list_cap);
    if (status != 200 && status != 206) {
        ESP_LOGE(TAG, "HLS: bad status %d for %s", status, h->url);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        hls_set_err(h, "HLS: 列表返回错误状态");
        return false;
    }
    /* 列表最长 8 KB 足够：63 个源实测都是 3~6 片 */
    while (h->list_len < h->list_cap - 1) {
        int n = esp_http_client_read(c, (char *)h->list + h->list_len,
                                     (int)(h->list_cap - 1 - h->list_len));
        if (n <= 0) break;
        h->list_len += (size_t)n;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    h->list[h->list_len] = '\0';
    ESP_LOGI(TAG, "m3u8 body: %u B", (unsigned)h->list_len);
    return h->list_len > 0;
}

/* 把列表里的分片名（已解析成完整 URL）搬进 seg[]。
 * 返回分片个数。*/
static int hls_parse_segments(hls_state_t *h, const char *dir_base)
{
    int cnt = 0;
    char *p = (char *)h->list;
    while (*p && cnt < HLS_MAX_SEG) {
        /* 跳到行首 */
        while (*p == '\r' || *p == '\n') p++;
        if (*p == '\0') break;
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = '\0';
        /* 掐掉行尾 \r */
        size_t L = strlen(line);
        if (L && line[L - 1] == '\r') line[L - 1] = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;   /* 标签/空行 */

        hls_resolve(dir_base, line, h->seg[cnt], HLS_URL_MAX);
        ESP_LOGI(TAG, "seg[%d] = %.90s", cnt, h->seg[cnt]);
        cnt++;
    }
    h->seg_n = cnt;
    return cnt;
}

/* ---------------- 拉一个分片 ---------------- */

static size_t hls_get_one_segment(hls_state_t *h, const char *url);   /* 定义在下面 */

/* 下载一个 .ts 分片到 h->seg_buf（PSRAM），返回字节数，0 = 失败。
 *
 * ★★★ 2026-10-04 第二十一次修正了分片大小的估算错误：
 *   原注释按「64 kbps × 11 秒 ≈ 90 KB」估，缓冲给 160 KB。
 *   但电脑上真拉 CCTV-1 测出来是 **199~200 KB / 10 秒**（约 160 kbps），
 *   160 KB 缓冲每片都截掉 40 KB ⇒ 每 10 秒丢一次包 ⇒ 听起来就是「卡」。
 *   ⇒ HLS_SEG_MAX 已提到 384 KB（见 app_hls.h 的说明）。
 *
 * ★ 缓冲在 PSRAM：内部 SRAM 只剩 45 KB，384 KB 放内部 RAM 会直接 NO_MEM。*/
static size_t hls_get_one_segment(hls_state_t *h, const char *url)
{
    if (h->seg_buf == NULL) {
        /* ★★★ 10-04 关键修正：这条注释原来写「缓冲在 PSRAM」，但代码是
         *   malloc() —— 那是【内部 SRAM】！384 KB 根本拿不到（LVGL 占掉
         *   114 KB、内部 SRAM 只剩 45 KB），于是 init 返回 NULL，
         *   而下面 `if (!c) return 0;` 是全函数唯一一条【不打日志】的路径
         *   ⇒ 界面上表现为「播 20 秒就没声了」，日志里只有
         *   「seg[N] fetch failed, skip」+「list ended -> reload」无限刷，
         *   看不到任何错误码，最难查的一类。
         *   实测：seg_buf 分配失败后，10 分钟内刷了 10370 次 fetch failed。*/
        h->seg_buf = heap_caps_malloc(HLS_SEG_MAX,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!h->seg_buf) {
            /* PSRAM 也没有 ⇒ 退到内部 RAM，但一定要打日志说明降级了 */
            h->seg_buf = malloc(HLS_SEG_MAX);
            ESP_LOGW(TAG, "seg_buf %.0f KB 分配失败（PSRAM 与内部都没有？）",
                     (double)HLS_SEG_MAX / 1024.0);
        }
        if (!h->seg_buf) {
            hls_set_err(h, "HLS: 分片缓冲分配失败");
            ESP_LOGE(TAG, "seg_buf malloc(%d) 彻底失败，HLS 没法播",
                     (int)HLS_SEG_MAX);
            return 0;
        }
        ESP_LOGI(TAG, "seg_buf %.0f KB 已分配", (double)HLS_SEG_MAX / 1024.0);
    }
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 8000,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* 同上：https 必需 */
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    /* ★★★ 10-04：这条分支原来完全静默，是本次「20 秒断播」查不出原因的关键。
     *   esp_http_client_init() 失败最常见的就是内存不足（内部 RAM 被
     *   LVGL 池 / 音频缓冲占满），不打日志就等于没发生。*/
    if (!c) {
        ESP_LOGE(TAG, "seg esp_http_client_init 返回 NULL url=%.80s", url);
        hls_set_err(h, "HLS: 分片连接建不了");
        return 0;
    }
    esp_http_client_set_method(c, HTTP_METHOD_GET);
    esp_http_client_set_header(c, "User-Agent", "Mozilla/5.0 (XianDial)");
    esp_err_t oe = esp_http_client_open(c, 0);
    if (oe != ESP_OK) {
        ESP_LOGE(TAG, "seg open failed err=%d (%s) url=%.80s", (int)oe,
                 esp_err_to_name(oe), url);
        if (strncmp(url, "https", 6) == 0) {
            int tcode = 0, tflags = 0;
            esp_http_client_get_and_clear_last_tls_error(c, &tcode, &tflags);
            ESP_LOGE(TAG, "seg TLS err=0x%04X (-0x%04X) flags=0x%04X",
                     (unsigned)tcode, (unsigned)(-tcode), (unsigned)tflags);
        }
        esp_http_client_cleanup(c);
        return 0;
    }
    int total = esp_http_client_fetch_headers(c);
    if (total <= 0) {
        ESP_LOGE(TAG, "seg fetch_headers failed ret=%d url=%.80s", total, url);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return 0;
    }
    int st = esp_http_client_get_status_code(c);
    if (st != 200 && st != 206) {
        ESP_LOGE(TAG, "seg status=%d url=%.80s", st, url);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return 0;
    }
    size_t got = 0;
    while (got < HLS_SEG_MAX) {
        int n = esp_http_client_read(c, (char *)h->seg_buf + got,
                                     (int)(HLS_SEG_MAX - got));
        if (n <= 0) break;
        got += (size_t)n;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (got == 0) {
        ESP_LOGW(TAG, "segment empty: %.80s", url);
        return 0;
    }
    ESP_LOGI(TAG, "segment %.80s -> %u B", url, (unsigned)got);
    return got;
}

/* ---------------- 公开接口 ---------------- */

bool hls_start(hls_state_t *h, const char *m3u8)
{
    if (!h || !m3u8) return false;
    memset(h, 0, sizeof(*h));
    if (h->list == NULL) {
        /* 16 KB 也放 PSRAM：内部 RAM 只剩 45 KB，这里没必要抢。*/
        h->list = heap_caps_malloc(HLS_LIST_CAP,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!h->list) h->list = malloc(HLS_LIST_CAP);   /* 退到内部 RAM */
        if (!h->list) { hls_set_err(h, "HLS: 内存不足"); return false; }
    }
    h->list_cap = HLS_LIST_CAP;
    snprintf(h->url, sizeof(h->url), "%s", m3u8);

    if (!hls_fetch_list(h)) return false;

    /* 取 base 目录（分片相对路径的拼接基准）＝ 最后一个 '/' 之后 */
    char dir[HLS_URL_MAX];
    snprintf(dir, sizeof(dir), "%s", h->url);
    char *slash = strrchr(dir, '/');
    if (slash) *(slash + 1) = '\0';
    else      snprintf(dir, sizeof(dir), "/");

    /* ★ master list 检测：万一是，得先取变体流。
     *   实测 63 个源没有一个是，但这是 HLS 标准的一部分，
     *   以后换了源不至于直接黑屏。*/
    if (strstr((char *)h->list, "#EXT-X-STREAM-INF")) {
        ESP_LOGW(TAG, "master list detected -> pick first variant");
        const char *ml = strstr((char *)h->list, "#EXT-X-STREAM-INF");
        if (ml) {
            ml = strchr(ml, '\n');
            if (ml) {
                ml++;
                char var[HLS_URL_MAX];
                size_t n = 0;
                while (ml[n] && ml[n] != '\n' && ml[n] != '\r' && n < HLS_URL_MAX - 1) {
                    var[n] = ml[n];
                    n++;
                }
                var[n] = '\0';
                if (n > 0) {
                    hls_resolve(dir, var, h->url, sizeof(h->url));
                    ESP_LOGI(TAG, "variant = %.90s", h->url);
                    if (!hls_fetch_list(h)) return false;
                    snprintf(dir, sizeof(dir), "%s", h->url);
                    slash = strrchr(dir, '/');
                    if (slash) *(slash + 1) = '\0';
                }
            }
        }
    }

    h->seg_n = hls_parse_segments(h, dir);
    if (h->seg_n == 0) {
        hls_set_err(h, "HLS: 列表里没有分片");
        return false;
    }
    h->seg_i    = 0;
    h->seg_pos  = 0;
    h->seg_need = 0;
    h->wr       = 0;
    h->started  = true;
    ESP_LOGI(TAG, "ready: %d segments, first = %.60s", h->seg_n, h->seg[0]);
    return true;
}

/* ★★ 10-04：把「这片叫什么」记进 last_seg。
 *   hls_start_next_segment() 会把 seg_i 加 1，所以要在它【之前】记当前片。
 *   直播列表重拉后第一片常常还是刚播完那片，不比对就会重复播一段。*/
static void hls_mark_played(hls_state_t *h)
{
    if (!h || h->seg_i < 0 || h->seg_i >= h->seg_n) return;
    const char *s = h->seg[h->seg_i];
    const char *b = s ? strrchr(s, '/') : NULL;
    /* ⚠️ -Werror=format-truncation：gcc 按 h->seg[] 的声明尺寸（512）算最坏，
 *   而 last_seg 只有 72 ⇒ 必须给精度，单纯加大数组没用。*/
    snprintf(h->last_seg, sizeof(h->last_seg), "%.71s",
             b ? b + 1 : (s ? s : ""));
}

bool hls_start_next_segment(hls_state_t *h)
{
    if (!h) return false;
    h->seg_i++;
    if (h->seg_i >= h->seg_n) {
        /* 列表播完一轮 ⇒ 直播要重拉列表（分片会滚动更新）*/
        ESP_LOGI(TAG, "list ended -> reload");
        char keep[HLS_URL_MAX];
        snprintf(keep, sizeof(h->url), "%s", h->url);
        if (!hls_fetch_list(h)) return false;
        char dir[HLS_URL_MAX];
        snprintf(dir, sizeof(dir), "%s", keep);
        char *slash = strrchr(dir, '/');
        if (slash) *(slash + 1) = '\0';
        h->seg_n = hls_parse_segments(h, dir);
        if (h->seg_n == 0) { hls_set_err(h, "HLS: 重拉列表没分片"); return false; }
        h->seg_i = 0;
        /*★★ 10-04：跳过刚播完的那片。
         *   直播列表每次重拉都往前滚几片，但第一片常常仍是我们刚播完的
         *   （CDN 缓存 + 列表刷新有延迟）。不跳过就重播一遍，
         *   听感是「同一段话重复出现」。*/
        if (h->last_seg[0]) {
            for (int k = 0; k < h->seg_n; k++) {
                const char *b = strrchr(h->seg[k], '/');
                const char *nm = b ? b + 1 : h->seg[k];
                if (strcmp(nm, h->last_seg) == 0) {
                    h->seg_i = k + 1;
                    ESP_LOGI(TAG, "跳过刚播完的 %s（下一个是 %.40s）",
                             h->last_seg, h->seg[h->seg_i]);
                    break;
                }
            }
            /* 新列表全都很旧 ⇒ 一片都没跳过，说明我们落后了，从头播最新片 */
            if (h->seg_i >= h->seg_n) {
                ESP_LOGW(TAG, "新列表里没有新片，从头开始");
                h->seg_i = 0;
            }
        }
    }
    h->seg_pos  = 0;
    h->seg_need = 0;
    return true;
}

/* 从 hls 取最多 want 字节的 .ts 连续数据，落到 out。
 * 返回 true = 有数据；false = 需要重拉列表 / 出错。
 * ★ 本函数【不写 out】以外的任何状态机之外的语义，调用方喂解码器即可。*/
bool hls_pull(hls_state_t *h, uint8_t *out, size_t want, size_t *got)
{
    *got = 0;
    if (!h) return false;
    /* ★★ next_url 已删：它在 hls_start_next_segment() 里被置 true，于是换片
     *   成功后【下一次 hls_pull 又跳一片】⇒ 每片 10 秒里丢一段，声音断续。
     *   换片统一由下面的 seg_pos >= seg_need 驱动，只有一个地方管。*/

    /* wr 是「上一次从某片里已经取走多少字节」——片内偏移，不是列表偏移 */
    while (*got < want) {
        /* ★★★ 10-04 加熔断：原来没有失败上限，分片一直拉不下来时
         *   「拉不到 → skip → 重拉列表 → 还是那几片 → 再 skip」形成死循环。
         *   真机实测刷了 10 分钟 10370 次 fetch failed —— 白耗电、白刷日志、
         *   白占着 httpd 和 WiFi，界面表现就是「播 20 秒然后一直哑」。
         *   连续 8 片拿不到就认定这个源废了，干净地退出让上层报错。*/
        if (h->fail_streak >= 8) {
            hls_set_err(h, "HLS: 连续 8 片都拉不下来，源不可用");
            ESP_LOGE(TAG, "连续 %d 片子拉不下来，放弃这个源（累计失败 %d）",
                     h->fail_streak, h->fail_total);
            return false;
        }
        /* 这一片已取完 ⇒ 换下一片 */
        if (h->seg_pos >= h->seg_need) {
            if (h->seg_need > 0) {
                hls_mark_played(h);          /* 记下刚播完的片名 */
                if (!hls_start_next_segment(h)) return false;
                /*★★★ 10-04 核心修正：这里原来写着
                 *      if (h->seg_pos >= h->seg_need) return false;
                 *   而 hls_start_next_segment() 刚把 seg_pos=0、seg_need=0
                 *   （0 的含义是「这片还没下载」），于是 0 >= 0 恒为真
                 *   ⇒ 每次换片都返回 false ⇒ 上层 hls_start() 从 seg[0] 重来
                 *   ⇒ 同一片反复播，n_recon 攒到 3 就 fetch dead。
                 *   真机实测：CCTV-1 只出 29 秒声音，日志里 seg[0] 三次完全相同。
                 *   正确做法是【继续转一圈去把新片拉下来】，不是放弃。*/
                continue;
            } else {
                /* 第一次进这一片 ⇒ 建 HTTP 连接 */
                h->wr = hls_get_one_segment(h, h->seg[h->seg_i]);
                if (h->wr == 0) {
                    /* 这片拉不到（网络抖动/过期）⇒ 跳过，别卡死 */
                    ESP_LOGW(TAG, "seg[%d] fetch failed, skip", h->seg_i);
                    h->fail_streak++;
                    h->fail_total++;
                    if (!hls_start_next_segment(h)) return false;
                    continue;
                }
                /* ★ 拉到数据就把连续失败清零 —— 网络抖动是偶发的，
                 *   不能因为前面失败过就把后面正常的一直往下压。*/
                h->fail_streak = 0;
                h->seg_need = h->wr;
                h->seg_pos  = 0;
            }
        }

        size_t left = h->seg_need - h->seg_pos;
        size_t room = want - *got;
        size_t n    = (left < room) ? left : room;
        if (n == 0) break;
        memcpy(out + *got, h->seg_buf + h->seg_pos, n);
        h->seg_pos += n;
        *got += n;
    }
    return *got > 0;
}

/*★★ 10-04：自证「直播延迟了多少秒」。
 *  兰兰两次报「像是延迟」，但两次原因完全不同：修复前是重播老片（文件名是
 *  几分钟前的），修复后是真的只落后十几秒。**光听无法区分这两者**，
 *  所以把「当前正在播的这一片，它的文件名时间戳」和本机时间比出来。
 *
 *  各家源的分片命名不统一，实测三种：
 *    myalicdn（央视伴音）  ..._audio/1791089537_14554676.ts   ← 10 位秒戳
 *    央视频/腾讯          ..../1791089537.ts                 ← 同上
 *    有些（海外台）        seg_00042.ts                      ← 没有时间戳
 *  做法：在文件名里扫「第一个 10 位数字且值像 unix 时间戳（>2020 年）」的数。
 *
 *  ★★★ 10-04 收紧上限：兰兰在 CCTV1 上看到「延迟 28831s」（≈8 小时）。
 *     先怀疑「取到了 8 小时前的老分片」⇒ 电脑侧 curl 同一 m3u8 实测，
 *     **最新片只落后 5 秒，源是好的** ⇒ 那个归因是错的。
 *     真因见下面 hls_lag_seconds() 里的注释：**时区谎报了 8 小时**。
 *     MAXLAG=300 保留，但作用变成「兜底」而不是「碰巧挡住 28800」。
 *
 *     另外原判断是 `now - v < 86400`（一天内都算数）⇒ 任何离谱值都会被显示出来。
 *     现在只接受【0 ~ 300 秒】：超出就是「这个源算不准」，返回 0 让屏上不显示。
 *     宁可不显示，也不能显示一个错的 —— 兰兰原话「误差很大不用显示延迟了」。*/
long hls_lag_seconds(const hls_state_t *h)
{
    if (!h || h->seg_i < 0 || h->seg_i >= h->seg_n) return 0;
    const char *s = h->seg[h->seg_i];
    if (!s) return 0;
    /* 2020-01-01 = 1577836800。低于它就不是秒级时间戳。*/
    const long THRESH = 1577836800L;
    const long MAXLAG = 300L;      /* 直播合理延迟上限（10-04 加） */
    /*★★★ 10-04 关键修正：这里必须用【真 UTC】而不是 time()。
     *  app_sys 的 SNTP 回调把 tv_sec 加了 8 小时再写回系统时钟，
     *  所以 time() 返回的是「本地时间」（详见 app_sys.h 的铁律段）。
     *  分片文件名里的 10 位戳是真 UTC ⇒ 拿 time() 去减会凭空多 28800 秒：
     *      真延迟 31 秒  →  算成 31 + 28800 = 28831
     *  兰兰在 CCTV1 上看到的就是这个数。⇒ 兰兰说「延迟 28831」时我先归因成
     *  「取到了 8 小时前的老分片」，**那是错的**（源实测最新片只落后 5 秒）。
     *  真正的原因是时区谎报。MAXLAG=300 恰好把 28800 挡掉了，纯属碰巧。*/
    long now = app_sys_utc_now();

    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') continue;
        if (p != s && p[-1] >= '0' && p[-1] <= '9') continue;   /* 跳过数字中间的 */
        long v = 0;
        int k = 0;
        while (p[k] >= '0' && p[k] <= '9' && k < 10) { v = v * 10 + (p[k] - '0'); k++; }
        if (k == 10 && v > THRESH && now - v < 86400) {
            long d = now - v;
            if (d < 0)   return 0;         /* 未来时间戳：源不可信 */
            if (d > MAXLAG) {
                /* ★ 不当作有效延迟，但一定要留证据：到底是取到了多老的一片，
                 *   还是时间戳识错了。下次真机出问题看这行就能定案。*/
                ESP_LOGW(TAG, "hls lag implausible: %ld s, seg=%.64s", d, s);
                return 0;
            }
            return d;
        }
    }
    return 0;      /* 这个源的命名里没有时间戳 */
}

const char *hls_error(hls_state_t *h)
{
    return (h && h->err[0]) ? h->err : "";
}

void hls_stop(hls_state_t *h)
{
    if (!h) return;
    if (h->seg_buf) { free(h->seg_buf); h->seg_buf = NULL; }
    if (h->list)    { free(h->list);    h->list = NULL;    }
    h->started = false;
}
