/*
 * 拾声 · 多产品外壳 (ui_shell) —— 主菜单 + App 调度   v1.0
 * ============================================================
 *  板型：LCD Wiki ES3C35P（ESP32-S3 N16R8 / ST77922 320×480 竖屏 / FT6336G）
 *
 *  这一层做三件事：
 *    ① 建【主菜单】屏（四张产品卡），开机就停在这里；
 *    ② 点卡片 → 调该产品的 enter()，由它自己建屏并 lv_screen_load；
 *    ③ 产品内调 ui_shell_back() → 先切回主菜单屏，再销毁该产品。
 *
 *  ★★★ 顺序：back() 必须【先 lv_screen_load(主菜单) 再销毁 App】。
 *    反过来的话，销毁的是"当前活动屏"—— LVGL 删除活动屏时会去找下一个
 *    候选屏，而这时主菜单屏虽然存在却没被指定过，界面上会出现
 *    一帧空白甚至直接停在已删屏上（对象树已释放，下一帧就踩空）。
 *    先切后删，任何时候被删的都是"非活动屏"，这是安全路径。
 *
 *  ★★ 内存纪律：进入才创建、离开即销毁（见 ui_shell.h 的说明）。
 *    主菜单屏常驻 —— 它很轻（几块 + 十几个 label），
 *    换来的是每次退回都【零重建】，不会在菜单上反复吃对象池。
 *
 *  ★ 与灵签的关系：灵签（ui_lingqian.c）是这套外壳下的【第一个产品】，
 *    它的 ui_lingqian_init()/ui_lingqian_leave() 就是 enter/leave 契约的实现。
 * ============================================================
 */
#include "ui_shell.h"

#include "ui_lingqian.h"
#include "ui_weather.h"      /* ★ 10-08：第 2 个产品 */
#include "ui_stock.h"        /* ★ 10-08：第 3 个产品 */
#include "ui_radio_v.h"      /* ★ 10-08 晚：第 4 个产品（竖版拾声电台，置顶）*/
#include "app_settings.h"    /* ★ 10-08：全局设置（音量/背光/静音）*/
#include "app_prov.h"        /* ★ 10-08：设置里加「配网」（SoftAP + 网页）*/
#include "app_sys.h"         /* ★ 10-08：设置里加「设备信息」（硬件状态）*/
#include "app_pwr.h"         /* ★ 10-08 晚：主菜单左上角「关机」→ app_pwr_shutdown()*/
#include "app_version.h"

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "XSHELL";

/* ---------- 字体：与灵签共用同一套霞鹜文楷（_gen_fonts_lq.py 产出）----------
 *  ★ 外壳不另起一套字体：一来省 Flash（这 4 档合计就是几百 KB），
 *    二来保证"三个产品是同一台机器"的观感。*/
extern const lv_font_t xs_font_14;
extern const lv_font_t xs_font_18;
extern const lv_font_t xs_font_22;
extern const lv_font_t xs_font_30;
#define F_HERO  (&xs_font_30)
#define F_READ  (&xs_font_22)
#define F_MID   (&xs_font_18)
#define F_SMALL (&xs_font_14)

/* ---------- 外壳配色：沿用灵签的宣纸调（保持整机一致）---------- */
#define C_PAPER   lv_color_hex(0xF3ECE0)   /* 宣纸底 */
#define C_PAPER2  lv_color_hex(0xFBF6EA)   /* 更亮的纸 */
#define C_INK     lv_color_hex(0x3B2E1E)   /* 墨褐 */
#define C_INK2    lv_color_hex(0x6B5A45)   /* 次墨 */
#define C_MUTE    lv_color_hex(0x9A8A74)   /* 淡墨 */
#define C_GOLD    lv_color_hex(0xB8914F)   /* 金棕 */
#define C_GOLD2   lv_color_hex(0xD9B978)   /* 亮金 */
#define C_LINE    lv_color_hex(0xE0D6C4)   /* 极淡分割线 */
#define C_SEAL    lv_color_hex(0xB3372C)   /* 朱砂 —— 落款署名用（10-08）*/

#define SCR_W    320
#define SCR_H    480

/* 卡片几何（2×2 宫格，全部由宏算出，不各写各的）
 *     ┌──────────┐ ┌──────────┐    卡0 ( 10,  96)  卡1 (165,  96)
 *     │  拾声卡  │ │  灵签卡  │    卡2 ( 10, 274)  卡3 (165, 274)
 *     └──────────┘ └──────────┘    每张 145×150，右下角 310/424
 *
 * ★★ 10-08 晚：3 张 → 4 张（新增「拾声电台」并【置顶】）。
 *    兰兰："4张都在首页" ＋ "也可以做双列，你做的好看点就是"
 *    ⇒ 4 条单列太像列表，改 2×2 宫格：
 *       纵向 328px（96~424）分两行 ⇒ 2×150 + 28 = 328
 *       横向 300px 分两列      ⇒ 2×145 + 10 = 300（左右各留 10）
 *    下界仍是 424 —— 再往下是 30px 落款，动不得（兰兰刚要求它醒目）。
 *    卡内：accent 实色圆底 + 30px 图标字 / 主标题 18px / 副标题 14px。
 *    ⚠️ 这几个宏是【唯一真源】，卡内元素的 y 都跟着 CARD_H 算，不要各写各的。*/
#define CARD_COLS 2
#define CARD_W    145
#define CARD_H    150
#define CARD_X0   10
#define CARD_DX   155          /* 145 宽 + 10 列间距 */
#define CARD_Y0   96
#define CARD_DY   178          /* 150 高 + 28 行间距 */

/* ============================================================
 *  产品注册表
 * ============================================================
 *  ★ enter = 建自己的屏并接管显示；leave = 拆干净（停 timer、停音频、删屏）。
 *  ★★ leave 里【必须】把常驻 timer 停掉再删对象 ——
 *     timer 回调每 20ms 访问一次页面对象，屏删了它还在跑就是悬空指针，
 *     崩起来是随机的、还常被误判成"别的地方有问题"（灵签 anim_cb 就是这种）。
 *  ★ 没实现的产品把 enter 留 NULL，外壳会点一下弹"开发中"，不会崩。*/
typedef struct {
    const char *title;      /* 卡片主标题（宫格里 18px，4 个汉字最舒服）*/
    const char *sub;        /* 副标题 / 一句话说明（宫格卡宽只有 145，压到 4 字内）*/
    const char *glyph;      /* ★ 10-08 晚：宫格卡片的图标 —— 单个汉字，30px*/
    uint32_t    accent;     /* 本产品的标识色（圆底实色 / 卡片渐变的 tint）*/
    void      (*enter)(void);
    void      (*leave)(void);
} app_desc_t;

