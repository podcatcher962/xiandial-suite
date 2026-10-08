/*
 * 灵签 (LingQian) —— 3.5" 触摸屏抽签机 · 界面   v2.2
 * ============================================================
 *  板型：LCD Wiki ES3C35P（ESP32-S3 N16R8 / ST77922 QSPI 320×480 竖屏 / FT6336G）
 *  底座复用拾声(XianDial)：显示／触摸／字库／音频／电源。
 *
 *  ★★★ v2.2 改动（兰兰 10-07 夜）：
 *      「音量，屏幕亮度，静音，都做在设置按键栏里面，这样屏幕按键就少许多」
 *      「解签页最上面的静音按键挡住第几签标题」
 *      「朗读签文也做到设置按键里」
 *    ⇒ 做法：每页右上角只留【一个】"设 置"按钮，点开是一张覆盖式设置面板，
 *      面板里四行：音量（－/＋）、亮度（－/＋）、声音开关、朗读签文。
 *      页面上的静音块、朗读块【全部删除】——不再有浮在内容之上的小块。
 *    ⇒ 页面按键从"每页 3~4 个 + 右上静音"降到"每页 1~2 个主操作 + 1 个设置"，
 *      而且所有设置项都收在同一处，位置永远固定（右上），不用每页去找。
 *
 *  ★★★ 这一版是【推翻重做】，不是微调。改动的起因（兰兰原话）：
 *      「封面页不好看，字体小，像素感很严重，字要古雅」
 *      「灵签不需要很具象的抽签桶，HTML版的观音莲花形象就有意境」
 *      「默念心经，等天机具足再求签，还要选择需要求什么再出签」
 *      「轻点此处按键太小，点上去无效」
 *    对应四件事：① 视觉换成海报意境 ② 字体换楷体 ③ 加心经/心愿流程
 *                ④ 交互全部改成显式大按钮。
 *
 *  ★★ 设计蓝本 = 兰兰自己那个 HTML 版《观音灵签 · Avalokita Oracle》
 *     （2026-08-01-21-14-20/guanyin.html）。它的灵魂是这条链：
 *       观音海报开屏 → 六字大明咒背景音 → 默念《心经》→ 静候灵机
 *       → 选「所求何事」→ 揭晓 → 五层解签
 *       （签诗 / 解曰 / 典故 / 安心寄语 / 行动建议）
 *     硬件版就照这条链走 —— 这不是"参考"，是把它的魂搬过来。
 *
 *  ★ 与 HTML 版的一个【硬分叉】：签文数据。
 *    HTML 版那份签是 AI 仿作（签诗重写、吉凶整流、宫位按序号机械轮转），
 *    硬件版用传统原版：吉凶 上22/中60/下18，宫位是长度不等的真分段
 *    （详见 _gen_lingqian.py 文件头）。数据和设计分开评价，各取所长。
 *
 *  ★ 字体：霞鹜文楷（LXGW WenKai, OFL-1.1）
 *    换掉 Ark Pixel 的理由见 _gen_fonts_lq.py —— 像素字体正是"像素感"
 *    的来源，与宣纸/莲花/金线完全相反。
 *
 *  源码结构（状态机 ST_xxx，一条 lv_timer 20ms 走完全程）：
 *    COVER 封面      海报全屏 + 底部大按钮，背景放六字大明咒
 *    SUTRA 默念心经  心经淡入，「天机具足」按钮 2.5s 后才亮
 *    WISH  选心愿    20 个 chip，选中高亮
 *    DRAW  静候灵机  莲花光晕呼吸 2s，再出现「揭晓」按钮
 *    POEM  签文      竖排 30px 楷体签诗，自动朗读
 *    READ  解签      五层，可滚动
 *
 *  ★ 动画只用【一条 timer + 手算位移/透明度】，不用 lv_anim：
 *    lv_anim 每个动画挂一条 timer 和一个 exec_cb，中途删对象会踩悬空指针。
 * ============================================================
 */
#include "ui_lingqian.h"

#include "lingqian_data.h"
#include "app_radio.h"          /* 语音播放走拾声底座的播放器，不自己写解码链路 */
#include "app_audio.h"          /* app_radio_* 走这里；音量已改走 app_settings */
#include "app_display.h"
#include "app_settings.h"       /* ★ 10-08：音量/背光/静音已提升为全局设置 */
#include "app_sd.h"
#include "ui_shell.h"           /* ★ 10-08 多产品外壳：设置面板里的「主页」调 ui_shell_back() */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "lvgl.h"

static const char *TAG = "LQUI";

/* 封面海报（LVGLImage.py 生成，320×480 RGB565，见 assets/cover_img.c） */
LV_IMAGE_DECLARE(cover_img);

/* ---------- 字体（_gen_fonts_lq.py 产出：霞鹜文楷 14/18/30） ---------- */
extern const lv_font_t xs_font_14;
extern const lv_font_t xs_font_18;
extern const lv_font_t xs_font_30;
#define F_HERO  (&xs_font_30)   /* 题字 / 竖排签诗 / 咒语 */
extern const lv_font_t xs_font_22;
/* ★ 10-07 夜第三档：解签正文专用（兰兰「解签字体再大一点」）。
 *   22px 每行 13 字 —— 14px 是 17 字/屏，30px 只 9 字/屏都滚不动。
 *   这一档只给解签页用，那一页字最少、最需要放大。*/
#define F_READ  (&xs_font_22)
#define F_MID   (&xs_font_18)   /* 正文 / 按钮 */
#define F_SMALL (&xs_font_14)   /* 小字提示 */

/* ---------- 宣纸雅致配色（取自海报本身） ----------
 * ★ 上一版是"暗墨 · 鎏金"（深紫底 + 霓虹金），做赛博电台好看，做观音灵签
 *   是彻底跑偏 —— 兰兰的原话是"怎么是这样的背景和字体"。
 *   这一版全部改从海报上取样：宣纸米黄、墨褐、金棕、朱砂。*/
#define C_PAPER   lv_color_hex(0xF3ECE0)   /* 宣纸底 */
#define C_PAPER2  lv_color_hex(0xFBF6EA)   /* 更亮的纸（签纸/按钮底）*/
#define C_INK     lv_color_hex(0x3B2E1E)   /* 墨褐（正文）*/
#define C_INK2    lv_color_hex(0x6B5A45)   /* 次墨 */
#define C_MUTE    lv_color_hex(0x9A8A74)   /* 淡墨（提示）*/
#define C_GOLD    lv_color_hex(0xB8914F)   /* 金棕（线／边）*/
#define C_GOLD2   lv_color_hex(0xD9B978)   /* 亮金 */
#define C_CINNA   lv_color_hex(0xA63A2E)   /* 朱砂 */
#define C_LINE    lv_color_hex(0xE0D6C4)   /* 极淡分割线 */

#define SCR_W    320
#define SCR_H    480

/* ---------- 十二宫五行色（保持：宫位是"一签一色"的依据） ----------
 * 索引 = 地支序号（子0 丑1 寅2 卯3 辰4 巳5 午6 未7 申8 酉9 戌10 亥11）*/
static const uint32_t GONG_HEX[12] = {
    0x4A78D8, 0xC9A24A, 0x4FB07A, 0x4FB07A, 0xC9A24A, 0xD2543C,
    0xD2543C, 0xC9A24A, 0xCFD3D8, 0xCFD3D8, 0xC9A24A, 0x4A78D8,
};
static const char GONG_CH[12][4] = {
    "子", "丑", "寅", "卯", "辰", "巳", "午", "未", "申", "酉", "戌", "亥"
};

/* ---------- 竖排签诗几何 ----------
 * 30px 楷体：7 字 = 210px 高。列距 56（字宽 30 + 间隔 26），
 * 4 列占 3*56+30 = 198，居中 x0 = 61 → 61 / 117 / 173 / 229。
 * ★ 传统竖排【从右往左】：第 1 句在最右 ⇒ x = 61 + (3-i)*56。*/
#define POEM_TOP   116
#define POEM_STEP  56

/* ---------- 状态机 ---------- */
enum { ST_COVER = 0, ST_SUTRA, ST_WISH, ST_DRAW, ST_POEM, ST_READ };

static lv_obj_t *s_scr;

/* 通用 */
static lv_obj_t *s_page;              /* 当前整屏页面容器 */
static bool      s_voice_ok;          /* 当前签有语音文件可读 */
static char      s_voice_path[48];

/* 设置面板的控件指针（面板本身在下方"设置面板：实现"里建）
 * ★ 声明必须放在 voice_update_btn 【之前】—— 那个函数在刷"朗 读/停 止"，
 *   而朗读按钮如今住在设置面板里。v2.2 第一版就是把这段留在文件后面，
 *   编译直接报 's_play_lbl' undeclared。*/
static lv_obj_t *s_set_panel;
static lv_obj_t *s_set_btn, *s_set_btn_lbl;
static lv_obj_t *s_vol_lbl, *s_bl_lbl;
static lv_obj_t *s_snd_btn, *s_snd_lbl;
static lv_obj_t *s_play_btn, *s_play_lbl;
static bool      s_set_open;

static void set_panel_build(void);
void set_panel_refresh(void);
void set_entry_make(lv_obj_t *par, int y);

/* 心经页 */
static lv_obj_t *s_sutra_txt;
static lv_obj_t *s_sutra_btn, *s_sutra_btn_lbl;
static int       s_sutra_ok;          /* 0=还没到"天机具足" 1=可点 */

/* 心愿页 */
#define N_WISH 20
static lv_obj_t *s_chip[N_WISH];
static int       s_wish = -1;
static lv_obj_t *s_wish_sel;
static lv_obj_t *s_wish_btn, *s_wish_btn_lbl;

/* 抽签页 */
static lv_obj_t *s_halo[4];
static lv_obj_t *s_draw_lbl;
static lv_obj_t *s_draw_btn, *s_draw_btn_lbl;

/* 签文页 */
static lv_obj_t *s_poem_lbl[LQ_POEM_LINES];

/* 解签页 */
static lv_obj_t *s_read_body;

