/*
 * 拾声 · 极简网络取数 —— 实现
 * ============================================================
 *  设计与"为什么不用 cJSON / 不做 https"见 xs_http.h 顶部。
 *  这里只补充两条实现上的坑：
 *
 *  ★★ 超时设 8 秒：和 .h 里说的一样，这是"最长一次要卡多久"的上限。
 *     调用者（天气/股票的取数任务）必须把它当成【阻塞】的函数用 ——
 *     绝对不能在 LVGL 任务里直接调，那会让界面整 8 秒不响应触摸。
 *
 *  ★★ 读循环不能只看第一个 esp_http_client_read：
 *     分块传输（chunked）时它一次只给一个 chunk。
 *     必须循环读到返回 <= 0 为止，否则拿到的 JSON 是半截的 ——
 *     症状是"有时候解析失败、有时候正常"，最难查。
 * ============================================================ */
#include "xs_http.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "XSNET";

/* ============================================================
 *  ① HTTP GET
 * ============================================================ */
int xs_http_get(const char *url, char *buf, size_t cap)
{
    if (!url || !buf || cap < 2) return -1;

    /* ★ 只吃明文 http://。https:// 直接拒 —— 理由见 .h：
     *   内部 RAM 最大连续块不够跑 TLS 握手，让它去试只会白等 8 秒超时，
     *   然后在日志里留下一个看不出原因的失败。这里当场说清楚。
     *   （天气/股票两个接口都是明文，所以这不是"功能阉割"，
     *     而是把"不可能成功的事"提前拦掉。）*/
    if (strncmp(url, "http://", 7) != 0) {
        ESP_LOGE(TAG, "只支持明文 http://（TLS 握手要的连续内存不够）：%s", url);
        return -1;
    }

    esp_http_client_config_t cfg = {
        .url                    = url,
        .timeout_ms             = 8000,
        .buffer_size            = 1024,
        /* ★ 跟随重定向：接口域名偶尔会被 301 到 CDN。
         *   不设的话 esp_http_client 拿到 301 就当非 200 处理了。*/
        .max_redirection_count  = 3,
        /* ★ 不挂证书包：只有 https 才用得上，明文下挂上白占内存。*/
        .crt_bundle_attach      = NULL,
    };

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        ESP_LOGE(TAG, "句柄建不了（内部 RAM 不够？）");
        return -1;
    }

    /* 有些站点对没有 UA 的请求直接 403。给个正经的。*/
    esp_http_client_set_header(c, "User-Agent", "Mozilla/5.0 (XianDial)");

    int ret = -1;
    esp_err_t e = esp_http_client_open(c, 0);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "open 失败 err=%d (%s)", (int)e, esp_err_to_name(e));
        goto done;
    }

    (void)esp_http_client_fetch_headers(c);      /* 返回值是 content-length，chunked 时为 -1 */
    int status = esp_http_client_get_status_code(c);
    if (status != 200 && status != 206) {
        ESP_LOGE(TAG, "HTTP 状态 = %d（不是 200）", status);
        goto done;
    }

    /* ★ 循环读：chunked 时一次只给一个 chunk，只读一次会拿到半截 JSON */
    size_t n = 0;
    while (n < cap - 1) {
        int r = esp_http_client_read(c, buf + n, (int)(cap - 1 - n));
        if (r <= 0) break;
        n += (size_t)r;
    }
    buf[n] = '\0';
    ret = (int)n;

done:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ret;
}

/* ============================================================
 *  ② 迷你 JSON
 * ============================================================ */

/* 跳过空白 */
static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

bool xs_json_get(const char *js, const char *key, char *out, size_t cap)
{
    if (!js || !key || !out || cap < 2) return false;
    out[0] = '\0';

    /* 搜索模式带引号，天然避免 "city" 命中 "citykey"、"time" 命中 "updateTime" */
    char pat[80];
    if (strlen(key) + 3 > sizeof(pat)) return false;
    snprintf(pat, sizeof(pat), "\"%s\"", key);

    const char *p = strstr(js, pat);
    if (!p) return false;

    p = skip_ws(p + strlen(pat));
    if (*p != ':') return false;
    p = skip_ws(p + 1);

    size_t n = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
        /* 引号没闭合 ⇒ 数据被截断了，宁可报失败也不要半截值 */
        if (*p != '"') { out[0] = '\0'; return false; }
    } else {
        while (*p && *p != ',' && *p != '}' && *p != ']' &&
               *p != '\n' && *p != '\r' && n + 1 < cap) {
            out[n++] = *p++;
        }
        while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t')) n--;
    }
    out[n] = '\0';
    return n > 0;
}