/* ★★ 数组顺序 = 首页宫格的阅读顺序：左上 → 右上 → 左下 → 右下。
 *   10-08 晚按兰兰要求「拾声电台」放【最上方】（左上第一格）。*/
static const app_desc_t APPS[] = {
    { "拾声电台", "网络电台", "声", 0x2E7D6E, ui_radio_v_init,   ui_radio_v_leave   },
    { "观音灵签", "求签解签", "签", 0xB8914F, ui_lingqian_init,  ui_lingqian_leave  },
    { "天气时钟", "实时天气", "天", 0x3E6E9E, ui_weather_init,   ui_weather_leave   },
    { "股票行情", "自选涨跌", "股", 0xA63A2E, ui_stock_init,     ui_stock_leave     },
};
#define APP_N ((int)(sizeof(APPS) / sizeof(APPS[0])))

/* ============================================================
 *  UI 小工具（照灵签那套写：可点的元素一律有可见边界、自己绑事件）
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

static lv_obj_t *sframed(lv_obj_t *par, int x, int y, int w, int h,
                         lv_color_t fill, int radius, int bw, lv_color_t bc)
{
    lv_obj_t *o = sbox(par, x, y, w, h, fill, radius);
    lv_obj_set_style_border_width(o, bw, 0);
    lv_obj_set_style_border_color(o, bc, 0);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
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
 *  主菜单
 * ============================================================ */
static lv_obj_t *s_home;      /* 主菜单屏（常驻，永不删）*/
static int       s_cur = -1;  /* 当前产品下标；-1 = 正在看主菜单 */

static void card_cb(lv_event_t *e)
{
    ui_shell_launch((int)(intptr_t)lv_event_get_user_data(e));
}

/* 「开发中」提示：一条临时 label，1.6 秒后自己删掉
 * ★ 用 lv_timer 而不是 lv_anim：本工程统一规矩（见 ui_lingqian.c 文件头），
 *   lv_anim 中途删对象会踩悬空指针。*/
static void toast_del_cb(lv_timer_t *t)
{
    lv_obj_t *o = (lv_obj_t *)lv_timer_get_user_data(t);
    if (o) lv_obj_del(o);
    lv_timer_del(t);
}

static void toast(const char *msg)
{
    lv_obj_t *t = sframed(s_home, 40, 400, 240, 52, lv_color_hex(0x2A2118), 12, 0, C_LINE);
    sclabel(t, 15, 240, 0, F_MID, lv_color_hex(0xF3ECE0), msg);
    lv_timer_create(toast_del_cb, 1600, t);
}

/* ============================================================
 *  全局设置面板（10-08 兰兰：「设置按键在首页，不是到灵签里去设置」）
 * ============================================================
 *  ★ 这里放的是【整机级】的三个量：音量 / 亮度 / 声音。
 *    它们动的是硬件（ES8311 的 DAC 增益、背光 LEDC），三个产品共用一份，
 *    状态存在 app_settings（xs_cfg）里 —— 所以这里调完，进灵签/天气/股票
 *    都是同一个值，不会"首页调了灵签里没变"。
 *  ★ 几何宏【照抄 ui_lingqian.c 的设置面板】：同宽同高同间距，
 *    这样从首页点开的设置和进灵签点开的设置长得一模一样 ——
 *    同一台机器上，同一个功能换了个入口不该换个长相。
 *  ★ 「签文朗读」不在这 —— 那是灵签特有的，留在灵签自己的面板里。
 * ============================================================ */
#define SET_CX      32              /* 内容左缘 */
#define SET_CW      256             /* 内容宽 */
#define SET_NAME_W  72              /* 行首名称宽 */
#define SET_BTN_W   48              /* 加减键宽 */
#define SET_GAP     13
#define SET_ROW_H   44
/* ★★ 10-08：SET_ROW_Y0 从 78 改成 8 —— 行坐标现在相对【滚动内容层 pad】，
 *   不再相对卡片。面板项从 3 个涨到 5 个（音量/亮度/声音/配网/设备信息），
 *   一屏放不下了，所以内容层可滚；标题与右上角 ✕ 留在卡片上不跟着滚。*/
#define SET_ROW_Y0   8
#define SET_ROW_DY   56
#define SET_PAD_W   (SCR_W - 20 - 12)   /* 卡片内缘再留 6px 边 */
#define SET_PAD_H   520                 /* 滚动内容总高（只需覆盖到最后一行为止）*/
#define SET_MX      (SET_CX + SET_NAME_W + SET_GAP)
#define SET_VX      (SET_MX + SET_BTN_W + SET_GAP)
#define SET_PX      (SET_VX + SET_BTN_W + SET_GAP)

static lv_obj_t *s_set_panel;
static lv_obj_t *s_cfg_vol_lbl, *s_cfg_bl_lbl, *s_cfg_snd_lbl;
static lv_obj_t *s_cfg_snd_btn;
static lv_obj_t *s_cfg_prov_btn, *s_cfg_prov_lbl;   /* 配网行（10-08 新增）*/
static lv_obj_t *s_cfg_info;                       /* 设备信息正文（10-08 新增）*/
static lv_obj_t *s_cfg_sc;                         /* 面板的可滚动内容层（调试滚动用）*/
static lv_timer_t *s_cfg_tick;                     /* 面板打开期间的 1Hz 刷新 */

static void scls(lv_obj_t *par, int y, const char *name,
                 lv_obj_t **val_lbl, lv_event_cb_t cb)
{
    sclabel(par, y + 12, SET_NAME_W, SET_CX, F_MID, C_INK, name);

    lv_obj_t *minus = sframed(par, SET_MX, y, SET_BTN_W, SET_ROW_H, C_PAPER, 10, 1, C_LINE);
    lv_obj_add_event_cb(minus, cb, LV_EVENT_CLICKED, (void *)(intptr_t)-10);
    sclabel(minus, 12, SET_BTN_W, 0, F_MID, C_INK, "－");

    *val_lbl = sclabel(par, y + 12, SET_BTN_W, SET_VX, F_MID, C_GOLD, "0");

    lv_obj_t *plus = sframed(par, SET_PX, y, SET_BTN_W, SET_ROW_H, C_PAPER, 10, 1, C_LINE);
    lv_obj_add_event_cb(plus, cb, LV_EVENT_CLICKED, (void *)(intptr_t)10);
    sclabel(plus, 12, SET_BTN_W, 0, F_MID, C_INK, "＋");
}

static void cfg_refresh(void)
{
    char b[24];
    if (s_cfg_vol_lbl) {
        snprintf(b, sizeof(b), "%d", xs_cfg_volume());
        lv_label_set_text(s_cfg_vol_lbl, b);
    }
    if (s_cfg_bl_lbl) {
        snprintf(b, sizeof(b), "%d", xs_cfg_backlight());
        lv_label_set_text(s_cfg_bl_lbl, b);
    }
    if (s_cfg_snd_lbl) {
        lv_label_set_text(s_cfg_snd_lbl, xs_cfg_muted() ? "已静音" : "有声");
        lv_obj_set_style_text_color(s_cfg_snd_lbl, xs_cfg_muted() ? C_MUTE : C_GOLD, 0);
    }
}