static lv_timer_t *s_anim;
static int  s_tick;
static int  s_state = ST_COVER;
static int  s_pick;
static uint32_t s_accent = 0xB8914F;

/* ---------- 音量与静音 ----------
 * ★★ 10-08：这三个量【已提升到全局设置 app_settings（xs_cfg）】。
 *   原来它们存在本文件的 static 里，等于"灵签的私有财产" ——
 *   可它们动的是整机硬件（ES8311 的 DAC 增益、背光 LEDC 占空比），
 *   天气和股票一样要用，凭什么归灵签管？
 *   现在三个产品共用一份，首页也能调（兰兰：「设置按键在首页」）。
 *   默认值（音量 100 / 背光 50 / 声音【关】）在 app_settings.c 里定义。
 * ★ 静音仍然只是"音量归零"，不碰播放器 —— 一碰就会把正在播的也停了。
 * ★ 背景音（六字大明咒）默认不出声（因为全局默认静音），
 *   但用户手点「朗 读 签 文」时必须出声，见 voice_toggle_cb 的 boost。*/

/* 背景音（六字大明咒）：只在封面页循环，进其他页必须停 —— 播放器只有一路 */
static int  s_bgm_fail;
#define LQ_BGM_PATH  "/sdcard/ommani.mp3"
#define LQ_VOICE_FMT "/sdcard/lq%03d.mp3"

/* ============================================================
 *  数据：心愿 / 安心寄语 / 行动建议 / 心经
 *  （文案与 HTML 版同源，改成简体）
 * ============================================================ */
static const char *WISHES[N_WISH] = {
    "事业前程", "姻缘感情", "财运求财", "健康平安", "学业考试",
    "家宅平安", "出行平安", "子嗣孕育", "求医问病", "官非诉讼",
    "生意经营", "求职谋事", "婚姻嫁娶", "迁居置业", "人际恩怨",
    "寻人寻物", "运势流年", "投资理财", "寿元安康", "综合运势",
};

/* 安心寄语：按吉凶档（上 / 中 / 下）各三条，随机取一条。
 * ★ 这是 HTML 版最动人的一处 —— 签文本身是冷的（照录传统），
 *   这一层是暖的。抽签机给的不是判决，是一句安顿。*/
static const char *PEACE[3][3] = {
    {   /* 上 */
        "此签大吉，所求之事多有善果。但请记得：签是鼓励，不是依赖。"
        "福气已至，更要心怀感恩、脚踏实地，方能守得住这份好运。",
        "好运是风，努力是帆。有风也要扬帆，方能远航。"
        "把握当下，顺势而为，莫辜负这份天意。",
        "签文昭示吉祥，愿你乘势而上。然福至心灵者，更懂惜福感恩——"
        "守住本心，福气自会长久。",
    },
    {   /* 中 */
        "此签平平，不吉不凶。人生本就有起有伏，此刻的平淡，"
        "恰是让你休整、蓄力的时机。守住本心，静待时机。",
        "事情会成，但需耐心与时日。不必急，也不必慌，"
        "按自己的节奏来，终会水到渠成。",
        "吉中带平，平中藏机。眼前的平常，是为日后的精彩蓄力。"
        "稳住，你并不孤单。",
    },
    {   /* 下 */
        "此签虽下，请不必灰心。签是提醒，不是判决。"
        "低谷之后必有转机，只要你不放弃，柳暗花明终会到来。",
        "暂时的难，是为了让日后的甜更珍贵。此刻的困顿，不是终点，"
        "而是转弯。守住心，熬过去，光就在前面。",
        "签文警示，但绝非定数。命运的手里永远留着一道缝——"
        "那是给你的转机。别怕，慢慢走。",
    },
};

static const char *ACTS[3][3] = {
    { "趁势而为，把眼前的机会落实到具体行动",
      "心怀感恩，对帮助过你的人表达谢意",
      "保持谦逊，好运会眷顾踏实的人" },
    { "耐心耕耘，把眼前的小事做好",
      "提升自己，为转机储备实力",
      "保持平常心，不为一时得失焦虑" },
    { "先守住现有，不宜冒进求变",
      "寻求身边人的支持，别独自硬扛",
      "调整心态，把注意力放回当下能控制的事" },
};

/* 心经（默念页）。★ 320×480 放不下全篇，取最要紧的七句 ——
 *   少了不是偷懒：这几句正是"照见五蕴皆空""心无挂碍"的骨干。*/
static const char *SUTRA =
    "观自在菩萨，行深般若波罗蜜多时，照见五蕴皆空，度一切苦厄。\n"
    "舍利子，色不异空，空不异色；色即是空，空即是色。\n"
    "受想行识，亦复如是。\n"
    "舍利子，是诸法空相：不生不灭，不垢不净，不增不减。\n"
    "是故空中无色，无受想行识；无眼耳鼻舌身意。\n"
    "无苦集灭道，无智亦无得。以无所得故，菩提萨埵，\n"
    "依般若波罗蜜多故，心无挂碍；无挂碍故，无有恐怖。\n"
    "即说咒曰：揭谛揭谛，波罗揭谛，波罗僧揭谛，菩提萨婆诃。";

/* ============================================================
 *  小工具
 * ============================================================ */
static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h,
                     lv_color_t color, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* ---------- 宣纸底：加"温度"（兰兰「背景再好看点」）----------
 * ★ 为什么不用平涂 C_PAPER：一整块同色在320×480 上是"死"的，
 *   像一张没印过的纸。这也是拾声换成宣纸调后最容易显得寡淡的地方。
 * ★ 为什么不用 lv_grad：渐变要LVGL 的 draw 缓冲参与，开销与画质
 *   都不划算（而这个项目刚被 FULL 渲染模式的坑教育过）。
 *   ⇒ 6 条横向色带手工插值，从顶部暖金晕渐变到纸色。带高 80px，
 *     相邻两带差别仅 3~5 个色阶 ⇒ 视觉上连成渐变，实测看不出台阶。
 * ★ 每页建一次、盖在页面最底层（必须是第一个建的对象）。*/
static void paper_bg(lv_obj_t *parent)
{
    static const uint32_t TOP[6] = {
        0xF6EAD2, 0xF4E7D2, 0xF2E4D2, 0xF1E2D3, 0xF0E1D4, 0xEFE0D5,
    };
    for (int i = 0; i < 6; i++) {
        lv_obj_t *b = box(parent, 0, i * 80, SCR_W, 80,
                          lv_color_hex(TOP[i]), 0);
        lv_obj_move_background(b);       /* 压到最底，不挡正文 */
    }
    /* 底部再补一条纯纸色，把最后 80px 收平（480 - 6*80 = 0，正好六条铺满）*/
}

/* 带边框的块。★ 所有可点元素都用它建 —— 上一版"轻点此处点不动"
 *   的根因就是拿【透明大热区 + 一句提示文字】当按钮：
 *   文字 label 与热区是兄弟关系，点在文字上事件不会到热区；
 *   而且 66×28 的小块在 320 宽的屏上只有 ~11×5mm，手指根本压不准。
 *   这一版规矩：可点的一律 ≥ 160×48，看得见边界，自己绑事件。*/
static lv_obj_t *framed(lv_obj_t *parent, int x, int y, int w, int h,
                        lv_color_t fill, int radius, int bw, lv_color_t bc)
{
    lv_obj_t *o = box(parent, x, y, w, h, fill, radius);
    lv_obj_set_style_border_width(o, bw, 0);
    lv_obj_set_style_border_color(o, bc, 0);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, int x, int y, const lv_font_t *font,
                       lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, x, y);
    return l;
}

/* 居中文字：给满宽 + text_align=CENTER */
static lv_obj_t *clabel(lv_obj_t *parent, int y, int w, int x0,
                        const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = label(parent, x0, y, font, color, text);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

/* 「长江水」→「长\n江\n水」。
 * ★ 必须显式换行：中文没有词边界，靠 LVGL 自动换行会按容器宽度随便断，
 *   七言签诗被切成两截就不成句了。*/
static void vtext(const char *src, char *out, size_t cap)
{
    size_t o = 0;
    if (!src) { out[0] = 0; return; }
    for (const char *p = src; *p; ) {
        unsigned char c = (unsigned char)*p;
        int n = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0 ? 2 :
                                 ((c & 0xF0) == 0xE0 ? 3 : 4));
        if (o) { if (o + 1 >= cap) break; out[o++] = '\n'; }
        for (int i = 0; i < n && p[i]; i++) {
            if (o + 1 >= cap) break;
            out[o++] = p[i];
        }
        p += n;
    }
    out[o] = 0;
}

/* 中文按字数硬折行 —— 不给 LVGL 自动断行的机会（见 vtext 注释） */
static void wrap_cjk(const char *src, char *out, size_t cap, int per_line)
{
    size_t o = 0;
    int col = 0;
    if (!src) { out[0] = 0; return; }
    for (const char *p = src; *p; ) {
        unsigned char c = (unsigned char)*p;
        int n = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0 ? 2 :
                                 ((c & 0xF0) == 0xE0 ? 3 : 4));
        if (c == '\n') {                       /* 源里已有的换行照原样保留 */
            if (o + 1 < cap) out[o++] = '\n';
            col = 0; p += 1; continue;
        }
        if (col >= per_line) {
            if (o + 1 >= cap) break;
            out[o++] = '\n'; col = 0;
        }
        for (int i = 0; i < n && p[i]; i++) {
            if (o + 1 >= cap) break;
            out[o++] = p[i];
        }
        col++; p += n;
    }
    out[o] = 0;
}

/* 宫位名 → 五行色 */
static uint32_t gong_accent(const char *gong)
{
    if (!gong || !gong[0]) return 0xB8914F;
    for (int i = 0; i < 12; i++) {
        if ((unsigned char)gong[0] == (unsigned char)GONG_CH[i][0] &&
            (unsigned char)gong[1] == (unsigned char)GONG_CH[i][1] &&
            (unsigned char)gong[2] == (unsigned char)GONG_CH[i][2]) {
            return GONG_HEX[i];
        }
    }
    return 0xB8914F;
}

