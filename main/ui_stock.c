/*
 * 拾声 · 股票行情 App —— 实现   v2.0
 * ============================================================
 *  升级：v1.0（固定三个指数）→ v2.0（自选列表 + K线 + 自由添加）
 *  兰兰 10-08 的原话：「股票行情应该可以自由添加，可以上下滑动或者双列显示，
 *  点击具体股票应该出K线，日K线，等指标，默认添加上证指数，科创50指数，
 *  002056,515980,588780,589850,159278,159241,159599,512630」
 *
 *  ── 三件必须说清的事 ─────────────────────────────────────
 *
 *  ★★★ ① 名称是 GBK，板上现解
 *     腾讯返回 v_sh000001="1~上证指数~000001~3811.36~…"，"上证指数" 是 GBK。
 *     v1.0 的做法是"完全不用响应里的名称，自己维护 STK[] 表"—— 那对
 *     【固定三个指数】够用，但"自由添加任意代码"的名称是运行时才知道的，
 *     内置表覆盖不到。⇒ 用 xs_gbk（17 KB 的 GB2312 码表）现解。
 *     ⚠ 切分仍必须 GBK 感知（见 xs_split_tilde）：GBK 第二字节可能是 0x7E，
 *       那正好就是分隔符 '~'，按字节切会错位、取到别的数字而且不报错。
 *
 *  ★★★ ② 涨跌字段按 14 位时间戳锚定，不按下标硬取
 *     这套字段的下标随品种变化（股票/指数/基金/港美股档数不同），
 *     一旦错位，取到的是【另一个数字】—— 不报错、不崩，只是悄悄显示错的行情。
 *
 *  ★★ ③ 涨=红、跌=绿（国内规矩，与欧美相反）。
 *
 *  ── 屏幕结构 ──────────────────────────────────────────────
 *      s_scr（列表屏） ←→  s_k（K线屏）
 *   · 列表屏【常驻】：worker 每 60 秒刷它的 label，删了就是悬空指针。
 *   · K线屏按需建、返回即删 —— LVGL 对象池只有 62 KB，
 *     两屏同时存在的对象数要算着用（这正是 K线不做 60 根蜡烛的原因）。
 *   · 取数都是【独立任务】（优先级 2 < LVGL 的 3），
 *     LVGL 侧只读共享数据、只在自己任务里碰控件。
 * ============================================================
 */
#include "ui_stock.h"

#include "ui_shell.h"
#include "app_sys.h"
#include "xs_http.h"
#include "xs_gbk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "STKUI";

/* ---------- 字体 ---------- */
extern const lv_font_t xs_font_14;
extern const lv_font_t xs_font_18;
extern const lv_font_t xs_font_22;
extern const lv_font_t xs_font_30;
#define F_HERO  (&xs_font_30)
#define F_READ  (&xs_font_22)
#define F_MID   (&xs_font_18)
#define F_SMALL (&xs_font_14)

/* ============================================================
 *  默认自选（兰兰 10-08 指定的 10 个）
 * ============================================================ */
static const char *DEF_SYMS[] = {
    "sh000001",   /* 上证指数 */
    "sh000688",   /* 科创50 指数 */
    "sz002056",   /* 横店东磁 */
    "sh515980",   /* 人工智能ETF华富 */
    "sh588780",   /* 科创芯片设计ETF国联安 */
    "sh589850",   /* 科创50ETF东财 */
    "sz159278",   /* 机器人PH */
    "sz159241",   /* 航空TH */
    "sz159599",   /* 芯片指数 */
    "sh512630",   /* 卫星ETF广发 */
};
#define DEF_N ((int)(sizeof(DEF_SYMS) / sizeof(DEF_SYMS[0])))

/* 已知标的的名称兜底表。
 * ★ 用途有两个：① GB2312 表外的生僻字（会解成 '?'）时顶上去；
 *   ② 纯数字输入时代码前缀猜错时，用它反查正确写法。
 * ★ 不是"必需"——名称正常情况下是从响应里 GBK 解出来的。*/
static const struct { const char *code, *name; } KNOWN[] = {
    { "sh000001", "上证指数" }, { "sz399001", "深证成指" }, { "sz399006", "创业板指" },
    { "sh000688", "科创50"   }, { "sh000016", "上证50"   }, { "sh000300", "沪深300"  },
    { "sh000905", "中证500"  }, { "sh000852", "中证1000" },
    { "sz002056", "横店东磁" }, { "sh515980", "人工智能ETF华富" },
    { "sh588780", "科创芯片设计ETF国联安" }, { "sh589850", "科创50ETF东财" },
    { "sz159278", "机器人PH" }, { "sz159241", "航空TH" },
    { "sz159599", "芯片指数" }, { "sh512630", "卫星ETF广发" },
};
#define KNOWN_N ((int)(sizeof(KNOWN) / sizeof(KNOWN[0])))

#define STK_MAX        24        /* 自选上限（再多也看不完，且 URL 会变长）*/
#define STK_URL_HEAD   "http://qt.gtimg.cn/q="
#define STK_URL_CAP    384
#define STK_BODY_CAP   12288     /* 24 个标的 × 约 400 B ≈ 9.6 KB，留余量 */
#define STK_FAST_S     60        /* 交易时段刷新间隔 */
#define STK_SLOW_S     600       /* 非交易时段 */

/* ---------- 日K ---------- */
#define K_N            30        /* 取 30 个交易日 */
#define KLINE_URL_FMT  "http://money.finance.sina.com.cn/quotes_service/api/json_v2.php/" \
                       "CN_MarketData.getKLineData?symbol=%s&scale=240&ma=5&datalen=%d"
#define KLINE_URL_CAP  192

/* ---------- 分时（当日 5 分钟线）10-08 兰兰要求加 ----------
 * 兰兰原话：「K线图应该分上下两屏，上是当天线，下是日K线，都应该有网格，
 *           这样更直观」。
 * ★★ 数据源为什么复用【同一个新浪接口】只把 scale 从 240 改成 5：
 *   ① 这个域名/路径已经在真机上验证过是【明文 http、无重定向】——
 *      本板内部 RAM 最大连续块只有 31.7 KB，走 https 必然握手失败（铁律）。
 *      换一个新域名 = 重新赌一次"它会不会 302 到 https"，没必要。
 *   ② 返回体结构与日K【完全一样】（数组元素含 open/high/low/close，
 *      5 分钟粒度下 day 带时分），于是解析、日期过滤都能共用同一套写法。
 *   ③ datalen=60：A 股一天 48 根 5 分钟线，给 60 是留余量，
 *      再按"最后一根所在的日期"过滤掉上一交易日的尾巴。*/
#define M_N            60
#define MIN_URL_FMT    "http://money.finance.sina.com.cn/quotes_service/api/json_v2.php/" \
                       "CN_MarketData.getKLineData?symbol=%s&scale=5&ma=no&datalen=%d"
#define MIN_URL_CAP    192

/* ---------- 配色（与天气页同一套彩色体系）---------- */
#define C_BG_TOP   lv_color_hex(0x241A4E)
#define C_BG_BOT   lv_color_hex(0x0A2A4A)
#define C_GLASS    lv_color_hex(0xFFFFFF)
#define C_GLASS_OPA LV_OPA_20   /* LVGL 没有 OPA_14，档位是 10/20/30… */
#define C_EDGE     lv_color_hex(0x7FA8E0)
#define C_TXT      lv_color_hex(0xF2F6FF)
#define C_DIM      lv_color_hex(0x9FB4D8)
#define C_ACC      lv_color_hex(0x7FD4FF)
#define C_GOLD     lv_color_hex(0xFFD166)
/* ★★ A 股规矩：涨红、跌绿。这两个值不要跟欧美习惯对调。*/
#define C_UP       lv_color_hex(0xF0524F)   /* 涨 = 红 */
#define C_DN       lv_color_hex(0x2FBF71)   /* 跌 = 绿 */
#define C_FLAT     lv_color_hex(0x9FB4D8)
#define C_MA5      lv_color_hex(0xFFD166)
#define C_MA10     lv_color_hex(0x7FD4FF)
#define C_MA20     lv_color_hex(0xE08CFF)

#define SCR_W      320
#define SCR_H      480

/* ============================================================
 *  K线屏几何（10-08 兰兰「上下两屏」后的固定布局）
 * ============================================================
 *  ┌──────────────────────────────┐  0
 *  │ 名称 / 现价 / 涨跌           │     顶栏 0..50
 *  ├──────────────────────────────┤ 52
 *  │ 分时 · 今日                  │
 *  │   ▒▒▒ 网格 4×4 ▒▒▒           │     上屏 52..228
 *  │   ── 基准线 ──  折线         │
 *  ├──────────────────────────────┤ 234
 *  │ 日K · 近 30 日               │
 *  │   ▒▒▒ 网格 4×4 ▒▒▒  蜡烛+MA  │     下屏 234..410
 *  ├──────────────────────────────┤ 412
 *  │ MA5 MA10 MA20         [返回] │     图例 + 底栏
 *  └──────────────────────────────┘ 480
 *
 *  ★★ 为什么把绘图区定成宏，而不是各处散着写字面数字：
 *    这一屏有【四类必须互相咬合】的东西 —— 两个区的网格、分时折线、
 *    30 根蜡烛与三条均线。只要改一处而漏一处，屏幕上就是"网格歪了 /
 *    折线出框 / 蜡烛跑到另一个区"，而这种错编译期毫无提示，
 *    只能靠肉眼截图发现。⇒ 一份几何宏，建屏与绘制都从这里取。
 *  ★ 绘图区左右各留 12：320 - 2×12 = 296。右侧不再放价格刻度，
 *    所以左右必须严格对称（否则视觉上像整体偏了）。*/