static void cfg_vol_cb(lv_event_t *e)
{
    xs_cfg_set_volume(xs_cfg_volume() + (int)(intptr_t)lv_event_get_user_data(e));
    cfg_refresh();
}

static void cfg_bl_cb(lv_event_t *e)
{
    xs_cfg_set_backlight(xs_cfg_backlight() + (int)(intptr_t)lv_event_get_user_data(e));
    cfg_refresh();
}

static void cfg_snd_cb(lv_event_t *e)
{
    (void)e;
    xs_cfg_toggle_mute();
    cfg_refresh();
}

/* ---------- 配网（10-08 兰兰：「还有配网…要做到设置里」）----------
 *  ★ 这里只做【开关 + 状态显示】，界面之外的活全在 app_prov.c：
 *    起 SoftAP + HTTP + DNS 劫持，手机连上热点就自动弹配网页。
 *    为什么不在 LVGL 回调里直接 start：那里面会 esp_wifi_set_mode() 与
 *    起三个任务，耗时上百毫秒 —— 在 LVGL 任务里干这个会掉帧。
 *    （实测这一步可接受：WiFi 早已初始化好，起 AP 不是冷启动，
 *      不会踩到「lwip 还没起来就碰网络」那个断言，见铁律 53。）*/
static void cfg_prov_refresh(void)
{
    if (!s_cfg_prov_lbl) return;

    char b[40];
    if (!app_prov_is_active()) {
        lv_label_set_text(s_cfg_prov_lbl, "关 闭");
        lv_obj_set_style_text_color(s_cfg_prov_lbl, C_INK2, 0);
        return;
    }
    /* 手机提交账密后的结果：0 未提交 / 1 连接中 / 2 成功 / 3 密码错 / 4 参数错 */
    const char *tail = "等 待";
    switch (app_prov_result()) {
    case 1:  tail = "连接中"; break;
    case 2:  tail = "已连上"; break;
    case 3:  tail = "密码错"; break;
    case 4:  tail = "参数错"; break;
    default: break;
    }
    /* ★★ 热点名必须【完整】显示出来 —— 手机要连的就是它，少一个字符
     *   就等于让人猜。
     *   踩过的坑（10-08 真机截图发现）：原来写的是 %.12s，而热点名是
     *   "XianDial-XXXX"（13 字符）⇒ 屏上显示成 "XianDial-EFC"，
     *   **把唯一用于区分的那一位（MAC 尾号最后一位）截掉了**。
     *   两个不同设备的热点名会显示成同一个，用户照着输必然连不上。
     *   ⇒ 这类"看起来只是短了点"的截断，实际是功能性错误。
     *
     *   现在 %.20s：热点名最长 32 字符也能显示前 20 个；
     *   尾部状态串给 %.12s（最长的「参数错」= 9 字节）。
     *   两个 %s 都写死精度，既挡了 -Werror=format-truncation，
     *   也保证 b[40] 一定够（20 + 1 + 12 = 33 < 40）。
     *   行宽核对：F_SMALL(14px) 下最长一行「XianDial-XXXX 已连上」实测
     *   137 px，标签宽 156 px ⇒ 放得下，不会被裁。*/
    snprintf(b, sizeof(b), "%.20s %.12s", app_prov_ssid(), tail);
    lv_label_set_text(s_cfg_prov_lbl, b);
    lv_obj_set_style_text_color(s_cfg_prov_lbl, C_GOLD, 0);
}

static void cfg_prov_cb(lv_event_t *e)
{
    (void)e;
    if (app_prov_is_active()) {
        app_prov_stop();
        ESP_LOGI(TAG, "配网：热点已关");
    } else {
        app_prov_clear_result();
        esp_err_t r = app_prov_start();
        if (r == ESP_OK) ESP_LOGI(TAG, "配网：热点已开 %s", app_prov_ssid());
        else             ESP_LOGW(TAG, "配网：开不了热点（%s）", esp_err_to_name(r));
    }
    cfg_prov_refresh();
}

/* ---------- 设备信息（硬件状态）---------- */
static void cfg_info_refresh(void)
{
    if (!s_cfg_info) return;

    /* ★ app_sys_get 是【为每秒刷新设计的】（内部用带缓存的 SD 计数，
     *   不会去递归 stat 几千条目录项）—— 所以这里每秒调一次是安全的。*/
    app_status_t st;
    memset(&st, 0, sizeof(st));
    app_sys_get(&st);

    uint32_t u = st.uptime_s;
    const char *sd = st.sd_mounted ? "已挂载" : "未挂载";
    const char *au = st.audio_ready ? "就绪" : "未就绪";
    const char *ss = (st.net_ssid && st.net_ssid[0]) ? st.net_ssid : "未配置";
    const char *ip = (st.net_ip && st.net_ip[0]) ? st.net_ip : "-";
    const char *ns;
    switch (app_net_link_state()) {
    case APP_NET_OK:         ns = "已连接"; break;
    case APP_NET_CONNECTING: ns = "连接中"; break;
    case APP_NET_FAIL:       ns = "连接失败"; break;
    case APP_NET_NO_CRED:    ns = "未配置"; break;
    default:                 ns = "未启用"; break;
    }

    /* ★★★ 10-08 血泪：这里原本是【局部 char b[560] + snprintf + lv_label_set_text】。
     *   为什么必须改掉（真机现象，不是推测）：
     *     这条路径由串口命令 ui_demo cfg 驱动，而命令跑在 xs_stcmd 任务上
     *     （栈 5120）。560 字节局部缓冲 + 一次 newlib snprintf（它自己还要
     *     一层层 vfprintf 帧）叠加，把那个栈直接顶爆：
     *       ***ERROR*** A stack overflow in task xs_stcmd has been detected.
     *       Backtrace: ... Rebooting...
     *     ⇒ 症状是「发了 ui_demo cfg，板子重启回到主菜单」，截图上看着像
     *       "命令没生效"，极容易误判成解析器或命令名写错。
     *
     *   改用 lv_label_set_text_fmt()：格式化结果由 LVGL 在【自己那块池】
     *   （LV_MEM_SIZE = 96 KB）里分配，栈上只剩一层变参帧。
     *
     *   ★ 为什么不改成文件顶部的 static 大缓冲（那样更省栈）：
     *     本函数有【两个调用点】——① LVGL 的 1Hz tick 定时器（跑在 LVGL 任务）
     *     ② 串口 ui_demo cfg（跑在 xs_stcmd 任务）。两者可以并发，
     *     共用一个 static 缓冲就是数据竞争，屏上会出现随机串字。
     *     走 LVGL 池则每次调用各自持有自己的内存，天然没有这个问题。
     *
     *   ★ 每个 %s 都带精度，不是随手写的：
     *     gcc 的 -Werror=format-truncation 会按"最坏情况"估 %s 的输出长度，
     *     不带精度就报「可能被截断」而直接拒绝编译。
     *     （换成 lv_label_set_text_fmt 后这条约束不变 —— 它内部同样是
     *       先算长度再分配，gcc 一样会检查这个格式串。）*/
    lv_label_set_text_fmt(s_cfg_info,
             "固件  %.10s · %.8s\n"
             "运行  %02u:%02u:%02u\n"
             "内存  内部 %uK / 最大 %uK\n"
             "      PSRAM %uK\n"
             "SD卡  %.6s · %uM · %d 首\n"
             "音频  %.6s · 音量 %d\n"
             "网络  %.10s %.8s\n"
             "信号  %d dBm\n"
             "IP    %.15s\n"
             "芯片  %d MHz",
             XS_FW_VERSION, XS_FW_DATE,
             (unsigned)((u / 3600) % 100), (unsigned)((u / 60) % 60), (unsigned)(u % 60),
             (unsigned)st.heap_internal_kb,
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)st.heap_psram_kb,
             sd, (unsigned)st.sd_cap_mb, st.sd_audio_count,
             au, st.volume,
             ss, ns,
             st.rssi,
             ip,
             st.cpu_mhz);
}