/* 吉凶档位 → 0 上 / 1 中 / 2 下（数据侧只有这三个字）*/
static int level_idx(const char *lv)
{
    if (!lv || !lv[0]) return 1;
    if ((unsigned char)lv[0] == 0xE4 && (unsigned char)lv[1] == 0xB8 &&
        (unsigned char)lv[2] == 0x8A) return 0;              /* 上 U+4E0A */
    if ((unsigned char)lv[0] == 0xE4 && (unsigned char)lv[1] == 0xB8 &&
        (unsigned char)lv[2] == 0x8B) return 1;              /* 中 U+4E2D */
    return 2;                                                /* 下 */
}

/* 吉凶 → 字色（上=朱砂提亮，中=金棕，下=青灰）*/
static lv_color_t level_color(const char *lv)
{
    switch (level_idx(lv)) {
    case 0:  return lv_color_hex(0xB83A2A);   /* 上：朱砂 */
    case 2:  return lv_color_hex(0x5A6B78);   /* 下：青灰 */
    default: return lv_color_hex(0x9A7B3A);   /* 中：暗金 */
    }
}

/* ============================================================
 *  语音（签文朗读）＋ 背景音（六字大明咒）
 * ============================================================ */
static void voice_update_btn(void)
{
    if (!s_play_lbl) return;
    if (!s_voice_ok) { lv_label_set_text(s_play_lbl, "朗 读 签 文 · 无语音"); return; }
    lv_label_set_text(s_play_lbl, app_radio_is_playing() ? "停 止 朗 读" : "朗 读 签 文");
}

static void voice_play(int idx)
{
    if (app_radio_is_playing()) app_radio_stop();      /* 先让位 */
    snprintf(s_voice_path, sizeof(s_voice_path), LQ_VOICE_FMT, idx + 1);
    esp_err_t r = app_radio_play_file(s_voice_path);
    s_voice_ok = (r == ESP_OK);
    if (!s_voice_ok) {
        ESP_LOGW(TAG, "语音：打不开 %s（%s）", s_voice_path, esp_err_to_name(r));
    }
    voice_update_btn();
}

/* ★★ 「本次出声」旁路状态（机制说明见 app_settings.h）。
 * ★ 声明位置必须在 init/leave 之前 —— 那两处要用它做收尾。*/
static bool        s_boosted;
static lv_timer_t *s_boost_watch;
static void boost_watch_cb(lv_timer_t *t);

static void voice_toggle_cb(lv_event_t *e)
{
    (void)e;
    if (!s_voice_ok) return;
    if (app_radio_is_playing()) {
        app_radio_stop();
        if (s_boosted) { s_boosted = false; xs_cfg_audio_restore(); }
    } else {
        /* ★ 兰兰要求「默认声音关闭」，但【手点朗读必须听得见】——
         *   否则按钮看着像坏的。boost 只把这一次的 DAC 增益设到设定音量，
         *   不动 mute 标志；播完由 boost_watch_cb 自动还回去。*/
        xs_cfg_audio_boost();
        s_boosted = true;
        voice_play(s_pick);
    }
    voice_update_btn();
}

/* 500ms 一跳：朗读播完了就把增益还回静音（为什么需要它见 init 里的说明）*/
static void boost_watch_cb(lv_timer_t *t)
{
    (void)t;
    if (s_boosted && !app_radio_is_playing()) {
        s_boosted = false;
        xs_cfg_audio_restore();
        voice_update_btn();
        ESP_LOGI(TAG, "朗读结束，声音回到%s", xs_cfg_muted() ? "静音" : "设定音量");
    }
}

/* ============================================================
 *  设置面板（v2.2：音量 / 亮度 / 静音 / 朗读 全部收进来）
 * ============================================================
 * ★ 为什么做成"覆盖式面板"而不是每页摆控件：
 *   上一版每页右上角一个静音块、签诗页再来一个朗读块、解签页底栏再来一个朗读块
 *   —— 兰兰的原话是「屏幕按键就少许多」。浮在内容上的小块不只是多，
 *   它还会压住内容（解签页那个静音块正好压住「第 N 签」标题）。
 *   ⇒ 页面只留【一个】"设 置"入口，四项设置在同一个面板里，位置永远一样。
 *
 * ★ 面板挂在 s_scr（不是 s_page）：所以它盖在当前页之上，
 *   page_clear() 只删 s_page，删不到它 —— 所以 s_set_panel 必须显式登记并在
 *   page_clear 里一起删，否则会出现"页面换了、旧面板还浮在上面"。
 *
 * ★ 面板上的音量/亮度是【即时】的：加减一下立刻生效，能听见/看见，
 *   这是这类设置唯一有用的交互（要跳到别处再回来才知道有没有生效 == 没做）。
 *
 * ★ 行几何：所有行都用同一套 x 算式（铁律：绝不各自算 x，各自算必撞）
 *   算式：名称 72 → 间隔 13 → －48 → 间隔 13 → 数值 48 → 间隔 13 → ＋48
 *   合计 72+13+48+13+48+13+48 = 255 ≤ SET_CW(256)，右缘正好 287（留 33）。
 *   ★ 数值列【居中】在 48px 里，不是左对齐 —— "100"和"20"左对齐时
 *     数字右端参差，看着像没对齐。*/
#define SET_CX      32              /* 内容左缘 */
#define SET_CW      256             /* 内容宽 */
#define SET_NAME_W  72              /* 行首名称宽 */
#define SET_BTN_W   48              /* 加减键宽 */
#define SET_GAP     13              /* 名称/键/数值 之间的间隔 */
#define SET_ROW_H   44
#define SET_ROW_Y0  78
#define SET_ROW_DY  56
#define SET_MX      (SET_CX + SET_NAME_W + SET_GAP)          /* 减号 x = 117 */
#define SET_VX      (SET_MX + SET_BTN_W + SET_GAP)           /* 数值 x = 178 */
#define SET_PX      (SET_VX + SET_BTN_W + SET_GAP)           /* 加号 x = 239 */
#define SET_TOG_Y   (SET_ROW_Y0 + 3 * SET_ROW_DY)   /* 两行开关 y = 246 */

/* 封面背景音：播完自动接上。
 * ★ 只认 app_radio_is_playing() —— 念完/被打断/出错停下，三种情况
 *   我们自己都收不到通知，与其自己记状态，不如每次问它。
 * ★ 失败 3 次就放弃并记日志：否则卡上没有 ommani.mp3 时，
 *   这条循环会以 20ms 的节奏疯狂重试，把串口刷满还拖慢 UI。*/
static void bgm_tick(void)
{
    if (s_state != ST_COVER) return;
    if (s_bgm_fail >= 3) return;
    if (app_radio_is_playing()) return;
    esp_err_t r = app_radio_play_file(LQ_BGM_PATH);
    if (r != ESP_OK) {
        s_bgm_fail++;
        ESP_LOGW(TAG, "背景音打不开 %s（%s）第 %d 次",
                 LQ_BGM_PATH, esp_err_to_name(r), s_bgm_fail);
    } else {
        s_bgm_fail = 0;
    }
}

static void bgm_stop(void)
{
    if (app_radio_is_playing()) app_radio_stop();
}

/* ============================================================
 *  设置面板：实现（v2.2）
 * ============================================================
 * 面板层级：s_set_panel（遮罩+ 卡）直接挂在 s_scr 上，压在当前页之上。
 *   关闭 = lv_obj_del(s_set_panel)，它下面的页一动不动。
 *
 * ★ 为什么不用 lv_obj_add_flag(遮罩, GESTURE_BUBBLE) 那一套遮罩惯例：
 *   那是给"点外面关闭"用的。这里点外面【故意不关】——
 *   音量正在调的时候，手一抖碰到外面就把面板关了，等于白调。
 *   只认右上角那个 ✕ 和页面自己的按钮。
 */
static void set_close(lv_event_t *e)
{
    (void)e;
    if (s_set_panel) { lv_obj_del(s_set_panel); s_set_panel = NULL; }
    s_set_open = false;
    s_set_btn = NULL; s_set_btn_lbl = NULL;
    s_vol_lbl = s_bl_lbl = NULL;
    s_snd_btn = s_snd_lbl = NULL;
    s_play_btn = s_play_lbl = NULL;
}

static void set_open(lv_event_t *e)
{
    (void)e;
    if (s_set_panel) { lv_obj_del(s_set_panel); s_set_panel = NULL; }
    s_set_open = true;
    set_panel_build();
    set_panel_refresh();
}

/* 「主页」= 退出灵签、回多产品主菜单（10-08 新增，配合 ui_shell）
 * ★ 为什么放设置面板【左上角】、与右上角 ✕ 对称：
 *   ① 位置固定，用户不用每页去找；
 *   ② 它是"面板层面的操作"，不混在音量/亮度那几行"机器设定"里，
 *      免得调音量时手滑退出 —— 退出是重操作，要离常用键远一点。
 * ★ 为什么做成"两步"（每页的 设 置 → 主页）而不是常驻一个退出键：
 *   320×480 上版面已经很紧，每页再挂一个常驻键会跟内容抢地方，
 *   而灵签本来每页右上角就有一个「设 置」，顺路。*/
static void go_home_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "设置面板：返回主页");
    ui_shell_back();   /* 内部顺序：切回主菜单屏 → 再调 ui_lingqian_leave() */
}

/* ---------- 音量 / 亮度：加减即生效（写的是全局设置）---------- */
static void vol_delta(int d)
{
    xs_cfg_set_volume(xs_cfg_volume() + d);   /* 0~100 的夹紧在 app_settings 里做 */
    set_panel_refresh();
    ESP_LOGI(TAG, "音量 = %d%s", xs_cfg_volume(), xs_cfg_muted() ? "（当前静音）" : "");
}

static void bl_delta(int d)
{
    xs_cfg_set_backlight(xs_cfg_backlight() + d); /* 下限 20 在 app_settings 里做 */
    set_panel_refresh();
    ESP_LOGI(TAG, "背光 = %d", xs_cfg_backlight());
}

