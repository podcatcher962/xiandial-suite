/*
 * 拾声 · 股票行情 App（多产品外壳下的第 3 个产品）
 * ============================================================
 *  契约见 ui_shell.h：init 自建屏并 lv_screen_load，leave 拆干净。
 *
 *  ★ 数据源（两条，都是明文 http、无需 key）：
 *      · 实时行情 http://qt.gtimg.cn/q=<code,code,...>
 *      · 日K     http://money.finance.sina.com.cn/…/getKLineData?symbol=<code>&scale=240&datalen=30
 *    ⚠ 腾讯的 fqkline 接口【不能用】：它 302 跳到 https，而这块板的
 *      内部 RAM 最大连续块只有 31.7 KB —— TLS 握手要 16~40 KB 连续内存，
 *      必然失败。新浪那条是明文且无重定向（10-08 真机实测 HTTP 200）。
 *
 *  ★★ 配色守国内规矩：**涨 = 红、跌 = 绿**（与欧美相反）。兰兰 10-08 明确要求。
 *
 *  ★ 自选股：默认 10 个（兰兰指定），可增删，存 NVS（命名空间 xs_stk）。
 *    列表双列卡片、可上下滑动；点卡片进日K屏。
 *
 *  ★ 刷新节流：交易时段（工作日 9:15-11:30、13:00-15:00）每 60 秒一次，
 *    其余时段 10 分钟一次 —— 收盘后没必要一分钟拉一回。
 * ============================================================ */
#pragma once

void ui_stock_init(void);
void ui_stock_leave(void);

/* 打开第 idx 个自选标的的日K屏（内部用；串口调试也可调）*/
void ui_stock_open_kline(int idx);

/* 重建列表屏（自选增删后调用）*/
void ui_stock_rebuild_list(void);

/* ★ 10-08 调试用：直接弹开「添加自选」面板（数字键盘）。
 * 理由同 ui_shell_demo_cfg / ui_weather_demo_speak：
 * 板上没有物理手指、串口注入不了触摸，不给这条命令，
 * 这个新面板的排版（3×4 键盘格、提示行）就只能靠读代码保证。
 * ★ 需先 app_open 3 进股票（面板挂在列表屏上），且必须在 LVGL 锁内调用。*/
void ui_stock_demo_add(void);
