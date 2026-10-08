#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "lvgl.h"

/* 屏 + 触摸 + LVGL 全套初始化（失败返回错误码） */
esp_err_t app_display_init(void);

/* 背光亮度 0~100 */
void app_backlight_set(int percent);

/* LVGL 显示句柄 */
lv_display_t *app_display_get(void);

/* I2C0 主总线句柄（触摸与 ES8311 共用）—— 供音频模块挂 codec 用 */
i2c_master_bus_handle_t app_i2c_bus_get(void);

/* 最近一次触摸读数（原始物理坐标 + 换算后的逻辑坐标） */
void app_touch_get_last(int *rx, int *ry, int *lx, int *ly, bool *pressed);

/* ★★ 10-07 截图（调试用）：把当前 LVGL 整屏帧缓冲（320x480 RGB565，行主序）
 *   拷到 dst（需 >= LCD_H_RES * LCD_V_RES * 2 = 307200 字节）。
 *   返回 false = 还没出过帧（display ready 之前）。
 *
 *   ★ 为什么不直接返回内部指针（s_buf1）：
 *     LVGL 随时会拿它渲染下一帧，调用方看到的会是"半新半旧"的画面
 *     —— 调试时这种图最坑人，会让人以为是 UI 画错了。
 *     拷一份 307KB 在 PSRAM 上约 3~5 ms，换一个确定的结果，值。*/
bool app_display_snapshot(void *dst);