#define KG_PLOT_X   12
#define KG_PLOT_W   296
#define KG_A_Y      74           /* 上屏（分时）绘图区顶 */
#define KG_A_H      148          /* 上屏绘图区高：74 + 148 = 222，框底 228 */
#define KG_B_Y      256          /* 下屏（日K）绘图区顶 */
#define KG_B_H      148          /* 256 + 148 = 404，框底 410 */
#define KG_PANE_A_Y 52           /* 上屏外框（含标题条）*/
#define KG_PANE_B_Y 234
#define KG_PANE_H   176
#define KG_GRID     lv_color_hex(0x3A4E7A)   /* 网格线：比底色亮一档，不抢蜡烛 */

/* ============================================================
 *  ★★★ 股票用字的【字库清单】（同 ui_weather.c 那条，理由见那里的长注释）
 *  ⚠ 改这里之后必须重跑 _gen_fonts_lq.py，否则新字是豆腐块。
 * ============================================================ */
const char XS_STK_FONT_CHARS[] =
    /* 标题 / 按钮 / 状态 */
    "自选行情添加删除取消确定请输入代码返回主页在线离线载入中"
    "暂无数据获取失败网络未连接更新个已达上限已存在无效数字"
    /* K线相关 */
    "日K线开高低收成交量额图例指标五日十二十"
    /* 上下两屏（10-08 加）：分时 / 日K 两区标题 + 基准线
     * ⚠ 「昨」字故意不用 —— 当前四档字库都没有它，而重跑一次字库要十几分钟；
     *   基准线改叫「基准」，字都是现成的。*/
    "分时今日近基准上下"
    /* 常见标的词（名称一般来自接口，这里只为兜底表服务）*/
    "上证指数深证成指创业板科创沪深中证新材料机器人航空卫星芯片ETF"
    /* 符号（0x20-0x7F 之外的都要显式列）*/
    "▲▼…×－＋";

/* ============================================================
 *  数据
 * ============================================================ */
/* 代码缓冲区恒 10 B（8 字符代码 + 余量 + NUL）。凡把「长度未知的源串」拷进来，
 * 一律用 CODE_FMT 限定精度——否则 GCC 会按最坏情况算，判成 format-truncation 而 -Werror 挡下。*/
#define CODE_LEN 10
#define CODE_FMT "%.9s"

static char  s_code[STK_MAX][CODE_LEN];  /* 自选代码（带 sh/sz 前缀）*/
static int   s_n;                       /* 当前自选数 */

typedef struct {
    bool  ok;
    char  name[28];                     /* UTF-8（接口 GBK 解出来的）*/
    float price, chg, pct;
} quote_t;

static quote_t           s_q[STK_MAX];
static char              s_upd[20] = "更新 --:--";
static bool              s_valid;
static SemaphoreHandle_t s_mtx;
static volatile bool     s_alive;
static volatile bool     s_dirty;
static volatile bool     s_force;
static TaskHandle_t      s_task;
static lv_timer_t       *s_tick;
static TickType_t        s_enter_tick;

/* ---- K线（请求 / 结果）---- */
typedef struct {
    bool  ok;
    int   n;
    char  day[K_N][12];
    float open[K_N], high[K_N], low[K_N], close[K_N];
    char  name[28];                     /* 哪个标的的 K线（用于顶栏）*/
    float last, chg, pct;
} kline_t;

static kline_t           s_k;
static volatile bool     s_k_req;       /* 有待处理的 K线请求 */
static volatile bool     s_k_dirty;     /* 新 K线数据待上屏 */
static char              s_k_code[10];  /* 请求的代码 */

/* ---- 分时（当日 5 分钟线）----
 * ★ 只留 close[] 就够：分时图画的是一条"价格随时间的折线"。
 *   另存 base（当日第一根的 open）是为了在缺日K时还能画出基准线。
 *   ★ 不用 kline_t：那个含 30 天 × 5 个 float 还带 high/low，
 *     分时用不上，白白多占 1 KB 静态内存。*/
typedef struct {
    bool  ok;
    int   n;
    char  day[12];              /* 就是"哪一天"（用于基准线/日志）*/
    float close[M_N];
    float base;                 /* 当日第一根的 open（≈昨收，兜底用）*/
} mline_t;

static mline_t           s_m;
static volatile bool     s_m_dirty;

/* ---------- 界面对象 ---------- */
/* 对外接口（ui_stock.h 也声明了；这里再放一份是为了让定义顺序更自由）*/
void ui_stock_rebuild_list(void);

static lv_obj_t *s_scr;                              /* 列表屏（常驻）*/
static lv_obj_t *l_net, *l_upd;
static lv_obj_t *l_name[STK_MAX], *l_price[STK_MAX], *l_chg[STK_MAX];

static lv_obj_t *s_k_scr;                            /* K线屏（动态）*/
static lv_obj_t *k_title, *k_price, *k_chg;
static lv_obj_t *k_wick[K_N], *k_body[K_N];
static lv_obj_t *k_ma_line[3];
static lv_point_precise_t k_ma_pts[3][K_N];
/* 分时区（10-08 加）：一条折线 + 一条"昨收"基准线 + 两个区的网格线
 * ★ 网格线不放进数组：它们是【静态装饰】，建完就不动，
 *   没必要为它们维护下标；留着指针只是为了 leave() 里置 NULL。*/
static lv_obj_t *k_m_line;                 /* 分时折线 */
static lv_point_precise_t k_m_pts[M_N];
static lv_obj_t *k_m_base;                 /* 分时基准线（昨收）*/
static lv_obj_t *k_m_head;                 /* 「分时 · 今日」小标题 */
static lv_obj_t *k_k_head;                 /* 「日K · 近 30 日」小标题 */
static lv_obj_t *k_gr_k[6];                /* 日K区网格：3 横 + 3 竖 */
static lv_obj_t *k_gr_m[6];                /* 分时区网格：3 横 + 3 竖 */
static lv_obj_t *s_add_panel;                        /* 添加自选面板 */

/* ============================================================
 *  UI 小工具
 * ============================================================ */
static lv_obj_t *sbox(lv_obj_t *par, int x, int y, int w, int h,
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

static lv_obj_t *scard(lv_obj_t *par, int x, int y, int w, int h,
                       lv_color_t fill, int radius)
{
    lv_obj_t *o = sbox(par, x, y, w, h, fill, radius);
    lv_obj_set_style_bg_opa(o, C_GLASS_OPA, 0);          /* 玻璃感 */
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, C_EDGE, 0);
    lv_obj_set_style_border_opa(o, LV_OPA_30, 0);
    return o;
}

static lv_obj_t *slabel(lv_obj_t *par, int x, int y, const lv_font_t *font,
                        lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(par);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *sclabel(lv_obj_t *par, int y, int w, int x0,
                         const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = slabel(par, x0, y, font, color, text);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

/* ============================================================
 *  代码规范化（兰兰只输 6 位数字，前缀我们猜）
 * ============================================================
 *  ★ 前缀规则（A 股）：
 *      6xxxxx → sh（沪市主板/科创板 688）
 *      5xxxxx → sh（沪市基金/ETF：510/511/512/513/515/588）
 *      9xxxxx → sh（B 股）
 *      0xxxxx → sz（深市主板，但也有上证指数系列 000001/000300…）
 *      1xxxxx → sz（深市基金/ETF：159xxx）
 *      2xxxxx → sz（深市 B 股）
 *      3xxxxx → sz（创业板）
 *  ★★ 0 开头有歧义：000001 既是【上证指数】也是【平安银行】。
 *      ⇒ 纯数字输入时先查 KNOWN 表（那里有上证指数系列的正确写法），
 *        查不到才按"0 开头 = 深市"处理。
 *      ⇒ 想明确指定就带前缀写（sh000001）。
 * ============================================================ */
static void norm_code(const char *in, char *out, size_t cap)
{
    out[0] = '\0';
    if (!in || !in[0]) return;

    /* 已经带 sh/sz 前缀（大小写都收）*/
    if ((in[0] == 's' || in[0] == 'S') && (in[1] == 'h' || in[1] == 'H' ||
                                           in[1] == 'z' || in[1] == 'Z')) {
        snprintf(out, cap, "%c%c%s",
                 (in[0] == 'S' ? 's' : in[0]),
                 (in[1] == 'H' ? 'h' : (in[1] == 'Z' ? 'z' : in[1])),
                 in + 2);
        return;
    }
    /* 全数字 */
    bool allnum = true;
    for (const char *p = in; *p; p++)
        if (*p < '0' || *p > '9') { allnum = false; break; }
    if (!allnum) { snprintf(out, cap, "%s", in); return; }   /* 交给接口去报错 */

    /* 先查已知表：解决 000001/000300/000688 这些"指数 vs 深市股票"的歧义 */
    for (int i = 0; i < KNOWN_N; i++) {
        if (strcmp(KNOWN[i].code + 2, in) == 0) {
            snprintf(out, cap, "%s", KNOWN[i].code);
            return;
        }
    }
    char c = in[0];
    const char *pfx = (c == '6' || c == '5' || c == '9') ? "sh" : "sz";
    snprintf(out, cap, "%s%s", pfx, in);
}

/* 从 KNOWN 表里查名称（GBK 解码失败时的兜底）*/
static const char *known_name(const char *code)
{
    for (int i = 0; i < KNOWN_N; i++)
        if (strcmp(KNOWN[i].code, code) == 0) return KNOWN[i].name;
    return NULL;
}

/* ============================================================
 *  自选股持久化（NVS：换行分隔的代码串）
 * ============================================================ */
#define NVS_NS  "xs_stk"
#define NVS_KEY "syms"

static void syms_save(void)
{
    char buf[STK_MAX * 12];
    size_t o = 0;
    buf[0] = '\0';
    for (int i = 0; i < s_n; i++) {
        int w = snprintf(buf + o, sizeof(buf) - o, "%s\n", s_code[i]);
        if (w <= 0 || (size_t)w >= sizeof(buf) - o) break;
        o += (size_t)w;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY, buf);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "自选已保存（%d 个）", s_n);
}

static void syms_load(void)
{
    s_n = 0;
    char buf[STK_MAX * 12] = {0};
    size_t len = sizeof(buf) - 1;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, NVS_KEY, buf, &len) == ESP_OK && buf[0]) {
            char *p = buf;
            while (*p && s_n < STK_MAX) {
                char *nl = strchr(p, '\n');
                if (nl) *nl = '\0';
                if (*p) snprintf(s_code[s_n++], sizeof(s_code[0]), CODE_FMT, p);
                if (!nl) break;
                p = nl + 1;
            }
        }
        nvs_close(h);
    }

    /* 首次开机（NVS 里没有）⇒ 播种兰兰指定的 10 个默认标的 */
    if (s_n == 0) {
        for (int i = 0; i < DEF_N && i < STK_MAX; i++)
            snprintf(s_code[s_n++], sizeof(s_code[0]), CODE_FMT, DEF_SYMS[i]);
        ESP_LOGI(TAG, "无存档 ⇒ 播种默认 %d 个自选", s_n);
        syms_save();
    }
}

