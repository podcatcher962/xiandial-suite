/*
 * 拾声 · 天气时钟 App —— 实现   v1.0
 * ============================================================
 *  板型：xs35（ESP32-S3 / ST77922 320×480 竖屏 / FT6336G）
 *  数据：http://t.weather.itboy.net/api/weather/city/101020100（上海）
 *
 *  ── 一条必须说清的架构约束 ──────────────────────────────
 *  ★★★ 取数【绝不能】在 LVGL 任务里同步做。
 *     xs_http_get() 最长会阻塞 8 秒（超时），而 LVGL 任务负责
 *     触摸采样 + 整屏渲染 —— 在它里面等 8 秒，界面就是"点哪都没
 *     反应"的假死，而且连"正在加载"都画不出来（因为画不动）。
 *     所以这里用【独立取数任务 + 共享数据 + LVGL 侧定时器读】：
 *
 *         wx_worker（自己的任务，优先级 2 < LVGL 的 3）
 *              │  xs_http_get() 阻塞取数（8 秒也是它自己扛）
 *              ▼
 *         s_wx（受 s_mtx 保护的共享数据）  ← 只在这里写
 *              ▼
 *         wx_tick_cb（LVGL 定时器，500ms）→ 读 → 刷 label
 *
 *     好处是【LVGL 侧完全不碰锁以外的同步原语】，UI 更新永远发生在
 *     LVGL 任务里，不存在"从别的任务改控件"的悬空风险。
 *
 *  ── 退出时的竞态 ─────────────────────────────────────────
 *  ★★ s_alive 的读写在锁内做：worker 写完数据前要再确认自己还"活着"，
 *     否则会出现"用户已经退回主菜单了，worker 把一个已经删掉的屏
 *     的 label 指针更新了一遍"——那是随机崩溃，最难查。
 *     leave() 拿锁置 s_alive=false 并清 dirty，worker 拿锁才写数据，
 *     两者互斥 ⇒ 不存在"清完又被写脏"的窗口。
 *
 *  ── 视觉 ─────────────────────────────────────────────────
 *  深色渐变底 + 圆角卡片（现代卡片风），刻意与灵签的宣纸古风区分。
 *  兰兰 10-08：「天气时钟 UI 设计潮流一点」。
 * ============================================================ */
#include "ui_weather.h"

#include "ui_shell.h"           /* ui_shell_back()：底部「主 页」按钮 */
#include "app_sys.h"            /* app_net_connected()：顶栏在线状态 */
#include "xs_http.h"            /* 明文 HTTP + 迷你 JSON */
#include "voice_speak.h"        /* 语音播报（10-08 加：播报当前时间/天气）*/

#include <stdio.h>
#include <stdlib.h>             /* atoi */
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "WXUI";

/* ---------- 字体（与灵签共用一套霞鹜文楷，_gen_fonts_lq.py 产出）---------- */
extern const lv_font_t xs_font_14;
extern const lv_font_t xs_font_18;
extern const lv_font_t xs_font_22;
extern const lv_font_t xs_font_30;

/* ★★ 10-08 晚：时钟专用 72px 字库（只含 0-9 : -，12 个字形 / 约 31 KB）。
 *   为什么不复用 xs_font_30：那是主档最大的字号，而 30px 的 "20:15"
 *   在这块 320px 宽的屏上只有约 75px 宽 —— 撑不起"这页最重要的元素"。
 *   72px 下 "20:15" 宽 198px（= 2.75 em × 72，实测字体 advance），
 *   左右各留 61px，正好当主视觉。
 *   ⚠️ 它【只】有数字与冒号：别拿它去显示天气词或日期，会满屏方块。
 *      由 _gen_fonts_lq.py 的 EXTRA_JOBS 生成，符号名见那里。*/
extern const lv_font_t xs_font_clk72;

#define F_CLOCK (&xs_font_clk72)  /* 时钟专用（仅数字/冒号/减号）*/
#define F_HERO  (&xs_font_30)   /* 大温度（CJK 主档最大档）*/
#define F_READ  (&xs_font_22)   /* 天气词、格内数值 */
#define F_MID   (&xs_font_18)   /* 卡片标题、预报行 */
#define F_SMALL (&xs_font_14)   /* 说明小字 */

/* ---------- 数据源 ----------
 * ★ 城市码是"中国天气网"那套 9 位码。换城市只改这一行：
 *     上海 101020100 ／ 北京 101010100 ／ 广州 101280101
 *   码表：http://www.weather.com.cn/ 搜城市，URL 里那个 9 位数。*/
#define WX_CITY_CODE   "101020100"
#define WX_URL         "http://t.weather.itboy.net/api/weather/city/" WX_CITY_CODE

#define WX_REFRESH_S   600      /* 刷新周期：10 分钟（itboy 自身约 8~10 分钟一更）*/
#define WX_BODY_CAP    4096     /* 实测整包 3.1 KB，留 4 KB（PSRAM 里，不占内部 RAM）*/

/* ---------- 彩色渐变 + 玻璃卡片（10-08 兰兰：背景彩色 / 字体彩色 / 灵动）----------
 * ★ 上一版是"夜色深蓝"，安全但保守。这一版改成：
 *     背景 = 紫 → 青蓝 的彩色渐变，再叠两个缓慢漂移的彩色光斑（极光感）；
 *     卡片 = 半透明白玻璃（透出底色，所以背景的颜色会透过卡片显现）；
 *     字体 = 【每个语义一处色】，不是"每个字一个颜色"。
 *   城市青、时钟白、温度暖黄、天气词青绿、湿度蓝、空气绿、PM 橙 ——
 *   颜色承担信息，跟灵签的「宫位五行色」是同一个思路。*/