bool xs_json_elem(const char *js, const char *key, int idx, char *out, size_t cap)
{
    if (!js || !key || !out || cap < 2 || idx < 0) return false;
    out[0] = '\0';

    char pat[80];
    if (strlen(key) + 3 > sizeof(pat)) return false;
    snprintf(pat, sizeof(pat), "\"%s\"", key);

    const char *p = strstr(js, pat);
    if (!p) return false;
    p = strpbrk(p + strlen(pat), "[");      /* 跳到数组起点 */
    if (!p) return false;
    p++;                                    /* 越过 '[' */

    for (int i = 0; i <= idx; i++) {
        p = strpbrk(p, "{");                /* 跳到下一个元素 */
        if (!p) return false;
        const char *start = p;
        int depth = 0;
        /* 花括号配对：元素内部理论上不会再嵌对象，但配对写法更稳 */
        while (*p) {
            if (*p == '{') depth++;
            else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
            p++;
        }
        if (i == idx) {
            size_t n = (size_t)(p - start);
            if (n > cap - 1) n = cap - 1;
            memcpy(out, start, n);
            out[n] = '\0';
            return true;
        }
    }
    return false;
}

bool xs_json_top_elem(const char *js, int idx, char *out, size_t cap)
{
    if (!js || !out || cap < 2 || idx < 0) return false;
    out[0] = '\0';

    /* 直接从整串里第一个 '[' 开始数 —— 新浪日K就是个裸数组。
     * ★ 用 strpbrk 而不是 strchr：语义一样，但和上面 xs_json_elem 保持同款写法，
     *   改一处时不容易漏另一处。*/
    const char *p = strpbrk(js, "[");
    if (!p) return false;
    p++;                                    /* 越过 '[' */

    for (int i = 0; i <= idx; i++) {
        p = strpbrk(p, "{");
        if (!p) return false;
        const char *start = p;
        int depth = 0;
        while (*p) {
            if (*p == '{') depth++;
            else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
            p++;
        }
        if (i == idx) {
            size_t n = (size_t)(p - start);
            if (n > cap - 1) n = cap - 1;
            memcpy(out, start, n);
            out[n] = '\0';
            return true;
        }
    }
    return false;
}

/* ============================================================
 *  ③ '~' 切分（GBK 安全）
 * ============================================================ */
bool xs_split_tilde(const char *s, int idx, char *out, size_t cap)
{
    if (!s || !out || cap < 2 || idx < 0) return false;
    out[0] = '\0';

    const unsigned char *p = (const unsigned char *)s;
    int    cur = 0;
    size_t n   = 0;

    while (*p) {
        /* ★ 见 .h：GBK 第二字节可能是 0x7E，必须整体吞掉。
         *   GBK 高字节恒 >= 0x81；ASCII 与 '~' 恒 < 0x80。*/
        int w = (*p >= 0x81 && p[1]) ? 2 : 1;

        if (cur == idx) {
            if (w == 1 && *p == '~') break;             /* 到下一个分隔符 */
            for (int k = 0; k < w && p[k] && n + 1 < cap; k++)
                out[n++] = (char)p[k];
        } else if (w == 1 && *p == '~') {
            cur++;
        }
        p += w;
    }
    out[n] = '\0';
    return cur == idx;
}

/* ============================================================
 *  ④ 抠数字
 * ============================================================ */
void xs_pick_num(const char *src, char *out, size_t cap)
{
    if (!out || cap < 2) return;
    out[0] = '\0';
    if (!src) return;

    const char *p = src;
    while (*p && !(*p >= '0' && *p <= '9')) p++;
    if (!*p) return;                       /* 整串没有数字 */

    size_t n = 0;
    while (*p >= '0' && *p <= '9' && n + 1 < cap) out[n++] = *p++;
    if (*p == '.' && n + 1 < cap) {        /* 允许一位小数（温度有 25.5）*/
        out[n++] = *p++;
        while (*p >= '0' && *p <= '9' && n + 1 < cap) out[n++] = *p++;
    }
    out[n] = '\0';
}