/* 面板打开期间每秒刷一次。★ 为什么需要：
 *   配网的结果是【手机那边异步发生的】（提交密码 → 连上 ⇒ 屏上从不显示
 *   变成"已连上"），设备信息的运行时间/内存也一直在动 —— 不刷就是一屏死数。*/
static void cfg_tick_cb(lv_timer_t *t)
{
    (void)t;
    cfg_prov_refresh();
    cfg_info_refresh();
}

static void cfg_close_cb(lv_event_t *e)
{
    (void)e;
    /* ★★★ 顺序：先删 timer 再删面板对象。
     *   timer 回调每秒都在访问面板里的 label —— 先删对象的话，
     *   在下一次 tick 到来前若 LVGL 还没处理完删除，就是悬空指针。
     *   （本工程统一规矩：leave/tick 一律"先停 timer 再删对象"。）*/
    if (s_cfg_tick) { lv_timer_del(s_cfg_tick); s_cfg_tick = NULL; }

    /* ★ 异步删：本回调来自面板【内部】的按钮，同步删 = 事件处理中途
     *   拆掉事件源对象的祖先（LVGL 事件返回后仍会访问它）。*/
    if (s_set_panel) { lv_obj_del_async(s_set_panel); s_set_panel = NULL; }
    s_cfg_vol_lbl = s_cfg_bl_lbl = s_cfg_snd_lbl = NULL;
    s_cfg_snd_btn = NULL;
    s_cfg_prov_btn = s_cfg_prov_lbl = NULL;
    s_cfg_info = NULL;
    s_cfg_sc = NULL;
}

/* ============================================================
 *  ★★ 10-08 晚：主菜单左上角「关 机」
 * ============================================================
 *  兰兰原话：「再做个关闭按键，能关闭整机」。
 *
 *  位置为什么是【左上角】：右上角已经是「设 置」；下方被署名彩带
 *  （y=428）占满；卡片区从 y=96 起。只剩左上角是空的，
 *  且与「设 置」左右对称（都在 y=28、72×36），视觉上成对。
 *
 *  ★★ 为什么【必须两步确认】（不是把简单事做复杂）：
 *     这个键的下沿是 y=64，而第一张卡片从 y=96 开始，中间只隔 32px。
 *     手指点卡片时偏上一点就会打到它 —— 而 deep sleep 是【不可逆】的：
 *     只能按 BOOT 键唤醒，且唤醒时要【按住等屏亮再松】才不落进下载模式
 *     （根因见 app_pwr.h：BOOT 键本来就是下载模式引脚）。
 *     ⇒ 第一下只「上膛」（变朱砂色 + 改字为"再点一次"），
 *       4 秒内再点一下才真关；不点则自动回弹，不留"上膛"状态。
 *
 *  ⚠️ 诚实说明：这是 deep sleep，**不是断电**。
 *     原理图里 Q3 的栅极被 R13 100K 下拉到 GND、根本没接 MCU
 *     ⇒ 电池与 +5V 轨之间没有可控开关，软件永远切不断电池
 *     （RTC 域仍耗 ≈50~150 µA）。要真断电只能电池线上串物理开关。
 * ============================================================ */
static lv_timer_t *s_pwr_timer;
static lv_obj_t   *s_pwr_lbl;
static bool        s_pwr_armed;

static void pwr_reset_ui(void)
{
    s_pwr_armed = false;
    if (s_pwr_lbl) {
        lv_label_set_text(s_pwr_lbl, "关 机");
        lv_obj_set_style_text_color(s_pwr_lbl, C_GOLD, 0);
    }
}

/* 4 秒没跟进 ⇒ 自动回弹（用户可能是误触）。
 * ★★ 这个回调是【定时器自己的回调】，而该 timer 建的时候设了
 *    repeat_count = 1 ⇒ LVGL 跑完这一次会【自动删掉它】。
 *    所以这里【绝对不能】再写 lv_timer_del(t)：那是重复删除，
 *    轻则断言、重则释放后又被 LVGL 的 timer 链表遍历到 → 随机崩。
 *    只把句柄置空（真删由 LVGL 做）+ 复位 UI。*/
static void pwr_revert_cb(lv_timer_t *t)
{
    (void)t;
    s_pwr_timer = NULL;
    pwr_reset_ui();
    ESP_LOGI(TAG, "关机已自动回弹（4 秒内没再点）");
}

static void power_off_cb(lv_event_t *e)
{
    (void)e;
    if (!s_pwr_armed) {
        s_pwr_armed = true;
        if (s_pwr_lbl) {
            lv_label_set_text(s_pwr_lbl, "再点一次");
            lv_obj_set_style_text_color(s_pwr_lbl, C_SEAL, 0);
        }
        if (s_pwr_timer) { lv_timer_del(s_pwr_timer); s_pwr_timer = NULL; }
        s_pwr_timer = lv_timer_create(pwr_revert_cb, 4000, NULL);
        lv_timer_set_repeat_count(s_pwr_timer, 1);
        ESP_LOGW(TAG, "关机已上膛：4 秒内再点一次才真关");
        return;
    }
    ESP_LOGW(TAG, "关机：app_pwr_shutdown()（关播→关功放→关背光→关WiFi→deep sleep）");
    /* 不需要先 app_exit()：这个键只存在于主菜单，
     * 而「回到主菜单」这条路本身已经销毁了产品（见 app_exit 的契约）；
     * 且 app_pwr_shutdown() 内部第一步就是 app_radio_stop()。*/
    pwr_reset_ui();
    if (s_pwr_timer) { lv_timer_del(s_pwr_timer); s_pwr_timer = NULL; }
    app_pwr_shutdown();      /* ★ 不返回 */
}