static void vol_cb(lv_event_t *e)
{
    vol_delta((int)(intptr_t)lv_event_get_user_data(e));
}
static void bl_cb(lv_event_t *e)
{
    bl_delta((int)(intptr_t)lv_event_get_user_data(e));
}

/* 声音开关：静音只是音量归零，不碰播放器（理由见上方注释）*/
static void snd_cb(lv_event_t *e)
{
    (void)e;
    xs_cfg_toggle_mute();
    set_panel_refresh();
    ESP_LOGI(TAG, "声音：%s（音量设定 %d）", xs_cfg_muted() ? "已关" : "已开", xs_cfg_volume());
}

/* 朗读开关：只有签诗页/ 解签页有签可读 */
static void play_cb(lv_event_t *e)
{
    (void)e;
    voice_toggle_cb(e);
    set_panel_refresh();
}

void set_panel_refresh(void)
{
    char b[32];
    if (s_vol_lbl) {
        snprintf(b, sizeof(b), "%d", xs_cfg_volume());
        lv_label_set_text(s_vol_lbl, b);
    }
    if (s_bl_lbl) {
        snprintf(b, sizeof(b), "%d", xs_cfg_backlight());
        lv_label_set_text(s_bl_lbl, b);
    }
    if (s_snd_lbl) {
        lv_label_set_text(s_snd_lbl, xs_cfg_muted() ? "已静音" : "有声");
        lv_obj_set_style_text_color(s_snd_lbl, xs_cfg_muted() ? C_MUTE : C_CINNA, 0);
    }
    if (s_play_lbl) {
        if (!s_voice_ok)      lv_label_set_text(s_play_lbl, "无语音文件");
        else if (app_radio_is_playing()) lv_label_set_text(s_play_lbl, "停 止 朗 读");
        else                  lv_label_set_text(s_play_lbl, "朗 读 签 文");
        lv_obj_set_style_text_color(s_play_lbl, s_voice_ok ? C_INK : C_MUTE, 0);
    }
}

/* 一行「名称 －数值 ＋」：三行的 x 完全由宏算出，不各写各的
 *   （v2.1 底栏"差一点点压住"就是各自算 x 算出来的）。*/
static void set_step_row(lv_obj_t *par, int y, const char *name,
                         lv_obj_t **val_lbl, lv_event_cb_t cb)
{
    /* 名称列也居中：两行名称都是 3 个全角字宽，居中后与右侧的
       －/数值/＋ 一列形成一条干净的竖线，不左不对齐也不右不对齐。*/
    clabel(par, y + 12, SET_NAME_W, SET_CX, F_MID, C_INK, name);

    lv_obj_t *minus = framed(par, SET_MX, y, SET_BTN_W, SET_ROW_H,
                             C_PAPER, 10, 1, C_LINE);
    lv_obj_add_event_cb(minus, cb, LV_EVENT_CLICKED, (void *)(intptr_t)-10);
    clabel(minus, 12, SET_BTN_W, 0, F_MID, C_INK, "－");

    *val_lbl = clabel(par, y + 12, SET_BTN_W, SET_VX, F_MID, C_GOLD, "0");

    lv_obj_t *plus = framed(par, SET_PX, y, SET_BTN_W, SET_ROW_H,
                            C_PAPER, 10, 1, C_LINE);
    lv_obj_add_event_cb(plus, cb, LV_EVENT_CLICKED, (void *)(intptr_t)10);
    clabel(plus, 12, SET_BTN_W, 0, F_MID, C_INK, "＋");
}

static void set_panel_build(void)
{
    /* 半透明墨底：不是纯黑（纯黑在宣纸调里很脏），是暖褐 60% */
    s_set_panel = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_set_panel);
    lv_obj_set_size(s_set_panel, SCR_W, SCR_H);
    lv_obj_set_pos(s_set_panel, 0, 0);
    lv_obj_set_style_bg_color(s_set_panel, lv_color_hex(0x2A2118), 0);
    lv_obj_set_style_bg_opa(s_set_panel, LV_OPA_60, 0);
    lv_obj_clear_flag(s_set_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* 白纸卡片：300 宽，y=28 起、高 424（28~452）
     * ★★ 高度是【算出来的】，不是试出来的：卡内最后一行 © 在 y=388，
     *   18px 行高 ⇒ 底边 406；再加 16px 内边距 ⇒ 卡片至少 424 高。
     *   上一版给 372（底 426），结果 © 与提示那两行被卡片下沿切掉一半
     *   —— 截图上一眼就能看见，教训：卡片的"内容底"必须参与高度计算。*/
    lv_obj_t *card = framed(s_set_panel, 10, 28, SCR_W - 20, 424,
                            C_PAPER2, 10, 2, C_GOLD);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    clabel(card, 16, SCR_W - 20, 0, F_MID, C_INK, "设    置");

    /* 「主页」：左上角，与右侧 ✕ 同一水平线（10-08 加，见 go_home_cb 说明）。
     * 宽 72：两个全角字 18px = 36px，居中还有富余；高 44 与 ✕ 齐平。
     * ★ 用「主 页」而不是「←」：这块屏上没有系统级返回键的概念，
     *   一个裸箭头容易被理解成"返回上一页"（那是解签页底栏「返 回」的意思），
     *   写「主 页」才明确是"回产品主菜单"。*/
    lv_obj_t *hm = framed(card, 12, 10, 72, 44, C_PAPER, 22, 1, C_LINE);
    lv_obj_add_event_cb(hm, go_home_cb, LV_EVENT_CLICKED, NULL);
    clabel(hm, 12, 72, 0, F_MID, C_GOLD, "主 页");

    /* ✕ 关闭：右上角 44×44，够手指按
     * ★ 用「×」不用 emoji✕：emoji 在 14px 下会掉进缺字表变豆腐块，
     *   而 ×(U+00D7) 在霞鹜文楷里有。*/
    lv_obj_t *x = framed(card, SCR_W - 20 - 12 - 44, 10, 44, 44,
                         C_PAPER, 22, 1, C_LINE);
    lv_obj_add_event_cb(x, set_close, LV_EVENT_CLICKED, NULL);
    clabel(x, 12, 44, 0, F_MID, C_INK2, "×");

    box(card, 20, 64, SCR_W - 60, 1, C_LINE, 0);

    set_step_row(card, SET_ROW_Y0,      "音　量", &s_vol_lbl, vol_cb);
    set_step_row(card, SET_ROW_Y0 + SET_ROW_DY, "亮　度", &s_bl_lbl, bl_cb);

    /* 两个整宽开关：声音 / 朗读 */
    s_snd_btn = framed(card, SET_CX, SET_TOG_Y, SET_CW, SET_ROW_H,
                       C_PAPER, 10, 1, C_LINE);
    lv_obj_add_event_cb(s_snd_btn, snd_cb, LV_EVENT_CLICKED, NULL);
    clabel(s_snd_btn, 12, 88, 0, F_MID, C_INK, "声　音");
    s_snd_lbl = clabel(s_snd_btn, 12, SET_CW - 100, 88, F_MID, C_INK, "有声");

    s_play_btn = framed(card, SET_CX, SET_TOG_Y + SET_ROW_DY, SET_CW, SET_ROW_H,
                        C_PAPER, 10, 1, C_LINE);
    lv_obj_add_event_cb(s_play_btn, play_cb, LV_EVENT_CLICKED, NULL);
    clabel(s_play_btn, 12, 88, 0, F_MID, C_INK, "签　文");
    s_play_lbl = clabel(s_play_btn, 12, SET_CW - 100, 88, F_MID, C_INK, "朗 读");

    /* 底部两行：提示 + 署名。
     * ★ y 必须 > SET_TOG_Y + SET_ROW_DY + SET_ROW_H = 302+44 = 346，
     *   放 360 ⇒ 离开关行留 14px，不会"文字贴着按钮"（同解签页那条教训）。
     *   署名放 388，行高 18 ⇒ 底边 406，卡片底 452 ⇒ 余 46px 内边距。*/
    clabel(card, 360, SCR_W - 20, 0, F_SMALL, C_MUTE,
           "背景经文与签文朗读共用此音量");
    clabel(card, 388, SCR_W - 20, 0, F_SMALL, C_MUTE,
           "© 永远的兰兰");
}

/* 页面右上角那个唯一的「设 置」入口。所有页都调它，位置恒定。*/
void set_entry_make(lv_obj_t *par, int y)
{
    s_set_btn = framed(par, SCR_W - 12 - 72, y, 72, 36, C_PAPER2, 8, 1, C_GOLD);
    lv_obj_set_style_bg_opa(s_set_btn, LV_OPA_80, 0);
    lv_obj_add_event_cb(s_set_btn, set_open, LV_EVENT_CLICKED, NULL);
    s_set_btn_lbl = clabel(s_set_btn, 9, 72, 0, F_SMALL, C_GOLD, "设 置");
}

/* ============================================================
 *  页面一：封面（海报全屏）
 * ============================================================ */
static void to_sutra(lv_event_t *e);

