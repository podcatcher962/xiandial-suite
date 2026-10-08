/*
 * 拾声 · 竖版电台 App（多产品外壳下的第 4 个产品）
 * ============================================================
 *  契约见 ui_radio_v.h / ui_shell.h。
 *
 *  ★★ 三个"看起来像 bug、其实不是"的点，先写在最前面：
 *
 *  ① 没声音 ≠ 没解码。
 *     全局设置里那个「声音」开关（app_settings 的 xs_cfg.mute）做的是
 *     DAC 增益归零 —— 解码任务照跑、频谱照动、就是不出声。
 *     灵签默认就把它关着（兰兰：「观音灵签默认声音关闭」），
 *     所以从主菜单进电台会【继承这个静音】，表现为"点了台、柱子会跳、没声"。
 *     ⇒ 本 App 用 xs_cfg_audio_boost() 接管出声：进来了就是要听；
 *       离开时 audio_restore() 把静音语义原样还回去。
 *       ★ 借用的是 app_settings.h 里【已经存在】的旁路机制，不是新加概念 ——
 *         它的注释原文就是"默认不出声，你点了我才说"。
 *       ★ 不改 mute 标志 ⇒ 回灵签后六字大明咒照旧不出声，语义干净。
 *
 *  ② 列表行对象是【限量复用】的，不是一台一个对象。
 *     台单上限 2000 台；一台一个 label 会把 96 KB 的 LVGL 池直接抽干
 *     （CONFIG_LV_MEM_SIZE=98304）。只建 ROW_N 个行对象，滚动时按
 *     scroll_y 反算起始行、整体挪位置、换文字 —— 内存恒定，与台数无关。
 *
 *  ③ 筛选后还能"上一台/下一台"。
 *     app_radio_station_step() 是按【全局台序】走的，会把筛选丢开；
 *     所以本 App 自己在筛选池 s_pool 里走位（s_cur_pos），
 *     保证"戏曲"筛出来之后，下一台还是戏曲。
 *
 *  ★ 后端一个字没改：app_radio / app_stlist / app_sd / app_settings 照用。
 * ============================================================ */
#include "ui_radio_v.h"

#include "ui_shell.h"
#include "app_radio.h"
#include "app_stlist.h"
#include "app_sd.h"
#include "app_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "XSRADIO";

/* ---------- 字体 / 配色：与外壳、其余三个产品同一套（"同一台机器"的观感）---------- */
extern const lv_font_t xs_font_14;
extern const lv_font_t xs_font_18;
extern const lv_font_t xs_font_22;
#define F_READ  (&xs_font_22)
#define F_MID   (&xs_font_18)
#define F_SMALL (&xs_font_14)

/* ★★★ 台名【必须】用台名字库，不能用上面那三档。
 *   原因（拾声 10-03 真机踩过）：主字库只收"界面文案里写死的字"（两千来个），
 *   而台名还带繁体与二级生僻字 —— 「上海戏剧曲艺广播」正常，
 *   「听·越剧」的间隔号、「飛碟聯播網」的繁体、「郫都综合广播」的"郫"
 *   在界面文案里一个都没有，拿主字库画就是豆腐块。
 *
 *   符号名两个形态【故意相同】：自用版编 fonts/xs_font_st16.c（按台单用字烘），
 *   发布版编 fonts/xs_font_pub_st16.c（国标字集并集）。
 *   由 main/CMakeLists.txt 末尾的 if/else 二选一，两文件永不同时进编译。
 *   ⇒ 本文件一个字都不用为形态分叉。*/
extern const lv_font_t xs_font_st16;
#define F_ST (&xs_font_st16)

#define C_PAPER  lv_color_hex(0xF3ECE0)
#define C_PAPER2 lv_color_hex(0xFBF6EA)
#define C_INK    lv_color_hex(0x3B2E1E)
#define C_MUTE   lv_color_hex(0x9A8A74)
#define C_GOLD   lv_color_hex(0xB8914F)
#define C_LINE   lv_color_hex(0xE0D6C4)
#define C_ACC    lv_color_hex(0x2E7D6E)   /* 本产品标识色（与首页宫格同源）*/

#define SCR_W   320
#define SCR_H   480

#define ROW_H    52          /* 行距（两个列表共用）*/
#define ROW_N    9           /* 行对象数：可视 7 + 上下各 1 缓冲 */
#define LIST_TOP 118         /* 电台列表上沿（上面是标题/台数/两个入口按钮）*/
#define SD_TOP   76          /* 本地音频列表上沿 */
#define BAR_N    12          /* 频谱条数 */
#define BAR_BOT  280         /* 频谱条底边 y（往上长）*/
#define BAR_W    14
#define BAR_GAP  6
#define SD_MAX   200         /* 本地音频目录最多收多少条 */

/* 视图编号 */
enum { V_LIST = 0, V_PLAY, V_CAT, V_SD, V_N };

/* ============================================================
 *  行对象复用器（电台列表与本地音频列表共用）
 * ============================================================
 *  ★ 几何、滚动、复用逻辑完全一样，只有"第 i 行显示什么字 / 什么颜色 /
 *    点了做什么"不同 —— 那是 kind 决定的。
 *  ★ 两份各建一套行对象（都在各自视图里，同时只有一个可见）。
 *    共用一套会让"切视图时谁负责刷新"变成一团乱麻；9×2 = 18 个对象，
 *    池子扛得住。*/
typedef enum { KIND_ST = 0, KIND_SD } row_kind_t;

typedef struct {
    row_kind_t kind;
    lv_obj_t  *list;                 /* 滚动容器 */
    lv_obj_t  *rows[ROW_N];
    lv_obj_t  *labs[ROW_N];
    int        idx[ROW_N];           /* 该行当前显示的逻辑序号，-1 = 空 */
    int        first;                /* 当前起始行 */
    lv_obj_t  *spacer;               /* 撑高块：决定滚动范围 */
    int        total;                /* 逻辑总条数 */
} rowlist_t;

