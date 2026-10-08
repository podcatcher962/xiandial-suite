#include "voice_speak.h"

#include <stdio.h>
#include <stdint.h>     /* intptr_t：把"代次"当参数传给任务要用它 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "app_radio.h"
#include "app_sd.h"
#include "app_settings.h"

static const char *TAG = "VSPEAK";

/* ============================================================
 *  配置
 * ============================================================ */

/* 一句话最多几个原子。时间（北京时间+现在+15+点+20+分 = 6）＋天气
 * （今天+上海+晴+25+度 = 5）串起来约 11 个，给 24 是留足余量。*/
#define VS_MAX_ATOMS   24
#define VS_NAME_LEN    24      /* 原子名上限：n15 / w_leizhenyu / c_sh … */
#define VS_CHUNK       4096    /* 拼文件时的搬运块大小 */

/* 原子在卡上的位置，按顺序找：
 *   ① /sdcard/va/  —— 干净（将来给 st_putraw 加上建目录能力就搬进去）
 *   ② /sdcard/     —— ★ 现在只能这样：st_putraw 只写 /sdcard/<name>，
 *                     且 app_stlist.c 的 st_safe_name() 明令名字里不许有 '/'。
 *                     别改成只认 ①，否则推完卡一句都播不出来。*/
static const char *VS_DIRS[] = { "/sdcard/va", "/sdcard" };

/* 拼接产物。FatFs 只挂了 /sdcard 一处，临时文件也只能放这儿。
 * ★ 名字带下划线前缀：SD 根目录同时住着 lq001.mp3…lq100.mp3（灵签语音）
 *   和一堆原子，别和任何现存文件名撞上。
 * ★★ 为什么要【两个】名字交替用：app_radio_stop() 是【非阻塞】的
 *    （它只置标志，播放任务下一轮才真正关文件）。于是存在这个窗口：
 *      voice_speak_stop() 返回 → 播放任务还捏着 _va_tmp.mp3 的读句柄
 *      → 用户立刻再点播报 → 我们去 fopen(...,"wb") 同一个文件
 *    同一卷上一边读一边截断，FatFs 的行为是不保证的。
 *    交替两个名字 ⇒ 新一轮写的永远是播放任务没打开过的那个，窗口消失。*/
#define VS_TMP_FMT     "/sdcard/_va_tmp%d.mp3"

/* 播报最长等多久（每 500 ms 查一次，共 90 次 = 45 秒）。
 * ★ 为什么必须有这个上限：只靠「!app_radio_is_playing()」的话，
 *   万一解码器卡住不退出，本任务就永远不结束、s_busy 永远为真，
 *   下次点「播报」被挡在门外 —— 用户看到的现象是「按钮按了一次就失灵了」。*/
#define VS_WAIT_TICKS  (45 * 2)

/* ============================================================
 *  状态
 * ============================================================ */

static volatile bool s_busy;                                /* 正在播报 */
static char          s_atoms[VS_MAX_ATOMS][VS_NAME_LEN];    /* 待播的原子名 */
static int           s_atom_n;
/* ★ 代次：每起播一次 +1。任务只在自己的那一代里收尾，
 *   否则「停止后立刻再点一次」时，旧任务醒来会把新一次播报的 boost 抹掉。*/
static volatile int  s_gen;

static void vs_task(void *arg);

/* ============================================================
 *  对外
 * ============================================================ */

bool voice_speak(const char *const *atoms, int n)
{
    if (!atoms || n <= 0)  return false;
    if (s_busy)            return false;      /* 正在播，忽略重复点击 */

    int k = 0;
    for (int i = 0; i < n && k < VS_MAX_ATOMS; i++) {
        if (!atoms[i] || !atoms[i][0]) continue;   /* 容忍调用方塞空项 */
        /* 用精度限定，避免 GCC 的 -Werror=format-truncation */
        snprintf(s_atoms[k], VS_NAME_LEN, "%.*s", VS_NAME_LEN - 1, atoms[i]);
        k++;
    }
    if (k == 0) return false;

    s_atom_n = k;
    s_busy   = true;
    int my   = ++s_gen;

    /* 栈 5120：FatFs 的 fopen/fread 走 VFS，比裸 read() 多吃一些栈。
     * 优先级 2 ＜ LVGL 任务（3）—— 拼文件时不能让界面卡住；
     * 反正本任务大部分时间在 500 ms 一跳地等播完，让出 CPU 毫无代价。*/
    if (xTaskCreate(vs_task, "xs_speak", 5120, (void *)(intptr_t)my, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "播报任务建不起来（内存不够？）");
        s_busy = false;
        return false;
    }
    ESP_LOGI(TAG, "播报开始：%d 段", k);
    return true;
}