static void build_cover(void)
{
    s_page = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_page);
    lv_obj_set_size(s_page, SCR_W, SCR_H);
    lv_obj_set_pos(s_page, 0, 0);
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 海报：整屏铺满（HTML 版的开屏就是这张图）---- */
    lv_obj_t *img = lv_image_create(s_page);
    lv_image_set_src(img, &cover_img);
    lv_obj_set_pos(img, 0, 0);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);

    /* ---- 底部主按钮 ----
     * ★ 尺寸 208×54：上一版 66×28 的块就是兰兰说的"太小、点上去无效"。
     *
     * ★★★ 10-08 晚：整块【下移 22px、收窄到 196×44】—— 兰兰原话
     *     「静心求签按键把灵签首页的签筒挡住了，你把求签按键重新设计
     *       或者移动一下」。
     *     原因量化（对 cover_320x480.png 逐行测的，不是估的）：
     *       签筒的暗色主体（含筒身、散签）占 x≈60~135、y≈290~400；
     *       y=401 起整行"暗像素 = 0"（全是底部米白光雾，灰阶恒为 239）。
     *     ⇒ 旧按钮 y=380~434 正好压住签筒最下面那 20px。
     *     ⇒ 新按钮顶边从 380 挪到 402，落在纯雾区，与签筒留 2px 净空；
     *       高度 54→44 是【必须的】：下方还要塞下两行 14px 文案
     *       （旧版按钮底边 434，往下只剩 46px，两行放不下）。
     *       最终占用：按钮 402~446、标语 450~464、署名 464~478，
     *       底边留 2px —— 480 高的屏刚好放下，不要再往上加行。*/
    lv_obj_t *btn = framed(s_page, (SCR_W - 196) / 2, 402, 196, 44,
                           C_PAPER2, 22, 2, C_GOLD);
    lv_obj_set_style_bg_opa(btn, LV_OPA_90, 0);
    lv_obj_add_event_cb(btn, to_sutra, LV_EVENT_CLICKED, NULL);
    clabel(btn, 13, 196, 0, F_MID, C_INK, "静 心 求 签");

    clabel(s_page, 450, SCR_W, 0, F_SMALL, C_INK2, "一签一问 · 诚心则灵");

    /* ---- 作者署名（兰兰「首页作者署名还是需要的」）----
     * ★ 放最底（y=464~478），14px 淡墨 —— 海报主体在中上部，
     *   这一行落在光雾区，既不挡莲花也不撞按钮（按钮 402~446）。*/
    clabel(s_page, 464, SCR_W, 0, F_SMALL, C_INK2, "© 永远的兰兰");

    /* ---- 整屏可点：手指点不准，别让他找 ----
     * ★ 它必须【最后建】才能盖在最上层；但按钮已经在了，
     *   点按钮和点屏幕结果一样（都是进心经页），所以不冲突。
     * ★ 高度 398：刚好顶到按钮上沿（402）之前 —— 多 1px 就会吃掉
     *   按钮顶边的点击（透明热区盖在上面那类坑）。*/
    lv_obj_t *hot = lv_obj_create(s_page);
    lv_obj_remove_style_all(hot);
    lv_obj_set_size(hot, SCR_W, 398);          /* 只盖海报区，不挡按钮 */
    lv_obj_set_pos(hot, 0, 0);
    lv_obj_set_style_bg_opa(hot, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(hot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(hot, to_sutra, LV_EVENT_CLICKED, NULL);
    lv_obj_move_foreground(hot);

    /* ---- 唯一的「设 置」入口：右上角 72×36 ----
     * ★ v2.2 把原来这里的静音块换成设置面板入口，位置不变（右上 y=8），
     *   兰兰上一版形成的肌肉记忆继续有效。
     * ★ 它【必须建在 hot 之后】才能压住整屏热区 —— 否则热区盖在上面，
     *   点"设 置"会变成"进心经页"（这正是"点上去无效"那类坑的成因：
     *   控件建了、看得见，却被一块透明热区吃掉点击）。*/
    set_entry_make(s_page, 8);
}

/* ============================================================
 *  页面二：默念心经
 * ============================================================ */
static void to_wish(lv_event_t *e);

static void build_sutra(void)
{
    s_page = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_page);
    lv_obj_set_size(s_page, SCR_W, SCR_H);
    lv_obj_set_pos(s_page, 0, 0);
    lv_obj_set_style_bg_color(s_page, C_PAPER, 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);

    paper_bg(s_page);                   /* 宣纸渐层背景（兰兰「背景再好看点」）*/

    /* 题头 */
    clabel(s_page, 18, SCR_W, 0, F_SMALL, C_MUTE, "南 无 大 慈 大 悲 观 世 音 菩 萨");

    /* 六字大明咒 —— 这一行是整页的"眼"，用 30px 亮金 */
    clabel(s_page, 44, SCR_W, 0, F_HERO, C_GOLD2, "唵 嘛 呢 叭 咪 吽");

    box(s_page, 90, 92, 140, 1, C_LINE, 0);
    clabel(s_page, 102, SCR_W, 0, F_SMALL, C_CINNA, "心   经");

    /* 心经正文：14px + 行距 10，8 行 */
    s_sutra_txt = clabel(s_page, 132, SCR_W - 24, 12, F_SMALL, C_INK2, SUTRA);
    lv_obj_set_style_text_line_space(s_sutra_txt, 9, 0);
    lv_obj_set_style_bg_opa(s_sutra_txt, LV_OPA_TRANSP, 0);

    /* 「天机具足」按钮：先建好但【压暗且不响应】，2.5s 后才亮。
     * ★ 这是 HTML 版"静候灵机"那 2 秒的硬件化 —— 仪式感不是装饰，
     *   它让"求签"变成一件需要等一等的事。*/
    s_sutra_btn = framed(s_page, (SCR_W - 208) / 2, 386, 208, 54,
                         C_PAPER2, 27, 1, C_LINE);
    s_sutra_btn_lbl = clabel(s_sutra_btn, 14, 208, 0, F_MID, C_MUTE, "静 心 默 念…");
    s_sutra_ok = 0;

    set_entry_make(s_page, 8);
}

/* ============================================================
 *  页面三：选择所求之事
 * ============================================================ */
static void to_draw(lv_event_t *e);

static void wish_refresh(void)
{
    for (int i = 0; i < N_WISH; i++) {
        if (!s_chip[i]) continue;
        bool on = (i == s_wish);
        lv_obj_set_style_bg_color(s_chip[i], on ? C_GOLD2 : C_PAPER2, 0);
        lv_obj_set_style_border_color(s_chip[i], on ? C_CINNA : C_LINE, 0);
        lv_obj_set_style_border_width(s_chip[i], on ? 2 : 1, 0);
        lv_obj_t *l = lv_obj_get_child(s_chip[i], 0);
        if (l) lv_obj_set_style_text_color(l, on ? C_INK : C_INK2, 0);
    }
    if (s_wish_sel) {
        lv_label_set_text(s_wish_sel, s_wish < 0 ? "尚未选择" : WISHES[s_wish]);
        lv_obj_set_style_text_color(s_wish_sel,
            s_wish < 0 ? C_MUTE : C_CINNA, 0);
    }
    if (s_wish_btn_lbl) {
        lv_obj_set_style_text_color(s_wish_btn_lbl, s_wish < 0 ? C_MUTE : C_INK, 0);
    }
    if (s_wish_btn) {
        lv_obj_set_style_border_color(s_wish_btn, s_wish < 0 ? C_LINE : C_GOLD, 0);
    }
}

static void chip_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= N_WISH) return;
    s_wish = (s_wish == i) ? -1 : i;       /* 再点一次取消 */
    wish_refresh();
}

static void build_wish(void)
{
    s_page = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_page);
    lv_obj_set_size(s_page, SCR_W, SCR_H);
    lv_obj_set_pos(s_page, 0, 0);
    lv_obj_set_style_bg_color(s_page, C_PAPER, 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);

    paper_bg(s_page);

    clabel(s_page, 16, SCR_W, 0, F_MID, C_INK, "所 求 何 事");
    clabel(s_page, 46, SCR_W, 0, F_SMALL, C_MUTE, "点击选择 · 一签一问");
    box(s_page, 90, 74, 140, 1, C_LINE, 0);

    /* 20 个 chip：4 列 × 5 行
     * 72×40 + 横间隔 6 ⇒ 4*72+3*6 = 306，左起 7
     * 纵间隔 8 ⇒ 5*40+4*8 = 232，从 y=88 到 320 */
    for (int i = 0; i < N_WISH; i++) {
        int r = i / 4, c = i % 4;
        int x = 7 + c * 78;
        int y = 88 + r * 48;
        s_chip[i] = framed(s_page, x, y, 72, 40, C_PAPER2, 8, 1, C_LINE);
        lv_obj_add_event_cb(s_chip[i], chip_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        clabel(s_chip[i], 9, 72, 0, F_SMALL, C_INK2, WISHES[i]);
    }

    s_wish_sel = clabel(s_page, 334, SCR_W, 0, F_SMALL, C_MUTE, "尚未选择");

    s_wish_btn = framed(s_page, (SCR_W - 208) / 2, 364, 208, 54,
                        C_PAPER2, 27, 1, C_LINE);
    lv_obj_add_event_cb(s_wish_btn, to_draw, LV_EVENT_CLICKED, NULL);
    s_wish_btn_lbl = clabel(s_wish_btn, 14, 208, 0, F_MID, C_MUTE, "求   签");

    clabel(s_page, 430, SCR_W, 0, F_SMALL, C_MUTE, "诚心求签 · 仅供文化参考");

    set_entry_make(s_page, 8);

    s_wish = -1;
    wish_refresh();
}

/* ============================================================
 *  页面四：静候灵机（莲花光晕呼吸）
 * ============================================================ */
static void reveal(lv_event_t *e);

static void build_draw(void)
{
    s_page = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_page);
    lv_obj_set_size(s_page, SCR_W, SCR_H);
    lv_obj_set_pos(s_page, 0, 0);
    lv_obj_set_style_bg_color(s_page, C_PAPER, 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);

    paper_bg(s_page);
    set_entry_make(s_page, 8);

    /* 莲花光晕：四层同心圆，从外到内越来越淡、越来越大。
     * ★ 不画具象莲花（兰兰说"不需要很具象的"）—— 只留光。
     *   动画在 anim_cb 里做呼吸，这里只负责建。*/
    static const int  r0[4] = { 150, 108, 70, 34 };
    static const uint32_t c0[4] = { 0xF6EFE2, 0xF3E6CE, 0xEED9B4, 0xE8CB96 };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *h = box(s_page, 0, 0, r0[i] * 2, r0[i] * 2,
                          lv_color_hex(c0[i]), r0[i]);
        lv_obj_set_style_bg_opa(h, LV_OPA_40, 0);
        lv_obj_clear_flag(h, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(h, SCR_W / 2 - r0[i], 230 - r0[i]);
        s_halo[i] = h;
    }

    s_draw_lbl = clabel(s_page, 198, SCR_W, 0, F_MID, C_INK, "静 候 灵 机");
    clabel(s_page, 292, SCR_W, 0, F_SMALL, C_MUTE, "默念心愿 · 静心以待");

    /* 揭晓按钮：先藏着，2s 后现身（HTML 版是同一套节奏）*/
    s_draw_btn = framed(s_page, (SCR_W - 208) / 2, 386, 208, 54,
                        C_PAPER2, 27, 2, C_GOLD);
    lv_obj_add_event_cb(s_draw_btn, reveal, LV_EVENT_CLICKED, NULL);
    s_draw_btn_lbl = clabel(s_draw_btn, 14, 208, 0, F_MID, C_INK, "揭 晓 签 文");
    lv_obj_set_style_opa(s_draw_btn, LV_OPA_TRANSP, 0);
}