/* ---------- 状态 ---------- */
static lv_obj_t *s_scr;
static lv_obj_t *s_view[V_N];
static int       s_cur_view;

/* 电台列表 */
static rowlist_t s_rl_st;
static lv_obj_t *s_lbl_sum;
static int      *s_pool;             /* 筛选后的台下标（PSRAM）*/
static int       s_pool_n;
static int       s_f_cat  = -1;      /* -1 = 不筛 */
static int       s_f_prov = -1;
static int       s_cur_pos = -1;     /* 正在播的台在 s_pool 里的位置 */

/* 播放页 */
static lv_obj_t  *s_play_name;
static lv_obj_t  *s_play_state;
static lv_obj_t  *s_play_btn_lab;
static lv_obj_t  *s_lbl_vol;
static lv_obj_t  *s_bars[BAR_N];
static lv_timer_t *s_tick;
static int         s_tick_n;
static bool        s_playing_local;  /* 现在放的是本地文件（决定上下首怎么走）*/

/* 分类页 */
static lv_obj_t *s_cat_body;
static lv_obj_t *s_cat_tab[2];
static int       s_cat_tab_cur;

/* 本地音频页 */
static app_sd_entry_t *s_sd_ents;    /* PSRAM */
static int             s_sd_n;
static char            s_sd_dir[256];
static rowlist_t       s_rl_sd;
static lv_obj_t       *s_sd_path;

static bool s_alive;

/* ---------- 前向声明 ---------- */
static void sd_scan(const char *dir);
static void sd_fill(rowlist_t *rl, int j, int i);
static void st_fill(rowlist_t *rl, int j, int idx);
static void rl_refresh(rowlist_t *rl);
static void refresh_sum(void);
static void rebuild_cat_body(void);
static void show_view(int v);
static void play_refresh(void);

/* ============================================================
 *  小工具
 * ============================================================ */
