/*
 * ============================================================
 *  app_sleep.c —— 睡眠定时（到点自动停播）
 * ============================================================
 *  兰兰 10-04：「能不能设置睡眠定时，时间可以自己定」。
 *
 *  ★ 为什么单独开一个文件（而不是塞进 ui_xiandial.c）：
 *    这个模块有【状态 + 掉电保存 + 到点执行】三件事，
 *    而 ui_xiandial.c 已经 4000 行、LVGL 池只剩 26 KB。
 *    把它独立出来 ⇒ 逻辑能单独验，UI 只负责画和收点击。
 *
 *  ★★★ 三个设计决定，每一个都是踩过坑才定的：
 *
 *  ① 【倒计时用 uptime 算，不用系统时间】
 *     系统时间会被 SNTP 改（app_sys.c 里还会 +8h，见那儿的铁律）。
 *     如果用 now - set_at 算倒计时，改一次时间定时就乱了。
 *     uptime 是单调递增的，睡眠期间照走（不依赖 NVS）。
 *
 *  ② 【设置存 NVS，掉电不丢】
 *     睡前设好 30 分钟，半夜拔电/重启，定时不该丢。
 *     ★ 但存的是【设定值】（分钟数），不是【截止时刻】——
 *       存时刻的话重启后会拿旧时刻去比，语义就乱了。
 *
 *  ③ 【到点动作 = 停止播放，不是断电】
 *     兰兰明确选的（问过了）。理由也站得住：
 *       断电之后只能长按电源键开机，等于把设备「关死」了；
 *       停止播放 + 息屏则随时能按一下就回来。
 *     以后要做「真关机」也不难，把 act_stop() 换成关机调用即可，
 *     入口已经单独抽出来了。
 *
 *  用法（ui_xiandial.c）：
 *      app_sleep_init();
 *      app_sleep_set_minutes(30);      // 0 = 取消
 *      app_sleep_remaining_s();        // 剩余秒，0 = 没在跑
 *      app_sleep_is_on();
 *  到点由 app_sleep_tick() 驱动（挂在 1 秒定时器上）。
 * ============================================================ */
#include "app_sleep.h"

#include <stdio.h>
#include <string.h>

#include "app_radio.h"     /* app_radio_stop()：到点要停播 */

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "XSSLEEP";

#define NVS_NS   "xs_sleep"
#define NVS_KEY  "mins"

static int      s_mins    = 0;        /* 0 = 不定时；>0 = 设定分钟数 */
static int64_t  s_set_at  = 0;        /* 设定时刻的 uptime（微秒） */
static bool     s_fired   = false;    /* 已经到点停过一次，等用户重新设 */

static void load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    int32_t v = 0;
    esp_err_t r = nvs_get_i32(h, NVS_KEY, &v);
    nvs_close(h);
    if (r != ESP_OK || v <= 0) return;
    /* ★ 只恢复「设定分钟数」，不恢复倒计时进度。
     *   重启后从头开始数，而不是接着上次数 —— 语义上更符合
     *   「我设了睡 30 分钟」这句话（用户重开机的动机通常就是重新开始）。*/
    s_mins   = (int)v;
    s_set_at = esp_timer_get_time() + (int64_t)s_mins * 60 * 1000000LL;
    ESP_LOGI(TAG, "睡眠定时恢复：%d 分钟（从头开始数）", s_mins);
}

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, NVS_KEY, s_mins);
    nvs_commit(h);
    nvs_close(h);
}

esp_err_t app_sleep_init(void)
{
    s_mins   = 0;
    s_set_at = 0;
    s_fired  = false;
    load();
    return ESP_OK;
}

void app_sleep_set_minutes(int mins)
{
    if (mins <= 0) {
        bool was = (s_mins > 0);
        s_mins   = 0;
        s_set_at = 0;
        s_fired  = false;
        if (was) save();               /* 只在「从开着改成关」时写 NVS */
        ESP_LOGI(TAG, "睡眠定时已取消");
        return;
    }
    /* ★ 上限 4 小时（240 分）。再长没有实际意义，
     *   而且倒计时显示只有 4 位数字，超了会显示成 9999 分那种鬼样子。*/
    if (mins > 240) mins = 240;
    s_mins   = mins;
    s_set_at = esp_timer_get_time() + (int64_t)mins * 60 * 1000000LL;
    s_fired  = false;
    save();
    ESP_LOGI(TAG, "睡眠定时：%d 分钟后自动停播", mins);
}

int app_sleep_remaining_s(void)
{
    if (s_mins <= 0 || s_fired) return 0;
    int64_t left = s_set_at - esp_timer_get_time();
    if (left <= 0) return 0;
    return (int)(left / 1000000LL);
}

bool app_sleep_is_on(void)
{
    return s_mins > 0 && !s_fired;
}

int app_sleep_minutes(void)
{
    return s_mins;
}

bool app_sleep_fired(void)
{
    return s_fired;
}

/* 到点要做的事。抽成独立函数：以后要改成「真关机」，
 * 改这一个函数就行，计时逻辑不用动。*/
static void act_stop(void)
{
    ESP_LOGI(TAG, "到点了 —— 自动停止播放");
    /* 只调停播，不碰屏幕亮度：夜间页那页本来就暗，
     * 再关背光的话用户按「亮屏键」之前会以为死机了。*/
    app_radio_stop();
}

bool app_sleep_tick(void)
{
    if (s_mins <= 0 || s_fired) return false;
    int64_t left = s_set_at - esp_timer_get_time();
    if (left > 0) return false;
    s_fired = true;                 /* 只触发一次，重复调用不重复停 */
    act_stop();
    return true;
}