/* ============================================================
 *  页面五：签文（竖排楷体）
 * ============================================================ */
static void show_read(lv_event_t *e);
static void back_cover(lv_event_t *e);
static void back_poem(lv_event_t *e);
static void again_draw(lv_event_t *e);

static char s_vbuf[LQ_POEM_LINES][48];

static void build_poem(void)
{
    const lq_entry_t *en = lq_get(s_pick);
    char buf[64];

    s_accent = gong_accent(en->gong);
    lv_color_t ac = lv_color_hex(s_accent);

    s_page = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_page);
    lv_obj_set_size(s_page, SCR_W, SCR_H);
    lv_obj_set_pos(s_page, 0, 0);
    lv_obj_set_style_bg_color(s_page, C_PAPER, 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);

    paper_bg(s_page);

    /* ---- 签纸：内缩的金色细框（一张"纸"的感觉）---- */
    lv_obj_t *sheet = framed(s_page, 8, 8, SCR_W - 16, SCR_H - 16,
                             C_PAPER2, 6, 2, C_GOLD);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
    lv_obj_clear_flag(sheet, LV_OBJ_FLAG_CLICKABLE);

    /* ---- 顶部：第 N 签 ---- */
    snprintf(buf, sizeof(buf), "第 %d 签", s_pick + 1);
    clabel(s_page, 22, SCR_W, 0, F_MID, C_INK, buf);

    /* ---- 吉凶 + 宫位：两粒胶囊 ---- */
    snprintf(buf, sizeof(buf), "%s签", en->level ? en->level : "");
    lv_obj_t *lvp = framed(s_page, 74, 56, 80, 32, C_PAPER, 16, 1,
                           level_color(en->level));
    clabel(lvp, 7, 80, 0, F_SMALL, level_color(en->level), buf);

    lv_obj_t *gp = framed(s_page, 166, 56, 80, 32, C_PAPER, 16, 1, ac);
    clabel(gp, 7, 80, 0, F_SMALL, ac, en->gong ? en->gong : "");

    box(s_page, 48, 100, SCR_W - 96, 1, ac, 0);

    /* ---- 竖排签诗：4 句 × 7 字，30px 楷体，从右往左 ----
     * ★ 行距必须自己压：lv_font_conv 的字形 line_height 比 30 大，
     *   不压的话 7 字会排到 300px 开外，直接盖住底部按钮。*/
    int lh = lv_font_get_line_height(F_HERO);
    for (int i = 0; i < LQ_POEM_LINES; i++) {
        vtext(en->poem[i] ? en->poem[i] : "", s_vbuf[i], sizeof(s_vbuf[i]));
        int x = 61 + (LQ_POEM_LINES - 1 - i) * POEM_STEP;
        s_poem_lbl[i] = label(s_page, x, POEM_TOP, F_HERO, C_INK, s_vbuf[i]);
        lv_obj_set_style_text_line_space(s_poem_lbl[i], 30 - lh, 0);
        lv_obj_set_style_text_letter_space(s_poem_lbl[i], 0, 0);
    }

    /* ---- 左上：换一签（88×36，够点）---- */
    lv_obj_t *ag = framed(s_page, 14, 24, 88, 36, C_PAPER, 10, 1, C_LINE);
    lv_obj_add_event_cb(ag, again_draw, LV_EVENT_CLICKED, NULL);
    clabel(ag, 9, 88, 0, F_SMALL, C_INK2, "换一签");

    /* ---- 右上：唯一的「设 置」入口（朗读与静音都在里面）---- */
    set_entry_make(s_page, 24);

    /* ---- 底部：揭晓解签（208×54）---- */
    lv_obj_t *rb = framed(s_page, (SCR_W - 208) / 2, 400, 208, 54,
                          C_PAPER2, 27, 2, C_GOLD);
    lv_obj_add_event_cb(rb, show_read, LV_EVENT_CLICKED, NULL);
    clabel(rb, 14, 208, 0, F_MID, C_INK, "看 解 签");

    /*进页自动朗读：这是"灵签机"的核心体验，不能等用户去点设置里的朗读。
     * ★ 声音开关/音量在设置里改的是播放增益，不会打断这一条。*/
    voice_play(s_pick);
}

/* ============================================================
 *  页面六：解签（五层，可滚动）
 * ============================================================ */
static char s_wbuf[1536];
/* ★ 拼「1. xxx」用的目的缓冲。必须和 s_wbuf 一样大 ——
 *   用 128 字节的 buf 装 1536 字节的 s_wbuf，-Werror=format-truncation
 *   会直接拦成编译错误（这个坑 10-07 踩过一次，不是警告是 error）。*/
static char s_abuf[1600];

static void read_section(lv_obj_t *parent, int y, const char *title,
                         lv_color_t tc, const char *body, int *next_y)
{
    box(parent, 8, y + 5, 4, 17, tc, 2);
    label(parent, 20, y, F_MID, tc, title);

    if (!body || !body[0]) { *next_y = y + 36; return; }
    /* ★ 正文 22px：每行 13 字（原 15 是给 14px 算的，字放大后必须同步改，
     *   否则按 15 字折行 ⇒ 22px 下每行实占 15×22=330px > 296px 可用宽，
     *   LVGL 会再断一次 ⇒ 行尾出现半截字。*/
    wrap_cjk(body, s_wbuf, sizeof(s_wbuf), 13);
    lv_obj_t *b = label(parent, 12, y + 30, F_READ, C_INK, s_wbuf);
    lv_obj_set_style_text_line_space(b, 10, 0);
    lv_obj_set_width(b, SCR_W - 24);
    lv_obj_update_layout(b);
    *next_y = y + 30 + lv_obj_get_height(b) + 22;
}

/* 按吉凶档取「安心寄语」「行动建议」——每次抽签随机换一条，
 * 让同一支签重抽也不至于一字不差。*/
static int peace_pick(void)
{
    return (int)(esp_random() % 3u);
}

static void build_read(void)
{
    const lq_entry_t *en = lq_get(s_pick);
    int li = level_idx(en->level);
    char buf[128];

    s_page = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_page);
    lv_obj_set_size(s_page, SCR_W, SCR_H);
    lv_obj_set_pos(s_page, 0, 0);
    lv_obj_set_style_bg_color(s_page, C_PAPER, 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);

    paper_bg(s_page);

    snprintf(buf, sizeof(buf), "第 %d 签 · %s签 · %s", s_pick + 1,
             en->level ? en->level : "", en->gong ? en->gong : "");
    clabel(s_page, 12, SCR_W, 0, F_SMALL, C_GOLD, buf);
    box(s_page, 48, 40, SCR_W - 96, 1, lv_color_hex(s_accent), 0);

    /* 正文容器：可滚（长文靠 LVGL 滚动，不写分页）
     ★ 高度 358：从 y=50 起到 408，底栏在 418 开始 ⇒ 留 10px 空隙，
       不会"文字贴着按钮"。*/
    s_read_body = lv_obj_create(s_page);
    lv_obj_remove_style_all(s_read_body);
    lv_obj_set_size(s_read_body, SCR_W, 358);
    lv_obj_set_pos(s_read_body, 0, 50);
    lv_obj_set_style_bg_opa(s_read_body, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_read_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_read_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_read_body, LV_SCROLLBAR_MODE_AUTO);

    int y = 4;
    /* ① 签诗（原文）—— 横排四句，用宫位色，是"照录"不是"解释" */
    snprintf(buf, sizeof(buf), "签 诗");
    {
        box(s_read_body, 8, y + 5, 4, 17, lv_color_hex(0x5A78A8), 2);
        label(s_read_body, 20, y, F_MID, lv_color_hex(0x5A78A8), buf);
        y += 30;
        for (int i = 0; i < LQ_POEM_LINES; i++) {
            lv_obj_t *p = label(s_read_body, 12, y, F_MID, C_INK,
                                en->poem[i] ? en->poem[i] : "");
            lv_obj_set_width(p, SCR_W - 24);
            lv_obj_set_style_text_align(p, LV_TEXT_ALIGN_CENTER, 0);
            y += 28;
        }
        y += 20;
    }
    /* ② 解曰 ③ 典故 */
    read_section(s_read_body, y, "解 曰", lv_color_hex(0x9A7B3A), en->jieyi, &y);
    snprintf(buf, sizeof(buf), "典 故 · %s", en->story ? en->story : "");
    read_section(s_read_body, y, buf, lv_color_hex(0xA63A2E), en->gushi, &y);
    /* ④ 安心寄语（HTML 版最动人的一层）*/
    read_section(s_read_body, y, "安 心 寄 语", lv_color_hex(0xB83A2A),
                 PEACE[li][peace_pick()], &y);
    /* ⑤ 行动建议（三条）*/
    {
        box(s_read_body, 8, y + 5, 4, 17, lv_color_hex(0x4F8A6A), 2);
        label(s_read_body, 20, y, F_MID, lv_color_hex(0x4F8A6A), "行 动 建 议");
        y += 30;
        for (int i = 0; i < 3; i++) {
            wrap_cjk(ACTS[li][i], s_wbuf, sizeof(s_wbuf), 13);
            snprintf(s_abuf, sizeof(s_abuf), "%d. %s", i + 1, s_wbuf);
            lv_obj_t *b = label(s_read_body, 12, y, F_READ, C_INK, s_abuf);
            lv_obj_set_width(b, SCR_W - 24);
            lv_obj_set_style_text_line_space(b, 10, 0);
            lv_obj_update_layout(b);
            y += lv_obj_get_height(b) + 12;
        }
    }

    /* ---------- 底部按钮栏：三个等大并排 ----------
     *   排布铁律：按钮绝不能各自算 x，必须走同一套算式。
     *   （10-07 夜踩过：朗读在 x=12 宽 88、再求一签在 x=80 宽 160，横向压了
     *   20px。这类"差一点点"最难自查，因为截图上两块颜色都还在。）
     *   ★ v2.2：底栏三块 = 返回 /设 置 / 再求一签。
     *     朗读与静音【搬进设置面板】，所以底栏腾出一格给"设 置"。
     *   ★★ 为什么"设 置"在解签页放【底栏】而不是和其他页一样放右上角：
     *     右上角 y=6~40 正好压住「第 N 签 · 上签 · 子宫」这行标题
     *     （兰兰 10-07 夜的原话：「解签页最上面的静音按键挡住第几签标题」）。
     *     底栏是这页唯一确定空着的地带，放这儿既不挡标题又不挡滚动正文。
     *   ★ 几何：三块各 100×48，间距 6，x = 3 / 109 / 215，总宽 318；
     *     底栏之上留 10px 空隙 ⇒ y = 480 - 48 - 14 = 418。*/
    {
        const int BW = 100, BH = 48, GAP = 6, BY = 418;

        lv_obj_t *bk = framed(s_page, 3, BY, BW, BH, C_PAPER2, 12, 2, C_GOLD);
        lv_obj_add_event_cb(bk, back_poem, LV_EVENT_CLICKED, NULL);
        clabel(bk, 11, BW, 0, F_MID, C_INK, "返 回");

        s_set_btn = framed(s_page, 3 + BW + GAP, BY, BW, BH,
                           C_PAPER2, 12, 2, C_GOLD);
        lv_obj_add_event_cb(s_set_btn, set_open, LV_EVENT_CLICKED, NULL);
        s_set_btn_lbl = clabel(s_set_btn, 11, BW, 0, F_MID, C_GOLD, "设 置");

        lv_obj_t *ag = framed(s_page, 3 + (BW + GAP) * 2, BY, BW, BH,
                              C_PAPER2, 12, 2, C_GOLD);
        lv_obj_add_event_cb(ag, back_cover, LV_EVENT_CLICKED, NULL);
        clabel(ag, 11, BW, 0, F_MID, C_INK, "再求一签");
    }

    /* ★ 解签页【不放】右上角的设置块：它会压住标题（见上面底栏注释）。
     *   面板里的"朗读签文"在这里仍可用 —— s_voice_ok 是全局状态，
     *   进这一页时签诗页已经把语音文件起播过，标志还在。*/
}