/* ============================================================
 *  解析
 * ============================================================ */

/* 从整包里抠出 v_<code>="…" 里的内容 */
static bool extract_quote(const char *body, const char *code, char *out, size_t cap)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "v_%s=", code);
    const char *p = strstr(body, pat);
    if (!p) return false;
    p += strlen(pat);
    if (*p == '"') p++;
    size_t n = 0;
    while (*p && *p != '"' && *p != '\n' && *p != '\r' && n + 1 < cap)
        out[n++] = *p++;
    out[n] = '\0';
    return n > 0;
}

/* 找 14 位时间戳字段的下标（见文件头 ② 的说明）。返回 -1 = 没找到。*/
static int find_ts_idx(const char *line)
{
    char f[24];
    for (int i = 0; i < 70; i++) {
        if (!xs_split_tilde(line, i, f, sizeof(f))) return -1;
        size_t n = strlen(f);
        if (n != 14) continue;
        if (f[0] != '2' || f[1] != '0') continue;   /* 年份以 20 开头 */
        bool allnum = true;
        for (size_t k = 0; k < n; k++)
            if (f[k] < '0' || f[k] > '9') { allnum = false; break; }
        if (allnum) return i;
    }
    return -1;
}

/* 解析单个标的。upd 是输出参数（本函数在锁外跑，不能直写全局）。*/
static bool stk_parse_item(const char *body, const char *code, quote_t *q,
                           char *upd, size_t upd_cap)
{
    static char line[1024];
    static char f[32];
    static char gbk[64];

    if (upd && upd_cap) upd[0] = '\0';
    memset(q, 0, sizeof(*q));

    if (!extract_quote(body, code, line, sizeof(line))) return false;

    /* ★ 校验：字段 2 必须是请求的 6 位代码。对不上 = 字段错位，
     *   宁可整条不要，也不要显示一个"看起来很合理"的错数字。*/
    if (!xs_split_tilde(line, 2, f, sizeof(f))) return false;
    if (strcmp(f, code + 2) != 0) {
        ESP_LOGW(TAG, "%s 代码字段对不上（读到 \"%s\"）—— 字段错位，丢弃", code, f);
        return false;
    }

    /* 名称 = 字段 1，GBK → UTF-8。
     * ★ 失败/表外字符回退：① KNOWN 表 → ② 只显示代码。*/
    if (xs_split_tilde(line, 1, gbk, sizeof(gbk)))
        xs_gbk_to_utf8(gbk, (int)strlen(gbk), q->name, sizeof(q->name));
    bool has_q = (strchr(q->name, '?') != NULL);
    if (q->name[0] == '\0' || has_q) {
        const char *kn = known_name(code);
        if (kn) snprintf(q->name, sizeof(q->name), "%s", kn);
        else if (q->name[0] == '\0') snprintf(q->name, sizeof(q->name), "%s", code);
    }

    /* 当前价 = 字段 3（下标跨品种稳定，实测与公认定义一致）*/
    if (!xs_split_tilde(line, 3, f, sizeof(f))) return false;
    q->price = (float)atof(f);
    if (q->price <= 0.f) return false;               /* 0 或负数 = 数据异常 */

    /* 涨跌 / 涨跌幅：时间戳后两位 */
    int ts = find_ts_idx(line);
    if (ts < 0) return false;
    if (!xs_split_tilde(line, ts + 1, f, sizeof(f))) return false;
    q->chg = (float)atof(f);
    if (!xs_split_tilde(line, ts + 2, f, sizeof(f))) return false;
    q->pct = (float)atof(f);

    /* 时间戳顺手当"更新时间"：20261008144645 → 14:46 */
    if (upd && xs_split_tilde(line, ts, f, sizeof(f)) && strlen(f) == 14)
        snprintf(upd, upd_cap, "更新 %c%c:%c%c", f[8], f[9], f[10], f[11]);

    q->ok = true;
    return true;
}

/* ============================================================
 *  K线解析（新浪：顶层 JSON 数组）
 * ============================================================
 *  [ {"day":"2026-09-28","open":"3878.409","high":"3878.409",
 *     "low":"3806.671","close":"3823.621","volume":"45235067500", …}, … ]
 *  ★ 注意数字也是【带引号的字符串】("open":"3878.409")，
 *    xs_json_get 会把引号剥掉，atof 直接能用。
 *  ★ MA 自己算，不用接口的 ma_price5 —— 因为要 MA5/10/20 三条，
 *    接口只给一个 ma；自己算一次循环就够，还省一个请求参数。
 * ============================================================ */
static bool kline_parse(const char *body, const char *code, kline_t *k)
{
    static char el[512];
    static char v[32];

    memset(k, 0, sizeof(*k));
    snprintf(k->name, sizeof(k->name), "%s", code);

    int n = 0;
    for (int i = 0; i < K_N; i++) {
        if (!xs_json_top_elem(body, i, el, sizeof(el))) break;

        /* ★ 每行只放一条 if(+一条语句)：gcc 的 -Werror=misleading-indentation
         *   会把「if (...) break;  后面同一行再跟一句」判成缩进误导而拒绝编译。*/
        if (!xs_json_get(el, "day", k->day[n], sizeof(k->day[0]))) break;
        if (!xs_json_get(el, "open", v, sizeof(v))) break;
        k->open[n] = (float)atof(v);
        if (!xs_json_get(el, "high", v, sizeof(v))) break;
        k->high[n] = (float)atof(v);
        if (!xs_json_get(el, "low", v, sizeof(v))) break;
        k->low[n] = (float)atof(v);
        if (!xs_json_get(el, "close", v, sizeof(v))) break;
        k->close[n] = (float)atof(v);
        n++;
    }
    if (n < 5) return false;                       /* 太少画不出趋势 */

    /* 低到高重排（新浪是按时间正序返回的，这里只是防御）*/
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && strcmp(k->day[j - 1], k->day[j]) > 0; j--) {
            char td[12]; float tf;
            memcpy(td, k->day[j], sizeof(td)); memcpy(k->day[j], k->day[j-1], sizeof(td));
            memcpy(k->day[j-1], td, sizeof(td));
            tf = k->open[j];  k->open[j]  = k->open[j-1];  k->open[j-1]  = tf;
            tf = k->high[j];  k->high[j]  = k->high[j-1];  k->high[j-1]  = tf;
            tf = k->low[j];   k->low[j]   = k->low[j-1];   k->low[j-1]   = tf;
            tf = k->close[j]; k->close[j] = k->close[j-1]; k->close[j-1] = tf;
        }
    }
    k->n = n;
    k->last = k->close[n - 1];
    if (n >= 2 && k->close[n - 2] > 0.f) {
        k->chg = k->last - k->close[n - 2];
        k->pct = k->chg / k->close[n - 2] * 100.f;
    }
    k->ok = true;
    return true;
}

/* MA：period 日均价，前 period-1 根没有值（用 -1 标记）*/
static void calc_ma(const kline_t *k, int period, float *out)
{
    for (int i = 0; i < k->n; i++) {
        if (i + 1 < period) { out[i] = -1.f; continue; }
        float s = 0.f;
        for (int j = 0; j < period; j++) s += k->close[i - j];
        out[i] = s / (float)period;
    }
}

