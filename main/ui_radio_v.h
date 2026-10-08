/*
 * 拾声 · 竖版电台 App（多产品外壳下的第 4 个产品）
 * ============================================================
 *  契约见 ui_shell.h：init 自建屏并 lv_screen_load，leave 拆干净。
 *
 *  ★ 为什么新写一个文件，而不是把 ui_xiandial.c（拾声横版，5818 行）改过来：
 *    那个文件整套坐标是按【横版 480×320】写死的，改竖版等于重画每一页；
 *    而它的后端（app_radio / app_stlist / app_sd / app_fav）一个字都不用动 ——
 *    所以这里只重写"界面层"，后端照用。
 *
 *  ★★ 一条硬规矩：进入才创建、离开即销毁（见 ui_shell.h 的说明）。
 *    内部 SRAM 只有 ~40 KB 余量，LVGL 池 96 KB（CONFIG_LV_MEM_SIZE=98304）。
 *    ⇒ 本 App 的四个视图都在 init 里建、leave 里随屏一起删，
 *      不往主菜单屏上挂任何常驻对象。
 *
 *  ★ 四个视图（都在同一个 s_scr 下，靠 HIDDEN 标志切换）：
 *      列表（网络电台，带栏目/地区筛选）
 *      播放（大台名 + 真频谱 + 音量 / 上下台 / 暂停）
 *      分类（栏目 13 / 地区 42，两页 tab）
 *      本地音频（SD 卡目录浏览 + 点播）
 *    为什么四个视图而不是四块屏：屏只有一个 ⇒ leave 一次删干净，
 *    不会留下孤儿屏（孤儿屏在 release 下是静默崩溃，见 ui_shell.h）。
 */
#pragma once

/* 建立并切到电台 App。调用方须持 LVGL 锁。 */
void ui_radio_v_init(void);

/* 拆除（先切屏、再删；顺序由 ui_shell 负责）。调用方须持 LVGL 锁。 */
void ui_radio_v_leave(void);

/* ============================================================
 *  ★ 串口调试口（app_stlist.c 的 `radio <sub>` 命令）
 * ============================================================
 *  为什么需要：这块板子【没有物理手指】，串口也注入不了触摸 ——
 *  分类筛选、列表滚动（行对象复用是这里最容易写错的一段）、
 *  本地音频点播，全都只能靠命令驱动，否则每轮改完只能靠人点+转述。
 *  ★ 全部走的是与手指完全相同的回调路径，不是另抄一份动作序列
 *    （抄一份只能证明"抄的那份对"）。
 *  ★ 都必须在 LVGL 锁内调用。*/
void ui_radio_v_demo_tap(int n);            /* 点第 n 个可见行 */
void ui_radio_v_demo_scroll(int y);         /* 列表滚到 y（验行复用）*/
void ui_radio_v_demo_filter(int cat, int prov);  /* -1 = 不筛 */
void ui_radio_v_demo_view(int v);           /* 0列表 1播放 2分类 3本地 */
void ui_radio_v_demo_cat_tab(int tab);      /* 0 栏目 / 1 地区 */
void ui_radio_v_demo_sd(const char *path);  /* 进本地音频并切到 path */