/* ============================================================
 *  页面切换
 * ============================================================ */
/* 只把引用清空，【不碰对象】。
 * ★ 给"整机退出"用：那时所有对象由删屏统一回收，
 *   而在事件回调链里不能同步删对象（理由见 ui_lingqian_leave）。
 * ★ 单列一层的意义：指针置空和对象删除是两件事。
 *   混在一起写，就会出现"想只清引用却顺手把对象删了"这种
 *   在事件回调里必然出事的代码 —— 这正是 leave 不能直接调 page_clear 的原因。*/
static void page_forget(void)
{
    s_set_panel = NULL;
    s_set_open = false;
    s_set_btn = NULL; s_set_btn_lbl = NULL;
    s_vol_lbl = NULL; s_bl_lbl = NULL;
    s_snd_btn = NULL; s_snd_lbl = NULL;
    s_play_btn = NULL; s_play_lbl = NULL;
    s_page = NULL;
    s_sutra_txt = NULL; s_sutra_btn = NULL; s_sutra_btn_lbl = NULL;
    s_wish_sel = NULL; s_wish_btn = NULL; s_wish_btn_lbl = NULL;
    s_draw_lbl = NULL; s_draw_btn = NULL; s_draw_btn_lbl = NULL;
    s_read_body = NULL;
    for (int i = 0; i < N_WISH; i++) s_chip[i] = NULL;
    for (int i = 0; i < 4; i++) s_halo[i] = NULL;
    for (int i = 0; i < LQ_POEM_LINES; i++) s_poem_lbl[i] = NULL;
}

static void page_clear(void)
{
    /* ★★★ 设置面板必须【先】关掉再删页。
     *   面板挂在 s_scr 而页面挂在 s_page 之下 ⇒ 删 s_page 删不到它。
     *   不在这里清，就会出现"页面已经换到下一页，旧设置面板还浮在上面"，
     *   而且它的 ✕ 还能点、点关闭后底下才露出新页 —— 现场极难解释。
     *   顺序也有讲究：先删面板（它盖在上层），再删页，LVGL 才不会在
     *   同一帧里对一个已被子对象引用的父对象做删除。*/
    if (s_set_panel) { lv_obj_del(s_set_panel); }
    if (s_page) { lv_obj_del(s_page); }
    page_forget();
}

static void enter(int st)
{
    page_clear();
    s_state = st;
    s_tick  = 0;
    switch (st) {
    case ST_COVER: build_cover(); s_bgm_fail = 0; break;
    case ST_SUTRA: build_sutra(); break;
    case ST_WISH:  build_wish();  break;
    case ST_DRAW:  build_draw();  break;
    case ST_POEM:  build_poem();  break;
    case ST_READ:  build_read();  break;
    default: break;
    }
}

static void to_sutra(lv_event_t *e)
{
    (void)e;
    if (s_state != ST_COVER) return;
    bgm_stop();                       /* 播放器只有一路，进下一页必须让开 */
    s_voice_ok = false;
    enter(ST_SUTRA);
}

static void to_wish(lv_event_t *e)
{
    (void)e;
    if (s_state != ST_SUTRA || !s_sutra_ok) return;   /* 没到"天机具足"不给过 */
    enter(ST_WISH);
}

static void to_draw(lv_event_t *e)
{
    (void)e;
    if (s_state != ST_WISH) return;
    if (s_wish < 0) {                 /* 没选不给过，但别静默 —— 提示一下 */
        if (s_wish_sel) lv_label_set_text(s_wish_sel, "请先点选一件事");
        ESP_LOGI(TAG, "求签：还没选所求之事");
        return;
    }
    /* 抽一支：均匀随机，并保证连抽不会连着出同一支 */
    int prev = s_pick;
    for (int g = 0; g < 8; g++) {
        s_pick = (int)(esp_random() % (uint32_t)lq_count());
        if (s_pick != prev || lq_count() < 2) break;
    }
    const lq_entry_t *en = lq_get(s_pick);
    ESP_LOGI(TAG, "求签：所求「%s」→ 第 %d 签（%s · %s · %s）",
             WISHES[s_wish], s_pick + 1, en->level, en->gong, en->story);
    enter(ST_DRAW);
}

static void reveal(lv_event_t *e)
{
    (void)e;
    if (s_state != ST_DRAW) return;
    enter(ST_POEM);
}

static void show_read(lv_event_t *e)
{
    (void)e;
    if (s_state != ST_POEM) return;
    enter(ST_READ);
}

static void again_draw(lv_event_t *e)
{
    (void)e;
    /* 「换一签」= 直接重抽（保留已选心愿），不走完整仪式 —— 连抽时更快 */
    if (app_radio_is_playing()) app_radio_stop();
    s_voice_ok = false;
    int prev = s_pick;
    for (int g = 0; g < 8; g++) {
        s_pick = (int)(esp_random() % (uint32_t)lq_count());
        if (s_pick != prev || lq_count() < 2) break;
    }
    const lq_entry_t *en = lq_get(s_pick);
    ESP_LOGI(TAG, "换一签 → 第 %d 签（%s · %s）", s_pick + 1, en->level, en->gong);
    enter(ST_POEM);
}

static void back_cover(lv_event_t *e)
{
    (void)e;
    if (app_radio_is_playing()) app_radio_stop();
    s_voice_ok = false;
    enter(ST_COVER);
}

/* 「返回上一页」（兰兰10-07 夜要的）：从解签页回到签诗页。
 * ★ 要 stop 语音 —— 否则退回去以后上一签还在念，屏幕和声音对不上。
 * ★ 回到签诗页后不自动重念：那正是用户刚"跳过朗读"的意思。*/
static void back_poem(lv_event_t *e)
{
    (void)e;
    if (app_radio_is_playing()) app_radio_stop();
    s_voice_ok = false;
    enter(ST_POEM);
}

/* ============================================================
 *  动画（一条 timer 走完全程）
 * ============================================================ */
static void anim_cb(lv_timer_t *t)
{
    (void)t;
    s_tick++;

    /* 背景音心跳：封面页负责续播 */
    if ((s_tick % 25) == 0) bgm_tick();

    /* 语音按钮文字跟着真实播放状态走（300ms 一次，够跟手）
     * ★ v2.2：朗读按钮现在【只在设置面板里】，而面板是盖在任意页之上的
     *   —— 所以这里不能按 s_state 过滤，否则在封面页打开面板时
     *   "朗 读/停 止" 会永远不刷新。判据改成"面板开着"（s_play_lbl 非空
     *   本身就代表面板在，voice_update_btn 里有NULL 检查）。*/
    if ((s_tick % 15) == 0 && s_play_lbl) {
        voice_update_btn();
    }

    switch (s_state) {

    case ST_COVER:
        break;

    case ST_SUTRA:
        /* 心经淡入：1.2s 从透明到实（60 帧）*/
        if (s_sutra_txt) {
            int op = s_tick * 5;
            lv_obj_set_style_opa(s_sutra_txt,
                (lv_opa_t)(op > 255 ? 255 : op), 0);
        }
        /* 2.5s（125 帧）后「天机具足」——按钮换字、亮边、才开始收点击 */
        if (s_tick == 125 && !s_sutra_ok) {
            s_sutra_ok = 1;
            if (s_sutra_btn_lbl) {
                lv_label_set_text(s_sutra_btn_lbl, "天 机 具 足 · 求 签");
                lv_obj_set_style_text_color(s_sutra_btn_lbl, C_INK, 0);
            }
            if (s_sutra_btn) {
                lv_obj_set_style_border_color(s_sutra_btn, C_GOLD, 0);
                lv_obj_set_style_border_width(s_sutra_btn, 2, 0);
                lv_obj_add_event_cb(s_sutra_btn, to_wish, LV_EVENT_CLICKED, NULL);
            }
        }
        break;

    case ST_DRAW: {
        /* 莲花光晕呼吸：半径做 ±6% 的正弦浮动，透明度同步 */
        static const int r0[4] = { 150, 108, 70, 34 };
        for (int i = 0; i < 4; i++) {
            if (!s_halo[i]) continue;
            float ph = (float)s_tick * 0.06f + (float)i * 0.7f;
            int r = r0[i] + (int)(6.0f * sinf(ph));
            lv_obj_set_size(s_halo[i], r * 2, r * 2);
            lv_obj_set_pos(s_halo[i], SCR_W / 2 - r, 230 - r);
            lv_obj_set_style_radius(s_halo[i], r, 0);
        }
        /* 2s（100 帧）后揭晓按钮现身 */
        if (s_tick == 100 && s_draw_btn) {
            lv_obj_set_style_opa(s_draw_btn, LV_OPA_COVER, 0);
            if (s_draw_lbl) lv_label_set_text(s_draw_lbl, "灵 机 已 驻");
        }
        break;
    }

    default:
        break;
    }
}

