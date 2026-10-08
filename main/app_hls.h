#pragma once
/* ============================================================
 *  app_hls.h —— HLS（m3u8）直播流拉流器（10-03 第十五次加）
 * ============================================================
 *  做什么：把 m3u8 播放列表变成一条【连续的 .ts 字节流】，
 *        交给 app_radio.c 那套 esp_audio_simple_dec 解码。
 *        ——所以 app_radio.c 那边几乎不用改，只要把
 *          「从 http 读数据」换成「从 hls_pull 拿数据」。
 *
 *  为什么这些源能这么干（_probe_hls2.py 实测 5 个源）：
 *    全是 media list（无 master）、切片全 .ts、全无加密、每列表 3~6 片。
 *    ⇒ 不需要解复用、不需要解密、不需要选变体流（master 分支留着以防万一）。
 *
 *  ★ 铁律 17：全部用 GET，绝不用 HEAD（蜻蜓系/阿里 CDN 对 HEAD 返 400）。
 * ============================================================ */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HLS_MAX_SEG   12          /* 一个列表里的分片上限（实测 3~6，留一倍余量）*/
#define HLS_URL_MAX   512
#define HLS_LIST_CAP  16384       /* 播放列表文本上限。
 *   ★ 从 8192 提到 16384：master list 要嵌变体 URL、chunklist 还要再套一层，
 *   281 个源实测里有几类列表文本接近 8 KB，截断会直接导致「解析不出分片」。*/
#define HLS_SEG_MAX   (384 * 1024)  /* 单个 .ts 分片上限。
 *   ★★★ 2026-10-04 第二十一次：从 160 KB 提到 384 KB。
 *   起因：兰兰报「央视还是 HLS 连不上无法播放」。电脑上实测 CCTV-1/11/13
 *   的切片是 **199~200 KB / 10 秒**（不是原先估的 64 kbps ≈ 90 KB ——
 *   那是低码率音频的算法，电视伴音源实际是 160 kbps 级别），
 *   160 KB 缓冲会把每一片都截掉 40 KB ⇒ 丢包 ⇒ 播出来就是「卡」。
 *   384 KB 放在 PSRAM（8 MB 的一半给环形缓冲，seg_buf 是另分配的），
 *   内部 SRAM 一字节不占 —— 这是唯一能放这么大的地方。*/

typedef struct {
    char        url[HLS_URL_MAX];     /* m3u8 地址（或 master 的变体流地址）*/
    char       *list;                 /* 播放列表文本（malloc，PSRAM）*/
    size_t      list_len;
    size_t      list_cap;
    char        seg[HLS_MAX_SEG][HLS_URL_MAX];   /* 分片完整 URL（已解析）*/
    int         seg_n;                /* 分片个数 */
    int         seg_i;                /* 当前片序号 */
    uint8_t    *seg_buf;              /* 当前片的 .ts 原始数据（PSRAM）*/
    size_t      seg_need;             /* 当前片实际字节数 */
    size_t      seg_pos;              /* 已从当前片取走多少（片内偏移）*/
    size_t      wr;                   /* 兼容字段：最后一次拉到的字节数 */
    /* last_seg：最近播完的分片【文件名】（不含目录）。
     *   ★ 直播列表会滚动，重拉后 seg[0] 常常就是刚播完那片 ⇒ 不比对就会
     *   把同一片重播一遍（听感是「重复一段」）。*/
    char        last_seg[72];
    /*★★ next_url 已废弃（10-04 第四版删掉）。
     *   它原意是「下次 hls_pull 先换片」，但换片这件事 seg_pos>=seg_need
     *   已经管了；留着反而会在换片成功后【再跳一片】⇒ 每 10 秒丢一段声音。
     *   字段位置保留，勿再读写。*/
    /* ★★ 10-04 加：连续失败计数。原来没有熔断，分片一直拉不下来时
     *   会无限「skip → 重拉列表 → 再 skip」，实测刷 10 分钟 10370 次。*/
    int         fail_streak;          /* 连续失败片数，>=8 就放弃这个源 */
    int         fail_total;           /* 累计失败片数，只为日志好读 */
    bool        started;
    char        err[48];
} hls_state_t;

/* 拉列表 + 解析分片。成功返回 true。*/
bool hls_start(hls_state_t *h, const char *m3u8);

/* 取最多 want 字节连续数据到 out；*got = 实际拿到的字节数。
 * 返回 false 表示列表/分片全挂了（调用方应当重连或报错）。*/
bool hls_pull(hls_state_t *h, uint8_t *out, size_t want, size_t *got);

/* ★★ 自证直播延迟：从当前分片文件名里解出 unix 时间戳，与本机时间相比。
 * 返回秒数；0 = 这个源的命名里没有时间戳，判断不了。
 * 为什么需要它：「听着延迟」有两种完全不同的原因——
 *   (a) 重播老片（修复前的 bug，文件名是几分钟前的）；
 *   (b) 源本身有延迟（就是现在这样，十几秒）。
 * 光听分不出来，屏上打出数字才不用猜。*/
long hls_lag_seconds(const hls_state_t *h);

/* 释放内部缓冲（list / seg_buf 都可能几百 KB）。不用了必须调。*/
void hls_stop(hls_state_t *h);

/* 最近一次失败的简短原因（中文，UI 直接显示）*/
const char *hls_error(hls_state_t *h);
