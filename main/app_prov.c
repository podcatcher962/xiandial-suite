/*
 * 拾声 (XianDial) 硬件版 —— SoftAP + Captive Portal 配网
 * 头文件里有原理说明，这里不重复。
 *
 * 组成：
 *   1) 热点   —— WIFI_MODE_APSTA，开放网络（不要密码，见 ap_start 的说明）
 *   2) HTTP   —— esp_http_server，5 个 URI
 *   3) DNS    —— 自己写的极小 responder，对所有 A 查询回 192.168.4.1
 *   4) 结果跟踪 —— 提交后有个小任务盯连接结果，顺带 20 秒后自动关热点
 *
 * 参照实现：IDF 自带 examples/protocols/http_server/captive_portal
 */

#include "app_prov.h"
#include "app_sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "XSPROV";

#define PROV_DNS_PORT   53
#define PROV_RESULT_NONE    0
#define PROV_RESULT_DOING   1
#define PROV_RESULT_OK      2
#define PROV_RESULT_FAIL    3
#define PROV_RESULT_BADARG  4

static volatile bool s_on = false;
static char     s_ssid[APP_PROV_SSID_MAX] = "";
static volatile int s_result = PROV_RESULT_NONE;
static httpd_handle_t   s_httpd = NULL;
static volatile int     s_dns_alive = 0;
static volatile int     s_dns_sock = -1;

/* ============================================================
 *  1) 热点
 * ============================================================ */
/* ★ 为什么热点【不加密码】：
 *   加了密码，用户得在手机上打 8 位；开放网络只要点一下
 *   （iOS 会多弹一句「此网络不安全」，点「仍要加入」）。
 *   打字 ≫ 点一下，所以开放。配网窗口只有几十秒、之后自动关，
 *   这段时间的暴露面是「邻居能连上这个热点，但拿不到你家密码」——
 *   提交表单要的是你家 WiFi 密码，没有它谁也连不上你家。
 *   要收紧就把 authmode 改成 WIFI_AUTH_WPA2_PSK 并把密码画到屏幕上。*/
static esp_err_t ap_start(void)
{
    uint8_t mac[6] = { 0 };
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        snprintf(s_ssid, sizeof(s_ssid), "XianDial-%02X%02X", mac[4], mac[5]);
    } else {
        snprintf(s_ssid, sizeof(s_ssid), "XianDial");
    }

    /* 记下 AP netif 句柄，后面 stop 要销毁它 —— 靠 ifkey 反查虽然也能，
     * 但创建时直接存最省事，也少一次字符串比较。*/
    static esp_netif_t *ap_netif = NULL;
    if (!ap_netif) ap_netif = esp_netif_create_default_wifi_ap();
    if (!ap_netif) {
        ESP_LOGE(TAG, "创建 AP netif 失败");
        return ESP_FAIL;
    }

    /* 固定 192.168.4.1/24。IDF 默认就是这个值，但显式写一遍：
     * 万一以后有人改了 sdkconfig 的 DHCP 网段，屏幕上的提示就与实际不符。
     * ⚠️ esp_netif_str_to_ip4() 的第 2 参类型是 esp_ip4_addr_t，
     *   不是 esp_netif_ip_addr_t（后者不存在，gcc 会报 unknown type name）。*/
    esp_netif_ip_info_t ip = { 0 };
    esp_ip4_addr_t a4, m4, g4;
    esp_netif_str_to_ip4("192.168.4.1", &a4);
    esp_netif_str_to_ip4("255.255.255.0", &m4);
    esp_netif_str_to_ip4("192.168.4.1", &g4);
    ip.ip = a4; ip.netmask = m4; ip.gw = g4;
    esp_netif_set_ip_info(ap_netif, &ip);

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.ap.ssid, s_ssid, sizeof(wc.ap.ssid) - 1);
    wc.ap.ssid_len       = (uint16_t)strlen(s_ssid);
    wc.ap.channel        = 1;
    wc.ap.max_connection = 4;
    wc.ap.authmode       = WIFI_AUTH_OPEN;      /* ★ 开放，见函数上的说明 */
    wc.ap.pmf_cfg.capable = false;

    /* ★ APSTA：热点开着的同时 STA 仍然能连家里的 WiFi。
     *   网页里的下拉列表要靠 esp_wifi_scan_get_ap_records 拿周围 AP。*/
    esp_err_t e = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "切 APSTA 失败: %s", esp_err_to_name(e));
        return e;
    }
    e = esp_wifi_set_config(WIFI_IF_AP, &wc);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "AP 参数失败: %s", esp_err_to_name(e));
        return e;
    }
    ESP_LOGI(TAG, "SoftAP 已开：%s  ->  http://%s/", s_ssid, APP_PROV_IP_STR);
    return ESP_OK;
}

