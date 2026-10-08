/*
 * 灵签 (LingQian) —— 3.5" 触摸屏抽签机 · 界面入口
 *
 * 与拾声(XianDial)的关系：
 *   共用底座（显示/触摸/字库/电源），界面全新。
 *   两台机器风格刻意相反：拾声是赛博荧光，灵签是暗墨 + 朱砂 + 鎏金。
 */
#pragma once

/* 建立整个界面（须在 app_display_init() 之后调用，且持 LVGL 锁） */
void ui_lingqian_init(void);

/* ★ 10-08 多产品外壳：灵签作为「产品之一」的退出契约。
 *   由 ui_shell_back() 调用（顺序是【先切回主菜单屏、再调它】）。
 *   做四件事，顺序不能乱：
 *     ① 停背景音与语音（播放器只有一路，不还回去天气/股票就没声音）
 *     ② 停常驻动画 timer —— ★★ 必须先停再删对象：anim_cb 每 20ms 访问
 *        s_page/s_read_body 等，屏删了它还在跑就是悬空指针，
 *        崩溃是随机的（现场会像"别的地方有问题"）
 *     ③ page_clear() 清掉页面与设置面板
 *     ④ 删屏并复位全部全局指针，让下次 ui_lingqian_init() 是干净的重来
 *   ⚠ 同样只能在 LVGL 任务里 / 持锁上下文调用。*/
void ui_lingqian_leave(void);

/* ★ 10-07 夜：调试用跳页（配合串口 lq_demo，PC 侧 tools/_lq_shot.py）
 *   page = cover|sutra|wish|draw|poem|read|set，pick = 签号（1~100，仅 poem/read 有意义）
 *   ★ v2.2 新增 page="set"：直接弹出设置面板（覆盖在签诗页之上），
 *     这样截设置面板不必靠真手点。
 *   ★ 为什么需要：改 UI 最怕"我看不到"。以前每轮改完都要烧固件、
 *     让人点一遍、再转述一次描述 —— 一轮十几分钟，且描述会失真。
 *     有了它 + lq_shot，改完自己先看，把明显不对的轮次挡在自己这边。
 *   ⚠ 必须持 LVGL 锁调用（内部会重建页面）。 */
void ui_lingqian_demo(const char *page, int pick);