#define C_BG_TOP   lv_color_hex(0x4A1E8C)   /* 顶：紫 */
#define C_BG_MID   lv_color_hex(0x24386E)   /* 中：靛 */
#define C_BG_BOT   lv_color_hex(0x0B6E9E)   /* 底：青蓝 */
#define C_GLASS    lv_color_hex(0xFFFFFF)   /* 玻璃卡片：白 + 低不透明度 */
#define C_GLASS_OPA LV_OPA_20
#define C_EDGE     lv_color_hex(0xC9A8FF)   /* 描边：淡紫 */
#define C_TXT      lv_color_hex(0xFFFFFF)   /* 主文字 */
#define C_DIM      lv_color_hex(0xC7D6F5)   /* 次文字 */
#define C_CITY     lv_color_hex(0x7FE7FF)   /* 城市：亮青（兰兰：字大一点）*/
#define C_TEMP     lv_color_hex(0xFFD166)   /* 温度：暖黄 */
#define C_COND     lv_color_hex(0x8DFFC2)   /* 天气词：青绿 */
#define C_HUM      lv_color_hex(0x8FD8FF)   /* 湿度：天蓝 */
#define C_AIR      lv_color_hex(0x9BF7B4)   /* 空气：嫩绿 */
#define C_PM       lv_color_hex(0xFFB870)   /* PM2.5：暖橙 */
#define C_ACC      lv_color_hex(0x9FE8FF)   /* 强调（状态字、预报温度）*/

#define SCR_W      320
#define SCR_H      480

/* ---------- 共享数据 ---------- */
typedef struct {
    bool ok;                    /* 是否拿到过有效数据 */
    char city[24];              /* 上海市 */
    char temp[12];              /* 25.0 */
    char cond[24];              /* 晴（比 fc[].cond 大一档，见下）*/
    char wind[20];              /* 东北风 */
    char humid[12];             /* 33% */
    char air[16];               /* 良 */
    char pm[12];                /* 34 */
    /* ★★ date/hl 开得比"看起来需要"的大一截，是被编译器逼的：
     *   gcc 的 -Werror=format-truncation 按【最坏情况】估算 snprintf ——
     *   "°.%s°" 里那个 '°' 是 3 字节 UTF-8，两个 %.s 各按源缓冲上限算，
     *   它会得出"可能写 36~40 字节"，于是 20/24 字节的目标直接判错编不过。
     *   实际内容（"10月8日 星期四" 21 字节、"25°/16°" 11 字节）远小于此，
     *   但编译器不认账 —— 这里选择【扩缓冲】而不是加精度截断，
     *   因为 L"%.5s" 这类按字节截断会把多字节汉字劈成乱码。*/
    char date[56];              /* 10月8日 星期四 */
    char upd[24];               /* 更新 14:46 */
    struct {
        char day[16];           /* 今天 / 明天 / 后天 */
        char cond[20];          /* 晴 */
        char hl[48];            /* 25°/16° */
    } fc[3];
} wx_data_t;

static wx_data_t         s_wx;                 /* 共享数据（s_mtx 保护）*/
static SemaphoreHandle_t s_mtx;                /* 保护 s_wx / s_alive / s_dirty */
static volatile bool     s_alive;              /* App 是否在屏上 */
static volatile bool     s_dirty;              /* worker 有新数据待上屏 */
static volatile bool     s_force;              /* enter 时置位 ⇒ worker 立刻取一次 */
static TaskHandle_t      s_task;
static lv_timer_t       *s_tick;
static TickType_t        s_enter_tick;         /* enter 时刻，用于"5 秒还没数据"的提示 */

/* ---------- 界面对象（leave 时全部置 NULL）---------- */
static lv_obj_t *s_scr;
static lv_obj_t *l_city, *l_net;
static lv_obj_t *l_clock, *l_date;
static lv_obj_t *l_temp, *l_cond, *l_wind;
static lv_obj_t *l_humid, *l_air, *l_pm;
static lv_obj_t *l_fc_day[3], *l_fc_cond[3], *l_fc_hl[3];
static lv_obj_t *l_upd;
static char      s_clock_cache[8] = {0};

/* 背景彩色光斑（灵动）+ 驱动它的 timer */
static lv_obj_t *l_glow1, *l_glow2;
static lv_timer_t *s_anim;
static int         s_anim_t;

/* ============================================================
 *  ★★★ 天气用字的【字库清单】
 * ============================================================
 *  下面这个字符串运行时【一个字节都用不到】，它存在的唯一目的是
 *  让 firmware/_gen_fonts_lq.py 把这些字烘进固件。
 *
 *  ★ 为什么必须有它：gen_fonts.py 扫源码取字，但会【先剥掉注释】——
 *    所以"天气词"如果只写在注释里，字库里就没有，真机遇上一场
 *    "雷阵雨伴有冰雹" 就是一排豆腐块。这不是假想：10-08 加多产品
 *    外壳时就因为没重跑生成器，主菜单上「股□行情」缺了个"票"字。
 *  ★ 为什么不靠"接口返回什么就有什么"：字库是编译期烘死的，
 *    运行期不可能补字。只能把中国天气网可能吐出来的天气现象列全。
 *  ⚠ 改这里之后必须重跑 _gen_fonts_lq.py，否则等于没写。
 * ============================================================ */