static void ap_stop(void)
{
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (n) esp_netif_destroy(n);
}

/* ============================================================
 *  2) HTTP
 * ============================================================ */
/* ⚠️ 这段 HTML 是【一整块字符串】，所以：
 *   - 里面的双引号必须转义（\"），单引号不用；
 *   - 不能出现中文全角标点混进 JS 表达式（会语法错、整页瘫）；
 *   - 改动时整块替换，别用 Edit 局部改 —— 少一个转义就是白屏。
 *   v1.24 初版就踩过一次：写 `j.m||'…')` 时把半角括号打成了全角「（」，
 *   页面直接不响应。所以下面这段是修好之后的版本，且已在串口留了
 *   「html 长度」日志，改完对一下数字就知道有没有被截断。 */
static const char PROV_HTML[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content=\"width=device-width,initial-scale=1\">"
"<title>拾声集 配网</title><style>"
"*{box-sizing:border-box}"
"body{margin:0;background:#0B0E12;color:#F2F5F8;"
"font:16px/1.6 -apple-system,'PingFang SC','Microsoft YaHei',sans-serif;padding:20px}"
"h1{font-size:20px;color:#34D399;margin:0 0 2px}"
"p.sub{color:#7C8894;font-size:13px;margin:0 0 18px}"
"label{display:block;font-size:13px;color:#7C8894;margin:14px 0 6px}"
"select,input{width:100%;padding:12px;border-radius:8px;border:1px solid #2A323C;"
"background:#151A21;color:#F2F5F8;font-size:16px}"
"button{width:100%;margin-top:22px;padding:14px;border:0;border-radius:8px;"
"background:#34D399;color:#06231A;font-size:17px;font-weight:600}"
"button:disabled{background:#2A323C;color:#7C8894}"
"#st{margin-top:16px;font-size:14px;color:#FBBF24;min-height:22px}"
"#tip{margin-top:24px;font-size:12px;color:#5A6672;line-height:1.7}"
".hide{display:none}"
"</style></head><body>"
"<h1>拾声集 XianDial Suite</h1><p class=sub>选一下你家的 WiFi，填密码就能连</p>"
"<label>WiFi 名称</label><select id=ss></select>"
"<div id=man class=hide><label>手动输入名称</label><input id=mss "
"placeholder=\"例如 ChinaNet-8f2a\"></div>"
"<label>密码（开放网络留空）</label><input id=pw type=password "
"autocapitalize=none autocorrect=off>"
"<button id=go>连接</button><div id=st></div>"
"<div id=tip>连上后这台机器会自己关掉热点、记住这个网络，以后开机自动连。"
"<br>&copy; 永远的兰兰</div>"
"<script>"
"var ss=document.getElementById('ss'),mss=document.getElementById('mss'),"
"pw=document.getElementById('pw'),go=document.getElementById('go'),"
"st=document.getElementById('st'),man=document.getElementById('man');"
"function draw(a){ss.innerHTML='';var o=document.createElement('option');"
"o.value='';o.textContent='- 请选择 -';ss.appendChild(o);"
"a.forEach(function(n){var e=document.createElement('option');e.value=n.s;"
"e.textContent=n.s+'  ('+n.q+')';ss.appendChild(e)});"
"var m=document.createElement('option');m.value='__manual__';"
"m.textContent='> 列表里没有？手动输入';ss.appendChild(m)}"
"function load(){fetch('/ssids').then(function(r){return r.json()})"
".then(function(j){draw(j.a||[])}).catch(function(){})}"
"ss.onchange=function(){man.className=(ss.value=='__manual__')?'':'hide'};"
"go.onclick=function(){var s=(ss.value=='__manual__')?(mss.value.trim()):ss.value;"
"if(!s){st.textContent='先选一个 WiFi';return}"
"go.disabled=true;st.textContent='正在连接...';"
"fetch('/connect',{method:'POST',headers:{'Content-Type':"
"'application/x-www-form-urlencoded'},"
"body:'ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(pw.value)})"
".then(function(){poll()}).catch(function(){st.textContent='提交失败，请重试';"
"go.disabled=false})};"
"var t=null;function poll(){if(t)clearTimeout(t);t=setTimeout(function(){"
"fetch('/state').then(function(r){return r.json()}).then(function(j){"
"if(j.r==2){st.innerHTML='<b>连上了！</b> 可以关掉这个网页了';}"
"else if(j.r==3){st.textContent='没连上：'+(j.m||'密码不对或找不到网络')+' 请检查后重试';"
"go.disabled=false;ss.value='';load();}"
"else{st.textContent='正在连接...';poll()}})"
".catch(function(){poll()})},1500)}"
"ss.onchange();load();"
"</script></body></html>";

/* JSON 字符串转义：SSID 里可能有 " \ 控制字符，直接塞进 JSON 会破格式 */
static void json_escape(const char *in, char *out, int outsz)
{
    int o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o < outsz - 8; p++) {
        if (*p == '"' || *p == '\\') {
            out[o++] = '\\';
            out[o++] = (char)*p;
        } else if (*p < 0x20) {
            o += snprintf(out + o, (size_t)(outsz - o), "\\u%04x", (unsigned)*p);
        } else {
            out[o++] = (char)*p;
        }
    }
    out[o] = '\0';
}