bool voice_speak_busy(void)
{
    return s_busy;
}

void voice_speak_stop(void)
{
    if (!s_busy) return;
    s_gen++;                    /* 让在跑的那一代收尾时别再动音量 */
    app_radio_stop();           /* 非阻塞：播放任务自己下一轮退出 */
    xs_cfg_audio_restore();
    s_busy = false;
}

/* ============================================================
 *  实现
 * ============================================================ */

/* 按 VS_DIRS 顺序找一个原子文件。找到就返回已打开的 FILE*，
 * 并把实际路径写进 out（诊断日志用）。找不到回 NULL。*/
static FILE *vs_open_atom(const char *name, char *out, size_t cap)
{
    for (size_t i = 0; i < sizeof(VS_DIRS) / sizeof(VS_DIRS[0]); i++) {
        snprintf(out, cap, "%s/%s.mp3", VS_DIRS[i], name);
        FILE *f = fopen(out, "rb");
        if (f) return f;
    }
    return NULL;
}

/* 把 s_atoms[] 里的原子按顺序拼成一个 mp3。返回总字节数，-1 = 一段都没拼成。
 * ★ 原子在生成时就剥掉了 ID3v2（见 _gen_va.py），所以这里是【纯字节拼接】——
 *   中间不会夹 ID3 头把解码器卡住。
 * ★ 实测 144 个原子全部以 FF F3 64 C4 开头（MPEG-1 Layer III，同一编码参数、
 *   同一比特率），所以拼起来是一个参数自洽的连续码流，解码器能一路解下去。
 * ★ 缺一个原子不算失败：跳过去接着拼，宁可少播一个词也别整句不出声。*/
static long vs_concat(const char *dst)
{
    FILE *fo = fopen(dst, "wb");
    if (!fo) {
        ESP_LOGE(TAG, "建不了 %s（卡满 / 只读？）", dst);
        return -1;
    }

    /* ★★ static：本任务栈只有 5 KB，4 KB 的搬运缓冲绝不能落在栈上。*/
    static char buf[VS_CHUNK];

    long total = 0;
    int  used  = 0;

    for (int i = 0; i < s_atom_n; i++) {
        char path[64];
        FILE *fi = vs_open_atom(s_atoms[i], path, sizeof(path));
        if (!fi) {
            ESP_LOGW(TAG, "缺原子 %s（跳过）", s_atoms[i]);
            continue;
        }

        size_t got;
        bool   bad = false;
        while ((got = fread(buf, 1, sizeof(buf), fi)) > 0) {
            if (fwrite(buf, 1, got, fo) != got) { bad = true; break; }
            total += (long)got;
        }
        fclose(fi);
        if (bad) {
            ESP_LOGE(TAG, "写 %s 时失败（卡满了？）", dst);
            break;
        }
        used++;
    }

    fclose(fo);
    return used ? total : -1;
}

static void vs_task(void *arg)
{
    int my = (int)(intptr_t)arg;

    /* ① 保证卡在 */
    if (!app_sd_is_mounted() && app_sd_mount() != ESP_OK) {
        ESP_LOGW(TAG, "播报：SD 没挂载");
        goto done;
    }

    /* ② 拼（两个名字交替，避开"上一轮播放任务还占着文件"的窗口）*/
    char tmp[48];
    snprintf(tmp, sizeof(tmp), VS_TMP_FMT, my & 1);

    long total = vs_concat(tmp);
    if (total <= 0) {
        ESP_LOGW(TAG, "播报：一个原子都没找到，跳过（原子推卡了吗？）");
        goto done;
    }
    ESP_LOGI(TAG, "播报：%d 段 → %ld 字节 → %s", s_atom_n, total, tmp);

    /* ③ 出声。★ 默认静音（兰兰指定），但这一下必须听得见 ——
     *    boost 只临时把 DAC 增益抬到设定音量，不动 mute 标志，
     *    播完 restore 就回到"默认不出声"。*/
    xs_cfg_audio_boost();
    if (app_radio_play_file(tmp) != ESP_OK) {
        ESP_LOGW(TAG, "播报：起播失败（%s）", app_radio_last_error());
        goto done;
    }

    /* ④ 等放完 */
    for (int i = 0; i < VS_WAIT_TICKS; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!app_radio_is_playing()) break;
    }

done:
    /* 收尾。★ 只在自己那一代里动手 —— 若中途被 voice_speak_stop() 或
     *   新一次播报取代（s_gen 变了），音量/忙标志都归它们管，这里别插手。*/
    if (my == s_gen) {
        xs_cfg_audio_restore();
        s_busy = false;
    }
    vTaskDelete(NULL);
}