/* ============================================================
 *  分时解析（同一个新浪接口，scale=5）
 * ============================================================
 *  [ {"day":"2026-10-08 14:35:00","open":"3796.118","high":"…",
 *     "low":"…","close":"3800.340","volume":"…"}, … ]
 *
 *  ★★ 两步：① 把"最后一根的日期"当作今天；② 只留同一天的。
 *    为什么不用板上时钟判断"今天"：
 *      · 非交易日（周末/节假日）接口返回的是【上一交易日】的分时，
 *        那时板上时钟已经是周六 ⇒ 拿板上日期过滤会一根都留不下，图是空的；
 *      · 用数据自带的日期就没这个问题，也不依赖 SNTP 有没有校上时。
 *    （日K那边不需要过滤：它本来就是每天一根。）*/
static bool mline_parse(const char *body, mline_t *m)
{
    static char el[512];
    static char v[32];
    static char d[24];

    memset(m, 0, sizeof(*m));

    /* ① 数出一共几根 */
    int total = 0;
    for (int i = 0; i < M_N; i++) {
        if (!xs_json_top_elem(body, i, el, sizeof(el))) break;
        total++;
    }
    if (total < 2) return false;

    /* 用最后一根的日期当"今天" */
    if (!xs_json_top_elem(body, total - 1, el, sizeof(el))) return false;
    if (!xs_json_get(el, "day", d, sizeof(d))) return false;
    snprintf(m->day, sizeof(m->day), "%.10s", d);   /* "2026-10-08" */

    /* ② 只收同一天的 */
    int n = 0;
    for (int i = 0; i < total && n < M_N; i++) {
        if (!xs_json_top_elem(body, i, el, sizeof(el))) break;
        if (!xs_json_get(el, "day", d, sizeof(d))) break;
        if (strncmp(d, m->day, 10) != 0) continue;  /* 上一交易日的尾巴，丢掉 */
        if (!xs_json_get(el, "close", v, sizeof(v))) break;
        m->close[n] = (float)atof(v);
        if (n == 0) {
            /* 当日第一根的 open ≈ 昨收（没有日K可参照时的兜底基准）*/
            if (xs_json_get(el, "open", v, sizeof(v)))
                m->base = (float)atof(v);
            else
                m->base = m->close[0];
        }
        n++;
    }
    if (n < 2) return false;

    m->n  = n;
    m->ok = true;
    return true;
}

/* ============================================================
 *  刷新节流
 * ============================================================ */
static int refresh_sec(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_wday == 0 || tm.tm_wday == 6) return STK_SLOW_S;   /* 周末 */
    int m = tm.tm_hour * 60 + tm.tm_min;
    if ((m >= 9 * 60 + 15 && m <= 11 * 60 + 30) ||
        (m >= 13 * 60     && m <= 15 * 60)) return STK_FAST_S;   /* 上午/下午盘中 */
    return STK_SLOW_S;
}

/* ============================================================
 *  取数任务（行情轮询 + 按需 K线）
 * ============================================================ */
static void stk_fetch_quotes(char *body, TickType_t *gap)
{
    char url[STK_URL_CAP];
    size_t o = (size_t)snprintf(url, sizeof(url), STK_URL_HEAD);
    for (int i = 0; i < s_n && o + 12 < sizeof(url); i++)
        o += (size_t)snprintf(url + o, sizeof(url) - o, "%s%s",
                              i ? "," : "", s_code[i]);

    int nb = xs_http_get(url, body, STK_BODY_CAP);
    if (nb <= 0) {
        ESP_LOGW(TAG, "行情取数失败（%d）", nb);
        return;
    }
    *gap = pdMS_TO_TICKS(refresh_sec() * 1000);

    /* ★ static：quote_t[24] ≈ 960 B，worker 栈只有 5120 B —— 放栈上
     *   再叠加 HTTP 客户端的临时用量就危险了。本函数单线程调用。*/
    static quote_t tmp[STK_MAX];
    char upd[20] = {0};
    int got = 0;
    for (int i = 0; i < s_n; i++) {
        if (stk_parse_item(body, s_code[i], &tmp[i], upd, sizeof(upd))) got++;
        else ESP_LOGW(TAG, "%s 解析失败", s_code[i]);
    }
    if (got == 0) {
        ESP_LOGW(TAG, "一个都没解出来，前 80 字：%.80s", body);
        return;
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_alive) {
        for (int i = 0; i < s_n; i++) s_q[i] = tmp[i];
        if (upd[0]) {
            strncpy(s_upd, upd, sizeof(s_upd) - 1);
            s_upd[sizeof(s_upd) - 1] = '\0';
        }
        s_valid = true;
        s_dirty = true;
    }
    xSemaphoreGive(s_mtx);

    if (s_alive)
        ESP_LOGI(TAG, "行情 OK：%s %.2f (%+.2f%%) 共 %d 个  %s",
                 tmp[0].name, tmp[0].price, tmp[0].pct, got, s_upd);
}