/* 表单是 application/x-www-form-urlencoded：把 %XX 与 + 还原 */
static void url_decode(const char *in, char *out, int outsz)
{
    int o = 0;
    while (*in && o < outsz - 1) {
        if (*in == '+') {
            out[o++] = ' ';
            in++;
        } else if (*in == '%' && in[1] && in[2]) {
            char h[3] = { in[1], in[2], 0 };
            out[o++] = (char)strtol(h, NULL, 16);
            in += 3;
        } else {
            out[o++] = *in++;
        }
    }
    out[o] = '\0';
}

static const char *signal_grade(int rssi)
{
    if (rssi >= -55) return "强";
    if (rssi >= -70) return "中";
    if (rssi >= -82) return "弱";
    return "很弱";
}

static esp_err_t h_root(httpd_req_t *r)
{
    ESP_LOGI(TAG, "GET %s", r->uri);
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, PROV_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_ssids(httpd_req_t *r)
{
    /* 24 条 × (32 字节 SSID 转义后可能翻倍) ⇒ 2 KB 够。放 PSRAM，别抢内部 RAM。*/
    char *buf = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(2048);
    if (!buf) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");

    int n = snprintf(buf, 2048, "{\"a\":[");
    int cnt = 0;
    /* 上界写 24 而不是 AP_MAX(16)：那个宏在 app_sys.c 里，这里拿不到。
     * 24 是安全的 —— 越界时 app_net_scan_get() 返回 false，这里就 break。
     * ★ 别把这个上界当成「能显示 24 个网络」，实际由 s_ap_count 决定。*/
    for (int i = 0; i < 24; i++) {
        char raw[40] = "", esc[120] = "";
        int rssi = 0;
        if (!app_net_scan_get(i, raw, sizeof(raw), &rssi)) break;
        if (n > 1800) break;
        json_escape(raw, esc, sizeof(esc));
        n += snprintf(buf + n, 2048 - (size_t)n, "%s{\"s\":\"%.100s\",\"q\":\"%s\"}",
                      (cnt ? "," : ""), esc, signal_grade(rssi));
        cnt++;
    }
    n += snprintf(buf + n, 2048 - (size_t)n, "],\"n\":%d}", cnt);
    ESP_LOGI(TAG, "GET /ssids -> %d 条", cnt);

    httpd_resp_set_type(r, "application/json; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(r, buf, (int)strlen(buf));
    free(buf);
    return e;
}

static esp_err_t h_state(httpd_req_t *r)
{
    int r0 = s_result;
    if (r0 == PROV_RESULT_DOING) {
        if (app_net_connected()) r0 = PROV_RESULT_OK;
        else if (app_net_link_state() == APP_NET_FAIL) r0 = PROV_RESULT_FAIL;
    }
    char buf[200] = "";
    snprintf(buf, sizeof(buf),
             "{\"r\":%d,\"m\":\"%.60s\",\"ip\":\"%.15s\",\"ssid\":\"%.32s\"}",
             r0, app_net_reason_str(app_net_last_reason()),
             app_net_ip_str(), app_net_ssid());
    httpd_resp_set_type(r, "application/json; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, buf, (int)strlen(buf));
}

static void result_task(void *arg)
{
    (void)arg;
    /* 盯 45 秒。连上了就报成功，等 20 秒再关热点（页面已经显示成功）*/
    for (int i = 0; i < 45; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (app_net_connected()) {
            s_result = PROV_RESULT_OK;
            ESP_LOGI(TAG, "配网成功 ip=%s ssid=%s", app_net_ip_str(), app_net_ssid());
            for (int k = 0; k < APP_PROV_AUTOCLOSE_S; k++) vTaskDelay(pdMS_TO_TICKS(1000));
            ESP_LOGI(TAG, "自动关闭配网热点");
            app_prov_stop();
            vTaskDelete(NULL);
            return;
        }
        if (app_net_link_state() == APP_NET_FAIL) {
            s_result = PROV_RESULT_FAIL;
            ESP_LOGW(TAG, "配网失败：%s", app_net_reason_str(app_net_last_reason()));
            vTaskDelete(NULL);
            return;
        }
    }
    s_result = PROV_RESULT_FAIL;
    ESP_LOGW(TAG, "配网超时（45 秒）");
    vTaskDelete(NULL);
}

static esp_err_t h_connect(httpd_req_t *r)
{
    char body[512] = "";
    int  got = 0;
    while (got < (int)sizeof(body) - 1) {
        int n = httpd_req_recv(r, body + got, sizeof(body) - 1 - (size_t)got);
        if (n < 0) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "recv");
        if (n == 0) break;                    /* 收完了 */
        got += n;
    }
    body[got] = '\0';

    char ssid[80] = "", pass[80] = "";
    char *p = body;
    while (p && *p) {
        char *amp = strchr(p, '&');
        if (amp) *amp = '\0';
        char *eq = strchr(p, '=');
        if (eq) {
            *eq = '\0';
            if      (!strcmp(p, "ssid")) url_decode(eq + 1, ssid, sizeof(ssid));
            else if (!strcmp(p, "pass")) url_decode(eq + 1, pass, sizeof(pass));
        }
        p = amp ? amp + 1 : NULL;
    }

    /* 802.11 规定 SSID ≤ 32 字节、PSK ≤ 63 字节。超了就是脏输入，直接拒。
     * ⚠️ 这里判的是【字节数】不是字符数 —— 中文 SSID 一个字 3 字节。*/
    if (ssid[0] == '\0' || strlen(ssid) > 32 || strlen(pass) > 63) {
        s_result = PROV_RESULT_BADARG;
        ESP_LOGW(TAG, "connect: 参数不合法（ssid %u 字节 / pass %u 字节）",
                 (unsigned)strlen(ssid), (unsigned)strlen(pass));
        httpd_resp_set_type(r, "application/json; charset=utf-8");
        return httpd_resp_send(r, "{\"r\":4}", 7);
    }

    ESP_LOGI(TAG, "网页提交：ssid=\"%s\"，密码 %s（%u 字节）",
             ssid, pass[0] ? "已填" : "空（开放网络）", (unsigned)strlen(pass));
    s_result = PROV_RESULT_DOING;
    esp_err_t cr = app_net_connect_creds(ssid, pass);
    ESP_LOGI(TAG, "app_net_connect_creds -> %s", esp_err_to_name(cr));
    if (cr != ESP_OK) s_result = PROV_RESULT_FAIL;
    else xTaskCreate(result_task, "xs_provres", 3072, NULL, 4, NULL);

    httpd_resp_set_type(r, "application/json; charset=utf-8");
    return httpd_resp_send(r, "{\"r\":1}", 7);
}

static esp_err_t h_wild(httpd_req_t *r)
{
    /* captive portal 探测路径（/hotspot-detect.html、/generate_204、
     * /ncsi.txt …）全都落到这里。返回 200 + 我们的页面 = 系统判定
     * 「这个网络需要登录」→ 自动弹出。 */
    return h_root(r);
}

static esp_err_t http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = APP_PROV_PORT;
    cfg.max_open_sockets = 3;      /* 平时只有一部手机；省内存 */
    cfg.max_uri_handlers = 6;
    cfg.max_resp_headers = 4;
    cfg.lru_purge_enable = true;   /* 断开后立刻回收会话，省内存 */
    cfg.stack_size       = 4096;
    cfg.recv_wait_timeout = 6;
    cfg.send_wait_timeout = 6;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;

    esp_err_t e = httpd_start(&s_httpd, &cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(e));
        return e;
    }
    /* ⚠️ 注册顺序 = 匹配优先级（httpd_uri_to_handler 从下标 0 往后遍历，
     *   命中第一个就返回）。所以具体的先注册，通配那一条最后。
     *   ⚠️ 中文块注释里别写「斜杠 + 星号」这种字面量（讲通配符时很容易顺手写上）：
     *   gcc 的 -Werror=comment 会报「注释内部又出现注释开始符」，
     *   而本文件的注释正好在讲通配符，很容易反复踩。
     *   需要写字面量时，改成「斜杠+星号」或拆成两段字符串。*/
    static httpd_uri_t u;
    u.uri = "/connect"; u.method = HTTP_POST; u.handler = h_connect; u.user_ctx = NULL;
    httpd_register_uri_handler(s_httpd, &u);
    u.uri = "/ssids";   u.method = HTTP_GET;  u.handler = h_ssids;   u.user_ctx = NULL;
    httpd_register_uri_handler(s_httpd, &u);
    u.uri = "/state";   u.method = HTTP_GET;  u.handler = h_state;   u.user_ctx = NULL;
    httpd_register_uri_handler(s_httpd, &u);
    u.uri = "/";        u.method = HTTP_GET;  u.handler = h_root;    u.user_ctx = NULL;
    httpd_register_uri_handler(s_httpd, &u);
    u.uri = "/*";       u.method = HTTP_GET;  u.handler = h_wild;    u.user_ctx = NULL;
    httpd_register_uri_handler(s_httpd, &u);
    ESP_LOGI(TAG, "httpd 已起（:80，5 个 URI），首页 %.1f KB",
             (double)strlen(PROV_HTML) / 1024.0);
    return ESP_OK;
}