const char XS_WX_FONT_CHARS[] =
    /* 城市名（换城市只改 WX_CITY_CODE，但城市名要在字库里）*/
    "上海市北京广州深圳杭州天津重庆成都武汉南京苏州"
    /* 顶栏 / 底栏 / 状态文案 */
    "在线离线载入中暂无数据获取失败稍后自动重试网络未连接"
    "月日星期更新主页面度"
    /* 三格标题 */
    "湿度空气优良轻度中度重度严重污染"
    /* 预报行的日序 */
    "今天明天后天大前"
    /* 天气现象（中国天气网全套 + 常见组合）*/
    "晴多云阴小中大暴阵雷夹雨雪雾霾浮尘扬沙尘冰雹冻特转"
    /* 风向 */
    "东南西北无持续向微和缓级"
    /* 语音播报按钮与播报用字（10-08 加；发音是预生成的 mp3，不靠字库，
     * 但按钮上的「播 报」两个字必须进字库，否则首页到天气页一路豆腐块）*/
    "播报现在是北京时间点分"
    /* 符号（0x20-0x7F 之外的都要显式列）*/
    "℃°";

/* ============================================================
 *  UI 小工具（照 ui_shell.c / ui_lingqian.c 那套写）
 * ============================================================ */
static lv_obj_t *wbox(lv_obj_t *par, int x, int y, int w, int h,
                      lv_color_t color, int radius)
{
    lv_obj_t *o = lv_obj_create(par);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *wcard(lv_obj_t *par, int x, int y, int w, int h,
                       lv_color_t fill, int radius)
{
    lv_obj_t *o = wbox(par, x, y, w, h, fill, radius);
    /* ★ 玻璃观感 = 白底 + 低不透明度：底下的彩色渐变会透上来，
     *   所以整屏看着是"彩色玻璃叠彩色背景"，而不是"几个灰盒子"。*/
    lv_obj_set_style_bg_opa(o, C_GLASS_OPA, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, C_EDGE, 0);
    lv_obj_set_style_border_opa(o, LV_OPA_40, 0);
    return o;
}

static lv_obj_t *wlabel(lv_obj_t *par, int x, int y, const lv_font_t *font,
                        lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(par);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, x, y);
    return l;
}

/* 定宽居中 label：宽度限死 + 文本居中 ⇒ 值变长变短都不会带着位置跑，
 * 三格/预报行的对齐全靠它。x0 = 相对父对象的左边界。*/