static lv_obj_t *mk_box(lv_obj_t *par, int x, int y, int w, int h,
                        lv_color_t c, int radius)
{
    lv_obj_t *o = lv_obj_create(par);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *mk_lab(lv_obj_t *par, int x, int y, const lv_font_t *f,
                        lv_color_t c, const char *t)
{
    lv_obj_t *l = lv_label_create(par);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_text(l, t);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *mk_clab(lv_obj_t *par, int y, int w, int x0,
                         const lv_font_t *f, lv_color_t c, const char *t)
{
    lv_obj_t *l = mk_lab(par, x0, y, f, c, t);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

/* 按钮 = 圆角块 + 居中文字（照 ui_shell.c 的做法）*/
static lv_obj_t *mk_btn(lv_obj_t *par, int x, int y, int w, int h,
                        const char *txt, lv_color_t bgc, lv_color_t fgc,
                        lv_event_cb_t cb)
{
    lv_obj_t *b = mk_box(par, x, y, w, h, bgc, 8);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    mk_clab(b, (h - 18) / 2, w, 0, F_SMALL, fgc, txt);
    return b;
}

/* 分类用的 chip：一个 label 就够（label 支持 bg/radius/pad）——
 * ★ 为什么不是"块 + 文字"两个对象：栏目 13 + 地区 42 = 55 个 chip，
 *   两个对象一个就是 110 个，池子经不起。label 自带背景，一个顶俩。*/
static lv_obj_t *mk_chip(lv_obj_t *par, int x, int y, int w, int h,
                         const char *txt, bool on,
                         lv_event_cb_t cb, void *ud)
{
    lv_obj_t *l = lv_label_create(par);
    lv_obj_set_style_text_font(l, F_SMALL, 0);
    lv_obj_set_style_text_color(l, on ? C_PAPER2 : C_INK, 0);
    lv_obj_set_style_bg_color(l, on ? C_ACC : C_PAPER2, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(l, h / 2, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(l, (h - 18) / 2, 0);
    lv_obj_set_style_pad_bottom(l, 0, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, h);
    lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(l, cb, LV_EVENT_CLICKED, ud);
    return l;
}

/* ============================================================
 *  电台筛选
 * ============================================================ */
static void apply_filter(void)
{
    int n = app_st_count();
    s_pool_n = 0;
    for (int i = 0; i < n && s_pool_n < APP_ST_MAX; i++) {
        const net_station_t *st = app_st_get(i);
        if (!st) continue;
        if (s_f_cat  >= 0 && (int)st->cat  != s_f_cat)  continue;
        if (s_f_prov >= 0 && (int)st->prov != s_f_prov) continue;
        s_pool[s_pool_n++] = i;
    }
}

static void refresh_sum(void)
{
    if (!s_lbl_sum) return;
    /* ⚠ 索引必须先夹住再取名字：
     *   地区可以是 NET_PROV_NONE(255)（全国/无归属的台，本机 151 台），
     *   直接拿 255 去索引 g_prov_name[42] 就是读越界。
     *   这个坑在"筛选"这条路上特别容易漏，因为 255 是合法值不是错误。*/
    const char *c = "全部栏目";
    if (s_f_cat >= 0 && s_f_cat < NET_CAT_N) c = g_cat_name[s_f_cat];
    const char *p = "全部地区";
    if (s_f_prov >= 0 && s_f_prov < NET_PROV_N) p = g_prov_name[s_f_prov];
    else if (s_f_prov == NET_PROV_NONE)         p = "全国";

    /* ★★ 这里的 printf 精度【千万不要】拿来"防溢出"：
     *   %.10s 的 10 是【字节】数，不是一个汉字算一个。
     *   汉字 3 字节 ⇒ %.10s 只放得下 3 个字零 1 字节，
     *   第 4 个字被劈成半个 UTF-8 序列 —— 实测屏上是
     *   「共 12 台 · 戏曲/全部地」（"区"没了），看着像排版挤，
     *   实际是功能性错误：用户根本读不到自己选的是哪一档。
     *   本行最长实测 =「共 1253 台 · 怀旧老歌 · 内蒙古」≈ 238 px < 320 px，
     *   缓冲区 96 B 绰绰有余 ⇒ 不做截断，超长交给 label 的 LONG_DOT 兜底
     *   （LONG_DOT 是按【字形】回退的，不会劈多字节）。*/
    char b[96];
    if (s_f_cat < 0 && s_f_prov < 0)
        snprintf(b, sizeof(b), "共 %d 台 · 全部", s_pool_n);
    else
        snprintf(b, sizeof(b), "共 %d 台 · %s · %s", s_pool_n, c, p);
    lv_label_set_text(s_lbl_sum, b);
}

/* ============================================================
 *  行对象复用器
 * ============================================================ */
static void rl_scroll_cb(lv_event_t *e)
{
    rowlist_t *rl = (rowlist_t *)lv_event_get_user_data(e);
    if (!s_alive || !rl) return;
    lv_obj_t *l = lv_event_get_target(e);
    int first = (int)lv_obj_get_scroll_y(l) / ROW_H;
    if (first < 0) first = 0;
    if (first == rl->first) return;

    rl->first = first;
    for (int j = 0; j < ROW_N; j++) {
        lv_obj_set_pos(rl->rows[j], 0, (first + j) * ROW_H);
        if (rl->kind == KIND_ST) {
            int i = first + j;
            st_fill(rl, j, (i < s_pool_n) ? s_pool[i] : -1);
        } else {
            sd_fill(rl, j, first + j);
        }
    }
}

static void st_row_cb(lv_event_t *e);        /* 前向 */
static void sd_row_cb(lv_event_t *e);        /* 前向 */

static void rl_build(rowlist_t *rl, lv_obj_t *par, int top,
                     row_kind_t kind, lv_event_cb_t rowcb)
{
    memset(rl, 0, sizeof(*rl));
    rl->kind  = kind;
    rl->first = 0;
    for (int j = 0; j < ROW_N; j++) rl->idx[j] = -1;

    rl->list = lv_obj_create(par);
    lv_obj_remove_style_all(rl->list);
    lv_obj_set_pos(rl->list, 0, top);
    lv_obj_set_size(rl->list, SCR_W, SCR_H - top);
    lv_obj_set_style_bg_opa(rl->list, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(rl->list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(rl->list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(rl->list, rl_scroll_cb, LV_EVENT_SCROLL, rl);

    /* 撑高块：把"内容高度"顶到 条数×行高 —— LVGL 的滚动范围是按
     * 子对象的外接矩形算的，没有它就只能滚到最后一个行对象那儿。*/
    lv_obj_t *sp = lv_obj_create(rl->list);
    lv_obj_remove_style_all(sp);
    lv_obj_set_pos(sp, 0, 0);
    lv_obj_set_size(sp, 1, 1);
    lv_obj_clear_flag(sp, LV_OBJ_FLAG_CLICKABLE);
    rl->spacer = sp;

    for (int j = 0; j < ROW_N; j++) {
        lv_obj_t *r = mk_box(rl->list, 0, j * ROW_H, SCR_W - 16, ROW_H - 6,
                             C_PAPER2, 10);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, rowcb, LV_EVENT_CLICKED, (void *)(intptr_t)j);
        rl->rows[j] = r;

        lv_obj_t *lab = mk_lab(r, 12, 14, F_ST, C_INK, "");
        lv_obj_set_width(lab, SCR_W - 16 - 24);
        lv_label_set_long_mode(lab, LV_LABEL_LONG_DOT);
        lv_obj_clear_flag(lab, LV_OBJ_FLAG_CLICKABLE);
        rl->labs[j] = lab;
    }
}

/* 重填全部行 + 重设滚动范围 + 回到顶部 */
static void rl_refresh(rowlist_t *rl)
{
    if (!rl || !rl->list) return;
    lv_obj_set_size(rl->spacer, 1,
                    (rl->total > 0 ? rl->total : 1) * ROW_H);
    lv_obj_scroll_to_y(rl->list, 0, LV_ANIM_OFF);
    rl->first = 0;
    for (int j = 0; j < ROW_N; j++) {
        lv_obj_set_pos(rl->rows[j], 0, j * ROW_H);
        if (rl->kind == KIND_ST) {
            st_fill(rl, j, (j < s_pool_n) ? s_pool[j] : -1);
        } else {
            sd_fill(rl, j, j);
        }
    }
}

/* ---------- 电台行 ---------- */
static void st_fill(rowlist_t *rl, int j, int idx)
{
    if (idx < 0 || idx >= app_st_count()) {
        lv_obj_add_flag(rl->rows[j], LV_OBJ_FLAG_HIDDEN);
        rl->idx[j] = -1;
        return;
    }
    lv_obj_clear_flag(rl->rows[j], LV_OBJ_FLAG_HIDDEN);

    const net_station_t *st = app_st_get(idx);
    const char *nm = (st && st->name) ? st->name : "?";
    lv_label_set_text(rl->labs[j], nm);
    /* 正在播的那台用标识色点出来 —— 列表和播放页对得上号 */
    lv_obj_set_style_text_color(rl->labs[j],
        (idx == app_radio_station_current()) ? C_ACC : C_INK, 0);
    rl->idx[j] = idx;
}

static void st_tap_row(int j)
{
    int idx = s_rl_st.idx[j];
    if (idx < 0) return;
    /* ⚠ 每个 %s 都必须带精度：本工程开 -Werror，不定长 %s 会触发
     *   format-truncation 直接编译失败（这条踩过不止一次）。*/
    const net_station_t *stq = app_st_get(idx);
    ESP_LOGI(TAG, "点列表第 %d 行 → 台 %d「%.24s」", j, idx,
             (stq && stq->name) ? stq->name : "?");
    s_playing_local = false;
    s_cur_pos = -1;
    for (int k = 0; k < s_pool_n; k++) {
        if (s_pool[k] == idx) { s_cur_pos = k; break; }
    }
    esp_err_t err = app_radio_play_station(idx);
    if (err != ESP_OK) ESP_LOGW(TAG, "  播放失败 err=%d", (int)err);
    show_view(V_PLAY);
}

static void st_row_cb(lv_event_t *e)
{
    if (!s_alive) return;
    st_tap_row((int)(intptr_t)lv_event_get_user_data(e));
}

/* ---------- 本地音频行 ---------- */
static void sd_fill(rowlist_t *rl, int j, int i)
{
    if (i < 0 || i >= s_sd_n) {
        lv_obj_add_flag(rl->rows[j], LV_OBJ_FLAG_HIDDEN);
        rl->idx[j] = -1;
        return;
    }
    lv_obj_clear_flag(rl->rows[j], LV_OBJ_FLAG_HIDDEN);
    const app_sd_entry_t *en = &s_sd_ents[i];
    char b[300];
    /* ★ 目录加一个尾斜杠当标识：不引符号字（▸ ▶ 之类不在字库里，
     *   画出来就是方块），也省一个对象。*/
    if (en->is_dir) snprintf(b, sizeof(b), "%.280s/", en->name);
    else            snprintf(b, sizeof(b), "%.280s",   en->name);
    lv_label_set_text(rl->labs[j], b);
    lv_obj_set_style_text_color(rl->labs[j], en->is_dir ? C_GOLD : C_INK, 0);
    rl->idx[j] = i;
}

static void sd_tap_row(int j)
{
    int i = s_rl_sd.idx[j];
    if (i < 0 || i >= s_sd_n) return;
    const app_sd_entry_t *en = &s_sd_ents[i];

    if (en->is_dir) {
        char np[256];
        if (strcmp(s_sd_dir, "/sdcard") == 0)
            snprintf(np, sizeof(np), "/sdcard/%.200s", en->name);
        else
            snprintf(np, sizeof(np), "%.180s/%.60s", s_sd_dir, en->name);
        ESP_LOGI(TAG, "本地音频：进目录 %.60s", np);
        sd_scan(np);
        rl_refresh(&s_rl_sd);
        if (s_sd_path) lv_label_set_text(s_sd_path, s_sd_dir);
        return;
    }

    char fp[300];
    snprintf(fp, sizeof(fp), "%.200s/%.90s", s_sd_dir, en->name);
    ESP_LOGI(TAG, "本地音频：播「%.40s」", fp);
    esp_err_t r = app_radio_play_file(fp);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "  播放失败：%s", esp_err_to_name(r));
        if (s_play_state) lv_label_set_text(s_play_state, "打不开这个文件");
        show_view(V_PLAY);
        return;
    }
    s_playing_local = true;
    s_cur_pos = -1;
    show_view(V_PLAY);
}

static void sd_row_cb(lv_event_t *e)
{
    if (!s_alive) return;
    sd_tap_row((int)(intptr_t)lv_event_get_user_data(e));
}

/* 扫目录：就地压实成「先目录、后音频」，其余（歌词/封面/文本）丢掉 */
static void sd_scan(const char *dir)
{
    s_sd_n = 0;
    if (!dir || !dir[0]) return;
    snprintf(s_sd_dir, sizeof(s_sd_dir), "%.200s", dir);
    s_rl_sd.total = 0;
    if (!app_sd_is_mounted()) {
        ESP_LOGW(TAG, "本地音频：SD 没挂载");
        return;
    }
    int n = app_sd_list(s_sd_dir, s_sd_ents, SD_MAX);
    if (n < 0) { ESP_LOGW(TAG, "本地音频：打不开 %.40s", s_sd_dir); return; }
    if (n > SD_MAX) n = SD_MAX;

    int w = 0;
    for (int i = 0; i < n; i++) {              /* 第一遍：目录提到前面 */
        if (s_sd_ents[i].is_dir) {
            if (i != w) { app_sd_entry_t t = s_sd_ents[i]; s_sd_ents[i] = s_sd_ents[w]; s_sd_ents[w] = t; }
            w++;
        }
    }
    int w2 = w;
    for (int i = w; i < n; i++) {              /* 第二遍：只留音频 */
        if (app_sd_is_audio(s_sd_ents[i].name)) {
            if (i != w2) { app_sd_entry_t t = s_sd_ents[i]; s_sd_ents[i] = s_sd_ents[w2]; s_sd_ents[w2] = t; }
            w2++;
        }
    }
    s_sd_n = w2;
    s_rl_sd.total = s_sd_n;
    ESP_LOGI(TAG, "本地音频：%.60s 目录 %d 项 / 音频 %d 首 / 合计显示 %d",
             s_sd_dir, n, w2 - w, s_sd_n);
}

/* ============================================================
 *  视图切换
 * ============================================================ */
static void show_view(int v)
{
    if (v < 0 || v >= V_N) return;
    s_cur_view = v;
    for (int i = 0; i < V_N; i++) {
        if (!s_view[i]) continue;
        if (i == v) lv_obj_clear_flag(s_view[i], LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_add_flag(s_view[i],   LV_OBJ_FLAG_HIDDEN);
    }
    if (v == V_CAT) rebuild_cat_body();
    if (v == V_PLAY) play_refresh();
}

/* ============================================================
 *  ① 列表视图
 * ============================================================ */
static void back_home_cb(lv_event_t *e)
{
    (void)e;
    ui_shell_back();          /* 外壳负责：先切主菜单屏，再调 leave() 拆我们 */
}
static void to_cat_cb(lv_event_t *e)  { (void)e; show_view(V_CAT); }
static void to_sd_cb(lv_event_t *e)   { (void)e; if (s_sd_n == 0) sd_scan(s_sd_dir); rl_refresh(&s_rl_sd); if (s_sd_path) lv_label_set_text(s_sd_path, s_sd_dir); show_view(V_SD); }

static void build_list_view(void)
{
    lv_obj_t *p = lv_obj_create(s_scr);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_size(p, SCR_W, SCR_H);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    s_view[V_LIST] = p;

    mk_btn(p, 10, 12, 62, 32, "返 回", C_PAPER2, C_GOLD, back_home_cb);
    mk_clab(p, 18, SCR_W, 0, F_READ, C_INK, "拾  声  电  台");

    s_lbl_sum = mk_clab(p, 50, SCR_W, 0, F_SMALL, C_MUTE, "");
    /* ★ 超长时用省略号收尾，而不是默认的换行 ——
     *   换行会往下压到 72 那条分隔线，且挡住第一行台名。
     *   （正常文案不会触发：最长实测 238 px < 320 px。）*/
    lv_label_set_long_mode(s_lbl_sum, LV_LABEL_LONG_DOT);
    mk_box(p, 20, 72, SCR_W - 40, 1, C_LINE, 0);

    mk_btn(p, 10,  78, 145, 34, "分 类",    C_PAPER2, C_INK, to_cat_cb);
    mk_btn(p, 165, 78, 145, 34, "本 地 音 频", C_PAPER2, C_INK, to_sd_cb);
    mk_box(p, 20, 116, SCR_W - 40, 1, C_LINE, 0);

    rl_build(&s_rl_st, p, LIST_TOP, KIND_ST, st_row_cb);
}

/* ============================================================
 *  ② 播放视图
 * ============================================================ */
static void play_back_cb(lv_event_t *e) { (void)e; show_view(V_LIST); }

static void vol_refresh(void)
{
    if (!s_lbl_vol) return;
    char b[48];
    snprintf(b, sizeof(b), "音 量 %d%%", xs_cfg_volume());
    lv_label_set_text(s_lbl_vol, b);
}
static void vol_minus_cb(lv_event_t *e)
{
    (void)e;
    xs_cfg_set_volume(xs_cfg_volume() - 10);
    vol_refresh();
    ESP_LOGI(TAG, "音量 = %d%%", xs_cfg_volume());
}
static void vol_plus_cb(lv_event_t *e)
{
    (void)e;
    xs_cfg_set_volume(xs_cfg_volume() + 10);
    vol_refresh();
    ESP_LOGI(TAG, "音量 = %d%%", xs_cfg_volume());
}

static void prev_cb(lv_event_t *e)
{
    (void)e;
    if (s_playing_local) {
        char nm[300] = {0};
        esp_err_t r = app_radio_play_next(s_sd_dir, -1, nm, (int)sizeof(nm));
        if (r != ESP_OK) ESP_LOGW(TAG, "上一首失败：%s", esp_err_to_name(r));
    } else if (s_pool_n > 0) {
        /* ★ 在【筛选池】里走位，不是全局走位 —— 见文件头 ③ */
        int pos = (s_cur_pos < 0) ? 0 : s_cur_pos;
        pos = (pos - 1 + s_pool_n) % s_pool_n;
        app_radio_play_station(s_pool[pos]);
        s_cur_pos = pos;
    }
    play_refresh();
}

static void next_cb(lv_event_t *e)
{
    (void)e;
    if (s_playing_local) {
        char nm[300] = {0};
        esp_err_t r = app_radio_play_next(s_sd_dir, +1, nm, (int)sizeof(nm));
        if (r != ESP_OK) ESP_LOGW(TAG, "下一首失败：%s", esp_err_to_name(r));
    } else if (s_pool_n > 0) {
        int pos = (s_cur_pos < 0) ? 0 : s_cur_pos;
        pos = (pos + 1) % s_pool_n;
        app_radio_play_station(s_pool[pos]);
        s_cur_pos = pos;
    }
    play_refresh();
}

static void toggle_cb(lv_event_t *e)
{
    (void)e;
    app_radio_toggle_pause();
    play_refresh();
}

static void play_refresh(void)
{
    if (!s_alive || !s_play_name) return;

    const char *np = app_radio_now_playing();
    lv_label_set_text(s_play_name, (np && np[0]) ? np : "未在播放");

    char st[96];
    const char *err = app_radio_last_error();
    int cur = app_radio_station_current();
    if (err && err[0]) {
        snprintf(st, sizeof(st), "%.60s", err);
    } else if (app_radio_is_paused()) {
        snprintf(st, sizeof(st), "已暂停 · %s", s_playing_local ? "本地文件" : "电台");
    } else if (s_playing_local) {
        snprintf(st, sizeof(st), "本地播放 · %d / %d 首", 1, s_sd_n);
    } else if (app_radio_is_stream()) {
        snprintf(st, sizeof(st), "LIVE · %d / %d 台（第 %d 号台）",
                 (s_cur_pos >= 0) ? s_cur_pos + 1 : 0, s_pool_n, cur + 1);
    } else if (app_radio_is_playing()) {
        snprintf(st, sizeof(st), "播放中 · 第 %d 号台", cur + 1);
    } else {
        snprintf(st, sizeof(st), "未在播放 · 共 %d 台", s_pool_n);
    }
    lv_label_set_text(s_play_state, st);
    lv_label_set_text(s_play_btn_lab, app_radio_is_paused() ? "继续" : "暂停");
    vol_refresh();
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_alive) return;
    if (!s_view[V_PLAY] || lv_obj_has_flag(s_view[V_PLAY], LV_OBJ_FLAG_HIDDEN)) return;

    int n = app_radio_spectrum_bins();
    for (int i = 0; i < BAR_N; i++) {
        int v = (i < n) ? app_radio_spectrum(i) : 0;
        if (v < 0)   v = 0;
        if (v > 100) v = 100;
        int h = 4 + v * 56 / 100;
        lv_obj_set_y(s_bars[i], BAR_BOT - h);
        lv_obj_set_height(s_bars[i], h);
    }

    if (++s_tick_n >= 5) {          /* 1 秒一次状态 */
        s_tick_n = 0;
        play_refresh();
    }
}

static void build_play_view(void)
{
    lv_obj_t *p = lv_obj_create(s_scr);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_size(p, SCR_W, SCR_H);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    s_view[V_PLAY] = p;

    mk_btn(p, 10, 12, 62, 32, "返 回", C_PAPER2, C_GOLD, play_back_cb);
    mk_clab(p, 20, SCR_W, 0, F_SMALL, C_MUTE, "正 在 播 放");

    /* 大台名（最多两行）—— 走台名字库 */
    s_play_name = mk_clab(p, 54, SCR_W - 40, 20, F_ST, C_INK, "");
    lv_obj_set_height(s_play_name, 68);
    lv_label_set_long_mode(s_play_name, LV_LABEL_LONG_WRAP);

    s_play_state = mk_clab(p, 130, SCR_W, 0, F_SMALL, C_MUTE, "");

    int total_w = BAR_N * BAR_W + (BAR_N - 1) * BAR_GAP;
    int x0 = (SCR_W - total_w) / 2;
    for (int i = 0; i < BAR_N; i++) {
        s_bars[i] = mk_box(p, x0 + i * (BAR_W + BAR_GAP), BAR_BOT - 4,
                           BAR_W, 4, C_ACC, BAR_W / 2);
    }

    /* 音量：一个标签 + 两个方键。
     * ★ 为什么电台要自带音量：全局「声音」开关在灵签那边，本 App 用
     *   audio_boost 旁路了它（见文件头 ①），所以这里必须给一个能调的口，
     *   否则用户遇到"没声"只能回灵签去改。*/
    s_lbl_vol = mk_clab(p, 300, 180, 10, F_SMALL, C_INK, "音 量 100%");
    mk_btn(p, 200, 296, 44, 40, "－", C_PAPER2, C_INK, vol_minus_cb);
    mk_btn(p, 252, 296, 44, 40, "＋", C_PAPER2, C_INK, vol_plus_cb);

    const int BY = 356, BW = 88, BH = 56;
    mk_btn(p, 16,  BY, BW, BH, "上一台", C_PAPER2, C_INK, prev_cb);
    lv_obj_t *b2 = mk_box(p, 116, BY, BW, BH, C_ACC, 12);
    lv_obj_add_flag(b2, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b2, toggle_cb, LV_EVENT_CLICKED, NULL);
    s_play_btn_lab = mk_clab(b2, 19, BW, 0, F_MID, C_PAPER2, "暂停");
    mk_btn(p, 216, BY, BW, BH, "下一台", C_PAPER2, C_INK, next_cb);
}

/* ============================================================
 *  ③ 分类视图
 * ============================================================ */
static void cat_back_cb(lv_event_t *e) { (void)e; show_view(V_LIST); }

static void cat_chip_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    int tab = v >> 16;
    int i   = (v & 0xFFFF) - 1;          /* 编了 +1，0 表示「全部」→ -1 */
    if (tab == 0) s_f_cat  = i;
    else          s_f_prov = i;

    apply_filter();
    s_cur_pos = -1;
    s_rl_st.total = s_pool_n;
    rl_refresh(&s_rl_st);
    refresh_sum();
    ESP_LOGI(TAG, "分类筛选 → 栏目=%d 地区=%d ⇒ %d 台",
             s_f_cat, s_f_prov, s_pool_n);
    rebuild_cat_body();                   /* 把高亮挪到新选项 */
    show_view(V_LIST);
}

static void cat_tab_cb(lv_event_t *e)
{
    int tab = (int)(intptr_t)lv_event_get_user_data(e);
    if (tab == s_cat_tab_cur) return;
    s_cat_tab_cur = tab;
    rebuild_cat_body();
}

static void rebuild_cat_body(void)
{
    if (!s_cat_body) return;
    lv_obj_clean(s_cat_body);             /* 清掉上一 tab 的 chips */

    for (int t = 0; t < 2; t++) {
        if (!s_cat_tab[t]) continue;
        /* 用背景色表示当前 tab（label 也支持 bg）*/
        bool on = (t == s_cat_tab_cur);
        lv_obj_set_style_bg_color(s_cat_tab[t], on ? C_ACC : C_PAPER2, 0);
        lv_obj_set_style_text_color(s_cat_tab[t], on ? C_PAPER2 : C_INK, 0);
    }

    /* chip 的编码：高 16 位 = tab，低 16 位 = 【值 + 1】
     *   （值 -1 = "全部" ⇒ 编码 0；地区那一栏最后多一个「全国」= 255
     *     ⇒ 编码 256，仍然塞得进低 16 位）*/
    const int CW = 145, CH = 34, GX = 10, GY = 44;
    int n = (s_cat_tab_cur == 0) ? (NET_CAT_N + 1) : (NET_PROV_N + 2);

    for (int i = 0; i < n; i++) {
        int x = GX + (i % 2) * (CW + 10);
        int y = (i / 2) * GY;
        int val;
        bool on;
        const char *txt;
        if (i == 0) {
            txt = "全部";
            val = -1;
            on  = (s_cat_tab_cur == 0) ? (s_f_cat < 0) : (s_f_prov < 0);
        } else if (s_cat_tab_cur == 0) {
            txt = g_cat_name[i - 1];
            val = i - 1;
            on  = (s_f_cat == val);
        } else if (i <= NET_PROV_N) {
            txt = g_prov_name[i - 1];
            val = i - 1;
            on  = (s_f_prov == val);
        } else {
            /* ★ 最后这一个不是"省"，是 NET_PROV_NONE：
             *   本机有 151 台没有地区归属（"全国"类），不给它一个 chip
             *   就永远筛不出来。*/
            txt = "全国/无地区";
            val = NET_PROV_NONE;
            on  = (s_f_prov == NET_PROV_NONE);
        }
        mk_chip(s_cat_body, x, y, CW, CH, txt, on,
                cat_chip_cb, (void *)(intptr_t)((s_cat_tab_cur << 16) | (val + 1)));
    }
}

static void build_cat_view(void)
{
    lv_obj_t *p = lv_obj_create(s_scr);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_size(p, SCR_W, SCR_H);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    s_view[V_CAT] = p;

    mk_btn(p, 10, 12, 62, 32, "返 回", C_PAPER2, C_GOLD, cat_back_cb);
    mk_clab(p, 18, SCR_W, 0, F_READ, C_INK, "分      类");

    s_cat_tab[0] = mk_chip(p, 34, 56, 120, 34, "栏 目", true,
                           cat_tab_cb, (void *)(intptr_t)0);
    s_cat_tab[1] = mk_chip(p, 166, 56, 120, 34, "地 区", false,
                           cat_tab_cb, (void *)(intptr_t)1);
    mk_box(p, 20, 100, SCR_W - 40, 1, C_LINE, 0);

    s_cat_body = lv_obj_create(p);
    lv_obj_remove_style_all(s_cat_body);
    lv_obj_set_pos(s_cat_body, 0, 108);
    lv_obj_set_size(s_cat_body, SCR_W, SCR_H - 108);
    lv_obj_set_style_bg_opa(s_cat_body, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(s_cat_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_cat_body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_top(s_cat_body, 6, 0);
    lv_obj_set_style_pad_bottom(s_cat_body, 40, 0);
}

/* ============================================================
 *  ④ 本地音频视图
 * ============================================================ */
static void sd_back_cb(lv_event_t *e) { (void)e; show_view(V_LIST); }

static void sd_up_cb(lv_event_t *e)
{
    (void)e;
    if (strcmp(s_sd_dir, "/sdcard") == 0) {
        ESP_LOGI(TAG, "本地音频：已经在根目录");
        return;
    }
    char *sl = strrchr(s_sd_dir, '/');
    if (sl && sl > s_sd_dir + 1) {
        *sl = '\0';
        if (strlen(s_sd_dir) < 7) snprintf(s_sd_dir, sizeof(s_sd_dir), "/sdcard");
    }
    sd_scan(s_sd_dir);
    rl_refresh(&s_rl_sd);
    if (s_sd_path) lv_label_set_text(s_sd_path, s_sd_dir);
}

static void build_sd_view(void)
{
    lv_obj_t *p = lv_obj_create(s_scr);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_size(p, SCR_W, SCR_H);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    s_view[V_SD] = p;

    mk_btn(p, 10, 12, 62, 32, "返 回", C_PAPER2, C_GOLD, sd_back_cb);
    mk_clab(p, 18, SCR_W, 0, F_READ, C_INK, "本 地 音 频");
    mk_btn(p, 245, 12, 65, 32, "上 级", C_PAPER2, C_GOLD, sd_up_cb);

    s_sd_path = mk_clab(p, 50, SCR_W, 0, F_SMALL, C_MUTE, "/sdcard");
    lv_label_set_long_mode(s_sd_path, LV_LABEL_LONG_DOT);
    mk_box(p, 20, 70, SCR_W - 40, 1, C_LINE, 0);

    rl_build(&s_rl_sd, p, SD_TOP, KIND_SD, sd_row_cb);
}

/* ============================================================
 *  对外的 init / leave
 * ============================================================ */
void ui_radio_v_init(void)
{
    app_st_load();                       /* 读 /sdcard/stations.tsv（幂等，没卡回落内置）*/

    /* ★★ 接管出声（根因与理由见文件头 ①）。必须在播放前做，
     *    否则第一台会「开了没声」，看起来像播失败。*/
    xs_cfg_audio_boost();

    if (!s_pool) {
        s_pool = heap_caps_malloc((size_t)APP_ST_MAX * sizeof(int),
                                  MALLOC_CAP_SPIRAM);
        if (!s_pool) s_pool = heap_caps_malloc((size_t)APP_ST_MAX * sizeof(int),
                                               MALLOC_CAP_8BIT);
    }
    if (!s_sd_ents) {
        s_sd_ents = heap_caps_malloc((size_t)SD_MAX * sizeof(app_sd_entry_t),
                                     MALLOC_CAP_SPIRAM);
        if (!s_sd_ents) s_sd_ents = heap_caps_malloc(
            (size_t)SD_MAX * sizeof(app_sd_entry_t), MALLOC_CAP_8BIT);
    }
    if (!s_pool || !s_sd_ents) {
        ESP_LOGE(TAG, "分配失败 pool=%p ents=%p —— 电台页不建，安全返回",
                 (void *)s_pool, (void *)s_sd_ents);
        return;
    }

    s_alive  = true;                     /* 先开闸：rl_refresh 会走 fill */
    s_tick_n = 0;
    s_cur_pos = -1;
    s_playing_local = false;
    s_cat_tab_cur = 0;
    snprintf(s_sd_dir, sizeof(s_sd_dir), "/sdcard");

    s_scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_style_bg_color(s_scr, C_PAPER, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    build_list_view();
    build_play_view();
    build_cat_view();
    build_sd_view();

    apply_filter();
    s_rl_st.total = s_pool_n;
    rl_refresh(&s_rl_st);
    refresh_sum();
    rebuild_cat_body();
    sd_scan(s_sd_dir);
    rl_refresh(&s_rl_sd);
    if (s_sd_path) lv_label_set_text(s_sd_path, s_sd_dir);

    show_view(V_LIST);
    s_tick = lv_timer_create(tick_cb, 200, NULL);

    lv_screen_load(s_scr);

    /* ★ 把"台单从哪来、有多少台、池子用了多少"打进日志 ——
     *   电台页出问题时第一要分的是"卡上那份读进去没有"，
     *   第二是"对象池够不够"（不够在 release 下是静默崩溃）。*/
    lv_mem_monitor_t mm;
    lv_mem_monitor(&mm);
    ESP_LOGI(TAG, "拾声电台(竖版)：%d/%d 台，来源=%s｜音量=%d%% 静音旁路=%s"
                  "｜LVGL 池 %u/%u B 用了 %d%% 碎片 %d%%",
             s_pool_n, app_st_count(),
             (app_st_source() == APP_ST_SRC_TSV) ? "SD /stations.tsv" : "内置台单",
             xs_cfg_volume(), xs_cfg_muted() ? "是" : "否",
             (unsigned)(mm.total_size - mm.free_size), (unsigned)mm.total_size,
             (int)mm.used_pct, (int)mm.frag_pct);
}

void ui_radio_v_leave(void)
{
    s_alive = false;                     /* 先关闸：timer 回调里靠它早退 */
    if (s_tick) { lv_timer_del(s_tick); s_tick = NULL; }

    app_radio_stop();                    /* 退出即停播 —— 主菜单上没有播放控制 */

    /* ★ 把静音语义还给灵签（见文件头 ①）。忘掉这一句的后果是：
     *   从电台回主菜单后，灵签的六字大明咒会突然出声。*/
    xs_cfg_audio_restore();

    /* ★ 必须异步删：这条路径通常是从按钮回调链里进来的
     *   （返回主菜单 = 按钮 → ui_shell_back → 这里），
     *   同步删等于在事件处理中途拆掉事件源对象。*/
    if (s_scr) { lv_obj_del_async(s_scr); s_scr = NULL; }

    for (int i = 0; i < V_N; i++) s_view[i] = NULL;
    s_lbl_sum = s_play_name = s_play_state = s_play_btn_lab = s_lbl_vol = NULL;
    s_cat_body = s_cat_tab[0] = s_cat_tab[1] = s_sd_path = NULL;
    for (int i = 0; i < BAR_N; i++) s_bars[i] = NULL;
    memset(&s_rl_st, 0, sizeof(s_rl_st));
    memset(&s_rl_sd, 0, sizeof(s_rl_sd));
    s_sd_n = 0;
    s_pool_n = 0;
    s_cur_pos = -1;
    s_playing_local = false;

    /* 两块 PSRAM 缓冲随本 App 一起释放（进入才创建、离开即销毁）*/
    if (s_pool)    { heap_caps_free(s_pool);    s_pool = NULL; }
    if (s_sd_ents) { heap_caps_free(s_sd_ents); s_sd_ents = NULL; }

    ESP_LOGI(TAG, "← 退出拾声电台，已释放（音量回到设置档）");
}

/* ============================================================
 *  串口调试口（radio <sub>）
 * ============================================================ */
void ui_radio_v_demo_tap(int n)
{
    if (!s_alive || s_cur_view != V_LIST) {
        ESP_LOGW(TAG, "demo_tap: 不在列表视图（view=%d）", s_cur_view);
        return;
    }
    if (n < 0 || n >= ROW_N) { ESP_LOGW(TAG, "demo_tap: 越界 %d", n); return; }
    st_tap_row(n);
}

void ui_radio_v_demo_scroll(int y)
{
    if (!s_alive || !s_rl_st.list) return;
    show_view(V_LIST);
    lv_obj_scroll_to_y(s_rl_st.list, y, LV_ANIM_OFF);
    ESP_LOGI(TAG, "demo_scroll: 滚到 y=%d（首行应为 %d）", y, y / ROW_H);
}

void ui_radio_v_demo_filter(int cat, int prov)
{
    if (!s_alive) return;
    s_f_cat  = (cat  >= NET_CAT_N)  ? -1 : cat;
    /* ⚠ NET_PROV_NONE(255) 是【合法筛选值】（"全国/无地区"那 151 台），
     *   不能跟"越界"一起折成 -1 —— 那样这个 chip 就永远筛不出东西，
     *   而且刷新状态行时还会拿 255 去索引 g_prov_name[42]。*/
    if (prov >= NET_PROV_N && prov != NET_PROV_NONE) s_f_prov = -1;
    else                                             s_f_prov = prov;
    apply_filter();
    s_cur_pos = -1;
    s_rl_st.total = s_pool_n;
    rl_refresh(&s_rl_st);
    refresh_sum();
    rebuild_cat_body();
    ESP_LOGI(TAG, "demo_filter: 栏目=%d 地区=%d ⇒ %d 台", s_f_cat, s_f_prov, s_pool_n);
}

void ui_radio_v_demo_view(int v)
{
    if (!s_alive) return;
    if (v == V_SD) {
        sd_scan(s_sd_dir);
        rl_refresh(&s_rl_sd);
        if (s_sd_path) lv_label_set_text(s_sd_path, s_sd_dir);
    }
    show_view(v);
    ESP_LOGI(TAG, "demo_view: 切到 %d", v);
}

void ui_radio_v_demo_cat_tab(int tab)
{
    if (!s_alive) return;
    s_cat_tab_cur = (tab == 1) ? 1 : 0;
    rebuild_cat_body();
    show_view(V_CAT);
    ESP_LOGI(TAG, "demo_cat_tab: %d（0 栏目 / 1 地区）", s_cat_tab_cur);
}

void ui_radio_v_demo_sd(const char *path)
{
    if (!s_alive) return;
    sd_scan((path && path[0]) ? path : "/sdcard");
    rl_refresh(&s_rl_sd);
    if (s_sd_path) lv_label_set_text(s_sd_path, s_sd_dir);
    show_view(V_SD);
    ESP_LOGI(TAG, "demo_sd: %.80s ⇒ %d 项", s_sd_dir, s_sd_n);
}