/* ============================================================
 *  入口
 * ============================================================ */
void ui_lingqian_init(void)
{
    ESP_LOGI(TAG, "灵签界面 v2.2 建立，共 %d 签（竖屏 %dx%d · 霞鹜文楷 · 设置集中面板）",
             lq_count(), SCR_W, SCR_H);

    /* ★★ 10-08：音量/背光/静音已提升到全局设置（app_settings）——
     *   这里【不再自己 apply】：由 xs_cfg_init() 在开机时统一应用一次，
     *   首页的「设 置」面板调的也是同一份。本处只打日志核对。
     *   默认值：音量 100 / 背光 50 / 声音【关】（兰兰 10-08 指定）。*/
    ESP_LOGI(TAG, "音量 = %d（静音=%d）· 背光 = %d",
             xs_cfg_muted() ? 0 : xs_cfg_volume(), (int)xs_cfg_muted(), xs_cfg_backlight());

    s_scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_style_bg_color(s_scr, C_PAPER, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ★★★ 漏了这一行 ⇒ 整屏纯白，而且【日志里看不出任何异常】。
     *   lv_obj_create(NULL) 只是"造了一个屏幕对象"，它默认【不在屏上】。
     *   必须 lv_screen_load() 把它挂成当前活动屏，LVGL 才会去渲染它。
     *   漏掉时：LQUI 日志正常打出来、界面对象都建好了、一帧都没 flush，
     *   屏上却什么都没有（背光一开就是一片白）——
     *   而背光关闭时连"白"都看不到，症状退化成"屏幕不亮"，最容易误判成硬件问题。
     *   拾声那边每处 lv_obj_create(NULL) 都配了 lv_screen_load()，这里漏了。
     *   判据：截图是纯白 + 日志无 frame# ⇒ 先查这一行，别去翻硬件。*/
    lv_screen_load(s_scr);

    build_cover();
    s_state = ST_COVER;

    /* 常驻 timer：20ms。所有页面的动效都由它驱动 */
    if (!s_anim) s_anim = lv_timer_create(anim_cb, 20, NULL);

    /* 常驻 timer：500ms。只干一件事 —— 盯"朗读本次出声"何时收工（见 boost_watch_cb）。
     * ★ 为什么需要它：朗读是异步播放，"播完"这件事没有回调。
     *   不盯的话，用户点了朗读、播完后音量会停在 boost 值，
     *   于是"默认静音"就悄悄失效了 —— 而这种失效没有任何症状，
     *   只在"后来又自己响了一声"时才暴露，极难归因。*/
    if (!s_boost_watch) s_boost_watch = lv_timer_create(boost_watch_cb, 500, NULL);
    s_boosted = false;
}

/* ============================================================
 *  退出（多产品外壳的 leave 契约 —— 由 ui_shell_back() 调用）
 * ============================================================
 *  ★★★ 顺序不能乱，尤其是「先停 timer 再删对象」这一条：
 *     anim_cb 每 20ms 摸一次 s_page / s_read_body / s_set_* 等，
 *     屏删了而 timer 还在跑 ⇒ 回调里读到的是已释放内存。
 *     这种崩是随机的（要恰好撞上 LVGL 那一次 tick），
 *     现场表现像"偶尔自己重启"，最难查。
 *  ★ 为什么连 s_pick/s_wish 也要复位：外壳允许用户反复进出灵签，
 *     上一次抽到第 88 签、选了"求姻缘"，这次进来就该是崭新的封面，
 *     而不是回到一半的流程 —— init() 里 build_cover() 只建封面，
 *     但这些状态变量是文件级 static，不复位就会"带着上次的签"。
 * ============================================================ */
void ui_lingqian_leave(void)
{
    /* ① 声音先还回去：播放器只有一路，不还回去天气/股票就没得用 */
    bgm_stop();
    if (app_radio_is_playing()) app_radio_stop();
    s_voice_ok = false;

    /* ①b 朗读的「本次出声」旁路要还原 —— 否则退出后音量停在 boost 值，
     *    下次别人（天气/股票）播报就会"莫名其妙有声音"。*/
    if (s_boosted) { s_boosted = false; xs_cfg_audio_restore(); }
    if (s_boost_watch) { lv_timer_del(s_boost_watch); s_boost_watch = NULL; }

    /* ② 停常驻动画 timer（★ 必须在删对象【之前】）*/
    if (s_anim) { lv_timer_del(s_anim); s_anim = NULL; }

    /* ③ 只清引用、不删对象 —— 对象交给下面的删屏统一回收。
     *   ★★ 这里【不能】调 page_clear()：它会同步 lv_obj_del，
     *      而本函数是从设置面板「主页」按钮的回调链里进来的，
     *      那个按钮正是 s_set_panel 的子对象 ⇒ 同步删 == 在事件处理中途
     *      拆掉事件源对象，LVGL 事件返回后还要访问它。*/
    page_forget();

    /* ④ 删屏：★★★ 必须【异步】。
     *   lv_obj_del_async 把真正的释放推到本帧末（届时回调栈已退干净），
     *   "点主页 → 删整屏"这条路径才安全。
     *   ★ 此刻 s_scr 已不是活动屏（ui_shell_back 先 lv_screen_load 了主菜单），
     *     所以删它不会让显示层失去当前屏。*/
    if (s_scr) { lv_obj_del_async(s_scr); s_scr = NULL; }

    /* ⑤ 复位状态，保证下次进来是从头开始 */
    s_state = ST_COVER;
    s_tick  = 0;
    s_pick  = -1;
    s_wish  = -1;
    s_sutra_ok = 0;

    ESP_LOGD(TAG, "灵签已退出：动画定时器与屏幕均已释放");
}

/* 底座可能引用（拾声的电台列表刷新）—— 灵签没有电台，留空桩 */
void ui_reload_stations(void) { }

/* ============================================================
 *  调试跳页（串口 lq_demo <page> [签号]）
 * ============================================================ */
void ui_lingqian_demo(const char *page, int pick)
{
    if (!page || !s_scr) { ESP_LOGW(TAG, "lq_demo: 界面还没建立"); return; }

    /* ★ v2.2 新增 "set"：直接弹出设置面板。
     *   为什么值得单开一个名字：设置面板是【覆盖层】不是页面，
     *   而 lq_demo 原来只能跳页面 ⇒ 想截设置面板的图就只能靠真手点，
     *   一轮"改→烧→让人点一下→再截"十几分钟。给了 set 之后，
     *   `lq_demo set` + `lq_shot` 就能自己看到面板长什么样。
     *   它必须排在 ST_POEM 之后 —— 先把底页落好，面板才有"盖在页面上"的语境。*/
    bool want_set = (strcmp(page, "set") == 0);
    const char *pg = want_set ? "poem" : page;

    static const struct { const char *name; int st; } MAP[] = {
        { "cover", ST_COVER }, { "sutra", ST_SUTRA }, { "wish",  ST_WISH  },
        { "draw",  ST_DRAW  }, { "poem",  ST_POEM  }, { "read",  ST_READ  },
    };
    int st = -1;
    for (unsigned i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
        if (strcmp(pg, MAP[i].name) == 0) { st = MAP[i].st; break; }
    }
    if (st < 0) { ESP_LOGW(TAG, "lq_demo: 页面名 '%s' 不认识", pg); return; }

    /* ★ 跳页时把该页的前置条件一并补上，否则会看到"空的/被拦住的"页面：
     *   sutra 页要点够时间才放行，poem/read 页要先有签。 */
    if (st == ST_SUTRA) s_sutra_ok = true;
    if (st == ST_WISH)  s_wish = pick > 0 ? pick - 1 : 0;
    if (st == ST_POEM || st == ST_READ) {
        int n = lq_count();
        int idx = (pick >= 1 && pick <= n) ? pick - 1 : 0;
        s_pick = idx;
    }
    if (st != ST_COVER) { bgm_stop(); s_voice_ok = false; }

    enter(st);
    ESP_LOGI(TAG, "lq_demo: 跳到 %s%s（签 %d）", pg,
             (st == ST_POEM || st == ST_READ) ? " · 已选签" : "",
             (st == ST_POEM || st == ST_READ) ? s_pick + 1 : 0);

    if (want_set) {
        if (s_set_panel) { lv_obj_del(s_set_panel); s_set_panel = NULL; }
        s_set_open = true;
        set_panel_build();
        set_panel_refresh();
        ESP_LOGI(TAG, "lq_demo: 设置面板已打开（音量 %d · 背光 %d · 静音 %d）",
                 xs_cfg_volume(), xs_cfg_backlight(), (int)xs_cfg_muted());
    }
}