static void cfg_open_cb(lv_event_t *e)
{
    (void)e;
    if (s_set_panel) return;                 /* 已开着，别建第二个 */

    lv_obj_t *p = lv_obj_create(s_home);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, SCR_W, SCR_H);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x2A2118), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_60, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    s_set_panel = p;

    /* ★★ 卡片高 424 → 444（10-08 改）。为什么：主菜单自己的落款画在
     *   y≈434~466，而卡片原来下沿只到 452 —— 于是那行字会从卡片底下
     *   漏出来，透过半透明的遮罩显示 ⇒ 截图上看着像"署名重复了一行"。
     *   把卡片下沿压到 472（屏高 480，留 8px 边）就把它整条盖住了。
     *   ⚠ 这类问题只看得见截图里，读代码永远发现不了。*/
    lv_obj_t *card = sframed(p, 10, 28, SCR_W - 20, 444, C_PAPER2, 10, 2, C_GOLD);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    sclabel(card, 16, SCR_W - 20, 0, F_MID, C_INK, "设    置");

    /* ✕ 关闭：右上角 */
    lv_obj_t *x = sframed(card, SCR_W - 20 - 12 - 44, 10, 44, 44, C_PAPER, 22, 1, C_LINE);
    lv_obj_add_event_cb(x, cfg_close_cb, LV_EVENT_CLICKED, NULL);
    sclabel(x, 12, 44, 0, F_MID, C_INK2, "×");

    sbox(card, 20, 64, SCR_W - 60, 1, C_LINE, 0);

    /* ---- 可滚动内容层 ----
     * ★ 设置项会一直加（配网/设备信息/以后的台源导入…），
     *   与其每次挪坐标、压缩行高，不如一次做对：内容层可滚，
     *   标题与 ✕ 留在卡片上（不跟着滚），行距/字号全不用动。*/
    lv_obj_t *sc = lv_obj_create(card);
    lv_obj_remove_style_all(sc);
    lv_obj_set_pos(sc, 6, 68);
    lv_obj_set_size(sc, SET_PAD_W, 368);
    lv_obj_set_style_bg_opa(sc, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(sc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(sc, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(sc, LV_SCROLLBAR_MODE_AUTO);
    s_cfg_sc = sc;                                  /* 记下来：串口调试要滚它 */

    lv_obj_t *pad = lv_obj_create(sc);
    lv_obj_remove_style_all(pad);
    lv_obj_set_pos(pad, 0, 0);
    lv_obj_set_size(pad, SET_PAD_W, SET_PAD_H);
    lv_obj_set_style_bg_opa(pad, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(pad, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 三个整机级设置（音量/亮度/声音）---- */
    scls(pad, SET_ROW_Y0,                "音　量", &s_cfg_vol_lbl, cfg_vol_cb);
    scls(pad, SET_ROW_Y0 + SET_ROW_DY,   "亮　度", &s_cfg_bl_lbl,  cfg_bl_cb);

    s_cfg_snd_btn = sframed(pad, SET_CX, SET_ROW_Y0 + 2 * SET_ROW_DY,
                            SET_CW, SET_ROW_H, C_PAPER, 10, 1, C_LINE);
    lv_obj_add_event_cb(s_cfg_snd_btn, cfg_snd_cb, LV_EVENT_CLICKED, NULL);
    sclabel(s_cfg_snd_btn, 12, 88, 0, F_MID, C_INK, "声　音");
    s_cfg_snd_lbl = sclabel(s_cfg_snd_btn, 12, SET_CW - 100, 88, F_MID, C_INK, "有声");

    /* ---- 配网 ---- */
    s_cfg_prov_btn = sframed(pad, SET_CX, SET_ROW_Y0 + 3 * SET_ROW_DY,
                             SET_CW, SET_ROW_H, C_PAPER, 10, 1, C_LINE);
    lv_obj_add_event_cb(s_cfg_prov_btn, cfg_prov_cb, LV_EVENT_CLICKED, NULL);
    sclabel(s_cfg_prov_btn, 12, 88, 0, F_MID, C_INK, "配　网");
    s_cfg_prov_lbl = sclabel(s_cfg_prov_btn, 12, SET_CW - 100, 88, F_SMALL, C_GOLD, "关 闭");

    /* ---- 设备信息 ---- */
    int iy = SET_ROW_Y0 + 4 * SET_ROW_DY;             /* 232 */
    sbox(pad, SET_CX, iy, SET_CW, 1, C_LINE, 0);
    sclabel(pad, iy + 12, SET_NAME_W + 40, SET_CX, F_MID, C_INK, "设备信息");

    s_cfg_info = slabel(pad, SET_CX, iy + 44, F_SMALL, C_INK2, "读取中…");
    lv_obj_set_width(s_cfg_info, SET_CW);
    lv_obj_set_style_text_line_space(s_cfg_info, 8, 0);

    cfg_refresh();
    cfg_prov_refresh();
    cfg_info_refresh();

    /* ★★ 落款与滚动内容总高，都【量出来】而不是手算。
     *   手算的坑：F_SMALL 每行多高取决于字体的 line_height（不是 14），
     *   再叠加 text_line_space，10 行差几十像素很正常 ——
     *   算小了最后一行看不见，算大了末尾空一大截还得多滚一屏。
     *   设为固定行数（10 行）之后高度是稳定的，所以量一次就够。*/
    lv_obj_update_layout(pad);
    int iy2 = lv_obj_get_y(s_cfg_info) + lv_obj_get_height(s_cfg_info) + 18;
    /* 面板内落款与首页同一套配色（永远的 淡墨 / 兰兰 朱砂），只是字号保持小档 ——
     * 面板里是"页脚"，抢眼会盖过设置项本身。*/
    lv_obj_t *fa = slabel(pad, 0, iy2, F_SMALL, C_MUTE, "© 永远的");
    lv_obj_t *fb = slabel(pad, 0, iy2, F_SMALL, C_SEAL, "兰兰");
    lv_obj_update_layout(fa);
    lv_obj_update_layout(fb);
    int fwa = lv_obj_get_width(fa);
    int fx = SET_CX + (SET_CW - fwa - lv_obj_get_width(fb)) / 2;
    lv_obj_set_x(fa, fx);
    lv_obj_set_x(fb, fx + fwa);
    lv_obj_set_height(pad, iy2 + 34);

    if (s_cfg_tick) lv_timer_del(s_cfg_tick);
    s_cfg_tick = lv_timer_create(cfg_tick_cb, 1000, NULL);
}

/* ============================================================
 *  落款（10-08 兰兰：「首页作者署名更明显一点，字体更大和颜色鲜艳，
 *                    充分利用彩屏」）
 * ============================================================
 *  不是"把字号调大 + 刷成大红"就完事，做两条：
 *
 *  ★★ ① 双色落款：「© 永远的」用次墨，「兰兰」用朱砂。
 *     这是传统书画落款的做法（正文墨色、名号朱色）——
 *     既"鲜艳"又不破坏整机的宣纸调；整行刷成大红色会像广告条，
 *     和灵签/天气/股票三页的气质都不搭。
 *
 *  ★★ ② 上方一条四段彩线（金 / 朱 / 蓝 / 青）。
 *     兰兰要"充分利用彩屏"：这条线是整屏唯一明确说出"这是彩屏"的元素，
 *     而且这四色【与三个产品卡的标识色同源】—— 视觉上把产品与落款
 *     串成"同一台机器"，而不是三张卡加一行字。
 *
 *  ★ 两个 label 的水平居中靠 lv_obj_update_layout() 量出【真实宽度】再摆。
 *    不能按"汉字宽 = 字号"手算：'©' 与空格的实际宽度和汉字不一样，
 *    手算出来的"居中"会明显偏右。*/
/* 落款：四段彩线 + 三色大字署名。
 * ------------------------------------------------------------
 * ★ 兰兰 10-08：「首页作者署名更明显一点，字体更大和颜色鲜艳，
 *   充分利用彩屏」—— 三件事分别对应下面三处改动：
 *     ① 更大：F_READ(22px) → F_HERO(30px)。
 *        【先量过再换】：30px 行高 33，落款 y=436 → 下沿 469，屏高 480 放得下
 *        （卡区到 424 为止，中间 12px 是彩线）。宽度也是量过的：
 *        30px 下正文 181px，屏宽 320 ⇒ 左右各余 ~70px，不会贴边。
 *     ② 更鲜艳：拆成三个 label —— © 金 / 永远的 墨 / 兰兰 朱砂。
 *        原来的「© 永远的」全是次墨色，彩屏上只剩一个字是红的。
 *     ③ 彩线加粗加宽（3px×200 → 4px×223），四段色块更像一条"彩带"。
 * ★★ 居中【必须量真实宽度】，不能手算：
 *    "©" 的步进既不是汉字宽也不随字号线性缩放（22px 是 17，30px 是 23），
 *    手算的常量每换一次字号就偏一次 —— 量一次是唯一稳的做法。*/
static void sign_build(lv_obj_t *par)
{
    static const uint32_t RULE[4] = { 0xB8914F, 0xC0653A, 0x3E6E9E, 0x2E7D6E };
    const int seg = 58, gap = 3, rh = 4, ry = 428;
    /* ★ 总占宽 = (4-1)*seg + (seg-gap) = 4*seg - gap —— 只减【一个】gap。
     *   上一版写成 4*seg - 3*gap（多减了两个），于是 rx 偏右 3px、
     *   整条彩带左右差 5px。5px 肉眼几乎看不出，但既然像素级量出来了就修掉。*/
    const int rw = 4 * seg - gap;                     /* 229 */
    const int rx = (SCR_W - rw) / 2;                  /* 45 */

    for (int i = 0; i < 4; i++) {
        lv_obj_t *s = sbox(par, rx + i * seg, ry, seg - gap, rh,
                           lv_color_hex(RULE[i]), 2);
        lv_obj_clear_flag(s, LV_OBJ_FLAG_CLICKABLE);      /* 装饰件不该吃点击 */
    }

    const int ty = 436;
    lv_obj_t *a = slabel(par, 0, ty, F_HERO, C_GOLD, "©");
    lv_obj_t *b = slabel(par, 0, ty, F_HERO, C_INK,  "永远的");
    lv_obj_t *c = slabel(par, 0, ty, F_HERO, C_SEAL, "兰兰");
    lv_obj_update_layout(a);
    lv_obj_update_layout(b);
    lv_obj_update_layout(c);

    const int SP = 8;                                 /* © 与汉字之间留一口气 */
    int wa = lv_obj_get_width(a);
    int wb = lv_obj_get_width(b);
    int wc = lv_obj_get_width(c);
    int x0 = (SCR_W - wa - wb - wc - SP) / 2;
    lv_obj_set_x(a, x0);
    lv_obj_set_x(b, x0 + wa + SP);
    lv_obj_set_x(c, x0 + wa + SP + wb);
}

static void home_build(void)
{
    /* 顶部题字（10-08 晚：整块上收 6px，给第 4 张卡腾高度）
     * ★★ 10-08 夜：`拾 声` → `拾 声 集`（兰兰定的合集名「拾声集」）。
     *   为什么叫这个 / 检索结论见 `2026-09-15-21-50-21/四合一-命名报告.md`：
     *     · 三条检索线全空（软件侧零同名、第 9 类无商标、无同名字号企业）
     *     · 是「拾声」的自然延伸 ⇒ 层级清楚：拾声集（整机）⊃ 拾声（电台子产品）
     *   ⚠️ 「集」字【已在主字库里】（来源是 EXTRA 的心经"苦集灭道"），
     *      四档 14/18/22/30 都有 —— 已读产物 Opts 行核实过，不用重跑字库。
     *      但以后若再改题字，先跑 firmware/_chk_font_gap.py 核一遍缺字。*/
    sclabel(s_home, 30, SCR_W, 0, F_HERO, C_INK, "拾  声  集");
    sclabel(s_home, 66, SCR_W, 0, F_SMALL, C_MUTE, "一 机 · 四 用");
    sbox(s_home, 60, 88, SCR_W - 120, 1, C_LINE, 0);

    /* 右上角「设 置」—— 全局设置入口（10-08 兰兰要求放在首页）。
     * ★ y=28 与题字同一水平线，题字居中（约 100~220），按钮在 236~308，不打架。*/
    lv_obj_t *sb = sframed(s_home, SCR_W - 12 - 72, 28, 72, 36, C_PAPER2, 8, 1, C_GOLD);
    lv_obj_set_style_bg_opa(sb, LV_OPA_80, 0);
    lv_obj_add_event_cb(sb, cfg_open_cb, LV_EVENT_CLICKED, NULL);
    sclabel(sb, 9, 72, 0, F_SMALL, C_GOLD, "设 置");

    /* ★★ 10-08 晚：左上角「关 机」—— 与右上角「设 置」左右对称。
     * ⚠️ 位置/两步确认的理由见 power_off_cb 上方的长注释，别随手改成"一下即关"。*/
    lv_obj_t *pb = sframed(s_home, 12, 28, 72, 36, C_PAPER2, 8, 1, C_SEAL);
    lv_obj_set_style_bg_opa(pb, LV_OPA_80, 0);
    lv_obj_add_event_cb(pb, power_off_cb, LV_EVENT_CLICKED, NULL);
    s_pwr_lbl = sclabel(pb, 9, 72, 0, F_SMALL, C_GOLD, "关 机");

    /* ★★ 10-08 晚：2×2 宫格（兰兰："也可以做双列，你做的好看点就是"）。
     *   每张卡 = accent 实色圆底图标（30px 汉字）+ 主标题 18px + 副标题 14px；
     *   卡片底色用 accent 的极淡 tint 做【竖向渐变】——
     *   纯色卡片平面感重，渐变让它"浮"起来一点，4 张各带自己的色倾向。
     * ★ 图标圆底必须 clear CLICKABLE：子对象在父对象之上，而 LVGL 事件默认
     *   【不冒泡】⇒ 不清的话点圆上等于点在"没绑事件的色块"上，卡片收不到。*/
    for (int i = 0; i < APP_N; i++) {
        const app_desc_t *a = &APPS[i];
        bool ready = (a->enter != NULL);
        int x = CARD_X0 + (i % CARD_COLS) * CARD_DX;
        int y = CARD_Y0 + (i / CARD_COLS) * CARD_DY;

        /* 卡片本体：整张可点（不是只有文字可点 —— "点上去无效"的老坑）*/
        lv_obj_t *c = lv_obj_create(s_home);
        lv_obj_remove_style_all(c);
        lv_obj_set_pos(c, x, y);
        lv_obj_set_size(c, CARD_W, CARD_H);
        lv_obj_set_style_radius(c, 16, 0);
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_border_color(c, ready ? C_GOLD : C_LINE, 0);
        lv_obj_set_style_border_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(c,
            ready ? lv_color_mix(lv_color_hex(a->accent), C_PAPER2, 42) : C_PAPER, 0);
        lv_obj_set_style_bg_grad_color(c, C_PAPER2, 0);
        lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(c, card_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        /* 圆底图标：直径 56，与卡片左右居中对齐 */
        lv_obj_t *dot = sbox(c, (CARD_W - 56) / 2, 22, 56, 56,
                             ready ? lv_color_hex(a->accent) : lv_color_hex(0xD8D0C2), 28);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        sclabel(dot, 12, 56, 0, F_HERO, C_PAPER2, a->glyph);

        /* 主标题 18px / 副标题 14px，都按卡宽居中 */
        sclabel(c, 88, CARD_W, 0, F_MID, ready ? C_INK : C_MUTE, a->title);
        sclabel(c, 114, CARD_W, 0, F_SMALL, C_MUTE, a->sub);
    }

    sign_build(s_home);
}

/* ============================================================
 *  调度
 * ============================================================ */

/* 退出当前产品（若在某个产品里），回到主菜单。
 * ★★★ 顺序：先 lv_screen_load(主菜单) 再销毁 —— 见文件头说明。
 * ★ 同时供 back() 与 launch() 使用：launch 里"换产品"必须先把
 *   当前产品退干净，否则后一个 init() 会直接覆盖前一个的屏指针。*/
static void app_exit(void)
{
    if (s_cur < 0) return;                 /* 已经在主菜单，忽略 */

    int idx = s_cur;
    s_cur = -1;

    if (s_home) lv_screen_load(s_home);
    if (APPS[idx].leave) APPS[idx].leave();

    ESP_LOGI(TAG, "← 退出「%s」 → 主菜单 (internal free=%u largest=%u)",
             APPS[idx].title,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void ui_shell_launch(int idx)
{
    if (idx < 0 || idx >= APP_N) {
        ESP_LOGW(TAG, "产品下标 %d 越界（只有 %d 个）", idx, APP_N);
        return;
    }

    /* ★★ 已经在某个产品里 → 先完整退掉（包括"重复进入同一个"）。
     *   不加这一步，连发两次 app_open（串口调试天然会这么用）会让
     *   第二次的 init() 执行 s_scr = lv_obj_create(NULL)，
     *   把旧屏指针【直接覆盖】—— 旧屏连同全部子对象再没人持有，
     *   永远回收不了。这是"每调一次漏一屏"的静默泄漏，不报错、不崩，
     *   只在反复进出后表现为"内存越来越少"。
     *   所以这条不是防呆，是必需。*/
    if (s_cur >= 0) app_exit();

    const app_desc_t *a = &APPS[idx];
    if (!a->enter) {
        ESP_LOGW(TAG, "「%s」还没做，弹提示", a->title);
        toast("该产品还 没 做 好");
        return;
    }

    ESP_LOGI(TAG, "→ 进入「%s」 (internal free=%u largest=%u)",
             a->title,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    s_cur = idx;
    a->enter();          /* 内部自建屏并 lv_screen_load，主菜单屏留着不删 */
}

void ui_shell_back(void)
{
    app_exit();
}

void ui_shell_demo_cfg(void)
{
    /* ★ 前置：必须停在主菜单 —— 面板是挂在 s_home 上的。
     *   若此刻在产品里，先退回来（app_exit 会先切屏再销毁，见文件头契约）。*/
    if (s_cur >= 0) app_exit();
    /* ★ 已经开着 ⇒ 只把它拉回顶部，不再往下走。
     *   为什么（10-08 踩的坑）：原来这里直接 return，于是"面板还开着、
     *   而且停在上次滚到的位置"这个状态会跨串口会话保留下来 ——
     *   下一轮发 ui_demo cfg，截出来的却是【下半屏】，
     *   看起来像"打开的面板少了音量/亮度那几行"，白白怀疑了一遍布局。
     *   调试命令必须是【可重复的】：同一个命令 ⇒ 同一个画面。*/
    if (s_set_panel) {
        if (s_cfg_sc) lv_obj_scroll_to_y(s_cfg_sc, 0, LV_ANIM_OFF);
        ESP_LOGI(TAG, "ui_demo cfg: 面板已开着，拉回顶部");
        return;
    }
    cfg_open_cb(NULL);       /* 该回调不用事件参数（内部只 (void)e）*/
}

void ui_shell_demo_prov(void)
{
    /* ★ 10-08：代按一次「配 网」按钮。
     * 为什么需要：配网的入口是手指，而这块板没有物理手指 ——
     * 不开这个口就只能"读代码相信按钮接对了"。
     * 走的是和按钮【同一个回调】（cfg_prov_cb），不是另抄一份动作序列：
     * 抄一份只能证明"抄的那份对"，证明不了按钮那条路通。*/
    if (!s_cfg_prov_btn) {
        ESP_LOGW(TAG, "ui_demo prov: 设置面板没开着，先发 ui_demo cfg");
        return;
    }
    cfg_prov_cb(NULL);
    ESP_LOGI(TAG, "ui_demo prov: 已代按配网键（现在 %s，状态「%s」）",
             app_prov_is_active() ? "热点开" : "热点关",
             app_prov_is_active() ? app_prov_ssid() : "-");
}

/* ★★ 10-08 新增：把【当前活动屏】的直接子对象全列出来。
 * ------------------------------------------------------------
 * 为什么需要（这次是被它救的）：
 *   现象是「串口发 ui_demo cfg 后，截图上还是主菜单」——
 *   而日志明明说面板已经建好了（ui_demo cfg: 面板已开着）。
 *   "对象存在"与"画在屏上"是两件事，中间隔着：
 *     · 父对象是不是【当前活动屏】？（挂在非活动屏上 ⇒ 永远不画）
 *     · 对象是不是被 HIDDEN？（上一版股票蜡烛就是栽在这）
 *     · 坐标/尺寸是不是 0 或跑到屏外？
 *   这三条读代码都看不出来，截图也只会给你一个"什么都没有"。
 *   ⇒ 必须能把对象树本身打出来。
 * 读法：active 与 s_home 必须是同一个指针；面板那条 size 必须是 320x480。*/
void ui_shell_demo_dump(void)
{
    lv_obj_t *scr = lv_screen_active();
    ESP_LOGI(TAG, "ui_dump: active=%p  s_home=%p  子对象=%d",
             (void *)scr, (void *)s_home, (int)lv_obj_get_child_count(scr));

    uint32_t n = lv_obj_get_child_count(scr);
    for (uint32_t i = 0; i < n && i < 20; i++) {
        lv_obj_t *c = lv_obj_get_child(scr, i);
        ESP_LOGI(TAG, "  [%u] pos=%d,%d size=%dx%d%s",
                 (unsigned)i, lv_obj_get_x(c), lv_obj_get_y(c),
                 lv_obj_get_width(c), lv_obj_get_height(c),
                 lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN) ? "  HIDDEN" : "");
    }
    if (s_set_panel) {
        ESP_LOGI(TAG, "  设置面板 p=%p 在其父下第 %d 个（父=%p）",
                 (void *)s_set_panel,
                 (int)lv_obj_get_index(s_set_panel),
                 (void *)lv_obj_get_parent(s_set_panel));
    } else {
        ESP_LOGI(TAG, "  设置面板：没建");
    }
}

/* ★★ 10-08 新增：代按设置面板右上角的 ✕（关闭）。
 * 为什么必须有（不是为了对称好看）：
 *   cfg_close_cb 里有个本工程的硬规矩要守 —— **先 lv_timer_del 再删对象**
 *   （面板打开期间有个 1 Hz 的 cfg_tick_cb 每秒访问面板里的 label，
 *     先删对象就是悬空指针，崩起来是随机的、最难查的那类）。
 *   而"点 ✕ 关闭"这条路【一次都没在真机上走过】：
 *   面板能用命令开（ui_demo cfg），但没有命令能关 ——
 *   于是关闭路径 + 那条 timer 删除顺序 = 完全没验证过。
 *   ⇒ 走的是和 ✕ 按钮【同一个回调】，不是另抄一份动作序列。
 *   ★ 验法：ui_demo cfg → ui_demo cfgclose → ui_demo dump（面板应消失）
 *     → lq_mem（LVGL 池 used% 应落回开面板之前的水平，这才叫没泄漏）。*/
void ui_shell_demo_cfg_close(void)
{
    if (!s_set_panel) {
        ESP_LOGW(TAG, "ui_demo cfgclose: 面板本来就没开");
        return;
    }
    cfg_close_cb(NULL);
    ESP_LOGI(TAG, "ui_demo cfgclose: 已代按 ✕（timer 先删，面板走异步删）");
}

/* ★★ 10-08 晚：代按主菜单左上角的「关 机」—— 【只上膛，不真关】。
 *
 * 为什么只能上膛、不能真按第二下：
 *   app_pwr_shutdown() 进 deep sleep 后，唯一唤醒源是板载 BOOT 键，
 *   而这块板现在插在阳台机上、没有手指去按 ——
 *   我一按第二下，机器就"睡死"到兰兰回来看它为止（远程救不回来）。
 *   ⇒ 这条命令只走【第一下】，验证两件事：
 *       ① 按钮的标签确实变成"再点一次"、颜色变朱砂（截图能看见）
 *       ② 4 秒后自动回弹（再截一张，标签应回到"关 机"）
 *     这两件正是两步确认逻辑的全部；第二下的动作就是 app_pwr_shutdown()，
 *     而那个函数本身早在 10-04 就真机验证过（低电自动关机走的是同一个）。
 *   ★ 走的仍然是 power_off_cb 本身，不是另抄一份。*/
void ui_shell_demo_power(void)
{
    if (!s_home) { ESP_LOGW(TAG, "ui_demo power: 主菜单还没建"); return; }
    power_off_cb(NULL);
    ESP_LOGI(TAG, "ui_demo power: 已代按第一下（上膛）；4 秒后应自动回弹");
}

void ui_shell_demo_cfg_scroll(int y)
{
    /* ★★ 为什么必须开这个口（10-08）：
     *   「设备信息」十行在面板的【下半屏】，而面板是可滚动的 ——
     *   这块板没有物理手指，串口也注入不了触摸。
     *   没有它，那十行到底画成什么样（豆腐字？行距挤成坨？最后一行被裁？）
     *   就【只能靠读代码相信它是对的】，而排版问题恰恰是读代码看不出来的。
     *   与 ui_shell_demo_cfg / lq_demo 同一个套路：给"只能靠手点"的界面开命令口。*/
    if (!s_cfg_sc) {
        ESP_LOGW(TAG, "ui_demo cfg: 设置面板没开着，先发 ui_demo cfg");
        return;
    }
    lv_obj_scroll_to_y(s_cfg_sc, 0, LV_ANIM_OFF);
    int maxy = lv_obj_get_scroll_bottom(s_cfg_sc);   /* 到 0 时的底部余量 = 可滚上限 */
    lv_obj_scroll_to_y(s_cfg_sc, y, LV_ANIM_OFF);
    ESP_LOGI(TAG, "ui_demo cfg: 内容层滚到 y=%d（可滚范围 0~%d，视口 %d）",
             y, maxy, lv_obj_get_height(s_cfg_sc));
}

void ui_shell_init(void)
{
    ESP_LOGI(TAG, "外壳 v1.0：%d 个产品（%s）", APP_N, XS_FW_VERSION);

    s_home = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_home);
    lv_obj_set_style_bg_color(s_home, C_PAPER, 0);
    lv_obj_set_style_bg_opa(s_home, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_home, LV_OBJ_FLAG_SCROLLABLE);

    /* ★★★ 漏了这一行 ⇒ 整屏纯白，日志里看不出任何异常。
     *   lv_obj_create(NULL) 只是"造了个屏幕对象"，它默认不在屏上。*/
    lv_screen_load(s_home);

    home_build();
    s_cur = -1;
}