/* ============================================================
 *  3) DNS —— captive portal 的关键
 * ============================================================ */
/* 只做 A 记录、只回自己的 IP，别的不解析。
 * 系统探测 captive.apple.com / connectivitycheck.gstatic.com 时，
 * 拿到的就是 192.168.4.1，于是请求落到我们的 HTTP 上。 */
static void dns_task(void *arg)
{
    (void)arg;
    uint8_t q[512];
    uint8_t rsp[640];

    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s < 0) {
        ESP_LOGE(TAG, "dns socket: %d", s);
        s_dns_alive = 0;
        vTaskDelete(NULL);
        return;
    }
    s_dns_sock = s;

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_port        = htons(PROV_DNS_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        ESP_LOGE(TAG, "dns bind :%d 失败", PROV_DNS_PORT);
        close(s);
        s_dns_sock = -1;
        s_dns_alive = 0;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "DNS 劫持已起（:%d，所有 A 查询 -> %s）",
             PROV_DNS_PORT, APP_PROV_IP_STR);
    s_dns_alive = 1;

    while (s_dns_alive) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s, q, sizeof(q), 0, (struct sockaddr *)&from, &fl);
        if (n < 12) {
            if (n < 0) vTaskDelay(pdMS_TO_TICKS(20));   /* 让退出标志有机会生效 */
            continue;
        }

        int qd = (q[4] << 8) | q[5];
        if (qd < 1) continue;
        /* 走一遍 question，找 QTYPE。名字是变长的 label 序列。*/
        int off = 12;
        while (off < n && q[off] != 0) off += 1 + q[off];
        off += 1;                       /* 那个结尾的 0 */
        if (off + 4 > n) continue;
        int qtype = (q[off] << 8) | q[off + 1];
        int qend  = off + 4;            /* question 结束位置 */

        /* 回包 = 头（改 flags/count） + 原 question + 答案 */
        int o = 0;
        rsp[o++] = q[0]; rsp[o++] = q[1];            /* Transaction ID 照抄 */
        rsp[o++] = (uint8_t)0x81;                    /* QR=1 RD=1 */
        rsp[o++] = (uint8_t)0x80;                    /* RA=1 RCODE=0 */
        rsp[o++] = 0; rsp[o++] = 1;                 /* QDCOUNT = 1 */
        int is_a = (qtype == 1);
        rsp[o++] = 0; rsp[o++] = (uint8_t)(is_a ? 1 : 0);   /* ANCOUNT */
        rsp[o++] = 0; rsp[o++] = 0;                 /* NSCOUNT */
        rsp[o++] = 0; rsp[o++] = 0;                 /* ARCOUNT */
        memcpy(rsp + o, q + 12, (size_t)(qend - 12));
        o += qend - 12;

        if (is_a) {
            rsp[o++] = (uint8_t)0xC0; rsp[o++] = 0x0C;  /* 名字压缩指针 -> 头部 */
            rsp[o++] = 0; rsp[o++] = 1;                 /* TYPE  = A  */
            rsp[o++] = 0; rsp[o++] = 1;                 /* CLASS = IN */
            rsp[o++] = 0; rsp[o++] = 0; rsp[o++] = 0; rsp[o++] = 60;  /* TTL */
            rsp[o++] = 0; rsp[o++] = 4;                 /* RDLENGTH */
            rsp[o++] = 192; rsp[o++] = 168;
            rsp[o++] = 4;   rsp[o++] = 1;
        }
        /* AAAA 一律回「无答案」：让客户端退回 IPv4，而不是卡住。*/
        sendto(s, rsp, (size_t)o, 0, (struct sockaddr *)&from, fl);
    }
    close(s);
    s_dns_sock = -1;
    ESP_LOGI(TAG, "DNS 任务退出");
    vTaskDelete(NULL);
}