static lv_obj_t *wclabel(lv_obj_t *par, int y, int w, int x0,
                         const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = wlabel(par, x0, y, font, color, text);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

/* ============================================================
 *  解析
 * ============================================================ */

/* "2026-10-08" → "10月8日"（不带前导零，像人说话）*/
static void fmt_ymd(const char *ymd, char *out, size_t cap)
{
    int y = 0, m = 0, d = 0;
    if (ymd && sscanf(ymd, "%d-%d-%d", &y, &m, &d) == 3)
        snprintf(out, cap, "%d月%d日", m, d);
    else
        snprintf(out, cap, "%s", ymd ? ymd : "");
}

/* "2026-10-08 14:46:40" → "更新 14:46" */
static void fmt_upd(const char *t, char *out, size_t cap)
{
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (t && sscanf(t, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6)
        snprintf(out, cap, "更新 %02d:%02d", h, mi);
    else
        snprintf(out, cap, "更新 --:--");
}

/* 把 JSON 拆成 wx_data_t。任一关键字段缺失即判失败（宁可显示旧数据，
 * 也不要把半截数据画上去 —— 半截数据看着"有内容"，最难发现是错的）。*/
static bool wx_parse(const char *js, wx_data_t *d)
{
    char buf[256];

    memset(d, 0, sizeof(*d));

    if (!xs_json_get(js, "city", d->city, sizeof(d->city)))      return false;
    if (!xs_json_get(js, "wendu", d->temp, sizeof(d->temp)))     return false;
    if (!xs_json_get(js, "shidu", d->humid, sizeof(d->humid)))   return false;
    if (!xs_json_get(js, "quality", d->air, sizeof(d->air)))     return false;
    (void)xs_json_get(js, "pm25", d->pm, sizeof(d->pm));         /* PM2.5 偶有缺，不致命 */

    /* 顶层 time = "2026-10-08 12:19:40" */
    if (xs_json_get(js, "time", buf, sizeof(buf))) fmt_upd(buf, d->upd, sizeof(d->upd));
    else snprintf(d->upd, sizeof(d->upd), "更新 --:--");

    /* 未来 3 天。forecast[0] 就是今天，用作日期与今天的天气词，
     * 卡片上方显示的今天温度取自 wendu（实时），比预报的区间更贴。*/
    static const char *DAY_CH[3] = { "今天", "明天", "后天" };
    for (int i = 0; i < 3; i++) {
        char el[320];
        snprintf(d->fc[i].day, sizeof(d->fc[i].day), "%s", DAY_CH[i]);

        if (!xs_json_elem(js, "forecast", i, el, sizeof(el))) return false;

        (void)xs_json_get(el, "type", d->fc[i].cond, sizeof(d->fc[i].cond));
        (void)xs_json_get(el, "fengxiang", d->wind, sizeof(d->wind));

        char hi[16] = {0}, lo[16] = {0};
        if (xs_json_get(el, "high", buf, sizeof(buf))) xs_pick_num(buf, hi, sizeof(hi));
        if (xs_json_get(el, "low",  buf, sizeof(buf))) xs_pick_num(buf, lo, sizeof(lo));
        snprintf(d->fc[i].hl, sizeof(d->fc[i].hl), "%s°/%s°", hi, lo);

        if (i == 0) {
            /* 日期 + 星期，只在第一元素里取 */
            char ymd[24] = {0}, wk[16] = {0}, dpart[24] = {0};
            (void)xs_json_get(el, "ymd", ymd, sizeof(ymd));
            (void)xs_json_get(el, "week", wk, sizeof(wk));
            fmt_ymd(ymd, dpart, sizeof(dpart));
            snprintf(d->date, sizeof(d->date), "%s %s", dpart, wk);
            /* 卡片上的天气词也用今天的 */
            snprintf(d->cond, sizeof(d->cond), "%s", d->fc[0].cond);
        }
    }

    /* 全空 = 解析失败（接口改版/被劫持的最典型症状）*/
    if (d->city[0] == '\0' || d->temp[0] == '\0') return false;

    d->ok = true;
    return true;
}

/* ============================================================
 *  取数任务（常驻，首次进入 App 时创建）
 * ============================================================
 *  ★ 为什么常驻而不是"进一次建一个"：
 *     任务创建/销毁本身要抢内部 RAM，而且 HTTP 阻塞期间被 vTaskDelete
 *     会留下未关闭的 socket 与 lwIP 控制块 —— 反复进出就是慢性泄漏。
 *     常驻一个、用 s_alive/s_force 指挥它，进出多少次都只有一份栈。
 *  ★ 优先级 2：低于 LVGL 任务（3）。它是慢活（等待网络），
 *     绝不能因为"网络卡住"把界面渲染挤掉。
 * ============================================================ */
static void wx_worker(void *arg)
{
    (void)arg;

    /* ★ 接收缓冲放 PSRAM：4 KB 常驻也不吃内部 RAM（内部只有 ~97 KB）*/
    char *body = (char *)heap_caps_malloc(WX_BODY_CAP, MALLOC_CAP_SPIRAM);
    if (!body) {
        ESP_LOGE(TAG, "取数缓冲分配失败（PSRAM 不够？）");
        vTaskDelete(NULL);
        return;
    }

    TickType_t last = 0;
    const TickType_t gap = pdMS_TO_TICKS(WX_REFRESH_S * 1000);

    for (;;) {
        /* 不在屏上：短睡等召唤。★ 用短睡而不是挂起信号量，
         * 因为 worker 也可能在"取数中"被离开，醒来后必须能看到 alive 已变。*/
        if (!s_alive) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        TickType_t now = xTaskGetTickCount();
        bool due = s_force || last == 0 || (now - last) >= gap;
        if (!due) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        s_force = false;

        ESP_LOGI(TAG, "取天气 … (internal free=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

        int n = xs_http_get(WX_URL, body, WX_BODY_CAP);
        last = xTaskGetTickCount();

        /* ★ 取数期间用户可能已经退回主菜单 ⇒ 结果直接丢，别往已删的屏上写 */
        if (n <= 0) {
            ESP_LOGW(TAG, "取数失败（返回 %d）", n);
            continue;
        }

        wx_data_t d;
        if (!wx_parse(body, &d)) {
            ESP_LOGW(TAG, "解析失败（%d 字节），前 80 字：%.80s", n, body);
            continue;
        }

        /* ★★ 与 leave() 互斥：拿锁后才检查 alive 并落数据。
         *    这样"写数据"与"alive=false + 清 dirty"不会互相穿插。*/
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        if (s_alive) {
            s_wx = d;
            s_dirty = true;
        }
        xSemaphoreGive(s_mtx);

        if (s_alive) {
            ESP_LOGI(TAG, "天气 OK：%s %s° %s 湿度%s 空气%s",
                     d.city, d.temp, d.cond, d.humid, d.air);
        }
    }
}

/* ============================================================
 *  上屏
 * ============================================================ */
static void wx_apply(const wx_data_t *d)
{
    if (!d->ok) return;

    /* 有数据 = 在线（顶栏那个状态字）*/
    if (l_net) {
        lv_label_set_text(l_net, "在线");
        lv_obj_set_style_text_color(l_net, C_ACC, 0);
    }

    if (l_city)  lv_label_set_text(l_city, d->city);
    if (l_date)  lv_label_set_text(l_date, d->date);
    if (l_temp)  {
        char t[20];
        snprintf(t, sizeof(t), "%s°", d->temp);
        lv_label_set_text(l_temp, t);
    }
    if (l_cond)  lv_label_set_text(l_cond, d->cond);
    if (l_wind)  lv_label_set_text(l_wind, d->wind);
    if (l_humid) lv_label_set_text(l_humid, d->humid);
    if (l_air)   lv_label_set_text(l_air, d->air);
    if (l_pm)    lv_label_set_text(l_pm, d->pm[0] ? d->pm : "--");
    if (l_upd)   lv_label_set_text(l_upd, d->upd);

    for (int i = 0; i < 3; i++) {
        /* ★★ 三列一个都不能漏。第一版就是漏了 l_fc_day，
         *   真机截图里预报行最左边一直是建屏时的占位符 "—"，
         *   数据其实已经解析出来了（右边类型/温度都对），
         *   纯粹是"没往那个 label 里写"。这类漏项不会报错、不会崩，
         *   只能靠看截图发现 —— 所以每次加字段都要对着建屏清单数一遍。*/
        if (l_fc_day[i])  lv_label_set_text(l_fc_day[i], d->fc[i].day);
        if (l_fc_cond[i]) lv_label_set_text(l_fc_cond[i], d->fc[i].cond);
        if (l_fc_hl[i])   lv_label_set_text(l_fc_hl[i], d->fc[i].hl);
    }
}

/* LVGL 定时器：500ms 一跳。
 *  ① 顶栏时钟 —— 只在【分钟】变化时才 set_text（1 秒一次的重排没必要）
 *  ② 新数据上屏
 *  ★ 这个回调跑在 LVGL 任务里，是【唯一】碰控件的地方。
 *     worker 只写数据、不碰控件，两条路彻底分开。*/
static void wx_tick_cb(lv_timer_t *t)
{
    (void)t;

    /* ① 时钟 */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    /* ★ 缓冲 32 而不是 6：gcc 按 %02d 的【最坏 int 值】估 snprintf 长度
     *   （单个数最长 11 字节），开小了会被 -Werror=format-truncation 拦下。
     *   实际内容恒定 5 字节。*/
    char clk[32];
    snprintf(clk, sizeof(clk), "%02d:%02d", tm.tm_hour, tm.tm_min);
    if (strcmp(clk, s_clock_cache) != 0) {
        strncpy(s_clock_cache, clk, sizeof(s_clock_cache) - 1);
        if (l_clock) lv_label_set_text(l_clock, clk);
    }

    /* ② 新数据 */
    if (s_dirty) {
        wx_data_t d;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        d = s_wx;
        s_dirty = false;
        xSemaphoreGive(s_mtx);
        wx_apply(&d);
    }

    /* ③ 进屏 5 秒仍无数据 ⇒ 把"载入中"换成明确的提示。
     *    不加这一步，断网时界面永远停在"载入中"，用户以为是卡住了。
     *    ★ 分两种：网络本身就没连上 vs 网在但接口没回来 —— 前者要去
     *      配网，后者只要等，提示必须能区分，否则用户会白折腾配网。*/
    if (l_net && !s_wx.ok) {
        TickType_t el = xTaskGetTickCount() - s_enter_tick;
        if (el > pdMS_TO_TICKS(5000)) {
            bool netup = app_net_connected();
            lv_label_set_text(l_net, netup ? "获取失败" : "网络未连接");
            lv_obj_set_style_text_color(l_net, C_DIM, 0);
        }
    }
}

/* ============================================================
 *  建屏
 * ============================================================ */
/* ============================================================
 *  语音播报（10-08 兰兰：「添加语音播报功能，播报当前时间，天气」）
 * ============================================================
 *  ★ 板上没有 TTS 引擎，在线 TTS 也走不通（有道 dictvoice 连"上海市"
 *    都返 500，百度已改成要 token 的 JSON）。所以这里是
 *    【预生成语音原子 + 运行时拼装】：见 voice_speak.h / _gen_va.py。
 *
 *  ★★ 两条硬规矩（下面两个映射表就是照这个写的）：
 *    ① 【宁缺勿错】：城市/天气词在原子表里没有对应项就跳过那一段。
 *       少播一个词只是不完整；拿一个不相干的原子顶上，是"机器在说假话"。
 *    ② 【先长后短】：天气词的匹配必须从长到短试。"雷阵雨"若排在
 *       "阵雨""雨"后面，strstr 会先命中短的，把"雷阵雨"念成"雨"。
 * ============================================================ */

/* 城市 → 原子名。★ 这是【白名单】不是"任意城市都能播"：
 * 原子是离线生成的，多一个城市就要重跑 _gen_va.py 并重推 SD 卡。
 * 换 WX_CITY_CODE 时，若新城市不在表里，天气那段会照播（只是不报城市名）。*/
static const char *wx_city_atom(const char *city)
{
    if (!city) return NULL;
    if (strstr(city, "上海")) return "c_sh";
    if (strstr(city, "北京")) return "c_bj";
    if (strstr(city, "广州")) return "c_gz";
    if (strstr(city, "深圳")) return "c_sz";
    if (strstr(city, "杭州")) return "c_hz";
    return NULL;
}

/* 天气现象 → 原子名。★ 顺序即优先级：长词在前（见上面规矩 ②）。*/
static const char *wx_cond_atom(const char *cond)
{
    if (!cond || !cond[0]) return NULL;
    static const struct { const char *k, *a; } MAP[] = {
        { "雷阵雨", "w_leizhenyu" },
        { "雨夹雪", "w_yujiaxue"  },   /* 必须排在"雨"前面 */
        { "沙尘暴", "w_shachen"   },
        { "浮尘",   "w_fuchen"    },
        { "扬沙",   "w_yangsha"   },
        { "冰雹",   "w_bingbao"   },
        { "多云",   "w_duoyun"    },
        { "小雨",   "w_xiaoyu"    },   /* 必须排在"雨"前面 */
        { "中雨",   "w_zhongyu"   },
        { "大雨",   "w_dayu"      },
        { "暴雨",   "w_baoyu"     },
        { "阵雨",   "w_zhenyu"    },
        { "小雪",   "w_xiaoxue"   },   /* 必须排在"雪"前面 */
        { "中雪",   "w_zhongxue"  },
        { "大雪",   "w_daxue"     },
        { "霾",     "w_mai"       },
        { "雾",     "w_wu"        },
        { "晴",     "w_qing"      },
        { "阴",     "w_yin"       },
        { "雨",     "w_yu"        },
        { "雪",     "w_xue"       },
    };
    for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++)
        if (strstr(cond, MAP[i].k)) return MAP[i].a;
    return NULL;
}

/* 底部「播 报」按钮的回调。
 * ★ 组装全在 LVGL 任务里完成（只是几行 snprintf + 一个 16 元素指针数组），
 *   SD 读写与播放由 voice_speak 内部另起任务做，这里不会卡界面。*/
static void speak_cb(lv_event_t *e)
{
    (void)e;
    if (voice_speak_busy()) return;         /* 正在播，忽略重复点击 */

    wx_data_t d;
    if (s_mtx) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        d = s_wx;
        xSemaphoreGive(s_mtx);
    } else {
        d = s_wx;
    }

    /* ★ 缓冲给 16 而不是 4：gcc 按 %02d 的【最坏 int 值】估 snprintf 长度
     *   （单个数最长 11 字节），开小了会被 -Werror=format-truncation 拦下。
     *   实际内容恒定 3 字节（"n15"）。与 wx_tick_cb 里 clk[32] 同一个坑。*/
    char hh[16], mm[16], tp[16];
    const char *atoms[16];
    int n = 0;

    /* ① 时间：现在是 N 点 M 分 */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(hh, sizeof(hh), "n%02d", tm.tm_hour);
    snprintf(mm, sizeof(mm), "n%02d", tm.tm_min);
    atoms[n++] = "p_now";
    atoms[n++] = hh;
    atoms[n++] = "u_dian";
    atoms[n++] = mm;
    atoms[n++] = "u_fen";

    /* ② 天气：今天 上海 晴 N 度（没数据的天气时段直接不提，别念错）*/
    if (d.ok) {
        int t = atoi(d.temp);
        if (t >= 0 && t <= 99) {
            snprintf(tp, sizeof(tp), "n%02d", t);

            const char *ca = wx_city_atom(d.city);
            const char *wa = wx_cond_atom(d.cond);

            atoms[n++] = "p_today";
            if (ca) atoms[n++] = ca;
            if (wa) atoms[n++] = wa;
            atoms[n++] = tp;
            atoms[n++] = "u_du";
        }
        /* ★ t < 0（零下）时【不播温度】：现在没有「零下」这个原子，
         *   硬把 -3 念成"3 度"就是把冷天说成暖天，宁可不说。*/
    }

    if (voice_speak(atoms, n)) {
        ESP_LOGI(TAG, "播报请求 %d 段（%s）", n, d.ok ? "时间+天气" : "仅时间");
    } else {
        ESP_LOGW(TAG, "播报没能启动（原子没推卡？内存不够？）");
    }
}

static void home_cb(lv_event_t *e)
{
    (void)e;
    ui_shell_back();        /* 外壳负责"先切屏再销毁"，见 ui_shell.c 文件头 */
}

static void build_screen(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_scr);
    /* ★ 竖向彩色渐变：紫 → 青蓝。不设 grad 就是一块纯色，撑不起"彩色/灵动"。*/
    lv_obj_set_style_bg_color(s_scr, C_BG_TOP, 0);
    lv_obj_set_style_bg_grad_color(s_scr, C_BG_BOT, 0);
    lv_obj_set_style_bg_grad_dir(s_scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 背景彩色光斑：必须【最先建】----
     * ★ 顺序即层级：LVGL 后建的对象画在上面。这两个圆是背景装饰，
     *   建晚了会盖住时钟和卡片上的字（而且是半透明地盖，最难看出原因）。*/
    l_glow1 = wbox(s_scr, -40, 30, 220, 220, lv_color_hex(0xFF4FA3), 110);
    lv_obj_set_style_bg_opa(l_glow1, LV_OPA_20, 0);
    lv_obj_clear_flag(l_glow1, LV_OBJ_FLAG_CLICKABLE);
    l_glow2 = wbox(s_scr, 170, 250, 210, 210, lv_color_hex(0x4FFFD1), 105);
    lv_obj_set_style_bg_opa(l_glow2, LV_OPA_20, 0);
    lv_obj_clear_flag(l_glow2, LV_OBJ_FLAG_CLICKABLE);

    /* ---- 顶栏：城市 22px（兰兰「当前城市字体大一点」）+ 状态 ---- */
    l_city = wlabel(s_scr, 18, 6, F_READ, C_CITY, "— —");
    l_net  = wlabel(s_scr, 210, 10, F_SMALL, C_ACC, "载入中…");

    /* ---- 大时钟 ----
     * ★★★ 10-08 晚：30px → 72px（兰兰：「时钟字体太小了，时钟是这页面最重要的」）。
     *   量出来的依据：霞鹜文楷数字 advance = 0.6 em、冒号 0.35 em
     *   ⇒ "20:15" = 2.75 em。30px 时只有 75px 宽（屏宽 320），
     *   72px 时 198px 宽，左右各留 61px —— 这才是"主角"该占的篇幅。
     *   行高 = 1.184 em（hhea ascent 928 + descent 256，upm 1000）
     *   ⇒ 72px 字号占 85px 高，所以下面的日期/卡片都跟着下移。
     *   ⚠️ F_CLOCK 只含 0-9 : -，换文案前先看它的定义处。*/
    l_clock = wlabel(s_scr, 0, 34, F_CLOCK, C_TXT, "--:--");
    lv_obj_set_width(l_clock, SCR_W);
    lv_obj_set_style_text_align(l_clock, LV_TEXT_ALIGN_CENTER, 0);

    /* 日期：14px → 22px（兰兰「让当前时钟和日期稍微大一点」）。
     * ★ 用 F_READ（22px，CJK 主档里有），不是 F_CLOCK ——
     *   日期里有"月/日/星期"这些汉字，数字字库里没有。
     * ★★ y=100 不是随手写的：72px 字库的 line_height = 1.184 em = 85px，
     *    而数字的墨迹只占顶部 52px（实测 ink y=34..85，盒 34..119）——
     *    下面那 34px 是 CJK 字体度量留下的空档。
     *    日期顶到 y=100（墨迹 105..122）正好落进这个空档，
     *    把第一版留下的 41px 空洞收成 20px。实测依据见本文件末的验尸记录。*/
    l_date = wlabel(s_scr, 0, 100, F_READ, C_DIM, "— — —");
    lv_obj_set_width(l_date, SCR_W);
    lv_obj_set_style_text_align(l_date, LV_TEXT_ALIGN_CENTER, 0);

    /* ---- 当前天气卡 ----
     * ★★★ 10-08 晚：104 → 84 高、上沿 122 → 130（兰兰：「当前天气栏可以减小」）。
     *   减下来的高度让给上面的大时钟与日期 —— 这一页的重点是时间。
     *   卡内仍然装得下：温度 30px（墨迹 22px）+ 风 14px + 天气词 22px。*/
    lv_obj_t *card = wcard(s_scr, 16, 130, SCR_W - 32, 84, C_GLASS, 18);

    l_temp = wlabel(card, 22, 16, F_HERO, C_TEMP, "--°");
    l_wind = wlabel(card, 24, 58, F_SMALL, C_DIM, "—");
    /* 天气词放卡片右侧，与温度形成"左数值右定性"的现代排版 */
    l_cond = wclabel(card, 28, 90, SCR_W - 32 - 16 - 90, F_READ, C_COND, "—");

    /* ---- 三格：湿度 / 空气 / PM2.5（标题与数值各自一色）---- */
    lv_obj_t *tile[3];
    int tx[3] = { 16, 116, 216 };
    for (int i = 0; i < 3; i++) {
        tile[i] = wcard(s_scr, tx[i], 222, 88, 76, C_GLASS, 14);
    }
    wclabel(tile[0], 12, 88, 0, F_SMALL, C_HUM, "湿度");
    wclabel(tile[1], 12, 88, 0, F_SMALL, C_AIR, "空气");
    wclabel(tile[2], 12, 88, 0, F_SMALL, C_PM,  "PM2.5");
    l_humid = wclabel(tile[0], 40, 88, 0, F_READ, C_TXT, "--");
    l_air   = wclabel(tile[1], 40, 88, 0, F_READ, C_TXT, "--");
    l_pm    = wclabel(tile[2], 40, 88, 0, F_READ, C_TXT, "--");

    /* ---- 三天预报 ----
     * ★ 10-08 晚：上移到 306、高度 100 → 104。
     *   行距仍是 30，三行 F_MID(18px) ⇒ 最后一行墨迹到约 397，104 装得下。
     *   下沿 410 与底栏（438）留 28px，和页面其它间距一致。*/
    lv_obj_t *fc = wcard(s_scr, 16, 306, SCR_W - 32, 104, C_GLASS, 18);
    for (int i = 0; i < 3; i++) {
        int y = 8 + i * 30;
        l_fc_day[i]  = wlabel(fc, 20, y, F_MID, C_DIM, "—");
        l_fc_cond[i] = wlabel(fc, 96, y, F_MID, C_COND, "—");
        l_fc_hl[i]   = wclabel(fc, y, 120, SCR_W - 32 - 16 - 120, F_MID, C_ACC, "—");
    }

    /* ---- 底栏：更新时间 + 播报按钮 + 返回按钮 ---- */
    l_upd = wlabel(s_scr, 18, 444, F_SMALL, C_DIM, "更新 --:--");

    /* ★ 播报按钮（10-08 兰兰要求）。放「主 页」左边：这两个都是
     *   "随时可点"的动作，同一层最顺；也避开了左下的更新时间文字。
     *   x=116 起、宽 96：更新时间是 14px 的"更新 14:46"，约 70px 宽，
     *   从 18 起算到 88 就结束，留出 28px 间隙。*/
    lv_obj_t *sb = wcard(s_scr, 116, 438, 96, 34, C_GLASS, 17);
    lv_obj_set_style_bg_opa(sb, LV_OPA_30, 0);
    lv_obj_add_flag(sb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(sb, C_ACC, LV_STATE_PRESSED);
    lv_obj_add_event_cb(sb, speak_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sbl = wclabel(sb, 8, 96, 0, F_SMALL, C_TXT, "播 报");
    lv_obj_clear_flag(sbl, LV_OBJ_FLAG_CLICKABLE);   /* 点击穿透到按钮 */

    lv_obj_t *btn = wcard(s_scr, SCR_W - 16 - 84, 438, 84, 34, C_GLASS, 17);
    lv_obj_set_style_bg_opa(btn, LV_OPA_30, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(btn, C_ACC, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, home_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = wclabel(btn, 8, 84, 0, F_SMALL, C_TXT, "主 页");
    lv_obj_clear_flag(bl, LV_OBJ_FLAG_CLICKABLE);   /* 点击穿透到按钮 */
}

/* ============================================================
 *  背景光斑动画（"灵动"就靠它）
 * ============================================================
 *  ★ 周期 200ms 而不是 20ms：改一次 style 会触发该区域重绘，光斑是
 *    220px 的大圆，20ms 一改等于 50fps 全区域重绘，纯烧 CPU 换不来观感。
 *    200ms（5fps）的慢速色相流动，肉眼已经是"极光在缓缓流动"。
 *  ★ 只用【三角波】做位移，不用 lv_trig_sin —— 三角函数在这个场景里
 *    是杀鸡用牛刀，而且三角波看起来已经足够"呼吸"。
 * ============================================================ */
static void wx_anim_cb(lv_timer_t *t)
{
    (void)t;
    s_anim_t++;

    int h1 = (s_anim_t * 5) % 360;              /* 每 200ms 走 5° ⇒ 约 14 秒一圈 */
    int h2 = (s_anim_t * 5 + 150) % 360;
    if (l_glow1) lv_obj_set_style_bg_color(l_glow1, lv_color_hsv_to_rgb(h1, 65, 100), 0);
    if (l_glow2) lv_obj_set_style_bg_color(l_glow2, lv_color_hsv_to_rgb(h2, 65, 100), 0);

    /* 三角波位移：0→14→0，60 个 tick（12 秒）一个来回 */
    int ph = s_anim_t % 60;
    int dx = (ph < 30) ? ph : (60 - ph);        /* 0..29..0 */
    if (l_glow1) lv_obj_set_pos(l_glow1, -40 - dx, 30 + dx / 2);
    if (l_glow2) lv_obj_set_pos(l_glow2, 170 + dx, 250 - dx / 2);
}

/* ============================================================
 *  进入 / 离开
 * ============================================================ */
void ui_weather_init(void)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();

    /* 取数任务：只在第一次进入时创建，之后常驻复用（理由见 wx_worker 注释）*/
    if (!s_task) {
        if (xTaskCreate(wx_worker, "wx_fetch", 5120, NULL, 2, &s_task) != pdPASS) {
            ESP_LOGE(TAG, "取数任务创建失败 —— 界面照常，但不会刷新");
            s_task = NULL;
        }
    }

    s_alive = true;
    s_force = true;                  /* 让 worker 立刻取一次，不等 10 分钟周期 */
    s_enter_tick = xTaskGetTickCount();
    s_clock_cache[0] = '\0';         /* 逼时钟先刷一次 */

    build_screen();

    /* ★★★ 漏了这行 ⇒ 整屏纯白（lv_obj_create(NULL) 只是造了屏对象，
     *   它默认不在屏上）。ui_shell.c 里同一个坑。*/
    lv_screen_load(s_scr);

    if (s_tick) lv_timer_del(s_tick);
    s_tick = lv_timer_create(wx_tick_cb, 500, NULL);

    /* 光斑动画：200ms 一跳（周期理由见 wx_anim_cb 注释）*/
    s_anim_t = 0;
    if (s_anim) lv_timer_del(s_anim);
    s_anim = lv_timer_create(wx_anim_cb, 200, NULL);

    /* 有上次的数据就先铺上（避免每次进来都闪一下"--°"）。
     * ★ 必须从锁内取快照再上屏：worker 此刻可能正在写 s_wx，
     *   直接 wx_apply(&s_wx) 会读到写了一半的字段。*/
    {
        wx_data_t d;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        d = s_wx;
        xSemaphoreGive(s_mtx);
        if (d.ok) wx_apply(&d);
    }

    ESP_LOGI(TAG, "天气 App 已进入 (internal free=%u largest=%u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

/* ★ 10-08 调试用：等价于点一次底部「播 报」按钮（走同一个回调，
 * 不是另抄一份 —— 抄一份就只能证明"抄的那份对"）。*/
void ui_weather_demo_speak(void)
{
    speak_cb(NULL);
}

void ui_weather_leave(void)
{
    /* ★★ 先"断线"再拆界面：置 alive=false 与 worker 落数据互斥，
     *   保证此后不会有人再往下面这些指针里写字。*/
    if (s_mtx) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_alive = false;
        s_dirty = false;
        xSemaphoreGive(s_mtx);
    } else {
        s_alive = false;
        s_dirty = false;
    }

    /* ★ 常驻 timer 必须先删再删对象：它 500ms 一次访问页面对象，
     *   屏删了它还在跑就是悬空指针（ui_shell.h 契约第 ④ 条）。*/
    if (s_tick) { lv_timer_del(s_tick); s_tick = NULL; }
    if (s_anim) { lv_timer_del(s_anim); s_anim = NULL; }   /* 光斑动画同理 */

    /* ★ 播报要一起停：它是"在后台放一段临时 mp3"，用户退回主菜单了
     *   声音还接着响就是 bug。stop 顺带把 boost 还原回静音。*/
    voice_speak_stop();

    l_city = l_net = l_clock = l_date = NULL;
    l_temp = l_cond = l_wind = NULL;
    l_humid = l_air = l_pm = l_upd = NULL;
    l_glow1 = l_glow2 = NULL;
    for (int i = 0; i < 3; i++) { l_fc_day[i] = l_fc_cond[i] = l_fc_hl[i] = NULL; }

    /* ★ 用异步删除：本函数通常是从「主 页」按钮的回调链里被调进来的，
     *   同步 lv_obj_del 等于在处理事件的中途拆掉事件源对象的祖先。*/
    if (s_scr) { lv_obj_del_async(s_scr); s_scr = NULL; }

    ESP_LOGI(TAG, "天气 App 已退出");
}