static void stk_fetch_kline(char *body)
{
    char url[KLINE_URL_CAP];
    snprintf(url, sizeof(url), KLINE_URL_FMT, s_k_code, K_N);

    ESP_LOGI(TAG, "取日K：%s", s_k_code);
    int nb = xs_http_get(url, body, STK_BODY_CAP);
    if (nb <= 0) {
        ESP_LOGW(TAG, "日K取数失败（%d）", nb);
        return;
    }

    /* ★ static：同 list_apply 的理由（1.2 KB 放 worker 栈上太占）*/
    static kline_t k;
    if (!kline_parse(body, s_k_code, &k)) {        ESP_LOGW(TAG, "日K解析失败（%d 字节），前 80 字：%.80s", nb, body);
        return;
    }
    /* 名称优先用行情表里那份（更友好），K线接口不带名称 */
    for (int i = 0; i < s_n; i++) {
        if (strcmp(s_code[i], s_k_code) == 0 && s_q[i].ok) {
            snprintf(k.name, sizeof(k.name), "%s", s_q[i].name);
            break;
        }
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_k = k;
    s_k_dirty = true;
    xSemaphoreGive(s_mtx);

    ESP_LOGI(TAG, "日K OK：%s %d 根 %s~%s 收 %.2f",
             k.name, k.n, k.day[0], k.day[k.n - 1], k.last);
}

/* 分时取数。★ 与日K共用同一块 PSRAM 缓冲（取完就解析、解析完就落盘到
 * s_m），所以不存在"两份 body 同时活着"的内存问题。*/
static void stk_fetch_minute(char *body)
{
    char url[MIN_URL_CAP];
    snprintf(url, sizeof(url), MIN_URL_FMT, s_k_code, M_N);

    ESP_LOGI(TAG, "取分时：%s", s_k_code);
    int nb = xs_http_get(url, body, STK_BODY_CAP);
    if (nb <= 0) {
        ESP_LOGW(TAG, "分时取数失败（%d）", nb);
        return;
    }

    /* ★ static：mline_t ≈ 260 B；worker 栈 5120 B，能省则省（与日K同规矩）*/
    static mline_t m;
    if (!mline_parse(body, &m)) {
        ESP_LOGW(TAG, "分时解析失败（%d 字节），前 80 字：%.80s", nb, body);
        return;
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_m = m;
    s_m_dirty = true;
    xSemaphoreGive(s_mtx);

    ESP_LOGI(TAG, "分时 OK：%s %d 根 收 %.2f", m.day, m.n, m.close[m.n - 1]);
}

static void stk_worker(void *arg)
{
    (void)arg;

    char *body = (char *)heap_caps_malloc(STK_BODY_CAP, MALLOC_CAP_SPIRAM);
    if (!body) {
        ESP_LOGE(TAG, "取数缓冲分配失败");
        vTaskDelete(NULL);
        return;
    }

    TickType_t last = 0;
    TickType_t gap  = pdMS_TO_TICKS(STK_FAST_S * 1000);

    for (;;) {
        /* K线请求优先：用户正盯着 K线屏，快一点体验完全不同 */
        if (s_k_req) {
            s_k_req = false;
            stk_fetch_kline(body);
            /* ★ 10-08：同一次请求里把【分时】也取回来（上下两屏要一起上屏）。
             *   两条接口同域名同路径、只差 scale，于是共用一个 body 缓冲
             *   —— 省下 12 KB PSRAM（本板 PSRAM 虽多，但没必要浪费）。*/
            stk_fetch_minute(body);
            continue;
        }

        if (!s_alive) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }

        TickType_t now = xTaskGetTickCount();
        if (!s_force && last != 0 && (now - last) < gap) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        s_force = false;

        stk_fetch_quotes(body, &gap);
        last = xTaskGetTickCount();
    }
}

/* ============================================================
 *  上屏
 * ============================================================ */
static lv_color_t chg_color(float chg)
{
    if (chg >  0.0001f) return C_UP;      /* 涨 = 红 */
    if (chg < -0.0001f) return C_DN;      /* 跌 = 绿 */
    return C_FLAT;
}

static const char *chg_arrow(float chg)
{
    if (chg >  0.0001f) return "▲";
    if (chg < -0.0001f) return "▼";
    return "-";
}

/* 列表屏：把行情刷进卡片（只在 LVGL 任务里调）*/
static void list_apply(void)
{
    /* ★ static 而不是栈上：quote_t[24] ≈ 960 字节，LVGL 任务栈只有 8 KB，
     *   再叠加列表重建时的临时对象就会顶到栈底。
     *   本函数只在 LVGL 任务里被调（tick / init），无重入，static 安全。*/
    static quote_t tmp[STK_MAX];
    char upd[20];

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    memcpy(tmp, s_q, sizeof(tmp));
    memcpy(upd, s_upd, sizeof(upd));
    s_dirty = false;
    xSemaphoreGive(s_mtx);

    if (l_net) {
        lv_label_set_text(l_net, "在线");
        lv_obj_set_style_text_color(l_net, C_ACC, 0);
    }

    for (int i = 0; i < s_n; i++) {
        if (!tmp[i].ok) continue;
        char p[24], c[32];
        snprintf(p, sizeof(p), "%.2f", tmp[i].price);
        snprintf(c, sizeof(c), "%s %+.2f%%", chg_arrow(tmp[i].chg), tmp[i].pct);

        if (l_name[i])  lv_label_set_text(l_name[i], tmp[i].name);
        if (l_price[i]) lv_label_set_text(l_price[i], p);
        if (l_chg[i]) {
            lv_label_set_text(l_chg[i], c);
            lv_obj_set_style_text_color(l_chg[i], chg_color(tmp[i].chg), 0);
        }
    }
    if (l_upd) lv_label_set_text(l_upd, upd);
}

/* ============================================================
 *  K线屏绘制（10-08 改：上屏分时 + 下屏日K，两区都有网格）
 * ============================================================
 *  ★ 为什么不用 lv_canvas：canvas 要一块 w*h*2 的缓冲
 *    （296×148 一张就是 87 KB），虽然能放 PSRAM，但要确认
 *    LV_USE_CANVAS 开着，而这块板子从没验证过。改用
 *    【矩形（蜡烛/网格）+ lv_line（折线/均线）】—— 全是核心组件，零配置风险。
 *  ★ 代价是对象数：30 根 × 2（实体 + 影线）+ 3 均线 + 12 网格 ≈ 90 个（外加文字）。
 *    所以 K线屏【必须】在返回时删掉，不能和列表屏长住 ——
 *    两屏同存会把 LVGL 池（64 KB）吃紧。
 *  ★ 折线的点数组必须是【静态】的：lv_line 保存的是指针，不拷贝。
 * ============================================================ */

/* 第 i 根蜡烛的中心 x。
 * ★ 分母恒为 K_N（30）而不是 k->n：数据只取到 20 根时右侧对齐、
 *   每根宽度不变，不会让"今天的柱子"因为变胖而看着像异常。*/
static int kbar_x(int i, int n)
{
    return KG_PLOT_X +
           (int)(((float)(K_N - n) + (float)i + 0.5f) * (float)KG_PLOT_W / (float)K_N);
}

/* ---------- 上屏：分时（当日 5 分钟线）---------- */
static void mline_draw(const kline_t *k)
{
    /* ★ static：mline_t ≈ 260 B。放栈上不至于溢出，但本函数在 LVGL 任务
     *   （栈 8 KB）里被调，且和 kline_t 的快照同栈 —— 能省就省。*/
    static mline_t m;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    m = s_m;
    xSemaphoreGive(s_mtx);

    if (!k_m_line) return;

    /* 没数据：折线与基准线一起藏掉，标题改成提示。
     * ★ 不要只是"不画"——那样屏幕上留一个空框，看不出是"今天没数据"
     *   还是"程序挂了"。有字才算把话说清楚。*/
    if (!m.ok || m.n < 2) {
        if (k_m_head) lv_label_set_text(k_m_head, "分时 · 暂无数据");
        lv_obj_add_flag(k_m_line, LV_OBJ_FLAG_HIDDEN);
        if (k_m_base) lv_obj_add_flag(k_m_base, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* 基准（昨收）：优先用【日K的上一根收盘】——那才是真正的昨收；
     * 日K还没回来（或失败）时，退回分时当日第一根的开盘价。*/
    float base = m.base;
    if (k->ok && k->n >= 2) base = k->close[k->n - 2];
    if (base <= 0.f) base = m.close[0];
    if (base <= 0.f) base = 1.f;                 /* 兜底，防除零 */

    /* ★★ 以基准为中心取【对称】值域 —— 这是分时图的标准画法：
     *    开盘价恒在图的正中，"涨了 1%" 和 "跌了 1%" 的高度一模一样、
     *    一眼可比。若按 min~max 取域，单边行情会把图挤到一角，看着像坏了。*/
    float dev = 0.f;
    for (int i = 0; i < m.n; i++) {
        float d = m.close[i] - base;
        if (d < 0.f) d = -d;
        if (d > dev) dev = d;
    }
    if (dev < base * 0.002f) dev = base * 0.002f;   /* 全平盘也得有刻度，否则缩成一个点 */
    dev *= 1.15f;                                   /* 留边，最高/最低不贴框 */

    const float sc = (float)KG_A_H / (2.f * dev);
    for (int i = 0; i < m.n; i++) {
        k_m_pts[i].x = KG_PLOT_X + (i * KG_PLOT_W) / (m.n - 1);
        k_m_pts[i].y = KG_A_Y + (int)((base + dev - m.close[i]) * sc);
    }
    lv_line_set_points(k_m_line, k_m_pts, m.n);
    lv_obj_remove_flag(k_m_line, LV_OBJ_FLAG_HIDDEN);

    /* 整条线一色，方向由【最后一根 vs 基准】决定。
     * ★ 单条 lv_line 做不到"基准以上红、以下绿"的分段染色（颜色是对象属性，
     *   不是点的属性）⇒ 与其不加方向感，不如整条按涨跌染色：红涨绿跌是
     *   国内硬规矩，宁可粗糙也不能反。*/
    lv_obj_set_style_line_color(k_m_line,
                                (m.close[m.n - 1] >= base) ? C_UP : C_DN, 0);

    /* 基准线固定在正中的那条网格位置（KG_A_H/2）—— 因为值域是对称的，
     * 基准必然落在中线上，不需要再算一次。*/
    if (k_m_base) {
        lv_obj_set_pos(k_m_base, KG_PLOT_X, KG_A_Y + KG_A_H / 2);
        lv_obj_set_size(k_m_base, KG_PLOT_W, 1);
        lv_obj_remove_flag(k_m_base, LV_OBJ_FLAG_HIDDEN);
    }

    if (k_m_head) {
        char b[32];
        float pct = (m.close[m.n - 1] - base) / base * 100.f;
        snprintf(b, sizeof(b), "分时 · 今日 %+.2f%%", pct);
        lv_label_set_text(k_m_head, b);
    }
}

/* ---------- 下屏：日K（蜡烛 + 三条均线）---------- */
static void kdraw_candles(const kline_t *k)
{
    /* 价格范围（含上下 3% 边距，让最高/最低不贴边）*/
    float pmin = k->low[0], pmax = k->high[0];
    for (int i = 1; i < k->n; i++) {
        if (k->low[i]  < pmin) pmin = k->low[i];
        if (k->high[i] > pmax) pmax = k->high[i];
    }
    if (pmax <= pmin) return;
    float pad = (pmax - pmin) * 0.03f;
    pmin -= pad; pmax += pad;

    const float sc = (float)KG_B_H / (pmax - pmin);

    for (int i = 0; i < k->n; i++) {
        int xc = kbar_x(i, k->n);
        int yh = KG_B_Y + (int)((pmax - k->high[i])  * sc);
        int yl = KG_B_Y + (int)((pmax - k->low[i])   * sc);
        int yo = KG_B_Y + (int)((pmax - k->open[i])  * sc);
        int yc = KG_B_Y + (int)((pmax - k->close[i]) * sc);

        lv_color_t col = (k->close[i] >= k->open[i]) ? C_UP : C_DN;

        /* 影线（细矩形）*/
        if (k_wick[i]) {
            if (yl <= yh) yl = yh + 1;
            lv_obj_set_pos(k_wick[i], xc - 1, yh);
            lv_obj_set_size(k_wick[i], 2, yl - yh);
            lv_obj_set_style_bg_color(k_wick[i], col, 0);
            /* ★★★ 10-08 补：这一句原来【漏了】。
             *   蜡烛对象在 ui_stock_open_kline() 里是带 HIDDEN 建的
             *   （因为建屏时还不知道要画几根），必须在这里取消隐藏才看得见。
             *   漏了它的症状非常隐蔽：三条均线照常画出来（均线那边有
             *   lv_obj_clear_flag，见本函数末尾），只有蜡烛一根不显示 ——
             *   看着像"K线图只画了均线"，而代码上一切正常。
             *   ⇒ 真机截图才发现的。同类对象成对出现时，"建隐藏 / 画时取消隐藏"
             *     两句必须成对写，别只写一半。*/
            lv_obj_remove_flag(k_wick[i], LV_OBJ_FLAG_HIDDEN);
        }
        /* 实体 */
        if (k_body[i]) {
            int yt = (yo < yc) ? yo : yc;
            int hh = (yo < yc) ? (yc - yo) : (yo - yc);
            if (hh < 2) hh = 2;           /* 平盘也要看得见 */
            lv_obj_set_pos(k_body[i], xc - 3, yt);
            lv_obj_set_size(k_body[i], 6, hh);
            lv_obj_set_style_bg_color(k_body[i], col, 0);
            lv_obj_set_style_bg_opa(k_body[i], LV_OPA_COVER, 0);
            lv_obj_remove_flag(k_body[i], LV_OBJ_FLAG_HIDDEN);   /* ★ 同上 */
        }
    }
    /* 数据不足的蜡烛隐藏（右对齐后左边那几根）*/
    for (int i = k->n; i < K_N; i++) {
        if (k_wick[i]) lv_obj_add_flag(k_wick[i], LV_OBJ_FLAG_HIDDEN);
        if (k_body[i]) lv_obj_add_flag(k_body[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* 三条均线 */
    const int periods[3] = { 5, 10, 20 };
    for (int m = 0; m < 3; m++) {
        float ma[K_N];
        calc_ma(k, periods[m], ma);
        int cnt = 0;
        for (int i = 0; i < k->n; i++) {
            if (ma[i] < 0.f) continue;
            k_ma_pts[m][cnt].x = kbar_x(i, k->n);
            k_ma_pts[m][cnt].y = KG_B_Y + (int)((pmax - ma[i]) * sc);
            cnt++;
        }
        if (k_ma_line[m] && cnt >= 2) {
            lv_line_set_points(k_ma_line[m], k_ma_pts[m], cnt);
            lv_obj_clear_flag(k_ma_line[m], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ---------- 整屏刷新：顶栏 + 上屏 + 下屏 ---------- */
static void kline_draw(void)
{
    /* ★ static 而不是栈上：kline_t ≈ 1.2 KB（30 根 × 5 个 float + 日期），
     *   LVGL 任务栈只有 8 KB，放栈上会直接溢出。
     *   本函数只在 LVGL 任务里被调（tick / 打开时），无重入。
     * ★★ 一次快照清掉【两个】dirty —— 日K与分时是同一次请求取的，
     *    分两次上屏只会让屏幕闪一下再定，没必要。*/
    static kline_t k;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    k = s_k;
    s_k_dirty = false;
    s_m_dirty = false;
    xSemaphoreGive(s_mtx);

    /* 顶栏：日K 没回来时也给个占位，别让顶栏空着 */
    if (k.ok) {
        if (k_title) lv_label_set_text(k_title, k.name);
        if (k_price) {
            char b[24];
            snprintf(b, sizeof(b), "%.2f", k.last);
            lv_label_set_text(k_price, b);
        }
        if (k_chg) {
            char b[32];
            snprintf(b, sizeof(b), "%s %+.2f  %+.2f%%", chg_arrow(k.chg), k.chg, k.pct);
            lv_label_set_text(k_chg, b);
            lv_obj_set_style_text_color(k_chg, chg_color(k.chg), 0);
        }
    }

    /* ★ 两个区【各自独立】判定：分时挂了不该连累日K，反之亦然。
     *   所以这里不写 `if (!k.ok) return;` —— 那会把分时也一起吞掉。*/
    mline_draw(&k);

    if (k.ok && k.n >= 5) kdraw_candles(&k);
    else if (k_k_head) lv_label_set_text(k_k_head, "日K · 暂无数据");
}

static void stk_tick_cb(lv_timer_t *t)
{
    (void)t;

    if (s_dirty) list_apply();
    /* ★ 两个区一起刷：日K与分时同一次请求回来，任一到位就整屏重画。
     *   写成 (s_k_dirty || s_m_dirty) 而不是只看 s_k_dirty —— 否则
     *   "日K失败但分时成功"时，分时永远上不了屏。*/
    if ((s_k_dirty || s_m_dirty) && s_k_scr) kline_draw();

    if (l_net && !s_valid) {
        TickType_t el = xTaskGetTickCount() - s_enter_tick;
        if (el > pdMS_TO_TICKS(5000)) {
            bool netup = app_net_connected();
            lv_label_set_text(l_net, netup ? "获取失败" : "网络未连接");
            lv_obj_set_style_text_color(l_net, C_DIM, 0);
        }
    }
}

/* ============================================================
 *  添加自选（数字键盘面板）
 * ============================================================ */
static char s_input[10];
static lv_obj_t *s_in_lbl, *s_add_hint;

static void add_panel_close(void);

static void add_refresh(void)
{
    if (s_in_lbl) lv_label_set_text(s_in_lbl, s_input[0] ? s_input : "6 位代码");
    if (s_in_lbl) lv_obj_set_style_text_color(s_in_lbl, s_input[0] ? C_TXT : C_DIM, 0);
}

static void key_cb(lv_event_t *e)
{
    const char *k = (const char *)lv_event_get_user_data(e);
    size_t n = strlen(s_input);
    if (strcmp(k, "<") == 0) {                 /* 退格 */
        if (n) s_input[n - 1] = '\0';
    } else if (n < 6) {                        /* 最多 6 位 */
        s_input[n] = k[0];
        s_input[n + 1] = '\0';
    }
    add_refresh();
}

static void add_build(void);                   /* 前向声明 */

static void add_ok_cb(lv_event_t *e)
{
    (void)e;
    if (!s_input[0]) return;

    char code[10];
    norm_code(s_input, code, sizeof(code));

    /* 重复检查 */
    for (int i = 0; i < s_n; i++) {
        if (strcmp(s_code[i], code) == 0) {
            if (s_add_hint) lv_label_set_text(s_add_hint, "已存在");
            return;
        }
    }
    if (s_n >= STK_MAX) {
        if (s_add_hint) lv_label_set_text(s_add_hint, "已达上限");
        return;
    }

    snprintf(s_code[s_n], sizeof(s_code[0]), CODE_FMT, code);
    s_n++;
    syms_save();

    ESP_LOGI(TAG, "添加自选：%s（共 %d 个）", code, s_n);

    add_panel_close();
    s_force = true;                            /* 立刻拉一次，不等周期 */
    ui_stock_rebuild_list();                   /* 列表屏要重建（多了一张卡）*/
}

static void add_panel_close(void)
{
    if (s_add_panel) { lv_obj_del_async(s_add_panel); s_add_panel = NULL; }
    s_in_lbl = NULL;
    s_add_hint = NULL;
}

static void add_cancel_cb(lv_event_t *e)
{
    (void)e;
    add_panel_close();
}

static void add_build(void)
{
    s_input[0] = '\0';

    lv_obj_t *p = lv_obj_create(s_scr);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, SCR_W, SCR_H);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x0A0F1E), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_70, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    s_add_panel = p;

    lv_obj_t *card = scard(p, 14, 70, SCR_W - 28, 350, lv_color_hex(0x1A2340), 16);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);

    sclabel(card, 12, SCR_W - 28, 0, F_MID, C_TXT, "添加自选");
    s_in_lbl = sclabel(card, 44, SCR_W - 28, 0, F_READ, C_DIM, "6 位代码");
    s_add_hint = sclabel(card, 74, SCR_W - 28, 0, F_SMALL, C_GOLD, " ");

    /* 数字键盘：3 列 × 4 行（最后一格空着，让 "0" 居中更好按）*/
    static const char *KEYS[12] = {
        "1", "2", "3",
        "4", "5", "6",
        "7", "8", "9",
        "",  "0", "<",
    };
    const int kw = 62, kh = 44, kx0 = 26, ky0 = 104, kdx = 8, kdy = 8;
    for (int i = 0; i < 12; i++) {
        if (KEYS[i][0] == '\0') continue;          /* 空格位不建对象 */
        int r = i / 3, c = i % 3;
        lv_obj_t *b = scard(card, kx0 + c * (kw + kdx), ky0 + r * (kh + kdy),
                            kw, kh, lv_color_hex(0x2A3556), 10);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, key_cb, LV_EVENT_CLICKED, (void *)KEYS[i]);
        lv_obj_t *lb = sclabel(b, 10, kw, 0, F_MID, C_TXT, KEYS[i]);
        lv_obj_clear_flag(lb, LV_OBJ_FLAG_CLICKABLE);
    }

    /* 底部两个键：取消 / 确定 */
    lv_obj_t *cx = scard(card, 26, 300, 120, 40, lv_color_hex(0x2A3556), 10);
    lv_obj_set_style_bg_opa(cx, LV_OPA_COVER, 0);
    lv_obj_add_flag(cx, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cx, add_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *c1 = sclabel(cx, 10, 120, 0, F_MID, C_DIM, "取 消");
    lv_obj_clear_flag(c1, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *ok = scard(card, 160, 300, 120, 40, lv_color_hex(0x1E5E3A), 10);
    lv_obj_set_style_bg_opa(ok, LV_OPA_COVER, 0);
    lv_obj_add_flag(ok, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ok, add_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *c2 = sclabel(ok, 10, 120, 0, F_MID, C_TXT, "确 定");
    lv_obj_clear_flag(c2, LV_OBJ_FLAG_CLICKABLE);

    add_refresh();
}

static void add_open_cb(lv_event_t *e)
{
    (void)e;
    if (s_add_panel) return;
    add_build();
}

/* ============================================================
 *  列表屏
 * ============================================================ */
static void card_cb(lv_event_t *e)
{
    ui_stock_open_kline((int)(intptr_t)lv_event_get_user_data(e));
}

static void home_cb(lv_event_t *e)
{
    (void)e;
    ui_shell_back();
}

/* 建整个列表屏（init 与"添加/删除后重建"共用）*/
static void list_build(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_style_bg_color(s_scr, C_BG_TOP, 0);
    lv_obj_set_style_bg_grad_color(s_scr, C_BG_BOT, 0);
    lv_obj_set_style_bg_grad_dir(s_scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(s_scr);                 /* ★ 漏了这行 = 整屏纯白 */

    /* 顶栏 */
    slabel(s_scr, 16, 12, F_MID, C_TXT, "自选行情");
    l_net = slabel(s_scr, 150, 16, F_SMALL, C_ACC, "载入中…");

    /* 「＋」添加按钮（右上角）*/
    lv_obj_t *addb = scard(s_scr, SCR_W - 14 - 52, 8, 52, 34, lv_color_hex(0x2A3556), 10);
    lv_obj_set_style_bg_opa(addb, LV_OPA_60, 0);
    lv_obj_add_flag(addb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(addb, add_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *al = sclabel(addb, 6, 52, 0, F_MID, C_GOLD, "＋");
    lv_obj_clear_flag(al, LV_OBJ_FLAG_CLICKABLE);

    /* ---- 滚动列表（双列卡片）---- */
    int rows = (s_n + 1) / 2;
    int cont_h = 396;
    int inner_h = rows * 80 + 4;
    if (inner_h < cont_h) inner_h = cont_h;

    lv_obj_t *cont = lv_obj_create(s_scr);
    lv_obj_remove_style_all(cont);
    lv_obj_set_pos(cont, 0, 50);
    lv_obj_set_size(cont, SCR_W, cont_h);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);

    /* 内容容器（决定可滚动高度）*/
    lv_obj_t *pad = lv_obj_create(cont);
    lv_obj_remove_style_all(pad);
    lv_obj_set_pos(pad, 0, 0);
    lv_obj_set_size(pad, SCR_W, inner_h);
    lv_obj_set_style_bg_opa(pad, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(pad, LV_OBJ_FLAG_SCROLLABLE);

    const int cw = 148, ch = 74, cx0 = 8, cy0 = 4, cdx = 156, cdy = 80;
    for (int i = 0; i < s_n && i < STK_MAX; i++) {
        int r = i / 2, c = i % 2;
        lv_obj_t *k = scard(pad, cx0 + c * cdx, cy0 + r * cdy, cw, ch,
                            lv_color_hex(0x223055), 12);
        lv_obj_set_style_bg_opa(k, LV_OPA_40, 0);
        lv_obj_add_event_cb(k, card_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        l_name[i]  = slabel(k, 12, 8, F_SMALL, C_DIM, s_code[i]);
        l_price[i] = slabel(k, 12, 26, F_READ, C_TXT, "--");
        l_chg[i]   = slabel(k, 12, 52, F_SMALL, C_FLAT, "--");
        lv_obj_clear_flag(l_name[i],  LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(l_price[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(l_chg[i],   LV_OBJ_FLAG_CLICKABLE);
    }
    /* 清空表尾指针，避免 worker 刷到不存在的卡 */
    for (int i = s_n; i < STK_MAX; i++) { l_name[i] = l_price[i] = l_chg[i] = NULL; }

    /* 底栏 */
    l_upd = slabel(s_scr, 16, 452, F_SMALL, C_DIM, "更新 --:--");

    lv_obj_t *btn = scard(s_scr, SCR_W - 14 - 84, 446, 84, 32, lv_color_hex(0x2A3556), 16);
    lv_obj_set_style_bg_opa(btn, LV_OPA_60, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, home_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = sclabel(btn, 7, 84, 0, F_SMALL, C_TXT, "主 页");
    lv_obj_clear_flag(bl, LV_OBJ_FLAG_CLICKABLE);
}

/* 供添加面板调用：重建列表屏（异步删旧的，再建新的）*/
static void list_rebuild_async(void)
{
    if (s_scr) { lv_obj_t *old = s_scr; s_scr = NULL; lv_obj_del_async(old); }
    list_build();
}

void ui_stock_rebuild_list(void)
{
    list_rebuild_async();
}

/* ============================================================
 *  ★★★ 进 K线屏前把列表屏【拆掉】—— 这不是优化，是保命
 * ============================================================
 *  真机实测（10-08 22:0x）：从列表点进 K线屏，几秒后板子自己重启。
 *  抓到的现场不是我的绘制代码，是渲染层：
 *      PC = lv_draw_add_task()  lv_draw.c:100   new_task->area = *coords;
 *      EXCCAUSE = 0x1d (StoreProhibited)  EXCVADDR = 0x00000008
 *  ⇒ 那一行的 new_task 是 NULL —— 分配失败了。
 *
 *  ★★ 为什么是"池"而不是"堆"：
 *   本板 LVGL 走【自带静态 TLSF 池】（CONFIG_LV_MEM_SIZE=65536，
 *   即 .bss 里一块 64 KB 的 work_mem_int），不走 IDF 的 heap。
 *   所以"内部 RAM 还剩 70 KB"这件事跟它一点关系都没有 ——
 *   串口上那句 internal free=69983 会让人以为内存很宽裕，其实池早就见底了。
 *
 *  ★★★ 为什么 K线屏特别吃池：lv_line 是【每段一个 draw task】。
 *   分时折线 48 点 = 47 段；三条均线 26/21/11 点 = 55 段；
 *   加上 60 根蜡烛 + 12 条网格线 ≈ 180 个 draw task，一次刷新要瞬时
 *   向池里要 180 多份（每份 = lv_draw_task_t + 线条 dsc，200 B 上下）。
 *   而列表屏那 100 多个对象常驻着 —— K线屏是【全屏】盖住它的，
 *   它一个像素都露不出来，却白白占着池。
 *
 *  ★ 方案：进 K线屏前把列表屏删掉，返回时 list_build() 原样重建。
 *    代价是返回列表时会重建一次（一屏放得下 10 个自选、本来也不滚动，
 *    不需要保留滚动位置），换来的是池里多出一整屏对象的余量。
 *
 *  ⚠ 必须用 lv_obj_del_async 而不是 lv_obj_delete：
 *    ui_stock_open_kline 也会从 card_cb 进来（点卡片 = 列表屏子对象的
 *    事件回调），在回调里【同步】删掉自己的祖先屏 = 当场 use-after-free。
 *    异步删除会把真正的析构推到本轮 lv_timer_handler 里、且在刷新之前
 *    完成 —— 于是画 K线屏时那份内存已经还给池了。*/
static void list_drop(void)
{
    if (!s_scr) return;
    /* ★ 指针必须先全清：tick 每秒会调 list_apply()，
     *   它靠 `if (l_xxx[i])` 判断要不要写 label；不清就是悬空指针。*/
    l_net = l_upd = NULL;
    for (int i = 0; i < STK_MAX; i++) { l_name[i] = l_price[i] = l_chg[i] = NULL; }
    lv_obj_t *old = s_scr;
    s_scr = NULL;
    lv_obj_del_async(old);
}

/* ============================================================
 *  K线屏（上：分时 / 下：日K）
 * ============================================================ */

/* 一个绘图区的外框（含标题条的位置留给调用者）。
 * ★ 用 sbox 而不是 scard：绘图底色要比卡片更暗，才能把亮色的折线/蜡烛衬出来；
 *   scard 是"玻璃感白 20%"，做图底会把蜡烛糊掉。*/
static void kpane(lv_obj_t *par, int y, int h)
{
    lv_obj_t *p = sbox(par, 8, y, SCR_W - 16, h, lv_color_hex(0x101830), 10);
    lv_obj_set_style_bg_opa(p, LV_OPA_40, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, C_EDGE, 0);
    lv_obj_set_style_border_opa(p, LV_OPA_30, 0);
}

/* 一个绘图区的网格：3 横 + 3 竖（把区域分成 4×4 格）。
 * ★ 为什么用矩形而不是 lv_line：一条 lv_line 会把相邻点【连起来】，
 *   画不出"十字网格"这种互不相连的线段；用 1px 的矩形最直接，
 *   而且半径/颜色都能直接设。代价是每格多一个对象（12 个/屏），
 *   在 64 KB 的 LVGL 池里是划算的。*/
static void kgrid(lv_obj_t *par, int y, int h, lv_obj_t **out)
{
    for (int i = 1; i <= 3; i++) {
        int gy = y + h * i / 4;
        int gx = KG_PLOT_X + KG_PLOT_W * i / 4;
        out[i - 1] = sbox(par, KG_PLOT_X, gy, KG_PLOT_W, 1, KG_GRID, 0);
        lv_obj_set_style_bg_opa(out[i - 1], LV_OPA_40, 0);
        out[i + 2] = sbox(par, gx, y, 1, h, KG_GRID, 0);
        lv_obj_set_style_bg_opa(out[i + 2], LV_OPA_40, 0);
    }
}

/* 把 K线屏的全部控件指针置空。
 * ★★ 抽出来成一个函数，是因为这段原来在 k_back_cb() 和 ui_stock_leave()
 *    里【各抄一份】—— 10-08 新增 6 个指针时，两边都漏了几个，
 *    症状是"返回后再进 K线，屏幕闪一下旧数据"（悬空指针被 tick 用到）。
 *    两处同源的清单，就必须只有一份。*/
static void kline_forget(void)
{
    k_title = k_price = k_chg = NULL;
    for (int i = 0; i < K_N; i++) { k_wick[i] = k_body[i] = NULL; }
    for (int i = 0; i < 3; i++) k_ma_line[i] = NULL;
    k_m_line = k_m_base = NULL;
    k_m_head = k_k_head = NULL;
    for (int i = 0; i < 6; i++) { k_gr_m[i] = NULL; k_gr_k[i] = NULL; }
}

static void k_back_cb(lv_event_t *e)
{
    (void)e;
    /* ★★ 顺序（10-08 改）：先重建列表屏，再把 K线屏交给异步删除。
     *   为什么是这个顺序：list_build() 内部会 lv_screen_load，"当前屏"
     *   立刻变成列表屏；之后 K线屏才被删 —— 删的那一刻它已经不是当前屏了。
     *   反过来写就会有一瞬间"当前屏正在被析构"。*/
    list_build();
    list_apply();                     /* 立刻把已知行情填回去，别让用户看 "--" */
    if (s_k_scr) { lv_obj_t *o = s_k_scr; s_k_scr = NULL; lv_obj_del_async(o); }
    kline_forget();
}

void ui_stock_open_kline(int idx)
{
    if (idx < 0 || idx >= s_n) return;

    /* ★★ 先拆列表屏腾 LVGL 池（理由见 list_drop 的长注释）。
     *   放在"清旧 K线屏"之前：稳态永远是"列表屏 XOR K线屏"，不会两者同存。*/
    list_drop();

    /* 再清掉可能残留的旧 K线屏 */
    if (s_k_scr) { lv_obj_t *o = s_k_scr; s_k_scr = NULL; lv_obj_del_async(o); }
    kline_forget();

    /* ★★ 把上一个标的的数据也清掉。不清的症状很具体：点开"横店东磁"，
     *   头几百毫秒会先显示【上一次看的那只】的顶栏和K线，然后才跳过来 ——
     *   看着像点错了。清数据 + 清 dirty，屏幕就是干净的等待态。*/
    if (s_mtx) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        memset(&s_k, 0, sizeof(s_k));
        memset(&s_m, 0, sizeof(s_m));
        s_k_dirty = false;
        s_m_dirty = false;
        xSemaphoreGive(s_mtx);
    }

    s_k_scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_k_scr);
    lv_obj_set_style_bg_color(s_k_scr, C_BG_TOP, 0);
    lv_obj_set_style_bg_grad_color(s_k_scr, C_BG_BOT, 0);
    lv_obj_set_style_bg_grad_dir(s_k_scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_k_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_k_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ── 顶栏：名称 / 现价 / 涨跌 ── */
    k_title = slabel(s_k_scr, 14, 6, F_MID, C_TXT, s_code[idx]);
    k_price = slabel(s_k_scr, 14, 26, F_READ, C_GOLD, "--");
    k_chg   = slabel(s_k_scr, 136, 32, F_SMALL, C_FLAT, "--");

    /* ── 上屏：分时 ──
     * ★★ 建屏顺序 = z 序（后建的盖在前面）。必须是：
     *      底框 → 网格 → 基准线 → 折线 → 标题
     *    把标题提前建，就会被底框盖住（真机上表现为"标题不见了"，
     *    而代码里明明有那一行）。*/
    kpane(s_k_scr, KG_PANE_A_Y, KG_PANE_H);
    k_m_head = slabel(s_k_scr, 18, KG_PANE_A_Y + 5, F_SMALL, C_ACC, "分时 · 今日");
    kgrid(s_k_scr, KG_A_Y, KG_A_H, k_gr_m);
    /* 基准线（昨收）：值域对称 ⇒ 它恒在绘图区正中，位置建屏时就定死 */
    k_m_base = sbox(s_k_scr, KG_PLOT_X, KG_A_Y + KG_A_H / 2, KG_PLOT_W, 1, C_DIM, 0);
    lv_obj_set_style_bg_opa(k_m_base, LV_OPA_60, 0);
    lv_obj_add_flag(k_m_base, LV_OBJ_FLAG_HIDDEN);
    /* 分时折线（点数 48 上下，最坏 60）*/
    k_m_line = lv_line_create(s_k_scr);
    lv_obj_set_style_line_width(k_m_line, 2, 0);
    lv_obj_set_style_line_color(k_m_line, C_ACC, 0);
    lv_obj_set_style_line_rounded(k_m_line, true, 0);
    lv_obj_add_flag(k_m_line, LV_OBJ_FLAG_HIDDEN);

    /* ── 下屏：日K ── */
    kpane(s_k_scr, KG_PANE_B_Y, KG_PANE_H);
    k_k_head = slabel(s_k_scr, 18, KG_PANE_B_Y + 5, F_SMALL, C_ACC, "日K · 近 30 日");
    kgrid(s_k_scr, KG_B_Y, KG_B_H, k_gr_k);

    /* 蜡烛：影线 + 实体。★ 建在 s_k_scr 上而不是绘图框上，
     *   是为了用【绝对坐标】（框有 1px 边框，坐标会整体偏移 1px）。*/
    for (int i = 0; i < K_N; i++) {
        k_wick[i] = sbox(s_k_scr, 0, KG_B_Y, 2, 10, C_FLAT, 0);
        lv_obj_add_flag(k_wick[i], LV_OBJ_FLAG_HIDDEN);
        k_body[i] = sbox(s_k_scr, 0, KG_B_Y, 6, 10, C_FLAT, 1);
        lv_obj_add_flag(k_body[i], LV_OBJ_FLAG_HIDDEN);
    }
    /* 均线（建在蜡烛之后 ⇒ 画在蜡烛之上）*/
    for (int m = 0; m < 3; m++) {
        k_ma_line[m] = lv_line_create(s_k_scr);
        lv_obj_set_style_line_width(k_ma_line[m], 2, 0);
        lv_obj_set_style_line_color(k_ma_line[m], m == 0 ? C_MA5 : (m == 1 ? C_MA10 : C_MA20), 0);
        lv_obj_set_style_line_rounded(k_ma_line[m], true, 0);
        lv_obj_add_flag(k_ma_line[m], LV_OBJ_FLAG_HIDDEN);
    }

    /* ── 图例 + 底栏 ── */
    slabel(s_k_scr, 16, 412, F_SMALL, C_MA5,  "— MA5");
    slabel(s_k_scr, 96, 412, F_SMALL, C_MA10, "— MA10");
    slabel(s_k_scr, 184, 412, F_SMALL, C_MA20, "— MA20");
    slabel(s_k_scr, 16, 448, F_SMALL, C_DIM,  "上分时 下日K");

    lv_obj_t *btn = scard(s_k_scr, SCR_W - 14 - 84, 438, 84, 32, lv_color_hex(0x2A3556), 16);
    lv_obj_set_style_bg_opa(btn, LV_OPA_60, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, k_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = sclabel(btn, 7, 84, 0, F_SMALL, C_TXT, "返 回");
    lv_obj_clear_flag(bl, LV_OBJ_FLAG_CLICKABLE);

    lv_screen_load(s_k_scr);

    /* 发起取数（worker 下一轮就处理；日K 与分时是同一次请求一起取的）*/
    snprintf(s_k_code, sizeof(s_k_code), CODE_FMT, s_code[idx]);
    s_k_req = true;

    ESP_LOGI(TAG, "打开K线屏：%s（上分时 / 下日K）", s_k_code);
}

/* ============================================================
 *  进入 / 离开
 * ============================================================ */
void ui_stock_init(void)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();

    if (!s_task) {
        if (xTaskCreate(stk_worker, "stk_fetch", 5120, NULL, 2, &s_task) != pdPASS) {
            ESP_LOGE(TAG, "取数任务创建失败 —— 界面照常，但不会刷新");
            s_task = NULL;
        }
    }

    syms_load();

    s_alive = true;
    s_force = true;
    s_enter_tick = xTaskGetTickCount();

    list_build();

    if (s_tick) lv_timer_del(s_tick);
    s_tick = lv_timer_create(stk_tick_cb, 1000, NULL);

    if (s_valid) list_apply();

    ESP_LOGI(TAG, "股票 App 已进入（自选 %d 个）(internal free=%u largest=%u)",
             s_n,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

/* ★ 10-08 调试用：直接弹开「添加自选」面板（走的就是 ＋ 按钮那个回调）。*/
void ui_stock_demo_add(void)
{
    add_open_cb(NULL);
}

void ui_stock_leave(void)
{
    if (s_mtx) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_alive = false;
        s_dirty = false;
        xSemaphoreGive(s_mtx);
    } else {
        s_alive = false;
        s_dirty = false;
    }

    if (s_tick) { lv_timer_del(s_tick); s_tick = NULL; }

    /* 面板 / K线屏都要收干净 */
    if (s_add_panel) { lv_obj_del_async(s_add_panel); s_add_panel = NULL; }
    s_in_lbl = s_add_hint = NULL;
    if (s_k_scr) { lv_obj_del_async(s_k_scr); s_k_scr = NULL; }
    kline_forget();

    l_net = l_upd = NULL;
    for (int i = 0; i < STK_MAX; i++) { l_name[i] = l_price[i] = l_chg[i] = NULL; }

    if (s_scr) { lv_obj_del_async(s_scr); s_scr = NULL; }

    ESP_LOGI(TAG, "股票 App 已退出");
}