/* ============================================================
 *  4) 对外
 * ============================================================ */
esp_err_t app_prov_start(void)
{
    if (s_on) return ESP_OK;

    s_result = PROV_RESULT_NONE;
    esp_err_t e = ap_start();
    if (e != ESP_OK) return e;

    e = http_start();
    if (e != ESP_OK) {
        ap_stop();
        return e;
    }
    if (!s_dns_alive) {
        if (xTaskCreate(dns_task, "xs_dns", 3072, NULL, 4, NULL) != pdPASS) {
            ESP_LOGE(TAG, "DNS 任务建不起来");
            httpd_stop(s_httpd);
            s_httpd = NULL;
            ap_stop();
            return ESP_FAIL;
        }
    }
    s_on = true;
    ESP_LOGI(TAG, "配网已开启：连热点 %s，浏览器会自动弹页；内部剩余 %u 字节",
             s_ssid, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

void app_prov_stop(void)
{
    if (!s_on && !s_httpd) return;
    s_on = false;

    /* ★ 退出 DNS 任务：光置标志没用 —— 它正阻塞在 recvfrom 里。
     *   用 shutdown() 把它叫醒，它看到 n<0 就会去检查标志。*/
    s_dns_alive = 0;
    if (s_dns_sock >= 0) shutdown(s_dns_sock, SHUT_RDWR);

    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    ap_stop();
    ESP_LOGI(TAG, "配网已关闭");
}

bool app_prov_is_active(void) { return s_on; }

const char *app_prov_ssid(void) { return s_on ? s_ssid : ""; }

int app_prov_clients(void)
{
    /* AP 上的关联站点数 —— 只用来打日志自证「手机到底连上没有」。*/
    wifi_sta_list_t lst = { 0 };
    if (esp_wifi_ap_get_sta_list(&lst) != ESP_OK) return 0;
    return (int)lst.num;
}

int  app_prov_result(void)        { return s_result; }
void app_prov_clear_result(void)  { s_result = PROV_RESULT_NONE; }
