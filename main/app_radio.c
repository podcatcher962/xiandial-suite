/*
 * 拾声 (XianDial) 硬件版 —— 播放器实现
 *
 * 两个音源共用同一条链路：
 *     SD 文件 / HTTP 直播流 -> esp_audio_simple_dec -> PCM -> app_audio_write
 *                                                              -> I2S -> ES8311 -> 功放
 *
 * ★ 为什么用 simple_dec 而不是直接用 esp_mp3_dec：
 *   simple_dec 声明「support input data of any size」，内部自带 parser 帮我们
 *   找帧边界。按块读字节直接丢进去即可，不用自己拼 MP3 帧头 —— 直播流尤其需要。
 *
 * ★★★ 为什么必须注册【两套】解码器（10-03 排查出的无声根因）★★★
 *   esp_audio_codec 把注册拆成两层：
 *     · esp_audio_dec_register_default()        → 纯帧解码器：MP3 / AAC / FLAC / OPUS …
 *     · esp_audio_simple_dec_register_default()  → 带容器 parser 的：WAV / M4A / TS / OGG
 *   simple_dec 自己【不含】MP3 解码能力，它只是转手调用上面那层。
 *   只调后者 ⇒ MP3 从没被注册 ⇒ 打开 MP3 直接 NOT_SUPPORT，任务静默退出，
 *   表现就是「点卡里的 mp3 完全没反应、也没有任何提示」。
 *   验证：esp_audio_dec_get_ops(ESP_AUDIO_TYPE_MP3) 非 NULL 才算注册上了。
 *   （⚠️ esp_audio_dec_get_avail_type() 不是位图，别拿它判断，10-03 更正）
 *
 * ★ 网络电台的取舍（10-03 实测后定）：
 *   内置台单 120 个，全部是【无 TLS 的 http】。理由：689/733 个可用流都在蜻蜓系 CDN
 *   （lhttp.qtfm.cn / lhttp.qingting.fm / lhttp-hw.qtfm.cn），http 和 https 都能播；
 *   用 http 就省掉了证书包（64 KB Flash）与 30~40 KB 的 TLS 握手堆 ——
 *   本机内部 RAM 只剩 80 KB，这是决定性的。而且这些台裸请求就 200，不用带 UA。
 *
 * ★ 另外两个必须自己处理的坑：
 *   ① 采样率：MP3 不一定是 44100（播客常见 24000/32000），
 *      按错采样率播会变调 —— 所以解出第一帧后立刻 app_audio_set_sample_rate() 对齐。
 *   ② 声道数：I2S 固定配成立体声，若源是单声道，直接把单声道 PCM 喂进去
 *      会变成「半速 + 左右错位」，必须自己复制成全双声道。
 */
#include "app_radio.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "app_audio.h"
#include "app_hist.h"
#include "app_sd.h"
#include "app_hls.h"
#include "net_stations.h"

#include "esp_audio_dec_default.h"
#include "esp_audio_dec_reg.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_stlist.h"

static const char *TAG = "XSRADIO";

/* ★★★ 10-03 第十七次：把「缓冲越大越好」这个错误彻底推翻（兰兰报
 *   「海外台一开始都听不到，法国国际等一会儿才有，还是卡，卡得比以前更严重」）。
 *
 *   第十六次我 IN_CHUNK 16384 → 65536，理由是「海外台 125~194 kbps，
 *   16 KB 只够 0.7~1.0 秒」。★ 这个推理漏了一环：
 *
 *     player_task 是【单任务】，「读网」和「解码送 I2S」在同一个循环里串行。
 *     `esp_http_client_read(http, buf, IN_CHUNK - in_len)` 要【凑满请求量】
 *     才返回（流式 socket 上它会一直 recv）。
 *     法国国际 124.9 kbps = 15.6 KB/s：
 *       16 KB → 阻塞 1.05 秒
 *       64 KB → 阻塞 4.2 秒
 *     这 1~4 秒里 player_task 卡在 read 上，【一帧 PCM 都送不出去】
 *     ⇒ 喇叭就是「断 1 秒 → 播 1 秒 → 断 1 秒」= 卡顿。
 *   ⇒ 大缓冲在这套结构里是【负优化】：缓冲越大，喂音频的间隔越长。
 *     第十六次「卡得比以前更严重」正是这个原因（间隔 2.1 s → 4.2 s）。
 *
 *   ★ 真正的解法是【让 read 早退】，不是【让缓冲变大】：
 *     LOW_WATER  = 缓冲低到这么多就必须去读
 *     READ_CHUNK = 每次最多读这么多（★ 关键：不是「读满剩余空间」）
 *     16 KB / 15.6 KB/s ⇒ 单次阻塞上限约 1.05 秒；
 *     配合 app_audio.c 里加大的 I2S DMA（4×512 帧），
 *     网络抖动由 DMA 顶住，解码循环不被长阻塞卡死。
 *
 *   为什么不另开一个「读网线程」+环形缓冲？那是正解，但：
 *     要多 4 KB 栈 + 一套锁 + 无 PSRAM 时的同步风险，
 *     而本板 8 MB PSRAM 富余、单任务本来就够用。
 *     ★ 若日后真需要，改成双任务 + 环形缓冲是下一步（记在速查卡里）。*/
#define IN_CHUNK   16384                      /* 输入缓冲总容量（PSRAM，不占内部 RAM）*/
#define LOW_WATER  4096                       /* 缓冲低于 4 KB 必须去读 */
#define READ_CHUNK 12288                      /* ★★ v1.42：6 KB → 12 KB */
#define PCM_CHUNK  8192                       /* 解码输出块 */
#define ST_CHUNK   (PCM_CHUNK * 2)            /* 单声道扩成双声道后的块 */

/* 缓冲低于这个比例就说明源在抖动（国内台正常情况永远不触发）*/
#define STALL_RATIO      25
#define STALL_LOG_EVERY  40   /* 每 40 次打一条，别刷屏 */

/* 直播流断了重连几次就放弃（每次间隔 2 秒左右）*/
#define STREAM_RETRY_MAX  6

static TaskHandle_t  s_task    = NULL;
static volatile bool s_stop    = false;
static volatile bool s_paused  = false;
static char          s_now[160] = "";
/* ★ 本地文件路径（网络电台为空）。seek 要靠它重新打开文件 ——
 *   esp_audio 这一套【没有提供 seek 函数】（翻遍 managed_components 的头文件，
 *   只有一句注释「Seek when use frame decoder」，没有 API），
 *   所以跳转到第 N 秒只能 fseek 之后重开一次，代价是一次解码器重建（约 200 ms）。
 *   本地 MP3 一首 3~5 分钟，跳一次用户等得过去；直播根本不能 seek。*/
static char          s_path[192] = "";
/* ★ 第十九次：原来这里还有一个 `static volatile int s_seek_req = -1;`
 *   —— 与下面 fetch 侧那组的 s_seek_req【同名但语义不同】（一个是秒数、
 *   一个是标志位），这正是第十八版 seek 改错地方的源头之一。
 *   现在统一只保留下面那一组（183~185 行），此处不再重复声明。*/
static volatile bool s_seek_run = false;        /* 播放任务看到它就重开 */
static volatile int  s_elapsed = 0;
static volatile int  s_total   = 0;
static volatile int  s_rate    = 0;
static volatile int  s_chan    = 0;
static volatile int  s_level   = 0;           /* 0~100 瞬时电平 */
static volatile bool s_is_url  = false;
static volatile int  s_station = -1;          /* 当前台下标，-1 = 不是电台 */
static int           s_sr_now  = 0;           /* 已对齐的采样率，避免每帧都重配 */
static char          s_err[64] = "";          /* 最近一次失败原因（中文短句）*/

/* ============================================================
 *  ★★★ 第十八次：读网独立任务 + 环形缓冲（治海外台卡顿的正解）
 * ============================================================
 * 前十七次的循环是【单任务串行】：
 *     读网（阻塞 0.4~4 秒） → 解码 → 写 I2S（阻塞） → 读网 → …
 * 读网那一段【一帧 PCM 都送不出去】，所以只要 RTT 有抖动，声音就是断的。
 * 第十六、十七次把 IN_CHUNK 从 16 KB 改到 64 KB 又改回来，
 * 都是在调「阻塞多久」，治不了「阻塞本身」。
 *
 * ⇒ 第十八次拆成两个任务：
 *     fetch_task：只管从 socket / SD / HLS 往 ring 填（PSRAM，64 KB）
 *     player_task：只管从 ring 取 → 解码 → 写 I2S
 *   两者用【二值信号量】交接：ring 满则 fetch 等，ring 空则 player 等。
 *   于是「等网络」发生在 fetch 任务里，**I2S 永远有人在喂**。
 *
 * ★ 为什么用信号量而不用无锁 ring：
 *   读写各一个指针、只有两个任务，正确实现不难；但一旦漏了 memory
 *   barrier 就是一个偶发花屏/杂音 bug，而这类 bug 极难复现和定位。
 *   本项目已有一堆「静默失败」的教训（LV_ASSERT 空操作、bitrate 单位、
 *   新旧代码不兼容），宁可多花几微秒换确定性。
 *
 * ★ 内存：ring 64 KB 在 PSRAM（板上 8 MB），fetch 任务栈 4 KB。
 *   内部 RAM 一分不占（现在只剩 45 KB，绝不能往那放）。
 */
#define RING_CAP     65536                     /* 环形缓冲 64 KB（PSRAM）*/
#define FETCH_STACK  4096
#define RING_LOW     24576                     /* ★★ v1.42：8192 → 24576 */

/* ★★★ 铁律 38（v1.42）：卡顿的真正判据是【码率】，不是【下载速率】
 *
 *  兰兰 10-05 报：「海外栏除了法国台不卡，其他有的台还是卡。伦敦明显、
 *  纽约不明显，一会流畅一会卡。是不是只对法国台做了优化？」
 *
 *  ★ 先答：代码里【没有任何按台名/地区的分支】，"法国台优化"不存在。
 *    法国国际在源码里被点名 5 次，全是 10-03 调缓冲时拿它当案例的注释。
 *
 *  ★ 实测三个台（curl 量 icy-br 与实际下载速率）：
 *
 *    台名              标称码率   需 KB/s   实测 KB/s   余量
 *    法国国际中文部      64 kbps    8.0       12.7       1.59x   ← 不卡
 *    纽约华语 WKDM     128 kbps   16.0       17.9       1.12x   ← 轻微卡
 *    伦敦国际          128 kbps   16.0       17.5       1.09x   ← ★明显卡
 *
 *  ⇒ 下载速率【法国最慢、伦敦最快】，与体验【完全相反】⇒ 判据是码率。
 *    且伦敦余量最低(1.09x)、卡得最明显，与兰兰的描述逐条对上。
 *
 *  ★★ 机理：抗抖能力 = 缓冲还能撑多久 = (补货水位 - 已消耗) / 码率
 *    RING_LOW=8KB 时：
 *        64 kbps  →  8.0/ 8.0 = 1.00 秒
 *        128 kbps →  8.0/16.0 = 0.50 秒   ← ★只够顶半个网络抖动
 *    WiFi 丢包重传 / 服务器换节点随便就 0.5~1 秒 ⇒ 断音。
 *
 *  ★★ 对策：RING_LOW 提到 24 KB，READ_CHUNK 提到 12 KB。
 *    128 kbps 的抗抖从 0.50 秒 → 1.50 秒（3 倍）；
 *    64 kbps 从 1.00 秒 → 3.00 秒。
 *    ★ 为什么加 READ_CHUNK：esp_http_client_read 读不满 n 不返回，
 *      单次读粒度越大，一轮能囤的数据越多，抖动后恢复越快。
 *      ★ 双任务下 fetch 阻塞【不影响 player 喂 I2S】，所以放大它安全。
 *    ★ 代价：tmp 从 6 KB 涨到 12 KB，在 PSRAM 里（板上 8 MB），不占内部 RAM。
 */

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   head;      /* fetch 写的位置 */
    size_t   tail;      /* player 读的位置 */
    volatile size_t fill;
    volatile bool  eos;      /* 源已读完 */
    volatile bool  dead;     /* 源彻底失败 */
    volatile int   err;      /* 重连次数 */
} ring_t;

static ring_t       s_ring;
static TaskHandle_t s_fetch_task = NULL;
static SemaphoreHandle_t s_ring_sem = NULL;   /* 计数信号量：可读字节数 */
static SemaphoreHandle_t s_space_sem = NULL;   /* 计数信号量：可写字节数 */

/* ============================================================
 *  ★★★ 第十九次：seek 请求的【归属权】—— 这是 10-03 唯一的重启源
 * ============================================================
 *  第十八版把 fopen 搬进了 fetch_task，但 seek 代码还留在 player_task，
 *  里面对 player_task 自己的 fp 调 fseek —— 而那个 fp 恒为 NULL
 *  （全文件只有 fetch_task 里那两行给 fp 赋值）⇒ 空指针 ⇒ 一碰就重启。
 *
 *  ⇒ 铁律：句柄归谁，seek 就必须由谁执行。
 *    fetch_task 持有本地文件的 fp，所以 seek 请求必须【发给 fetch_task】，
 *    player_task 只负责「解码器重建 + 进度记账」——它不碰任何文件句柄。
 *
 *  协议（三个 volatile 变量，单向，不回环）：
 *    app_radio_seek(sec)          → 写 s_seek_sec，s_seek_req = 1
 *    fetch_task 主循环每轮检查     → 在自己的 fp 上 fseek，
 *                                   成功写 s_seek_done = 1，失败 = -1
 *    player_task 每轮检查          → 看到 s_seek_done 就重建解码器
 */
static volatile int s_seek_sec  = 0;   /* 请求跳到第几秒 */
static volatile int s_seek_req  = 0;   /* 1 = 有新请求，等 fetch 执行 */
static volatile int s_seek_done = 0;   /* 1 = fetch 已跳好；-1 = 失败 */

bool        app_radio_is_playing(void)    { return s_task != NULL; }
bool        app_radio_is_paused(void)     { return s_paused; }
const char *app_radio_now_playing(void)   { return s_now; }
int         app_radio_elapsed_s(void)     { return s_elapsed; }

/* ============================================================
 *  ★★★ 本地文件总时长：解析 MP3 帧头（10-03 第十六次）
 * ============================================================
 * 兰兰报：「听本地音频时间戳会显示 50739:31 / 54536:04」——
 * 一首歌不可能 35 万分钟。根因是【上一版用 info.bitrate 估时长】：
 *
 *   s_total = fsz * 8 / info.bitrate
 *
 * 而 info.bitrate 这个字段在不同解码器路径下单位不一致（我先按 kbps 用、
 * 又按 bps 用，实测都不对），于是总时长被放大/缩小 1000 倍。
 * 更糟的是：进度条拖动时把 s_elapsed 也写成这个错值算出来的秒数
 * ⇒ 「已播」和「总长」两个数一起爆。
 *
 * ⇒ 正确做法【不要估】：MP3 帧头里就有精确信息。
 *   ① ID3v2 标签要先跳过（大小在头里，synchsafe 整数）
 *   ② 找第一个帧同步字 0xFF Ex
 *   ③ 帧头里能读出：采样率、码率、每帧采样数
 *   ④ 如果有 Xing / Info 头（几乎所有正规转码的 MP3 都有），
 *      里面直接写着【总帧数】和【总字节数】⇒ 时长 = 帧数 × 每帧采样 / 采样率
 *   ⑤ 没有 Xing（纯 CBR 且没写标签）⇒ 退到 fsz × 8 / 码率
 *
 * 返回秒数，0 = 算不出来（界面就显示 --:--，不显示假数字）。
 * 只读文件头 2 KB，不扫全文件，1 ms 内完成。
 */
static int mp3_frame_samples(int ver, int layer)
{
    /* 每帧的采样数（不含 CRC/填充）*/
    if (layer == 3) {           /* Layer I */
        return 384;
    }
    if (ver == 3) {            /* MPEG1 */
        return 1152;
    }
    return 576;                 /* MPEG2 / MPEG2.5 */
}

static int mp3_duration_s(const char *path, long fsz)
{
    if (fsz <= 0) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        /* ★ 第十七次加日志：兰兰报「显示读不出总长」。同一个路径 player_task
         *   已经 fopen 成功在播了，这里再失败就是编码/路径长度问题，必须留痕。*/
        ESP_LOGW(TAG, "mp3dur: fopen failed: %s", path);
        return 0;
    }

    uint8_t buf[2048];
    size_t  n = fread(buf, 1, sizeof(buf), f);
    if (n < 64) {
        ESP_LOGW(TAG, "mp3dur: short read %u B (fsz=%ld)", (unsigned)n, fsz);
        fclose(f);
        return 0;
    }

    size_t p = 0;
    /* ---- ① 跳过 ID3v2 ---- */
    if (n >= 10 && memcmp(buf, "ID3", 3) == 0) {
        /* 大小是 4 字节 synchsafe（每字节只用低 7 位）*/
        uint32_t sz = ((uint32_t)(buf[6] & 0x7F) << 21) | ((uint32_t)(buf[7] & 0x7F) << 14)
                    | ((uint32_t)(buf[8] & 0x7F) << 7)  |  (uint32_t)(buf[9] & 0x7F);
        p = 10 + sz;
        /* ★★★ 第十七次修「显示读不出总长」的真凶（兰兰真机报的）：
         *   旧代码是 `if (p + 64 > n) return 0;` —— 标签一旦超过 2 KB 就放弃。
         *   而 SD 卡的歌【普遍带专辑封面】，ID3v2 到几百 KB 是常事
         *   ⇒ 几乎每首歌都走这条路返回 0 ⇒ s_total 永远 0
         *   ⇒ app_radio_can_seek() 永远 false ⇒ 拖动和快进全部无效。
         *   （三条症状一个根因，就是这里。）
         * ⇒ 改成：算出标签真实长度后【直接 fseek 跳过】，再从头读 2 KB。
         *   这样无论标签多大都不影响，一次 fseek 就够，1 ms 内完成。*/
        ESP_LOGI(TAG, "mp3dur: ID3v2 tag %u B, seek past it", (unsigned)p);
        if (fseek(f, (long)p, SEEK_SET) != 0) { fclose(f); return 0; }
        n = fread(buf, 1, sizeof(buf), f);
        if (n < 64) {
            ESP_LOGW(TAG, "mp3dur: no data after ID3 (%u B)", (unsigned)n);
            fclose(f);
            return 0;
        }
        p = 0;   /* 读回来的是从音频起点开始，重新从头找帧 */
    }

    /* ---- ② 找第一个帧同步字 ---- */
    static const int BR_V1_L3[16] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
    static const int BR_V2_L3[16] = {0, 8,16,24,32,40,48, 56, 64, 80, 96,112,128,144,160,0};
    static const int SR_V1[4] = {44100, 48000, 32000, 0};
    static const int SR_V2[4] = {22050, 24000, 16000, 0};
    static const int SR_V25[4] = {11025, 12000, 8000, 0};

    while (p + 4 <= n) {
        if (buf[p] != 0xFF || (buf[p + 1] & 0xE0) != 0xE0) { p++; continue; }

        int ver_id  = (buf[p + 1] >> 3) & 0x03;   /* 3=MPEG1, 2=MPEG2, 0=MPEG2.5 */
        int layer   = (buf[p + 1] >> 1) & 0x03;   /* 3=I, 2=II, 1=III */
        int br_idx  = (buf[p + 2] >> 4) & 0x0F;
        int sr_idx  = (buf[p + 2] >> 2) & 0x03;
        if (ver_id == 1 || layer == 0 || br_idx == 0 || br_idx == 15 || sr_idx == 3) {
            p++; continue;                        /* 保留位不对，不是真帧头 */
        }
        int ver   = (ver_id == 3) ? 3 : 2;        /* 3=MPEG1, 2=MPEG2/2.5 同族 */
        int srate = (ver_id == 3) ? SR_V1[sr_idx]
                 : (ver_id == 2) ? SR_V2[sr_idx] : SR_V25[sr_idx];
        /* 只处理 Layer III（本地音乐绝大多数是 MP3）*/
        if (layer != 1 || srate == 0) { p++; continue; }
        int kbps = (ver_id == 3) ? BR_V1_L3[br_idx] : BR_V2_L3[br_idx];
        if (kbps == 0) { p++; continue; }
        int spf = mp3_frame_samples(ver, layer);   /* 每帧采样数 1152 / 576 */

        /* ---- ④ 找 Xing / Info 头（有 ⇒ 精确总帧数）----
         * ★ xoff 是【side info 的长度】，不含帧头那 4 字节，定位时别多加一次。
         *   （我第一版把 xoff 写成 36 = 4+32，代码里又 +4 ⇒ 偏移到 40，
         *     Xing 分支一次都没命中，静默退到 CBR 估算——铁律 14 的活教材：
         *     结果看着「差不多对」，但走的是错的分支。）*/
        int  xoff    = (ver == 3) ? 32 : 17;       /* side info：MPEG1 立体声 32 / MPEG2 17 */
        size_t xpos  = p + 4 + xoff;
        uint32_t nframes = 0, nbytes = 0;
        if (xpos + 12 <= n) {
            if (memcmp(buf + xpos, "Xing", 4) == 0 || memcmp(buf + xpos, "Info", 4) == 0) {
                uint32_t flags = ((uint32_t)buf[xpos + 4] << 24) | ((uint32_t)buf[xpos + 5] << 16)
                               | ((uint32_t)buf[xpos + 6] << 8)  |  (uint32_t)buf[xpos + 7];
                size_t q = xpos + 8;
                if (flags & 0x0001) {              /* FRAMES 字段存在 */
                    nframes = ((uint32_t)buf[q] << 24) | ((uint32_t)buf[q+1] << 16)
                            | ((uint32_t)buf[q+2] << 8) |  (uint32_t)buf[q+3];
                    q += 4;
                }
                if (flags & 0x0002) {              /* BYTES 字段存在 */
                    nbytes = ((uint32_t)buf[q] << 24) | ((uint32_t)buf[q+1] << 16)
                           | ((uint32_t)buf[q+2] << 8) |  (uint32_t)buf[q+3];
                }
            }
        }
        fclose(f);

        if (nframes > 0) {
            /* ★ 精确：总帧数 × 每帧采样 / 采样率 */
            uint64_t total = (uint64_t)nframes * (uint64_t)spf / (uint64_t)srate;
            if (total > 0 && total < 100000) {
                ESP_LOGI(TAG, "mp3dur: Xing %d s (%u frames)", (int)total, (unsigned)nframes);
                return (int)total;
            }
        }
        if (nbytes > 0) {
            uint64_t total = (uint64_t)nbytes * 8ULL / ((uint64_t)kbps * 1000ULL);
            if (total > 0 && total < 100000) {
                ESP_LOGI(TAG, "mp3dur: Xing-BYTES %d s", (int)total);
                return (int)total;
            }
        }
        /* ---- ⑤ 退回 CBR 估算（这次 kbps → bps 换算写对了：×1000）---- */
        {
            uint64_t total = (uint64_t)fsz * 8ULL / ((uint64_t)kbps * 1000ULL);
            if (total > 0 && total < 100000) {
                ESP_LOGI(TAG, "mp3dur: CBR %d s (kbps=%d spf=%d sr=%d, xing=%s)",
                         (int)total, kbps, spf, srate, nframes ? "yes" : "no");
                return (int)total;
            }
        }
        ESP_LOGW(TAG, "mp3dur: frame found but estimate out of range (fsz=%ld)", fsz);
        return 0;
    }
    /* ★ 一帧都没找到：文件头前 2 KB 里没有合法 MP3 帧同步字。
     *   常见原因：① ID3v2 标签超过 2 KB（带封面图的专辑封面动辄几百 KB）；
     *   ② 根本不是 MP3（m4a/flac/wav）。两种都要在日志里说清。*/
    ESP_LOGW(TAG, "mp3dur: no MP3 frame in first 2 KB (fsz=%ld, head=%02X %02X %02X %02X) "
                  "-- ID3 too big or not MP3",
             fsz, buf[0], buf[1], buf[2], buf[3]);
    fclose(f);
    return 0;   /* 不是 MP3（wav/m4a/flac）⇒ 算不出来就老实显示 --:-- */
}

/* ★ 拖动进度条（10-03 第十四次加）。只对本地文件有效，直播直接忽略。
 *   秒数超界/没在播/是直播 都会被丢掉，所以界面可以放心随便调。
 *
 *   ★★ 第十九次改法（10-03 晚定位的唯一重启源）：
 *      第十八版把这个函数写成「算好字节偏移，让 player_task 自己去 fseek」——
 *      可那个 fp 在 player_task 里恒为 NULL，一碰就重启。
 *      现在改成【只发请求】：偏移由持有 fp 的 fetch_task 去算、去做。
 *      本函数不碰任何文件句柄，跨任务只靠 s_seek_* 三个 volatile 变量通信。*/
esp_err_t app_radio_seek(int sec)
{
    if (s_task == NULL)      return ESP_ERR_INVALID_STATE;
    if (s_is_url)            return ESP_ERR_NOT_SUPPORTED;  /* 直播没法跳 */
    if (s_total <= 0)        return ESP_ERR_INVALID_STATE;  /* 还没算出总时长 */
    if (sec < 0)             sec = 0;
    if (sec >= s_total)      sec = s_total - 1;
    s_seek_sec  = sec;
    s_seek_done = 0;
    s_seek_req  = 1;         /* fetch_task 主循环会看到 */
    return ESP_OK;
}

int         app_radio_total_s(void)      { return s_total; }
bool        app_radio_can_seek(void)     { return s_task != NULL && !s_is_url && s_total > 0; }
int         app_radio_stream_rate(void)   { return s_rate; }
int         app_radio_stream_channel(void){ return s_chan; }
int         app_radio_level(void)         { return s_level; }
bool        app_radio_is_stream(void)     { return s_is_url; }
int         app_radio_station_current(void){ return s_station; }
int         app_radio_station_count(void) { return app_st_count(); }
const char *app_radio_last_error(void)    { return s_err; }

/* ---- 倍速（第二十二次）：只是把 I2S 时钟乘上去，实现在 app_audio.c ----
 * ★ 为什么要有这一层、而不是让 UI 直接调 app_audio_set_speed：
 *   「当前倍率」是【播放状态】的一部分（换台要沿用、要显示在界面上），
 *   放 app_radio 才能和 s_rate / s_paused 这些状态待在一起，
 *   也不至于让 UI 依赖到音频驱动的细节。*/
void app_radio_set_speed(int pct)
{
    app_audio_set_speed(pct);
    ESP_LOGI(TAG, "倍速 -> %d%%（playing=%d stream=%d）",
             app_audio_get_speed(), (int)app_radio_is_playing(),
             (int)app_radio_is_stream());
}

int app_radio_speed(void) { return app_audio_get_speed(); }

/* 参数缓冲：放 static 而不是任务栈（路径可能 260 字节）。
 * 安全性：radio_start() 会先等旧任务退出再覆盖它，不会有两个任务同时读。*/
static char s_arg_path[300];

esp_err_t app_radio_init(void)
{
    /* ★ 两套都要注册，少一套 MP3 就没声音（详见文件头注释）*/
    esp_audio_err_t r1 = esp_audio_dec_register_default();
    esp_audio_err_t r2 = esp_audio_simple_dec_register_default();

    ESP_LOGI(TAG, "decoders registered: base=%d simple=%d", (int)r1, (int)r2);

    /* ★★ 判断「解码器到底注册上没有」的唯一可靠办法 ★★
     *   esp_audio_dec_get_ops() 非 NULL = 该解码器已在注册表里。
     *   ⚠️ 不要拿 esp_audio_dec_get_avail_type() 当「已注册位图」看 —— 10-03 更正：
     *      它返回的是「下一个可用的【自定义】解码器类型号」，跟内置解码器注册数无关。*/
    const void *mp3  = (const void *)esp_audio_dec_get_ops(ESP_AUDIO_TYPE_MP3);
    const void *aac  = (const void *)esp_audio_dec_get_ops(ESP_AUDIO_TYPE_AAC);
    const void *flac = (const void *)esp_audio_dec_get_ops(ESP_AUDIO_TYPE_FLAC);
    ESP_LOGI(TAG, "base ops: MP3=%p AAC=%p FLAC=%p  (非 NULL = 已注册)", mp3, aac, flac);
    if (mp3 == NULL) {
        ESP_LOGE(TAG, "!! MP3 解码器没注册上，点播和电台都会一点声音都没有");
    }

    ESP_LOGI(TAG, "simple types: MP3=%d WAV=%d M4A=%d OGG=%d TS=%d  (0=支持)",
             (int)esp_audio_simple_check_audio_type(ESP_AUDIO_SIMPLE_DEC_TYPE_MP3),
             (int)esp_audio_simple_check_audio_type(ESP_AUDIO_SIMPLE_DEC_TYPE_WAV),
             (int)esp_audio_simple_check_audio_type(ESP_AUDIO_SIMPLE_DEC_TYPE_M4A),
             (int)esp_audio_simple_check_audio_type(ESP_AUDIO_SIMPLE_DEC_TYPE_OGG),
             (int)esp_audio_simple_check_audio_type(ESP_AUDIO_SIMPLE_DEC_TYPE_TS));

    /* ★ 台单构成统计搬到 app_stlist.c 的 app_st_dump_info() 里了（10-06）。
     *   原因：这段原来在 app_radio_init() 内，而 app_st_load() 在它【之后】才跑，
     *   所以 10-05 的日志里打出来是「内置台单: 0 台」——
     *   数据其实没丢（同一份日志里 XSTLIST 报 1254 条），只是统计跑得太早。
     *   ⇒ 台单有多少条、怎么组成的，等台单载入完再报。 */

    if (r1 != ESP_AUDIO_ERR_OK && r2 != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "no decoder registered at all");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ============================================================
 *  容器嗅探：选对 dec_type，否则 open 成功也会一帧都解不出来
 * ============================================================ */
static esp_audio_simple_dec_type_t type_by_ext(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;

    if (strcasecmp(dot, ".mp3")  == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    if (strcasecmp(dot, ".wav")  == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    if (strcasecmp(dot, ".flac") == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    if (strcasecmp(dot, ".m4a")  == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    if (strcasecmp(dot, ".mp4")  == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    if (strcasecmp(dot, ".aac")  == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    if (strcasecmp(dot, ".ogg")  == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
    if (strcasecmp(dot, ".opus") == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
    if (strcasecmp(dot, ".ts")   == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_TS;
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

/* 扩展名认不出来时，看文件头前 16 字节再判一次
 *
 * ★★★ 10-06 第二十四次修（兰兰导入 143 台后全部播不出声，真凶之二）：
 *   旧代码第一行就是 `if (memcmp(b,"ID3",3)==0) return MP3;`。
 *   本地 MP3 这么判没错（ID3 是 MP3 的标签），但 HLS 直播切片是
 *   【ID3v2 标签 + ADTS 裸 AAC】—— 标签后面才是真正的音频帧。
 *   河北/河南这批台（stream.hndt.com 等）实测分片：
 *       49 44 33 04 00 00 00 00 00 3f 50 52 49 56 00 00 | ff f1 5c 40 ...
 *       └─ ID3v2(63 B) ──────────────────────────────┘└─ ADTS ─┘
 *   看到 ID3 就返回 MP3 ⇒ 解码器拿 MP3 帧头去啃 ADTS 帧 ⇒ 永远解不出
 *   声音，界面表现是「点了没反应 / container 显示 MP3」。
 *   ⇒ 修法：识别到 ID3v2 就【按 synchsafe 算出标签长度跳过去】，
 *     再对真正的帧头重新判定。标签最多 256 B 时安全（调用方给 16 B，
 *     跳不动就退回原行为，绝不越界读）。 */
static esp_audio_simple_dec_type_t type_by_header(const uint8_t *b, size_t n)
{
    if (n < 4) return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;

    /* ---- ID3v2：先跳过标签，判它【后面】是什么 ---- */
    if (memcmp(b, "ID3", 3) == 0 && n >= 10) {
        /* 大小是 4 字节 synchsafe（每字节只用低 7 位）*/
        uint32_t tagsz = ((uint32_t)(b[6] & 0x7F) << 21)
                       | ((uint32_t)(b[7] & 0x7F) << 14)
                       | ((uint32_t)(b[8] & 0x7F) << 7)
                       |  (uint32_t)(b[9] & 0x7F);
        size_t off = 10 + (size_t)tagsz;
        if (off + 4 <= n) {
            const uint8_t *p = b + off;
            if (p[0] == 0xFF && (p[1] & 0xF0) == 0xF0) {
                /* 标签后面是 ADTS ⇒ 这是 AAC 直播切片（不是 MP3）*/
                ESP_LOGI(TAG, "type_by_header: ID3v2(%u B) + ADTS ⇒ AAC",
                         (unsigned)tagsz);
                return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
            }
            if (p[0] == 0x47) {
                ESP_LOGI(TAG, "type_by_header: ID3v2(%u B) + TS ⇒ TS",
                         (unsigned)tagsz);
                return ESP_AUDIO_SIMPLE_DEC_TYPE_TS;
            }
            if ((p[1] & 0xF6) == 0xF0) {
                return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
            }
        }
        /* 跳不动（缓冲不够长）或标签后认不出 ⇒ 退回「按 MP3 判」。
         * 本地 MP3 走的就是这条，正确。*/
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
    if (b[0] == 0xFF && (b[1] & 0xE0) == 0xE0) {
        /* 0xFFFx 既可能是 MP3 也可能是 AAC-ADTS。
         * ADTS 的特征：layer 位 = 00 —— 用 b[1]&0xF6==0xF0 判别。*/
        if ((b[1] & 0xF6) == 0xF0) return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
    if (n >= 12 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WAVE", 4) == 0)
        return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    if (memcmp(b, "fLaC", 4) == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    if (memcmp(b, "OggS", 4) == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
    if (n >= 12 && memcmp(b + 4, "ftyp", 4) == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    if (b[0] == 0x47) return ESP_AUDIO_SIMPLE_DEC_TYPE_TS;   /* MPEG-TS 同步字节 */
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

/* 从 URL 猜类型：先砍掉 ?query，再看扩展名；猜不出就按 MP3（电台 99% 是 mp3）*/
static esp_audio_simple_dec_type_t type_by_url(const char *url)
{
    char tmp[320];
    snprintf(tmp, sizeof(tmp), "%s", url);
    char *q = strchr(tmp, '?');
    if (q) *q = '\0';
    /* 砍掉 query 再砍掉末尾的 /live/xxx/64k 这类 —— 扩展名一般在最后一段 */
    esp_audio_simple_dec_type_t t = type_by_ext(tmp);
    return (t == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) ? ESP_AUDIO_SIMPLE_DEC_TYPE_MP3 : t;
}

/* ============================================================
 *  电平表 + 频谱：直接量解码后的 PCM
 * ============================================================
 *  直播没有进度可言，播放页那个环闲着可惜 —— 拿它当电平表，
 *  屏幕上跳的就是此刻真实的音量，不是假动画。抽样计算，几乎不占 CPU。
 *
 *  ★ 频谱（兰兰 10-03 第七次反馈「播放页如果是动态频谱更好」）：
 *   用【Goertzel 算法】在 12 个对数分布的频点上测能量，而不是做完整 FFT。
 *   理由：FFT 要复数运算 + 位反转 + 512~1024 字节暂存；
 *   而 Goertzel 每个频点只要两个 float 状态，复用同一份加窗样本，
 *   12 个频点轮流跑一遍就得到 12 根柱子。
 *   频点按对数分布 50 Hz ~ 12 kHz：低频窄（人声基频在 200 Hz 上下）、
 *   高频宽，和听感一致，不会出现「全挤在左边」。
 *
 *   开销实测：每 256 个样本（44.1 kHz 立体声下约 3 ms）跑 12×256 = 3072 次乘加
 *   + 12 次 logf ⇒ 每秒约 1000 次 / 3.6 万次乘加。
 *   240 MHz 双核上占千分之几，且全部在播放任务里，不碰渲染任务。*/
#define SPEC_BINS   12
#define SPEC_N      256                /* Goertzel 取样点数（= 2 的幂，不补零）*/
#define SPEC_KMAX   (SPEC_N / 2 - 16)  /* 112：22 kHz 以下都够用 */

static float          s_spec_re[SPEC_BINS];    /* 递推系数 2*cos(w) */
static float          s_spec_f[SPEC_BINS];     /* 平滑后的 0~100 */
static float          s_spec_peak[SPEC_BINS];  /* 各频段滚动峰值（自适应标定用）*/
static volatile int   s_spec[SPEC_BINS];       /* 对外快照，UI 直接读（不阻塞）*/
static int16_t        s_spec_win[SPEC_N];      /* 加 Hann 窗后的样本 */
static uint8_t        s_spec_hann[SPEC_N];    /* 窗系数 0~255（256 字节，不占 RAM 大头）*/
static int            s_spec_fill = 0;         /* 环形缓冲已写样本数 */
static int            s_spec_ready = 0;        /* 窗表建好没有 */
static volatile int   s_spec_rate = 0;         /* 系数按这个采样率算的 */

int app_radio_spectrum(int i)
{
    if (i < 0 || i >= SPEC_BINS) return 0;
    return s_spec[i];
}

int app_radio_spectrum_bins(void) { return SPEC_BINS; }

/* Hann 窗系数表（0~255 的抛物线），建一次就够 */
static void spec_build_window(void)
{
    if (s_spec_ready) return;
    for (int k = 0; k < SPEC_N; k++) {
        /* 0.5*(1-cos(2*pi*k/N)) = sin^2(pi*k/N) */
        double s = sin(3.14159265358979 * (double)k / (double)SPEC_N);
        int v = (int)(s * s * 255.0);
        if (v < 0)   v = 0;
        if (v > 255) v = 255;
        s_spec_hann[k] = (uint8_t)v;
    }
    s_spec_ready = 1;
}

/* 采样率变了要重算 Goertzel 系数：w = 2*pi*k/N，k = round(f*N/rate) */
static void spec_set_rate(int rate)
{
    if (rate <= 0 || rate == s_spec_rate) return;
    s_spec_rate = rate;
    spec_build_window();
    for (int b = 0; b < SPEC_BINS; b++) {
        const double f0 = 50.0, f1 = 12000.0;
        double f = f0 * pow(f1 / f0, (double)b / (double)(SPEC_BINS - 1));
        int    k = (int)(f * (double)SPEC_N / (double)rate + 0.5);
        if (k < 1)        k = 1;
        if (k > SPEC_KMAX) k = SPEC_KMAX;
        double w = 2.0 * 3.14159265358979 * (double)k / (double)SPEC_N;
        s_spec_re[b] = (float)(2.0 * cos(w));
    }
    ESP_LOGI(TAG, "spectrum: %d bins, rate=%d", SPEC_BINS, rate);
}

/* 攒够一窗就跑一次频谱。纯浮点，播放任务里跑，不碰 LVGL。*/
static void spec_feed(const int16_t *p, int n)
{
    for (int i = 0; i < n; i++) {
        s_spec_win[s_spec_fill++] = p[i];
        if (s_spec_fill < SPEC_N) continue;
        s_spec_fill = 0;

        /* 1) 加窗（整数移位，够快也够准）*/
        for (int k = 0; k < SPEC_N; k++) {
            s_spec_win[k] = (int16_t)(((int32_t)s_spec_win[k] * s_spec_hann[k]) >> 8);
        }
        /* 2) 12 个频点各跑一遍 Goertzel 递推 */
        for (int b = 0; b < SPEC_BINS; b++) {
            const float coef = s_spec_re[b];
            float s1 = 0.0f, s2 = 0.0f;
            for (int k = 0; k < SPEC_N; k++) {
                float s0 = (float)s_spec_win[k] + coef * s1 - s2;
                s2 = s1;
                s1 = s0;
            }
            /* 功率 P = s1^2 + s2^2 - coef*s1*s2；除以 N^2 后开方 ≈ 幅度。
               ★ 这里【不能】用统一 dB 刻度：P 的量纲是「幅度平方」，
                 int16 满量程 32767 ⇒ P 最大约 2.7e8；而语音的共振峰
                 幅度常在 1000~3000 ⇒ P 已经 1e6~1e7，任何固定系数乘上去
                 取 log 都会爆表 ⇒ 低频 4 根恒为 100，只有高频在动
                 （10-03 兰兰真机看到的正是这个）。*/
            float p = (s1 * s1 + s2 * s2 - coef * s1 * s2) / (float)(SPEC_N * SPEC_N);
            if (p < 0.0f) p = 0.0f;
            float amp = sqrtf(p) * 2.0f;          /* 落在 0..32767 附近的幅度 */
            if (amp > 32767.0f) amp = 32767.0f;

            /* 本段滚动峰值：瞬间涨上去、慢慢落下来。
               每窗 -0.4%，一窗 5.8 ms ⇒ 半衰约 1 秒，回落跟手不迟钝。*/
            if (amp > s_spec_peak[b]) {
                s_spec_peak[b] = amp;
            } else {
                s_spec_peak[b] *= 0.996f;
                if (s_spec_peak[b] < 60.0f) s_spec_peak[b] = 60.0f;  /* 地板，防除零放大底噪 */
            }

            /* 柱高 = 本段当前 / 本段峰值 —— 每段各自归一。
               这样不管放语音还是音乐，12 根柱子都在动，
               而各段之间的相对高低仍然看得出来。*/
            int v;
            if (amp < 80.0f || s_spec_peak[b] < 150.0f) {
                v = 0;                            /* 太安静：直接归零，别放大底噪 */
            } else {
                v = (int)(amp * 100.0f / s_spec_peak[b]);
                if (v > 100) v = 100;
            }
            /* 平滑：升快降慢（和电平表一个脾气），不然柱子会抽筋 */
            s_spec_f[b] = (v > s_spec_f[b]) ? (float)v
                                             : (s_spec_f[b] * 0.6f + (float)v * 0.4f);
            s_spec[b]   = (int)s_spec_f[b];
        }
    }
}

static void level_update(const int16_t *p, int n)
{
    if (n <= 0) return;
    int peak = 0;
    for (int i = 0; i < n; i += 16) {
        int v = p[i] < 0 ? -p[i] : p[i];
        if (v > peak) peak = v;
    }
    int lv = peak * 100 / 32000;
    if (lv > 100) lv = 100;
    /* 升快降慢，看着才不像抽筋 */
    s_level = (lv > s_level) ? lv : (s_level * 7 + lv) / 8;

    spec_feed(p, n);
}

/* ============================================================
 *  HTTP 流
 * ============================================================ */
static bool http_open(const char *url, esp_http_client_handle_t *out)
{
    esp_http_client_config_t cfg = {
        .url               = url,
        .timeout_ms        = 8000,
        .buffer_size       = 2048,
        .buffer_size_tx    = 512,
        .keep_alive_enable = false,
        .disable_auto_redirect = false,
        /* ★★★ 10-03 第十六次：https 必须挂 CA 证书包，否则握手直接失败。
         *   兰兰报「电视伴音央视的都不能播放」—— 这就是原因：
         *   64 个 HLS 台全是 https，而 esp_http_client 不配 crt_bundle_attach
         *   就等于「不信任任何 CA」，mbedtls 找不到根证书 ⇒ 握手失败。
         *   库和证书包本来就编进固件了（CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y，
         *   build/esp-idf/mbedtls/x509_crt_bundle 已生成），只是没挂上去。
         *   代价：证书包在 flash 里约 60 KB，不占内部 RAM。*/
        .crt_bundle_attach  = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        ESP_LOGE(TAG, "http init failed");
        return false;
    }
    /* ★ 明确不要 ICY 元数据：要了的话流里会插「StreamTitle='…'」这种文本块，
     *   解码器会把它们当垃圾数据，听感就是周期性的咔哒声。*/
    esp_http_client_set_header(c, "Icy-MetaData", "0");

    esp_err_t e = esp_http_client_open(c, 0);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "http open failed: %s", esp_err_to_name(e));
        esp_http_client_cleanup(c);
        return false;
    }
    int64_t cl = esp_http_client_fetch_headers(c);
    int     sc = esp_http_client_get_status_code(c);
    ESP_LOGI(TAG, "stream connected: status=%d content_length=%lld", sc, (long long)cl);
    if (sc != 200) {
        ESP_LOGW(TAG, "stream status %d != 200", sc);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return false;
    }
    *out = c;
    return true;
}

/* 直播流断了就重连。
 * 返回 true = 已处理（连上了，或这次没连上但还没到上限）；false = 放弃。*/
static bool stream_reconnect(const char *url, esp_http_client_handle_t *http,
                             int *n_recon)
{
    if (*n_recon >= STREAM_RETRY_MAX) {
        ESP_LOGE(TAG, "stream reconnect gave up after %d tries", *n_recon);
        return false;
    }
    (*n_recon)++;
    ESP_LOGW(TAG, "stream lost, reconnect %d/%d ...", *n_recon, STREAM_RETRY_MAX);

    if (*http) {
        esp_http_client_close(*http);
        esp_http_client_cleanup(*http);
        *http = NULL;
    }
    for (int i = 0; i < 8 && !s_stop; i++) vTaskDelay(pdMS_TO_TICKS(250));
    if (s_stop) return false;

    if (http_open(url, http)) {
        /* ★ 第十八次：解码器已经不在这个任务里了（player_task 持有），
         *   所以【不能】在这里调 esp_audio_simple_dec_reset()。
         *   player 侧会在 ring 重新有数据时自行处理（见 ring 语义：
         *   重连成功 = ring 里先排空再填新数据，parser 自然重新对齐）。*/
        ESP_LOGI(TAG, "stream resumed");
        return true;
    }
    return true;        /* 这次没成，下一轮再试，直到 STREAM_RETRY_MAX */
}

/* ============================================================
 *  播放任务
 * ============================================================ */
/* ============================================================
 *  环形缓冲原语（第十八次新增）
 * ============================================================ */
static void ring_reset(void)
{
    s_ring.head = s_ring.tail = 0;
    s_ring.fill = 0;
    s_ring.eos  = false;
    s_ring.dead = false;
    s_ring.err  = 0;
    /* ★★ 第十九次：必须连信号量一起排空。
     *   第十八版只清了 head/tail/fill，**两个计数信号量的残值留着**，
     *   下一轮 ring_read 上来就误判「有数据可读」，
     *   拿到的是上一首的残留 ⇒ 行为不可预测（兰兰报的「换台后宕机」）。*/
    if (s_ring_sem)  while (xSemaphoreTake(s_ring_sem,  0) == pdTRUE) { }
    if (s_space_sem) while (xSemaphoreTake(s_space_sem, 0) == pdTRUE) { }
}

/* ★ 第十九次：把 ring 里已有的数据全丢掉（seek 时用）。
 *   和 ring_reset 不同——它【不动】eos/dead，只清数据与计数，
 *   因为 seek 之后源还在正常播放，两个标志必须保持。*/
static void ring_discard(void)
{
    s_ring.head = s_ring.tail = 0;
    s_ring.fill = 0;
    if (s_ring_sem)  while (xSemaphoreTake(s_ring_sem,  0) == pdTRUE) { }
    if (s_space_sem) while (xSemaphoreTake(s_space_sem, 0) == pdTRUE) { }
}

/* player 侧：从 ring 取数据，返回实际字节数。timeout_ms 到期返回 0。*/
static size_t ring_read(uint8_t *dst, size_t max, TickType_t timeout_ms)
{
    if (xSemaphoreTake(s_ring_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return 0;
    size_t n = s_ring.fill;
    if (n > max) n = max;
    for (size_t i = 0; i < n; i++) {
        dst[i] = s_ring.buf[s_ring.tail];
        s_ring.tail = (s_ring.tail + 1) % s_ring.cap;
    }
    s_ring.fill -= n;
    /* 放掉写侧的信号量（每 1 字节 1 个计数太浪费，改成按块归还）*/
    for (size_t i = 0; i < n; i++) xSemaphoreGive(s_space_sem);
    xSemaphoreGive(s_ring_sem);
    return n;
}

/* fetch 侧：往 ring 写，返回实际写入字节数。ring 满则等。*/
static size_t ring_write(const uint8_t *src, size_t n)
{
    size_t wrote = 0;
    while (wrote < n && !s_ring.eos && !s_ring.dead) {
        size_t space = s_ring.cap - s_ring.fill;
        if (space == 0) {
            /* ring 满：等 player 消化。短超时以便能响应 s_stop。*/
            if (xSemaphoreTake(s_space_sem, pdMS_TO_TICKS(20)) != pdTRUE) {
                if (s_stop) break;
                continue;
            }
            continue;
        }
        size_t chunk = n - wrote;
        if (chunk > space) chunk = space;
        for (size_t i = 0; i < chunk; i++) {
            s_ring.buf[s_ring.head] = src[wrote + i];
            s_ring.head = (s_ring.head + 1) % s_ring.cap;
        }
        s_ring.fill += chunk;
        wrote += chunk;
        xSemaphoreGive(s_ring_sem);
    }
    return wrote;
}

/* ============================================================
 *  ★★★ 第十八次：读网任务（fetch_task）
 * ============================================================
 * 职责：打开音源，往 ring 里填。**不碰解码器、不碰 I2S。**
 * 关键收益：等 RTT / 等切片 / TLS 握手全都发生在这个任务里，
 *           player 任务永远只在「有数据」时转一圈 ⇒ I2S 不断音。
 */
/* ---------------- 直播延迟自证（10-04）----------------
 * hls_state_t 是 fetch 任务的局部变量，UI 拿不到 ⇒ 把算出来的秒数
 * 摊到一个全局量里。fetch 任务写、UI 任务读，用原子操作最省事
 * （volatile int 在 ESP32 上是单字读写，天然原子；不配 volatile
 *  编译器会把循环里的读提到外面去）。0 = 不是 HLS 或判断不了。*/
static volatile int s_hls_lag_s = 0;

long app_radio_hls_lag_s(void)
{
    return (long)s_hls_lag_s;
}

static void fetch_task(void *arg)
{
    (void)arg;
    const char *target = s_arg_path;
    bool is_url = s_is_url;
    bool is_hls = is_url && strstr(target, ".m3u8") != NULL;

    FILE *fp = NULL;
    esp_http_client_handle_t http = NULL;
    hls_state_t *hls = NULL;
    uint8_t *tmp = heap_caps_malloc(READ_CHUNK, MALLOC_CAP_SPIRAM);
    if (!tmp) { s_ring.dead = true; goto out; }

    if (is_hls) {
        hls = heap_caps_calloc(1, sizeof(hls_state_t), MALLOC_CAP_SPIRAM);
        if (!hls) { s_ring.dead = true; goto out; }
        if (!hls_start(hls, target)) {
            ESP_LOGE(TAG, "fetch: hls_start failed: %s", hls_error(hls));
            snprintf(s_err, sizeof(s_err), "%s", hls_error(hls));
            s_ring.dead = true;
            goto out;
        }
        ESP_LOGI(TAG, "fetch: HLS started, %d segments", hls->seg_n);
    } else if (is_url) {
        if (!http_open(target, &http)) {
            snprintf(s_err, sizeof(s_err), "连不上这个电台");
            s_ring.dead = true;
            goto out;
        }
    } else {
        fp = fopen(target, "rb");
        if (!fp) {
            snprintf(s_err, sizeof(s_err), "打不开文件");
            s_ring.dead = true;
            goto out;
        }
    }

    int  n_stall = 0;
    int  n_recon = 0;
    int  s_read_slow = 0;      /* ★ 第十九次：单次读超过 300 ms 的次数（卡顿自证）*/
    bool eos = false;
    long fsz_local = 0;      /* 本地文件总长：seek 时按比例算字节偏移 */

    if (fp) {
        if (fseek(fp, 0, SEEK_END) == 0) fsz_local = ftell(fp);
        fseek(fp, 0, SEEK_SET);
    }

    while (!s_stop && !s_ring.dead) {
        /* --- ★★★ 第十九次：seek 由【本任务】执行（fp 归我，不归 player）---
         *   第十八版的 bug 就在这：fopen 搬进来了，seek 却留在 player_task，
         *   那个 fp 恒为 NULL ⇒ 一碰进度条就空指针重启。
         *   现在协议是单向的：player 通过 s_seek_req 发「第几秒」，
         *   我在自己的 fp 上 fseek，成功后置 s_seek_done=1 告诉 player。*/
        if (s_seek_req && fp) {
            s_seek_req = 0;
            int want = s_seek_sec;
            long pos = (long)((double)want / (double)(s_total > 0 ? s_total : 1)
                              * (double)fsz_local);
            pos -= 4096;                      /* 退回 4 KB，避免落在帧中间 */
            if (pos < 0) pos = 0;
            /* 丢掉 ring 里的旧数据：它们属于跳转前的位置 */
            ring_discard();
            if (fseek(fp, pos, SEEK_SET) == 0) {
                ESP_LOGI(TAG, "fetch: seek -> %d s (offset %ld/%ld)", want, pos, fsz_local);
                s_seek_done = 1;
            } else {
                ESP_LOGE(TAG, "fetch: fseek failed (offset %ld)", pos);
                s_seek_done = -1;
            }
        }

        size_t got = 0;
        if (is_hls) {
            while (got == 0 && n_recon < 3 && !s_stop) {
                if (!hls_pull(hls, tmp, READ_CHUNK, &got)) {
                    n_recon++;
                    ESP_LOGW(TAG, "fetch: hls pull failed, reload list (%d)", n_recon);
                    if (!hls_start(hls, target)) break;
                }
            }
            /*★★ 10-04：顺手把「直播落后几秒」算出来给屏上显示。
             *  兰兰两次报「像延迟」，但修复前是重播老片（落后几分钟）、
             *  现在是正常直播延迟（十几秒）——光听分不出来，数字能分。*/
            if (got > 0) {
                long lag = hls_lag_seconds(hls);
                if (lag != (long)s_hls_lag_s) {
                    s_hls_lag_s = (int)lag;
                    /* ⚠️ 别写 hls->seg[hls->seg_i] + 40 这种偏移：
                     *   文件名长度不可控，越界读会打成乱码甚至崩。*/
                    const char *sn = (hls->seg_i >= 0 && hls->seg_i < hls->seg_n)
                                     ? hls->seg[hls->seg_i] : "";
                    ESP_LOGI(TAG, "live lag = %ld s（当前片 %.48s）", lag, sn);
                }
            }
            if (got == 0 && n_recon >= 3) {
                snprintf(s_err, sizeof(s_err), "HLS 拉不到切片");
                s_ring.dead = true;
                break;
            }
        } else {
            s_hls_lag_s = 0;      /* 不是 HLS ⇒ 屏上不显示延迟 */
            /* ★★ 第十九次：给「卡顿」加自证——分别记「读端耗时」和「空等次数」。
             *   兰兰 10-03 报法国台「断断续续」。要定案必须分清两种情况：
             *     (a) 读端慢（网络/TLS/源不稳）→ 读一次要很久；
             *     (b) 读端快但写得慢（ring 满被挤）→ 读 0 ms 却卡在 ring_write。
             *   只看「卡顿次数」区分不了这两者，所以这里把耗时打出来。*/
            int t0 = xTaskGetTickCount();
            int n = is_url ? esp_http_client_read(http, (char *)tmp, READ_CHUNK)
                           : (int)fread(tmp, 1, READ_CHUNK, fp);
            int dt_ms = (int)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);
            if (dt_ms > 300) {
                s_read_slow++;
                if (s_read_slow % STALL_LOG_EVERY == 1) {
                    ESP_LOGW(TAG, "fetch: SLOW READ %d ms (total %d) src=%s",
                             dt_ms, s_read_slow, s_now);
                }
            }
            if (n > 0) {
                got = (size_t)n;
            } else if (is_url) {
                if (!stream_reconnect(target, &http, &n_recon)) {
                    snprintf(s_err, sizeof(s_err), "网络断了，重连失败");
                    s_ring.dead = true;
                    break;
                }
                continue;
            } else {
                eos = true;                 /* 文件读完 */
            }
            /* 卡顿自证：ring 快空了才来补，说明 fetch 跟不上消费 */
            if (is_url && s_ring.fill < RING_LOW) {
                n_stall++;
                if (n_stall % STALL_LOG_EVERY == 1) {
                    ESP_LOGW(TAG, "fetch: ring low x%d (fill=%u/%u) src=%s",
                             n_stall, (unsigned)s_ring.fill,
                             (unsigned)s_ring.cap, s_now);
                }
            }
        }
        if (got > 0) {
            ring_write(tmp, got);
        } else if (eos) {
            break;
        }
    }
    s_ring.eos = true;
    ESP_LOGI(TAG, "fetch: done (eos, %u B filled, stalls=%d, slow_reads=%d, reconnects=%d)",
             (unsigned)s_ring.fill, n_stall, s_read_slow, n_recon);

out:
    /* ★★★ 第十九次：「把等待方叫醒」必须放在【所有路径】的公共收尾处。
     *   第十八版只在 while 正常跑完后写 s_ring.eos = true，
     *   而上面 hls_start 失败 / http 打不开 / fopen 失败都是 `goto out`
     *   ——**直接跳过了那一行** ⇒ player_task 还在 ring_read(...,5000)
     *   里白等整整 5 秒；这 5 秒里 UI 已经发起了下一次 start，
     *   旧 player 醒来后继续往同一个 s_ring 里写 ⇒ 两个任务抢同一个
     *   ring 和同一个解码器 ⇒ 兰兰报的「HLS 失败后再按国内台就宕机」。
     *   ⇒ 无条件置位：不管正常读完还是失败退出，读侧都必须能醒过来。*/
    s_ring.eos = true;
    if (tmp) free(tmp);
    if (hls) { hls_stop(hls); free(hls); }
    if (http) { esp_http_client_close(http); esp_http_client_cleanup(http); }
    if (fp) fclose(fp);
    s_fetch_task = NULL;
    vTaskDelete(NULL);
}

static void player_task(void *arg)
{
    (void)arg;
    const char *target = s_arg_path;          /* 文件路径 或 URL */
    bool        is_url = s_is_url;

    FILE  *fp   = NULL;
    esp_http_client_handle_t http = NULL;
    uint8_t *inbuf  = NULL;
    uint8_t *pcmbuf = NULL;
    uint8_t *stbuf  = NULL;
    esp_audio_simple_dec_handle_t dec = NULL;
    uint64_t pcm_total = 0;
    int      n_err     = 0;
    int      n_recon   = 0;
    int      n_stall   = 0;   /* 卡顿计数（第十八次起由 fetch 任务打，player 只转发）*/
    size_t   in_len    = 0;
    bool     eos       = false;
    bool     rate_done = false;
    int      channels  = 2;
    long     fsz       = 0;

    /* ★ HLS（m3u8）支持：判 URL 里有没有 .m3u8。真正的拉流在 fetch_task。*/
    bool     is_hls    = is_url && strstr(target, ".m3u8") != NULL;
    hls_state_t *hls = NULL;      /* 第十八次起只在 fetch 任务里用，这里仅占位 */

    ESP_LOGI(TAG, "play %s: %s", is_url ? "URL" : "file", target);

    /* ---- 启动读网任务：它负责打开源并往 ring 填 ---- */
    if (s_ring.buf == NULL) {
        s_ring.buf = heap_caps_malloc(RING_CAP, MALLOC_CAP_SPIRAM);
        if (!s_ring.buf) {
            snprintf(s_err, sizeof(s_err), "内存不足");
            goto done;
        }
        s_ring.cap = RING_CAP;
    }
    ring_reset();
    if (s_ring_sem == NULL) s_ring_sem = xSemaphoreCreateCounting(RING_CAP, 0);
    if (s_space_sem == NULL) s_space_sem = xSemaphoreCreateCounting(RING_CAP, 0);
    if (!s_ring_sem || !s_space_sem) {
        snprintf(s_err, sizeof(s_err), "内存不足");
        goto done;
    }
    if (xTaskCreate(fetch_task, "xs_fetch", FETCH_STACK, NULL, 6, &s_fetch_task) != pdPASS) {
        s_fetch_task = NULL;
        snprintf(s_err, sizeof(s_err), "任务创建失败");
        ESP_LOGE(TAG, "create fetch task failed");
        goto done;
    }
    /* 本地文件的总时长：先问一次（进度条/跳转都要它）*/
    if (!is_url) {
        FILE *tf = fopen(target, "rb");
        if (tf) {
            if (fseek(tf, 0, SEEK_END) == 0) fsz = ftell(tf);
            fclose(tf);
        }
    }

    /* 三块缓冲都放 PSRAM：内部 RAM 要留给 WiFi/TCP，音频只是搬运，放 PSRAM 足够快 */
    inbuf  = heap_caps_malloc(IN_CHUNK,  MALLOC_CAP_SPIRAM);
    pcmbuf = heap_caps_malloc(PCM_CHUNK, MALLOC_CAP_SPIRAM);
    stbuf  = heap_caps_malloc(ST_CHUNK,  MALLOC_CAP_SPIRAM);
    if (!inbuf || !pcmbuf || !stbuf) {
        snprintf(s_err, sizeof(s_err), "内存不足");
        ESP_LOGE(TAG, "buffer alloc failed (need %d+%d+%d B in PSRAM)",
                 IN_CHUNK, PCM_CHUNK, ST_CHUNK);
        goto done;
    }

    /* ============================================================
     *  ★★★ 第十八次：源的全部准备工作（打开 http / SD / HLS）都搬进了
     *   fetch_task。本任务只做三件事：从 ring 取 → 解码 → 写 I2S。
     *   ★ 容器嗅探仍需要前几个字节，所以启动时先等 ring 里有数据。
     * ============================================================ */
    {
        /* ★ 第十九次：5000 → 1500 ms。第十八版白等 5 秒才退出，
         *   这 5 秒够 UI 发起下一次播放，两个任务就撞在一起了。
         *   现在 fetch 侧无论成败都会置 s_ring.eos（见 fetch_task 的 out:），
         *   所以这里等不满 1.5 秒也能靠 eos 正常收场。*/
        size_t got = ring_read(inbuf, IN_CHUNK, 1500);
        if (got == 0 && (s_ring.dead || s_ring.eos)) {
            if (s_err[0] == '\0') snprintf(s_err, sizeof(s_err), "连不上这个电台");
            ESP_LOGE(TAG, "player: fetch died before any data (dead=%d eos=%d)",
                     (int)s_ring.dead, (int)s_ring.eos);
            goto done;
        }
        in_len = got;
    }

    /* ---- 选容器类型：扩展名优先，认不出看文件头 ----
     * ★★ 2026-10-04 第二十一次改：HLS 【不再一律认 TS】。
     *   原注释说「HLS 一律认 TS，真正要解的是切片里的 MP3/AAC」——
     *   那是【只验过 63 个央视源时的结论】，因为它们全是 .ts 切片。
     *   10-04 扩充台单时电脑侧实测 387 个源里：
     *       .ts 切片 165 个 ｜ .aac 5 个 ｜ .mp3 3 个（另 203 个是 master list）
     *   ⇒ 那 8 个若按 TS 喂，解码器解不出东西（表现为「点了没反应」）。
     *   修法：先按内容判（type_by_header 能认出 TS/AAC/MP3），
     *        认不出才退回 TS（.ts 是绝大多数，兜底不会错）。
     *   type_by_header 的判定顺序：ID3/0xFF 同步字 → MP3，
     *        'ADTS'(0xFFF1) → AAC，0x47 → TS。 */
    esp_audio_simple_dec_type_t dtype =
        is_hls  ? ESP_AUDIO_SIMPLE_DEC_TYPE_NONE :   /* HLS 交给内容判 */
        is_url  ? type_by_url(target)             : type_by_ext(target);
    if (dtype == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
        /* ★★ 10-06：探测长度从 16 字节提到 512。
         *   原因：要跳过 ID3v2 标签才能认出标签后面的真实帧头，
         *   而实测河南这批台的标签就有 63 B（10-04 查 SD 卡的歌见过
         *   几百 KB 的）。16 字节连标签都跳不过去 ⇒ 修的那段代码等于白写。
         *   512 B 覆盖绝大多数标签；inbuf 在 PSRAM，多看几百字节零成本。*/
        size_t peek = in_len > 512 ? 512 : in_len;
        dtype = type_by_header(inbuf, peek);
    }
    if (dtype == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
        /* 内容也认不出 ⇒ HLS 兜底按 TS（非 HLS 才是真认不出） */
        if (is_hls) {
            dtype = ESP_AUDIO_SIMPLE_DEC_TYPE_TS;
            ESP_LOGW(TAG, "HLS: header unrecognized, assume TS");
        } else {
            snprintf(s_err, sizeof(s_err), "认不出音频格式");
            ESP_LOGE(TAG, "unknown container: %s", target);
            goto done;
        }
    }
    ESP_LOGI(TAG, "  container=%s  size=%ld B  first_chunk=%u B%s",
             esp_audio_simple_dec_get_name(dtype), fsz, (unsigned)in_len,
             is_hls ? " (HLS, by content)" : "");

    /* ★★★ 10-06 第二十四次修的【第二步】：把 ID3v2 标签从数据流里剥掉。
     *   上面只是「认出」了标签后面是 ADTS-AAC，但 inbuf 里那几十字节标签
     *   还在最前面。AAC 解码器按帧头（0xFFF）找帧，找不到就一路
     *   ESP_AUDIO_ERR_DATA_LACK 死等 ⇒ 类型判对了照样没声音。
     *   ⇒ 打开解码器【之前】把标签整段 memmove 掉，in_len 同步减。
     *   ★ 为什么不能靠解码器自己跳：esp_audio_simple_dec 的 AAC 路径
     *   不解析 ID3v2 —— 它假设输入从帧头开始（本地 MP3 是靠 mp3_duration_s()
     *   那条路跳的，直播这条路没有）。 */
    if (in_len >= 10 && memcmp(inbuf, "ID3", 3) == 0) {
        uint32_t tagsz = ((uint32_t)(inbuf[6] & 0x7F) << 21)
                       | ((uint32_t)(inbuf[7] & 0x7F) << 14)
                       | ((uint32_t)(inbuf[8] & 0x7F) << 7)
                       |  (uint32_t)(inbuf[9] & 0x7F);
        size_t taglen = 10 + (size_t)tagsz;
        if (taglen < in_len) {
            memmove(inbuf, inbuf + taglen, in_len - taglen);
            in_len -= taglen;
            ESP_LOGI(TAG, "  剥掉 ID3v2 标签 %u B ⇒ 剩 %u B 真实音频",
                     (unsigned)taglen, (unsigned)in_len);
        } else {
            /* 标签比这一批还长：整个缓冲都是标签，不能剥，
             * 否则 in_len 变 0，解码器拿不到任何数据。*/
            ESP_LOGW(TAG, "  ID3v2 标签 %u B 盖住整批(%u B)，等下一批",
                     (unsigned)taglen, (unsigned)in_len);
        }
    }

    esp_audio_simple_dec_cfg_t dcfg = {
        .dec_type      = dtype,
        .dec_cfg       = NULL,
        .cfg_size      = 0,
        .use_frame_dec = false,        /* 让 parser 自己找帧边界 */
    };
    esp_audio_err_t oret = esp_audio_simple_dec_open(&dcfg, &dec);
    if (oret != ESP_AUDIO_ERR_OK) {
        if (oret == ESP_AUDIO_ERR_NOT_SUPPORT) {
            snprintf(s_err, sizeof(s_err), "解码器不支持 %s",
                     esp_audio_simple_dec_get_name(dtype));
        } else {
            snprintf(s_err, sizeof(s_err), "解码器打开失败(%d)", (int)oret);
        }
        ESP_LOGE(TAG, "decoder open failed ret=%d", (int)oret);
        goto done;
    }
    if (is_url) ESP_LOGI(TAG, "stream: 电台已开播，开始解码");

    /* ★★ 第二十二次：倍速要在【解码器打开之后、开始写 PCM 之前】生效。
     *   换台时 app_audio 里记的 s_src_rate 可能还留着上一首的采样率，
     *   所以这里无条件再调一次：保证「换台后倍率不丢、且基准采样率是新的」。
     *   （放到主循环里调会被每帧的 rate_done 判断挡掉，只在第一帧生效。）*/
    {
        int sr0 = s_rate ? s_rate : 44100;
        app_audio_set_src_rate(sr0);
        ESP_LOGI(TAG, "  倍速 %d%%（源 %d Hz → I2S %d Hz）",
                 app_audio_get_speed(), sr0, sr0 * app_audio_get_speed() / 100);
    }

    /* ============================================================
     *  主循环
     * ============================================================ */
    while (!s_stop) {
        /* --- 暂停：不读源、不写 I2S。DMA 欠载由 auto_clear 补零 = 静音 --- */
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        /* --- ★ 进度条拖动 → seek（只在本地文件上有意义，直播不能跳）
         *   ★★ 第十九次（10-03 晚，唯一重启源）：本任务【不再碰 fp】。
         *     fp 由 fetch_task 持有（第十八版搬过去的），这里对它 fseek
         *     就是空指针 —— 这正是「本地一拖进度条立刻重启」的真因。
         *     现在：app_radio_seek() 发请求 → fetch_task 执行 fseek →
         *     置 s_seek_done → 本任务只负责【重建解码器 + 进度记账】。
         *   esp_audio 没有 seek API，所以解码器只能重建（约 200 ms）。*/
        if (s_seek_done == 1 && !is_url) {
            int want = s_seek_sec;
            s_seek_done = 0;
            /* 丢掉输入缓冲里已读但没解的旧数据（ring 那边 fetch 已清过）*/
            in_len = 0;
            eos     = false;
            /* 重建解码器（parser 状态机必须从头开始）*/
            esp_audio_simple_dec_close(dec);
            dec = NULL;
            esp_audio_err_t r2 = esp_audio_simple_dec_open(&dcfg, &dec);
            if (r2 != ESP_AUDIO_ERR_OK) {
                snprintf(s_err, sizeof(s_err), "跳转失败");
                ESP_LOGE(TAG, "seek reopen failed ret=%d", (int)r2);
                goto done;
            }
            s_elapsed = want;
            /* PCM 累计也要跟着回退，否则结束判据（pcm_total vs 总长）会错 */
            pcm_total = (uint64_t)want * (uint64_t)(s_rate ? s_rate : 44100) * 4ULL;
            /* 频谱/电平归零，避免用新位置的音频配旧峰值 */
            for (int b = 0; b < SPEC_BINS; b++) {
                s_spec_f[b]    = 0.0f;
                s_spec_peak[b] = 0.0f;
                s_spec[b]      = 0;
            }
            s_spec_fill = 0;
            ESP_LOGI(TAG, "player: seek applied at %d s", want);
        } else if (s_seek_done == -1) {
            s_seek_done = 0;
            snprintf(s_err, sizeof(s_err), "跳转失败");
        } else if (s_seek_req && is_url) {
            s_seek_req = 0;   /* 直播不支持跳转，丢弃 */
            s_seek_done = 0;
        }

        /* --- 取数据（第十八次：数据由 fetch_task 填进 ring，这里只取）---
         * ★ 不再在这里直接读 socket —— 那正是「卡顿」的根源：
         *   单任务里读网是阻塞的，阻塞期间一帧 PCM 都送不出去。
         *   现在 fetch 独立成任务，ring 满它会自己等，
         *   本任务只在「ring 里有数据」时转一圈。*/
        if (in_len < IN_CHUNK) {
            size_t want = (size_t)(IN_CHUNK - in_len);
            if (want > READ_CHUNK) want = READ_CHUNK;
            size_t got = ring_read(inbuf + in_len, want,
                                   s_ring.eos ? 10 : 60);
            in_len += got;
            if (got == 0 && s_ring.eos && s_ring.fill == 0) eos = true;
            if (got == 0 && s_ring.dead && s_ring.fill == 0) {
                if (s_err[0] == '\0') snprintf(s_err, sizeof(s_err), "电台没回数据");
                ESP_LOGE(TAG, "player: fetch dead (err_frames=%d)", n_err);
                break;
            }
        }
        if (in_len == 0 && eos) break;

        /* --- 解码一块 --- */
        esp_audio_simple_dec_raw_t raw = {
            .buffer = inbuf,
            .len    = (uint32_t)in_len,
            .eos    = eos,
        };
        esp_audio_simple_dec_out_t out = {
            .buffer = pcmbuf,
            .len    = PCM_CHUNK,
        };
        esp_audio_err_t r = esp_audio_simple_dec_process(dec, &raw, &out);

        /* --- 把已消耗的输入前移 --- */
        if (raw.consumed > 0 && raw.consumed <= in_len) {
            memmove(inbuf, inbuf + raw.consumed, in_len - raw.consumed);
            in_len -= raw.consumed;
        }

        if (r == ESP_AUDIO_ERR_DATA_LACK) {
            if (eos) break;
            continue;
        }
        if (r == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            ESP_LOGW(TAG, "output buffer not enough (need %u), frame skipped",
                     (unsigned)out.needed_size);
            if (eos && in_len == 0) break;
            continue;
        }
        if (r != ESP_AUDIO_ERR_OK && r != ESP_AUDIO_ERR_CONTINUE) {
            /* ★ 只在前几次打日志：一个坏流能刷屏几百行 */
            if (n_err < 3) ESP_LOGW(TAG, "decode ret=%d (skip)", (int)r);
            n_err++;
            if (eos && in_len == 0) break;
            continue;
        }
        if (out.decoded_size == 0) continue;

        /* --- 第一帧出来后才拿得到真实参数：对齐采样率 + 估总时长 --- */
        if (!rate_done) {
            esp_audio_simple_dec_info_t info = { 0 };
            if (esp_audio_simple_dec_get_info(dec, &info) == ESP_AUDIO_ERR_OK) {
                channels = info.channel ? (int)info.channel : 2;
                s_rate = (int)info.sample_rate;
                s_chan = channels;
                /* ⚠ bitrate 只用来打日志看码率，【不要拿去算时长】（见下）。
                 *   我曾经信注释说它是 kbps、也信头文件说它是 bps，两个都不对——
                 *   用它算出来的时长差 1000 倍。这行日志的 kbps 标法也存疑，
                 *   真正可信的码率是 mp3_duration_s() 从帧头读出来的。*/
                ESP_LOGI(TAG, "stream: %u Hz / %u ch / %u bit / dec-bitrate=%u",
                         (unsigned)info.sample_rate, (unsigned)info.channel,
                         (unsigned)info.bits_per_sample, (unsigned)info.bitrate);
                if (info.sample_rate && (int)info.sample_rate != s_sr_now) {
                    /* ★ 第二十二次：改用 app_audio_set_src_rate() ——
                     *   它会把 I2S 对齐到「源采样率 × 当前倍率」。
                     *   以前直接调 app_audio_set_sample_rate(源采样率)，
                     *   那样一切换到 1.2X 就会被换台/换流重置回 1.0X
                     *   （这一行每次拿到新流都跑一遍）。*/
                    if (app_audio_set_src_rate((int)info.sample_rate) == ESP_OK) {
                        s_sr_now = (int)info.sample_rate * app_audio_get_speed() / 100;
                    }
                }
                /* 频谱的 Goertzel 系数跟着采样率走（k = f*N/rate）*/
                spec_set_rate(s_sr_now ? s_sr_now : 44100);
                /* ★★★ 总时长【不要用 info.bitrate 估】（10-03 第十六次）
                 *   兰兰报「本地音频时间戳显示 50739:31 / 54536:04」——
                 *   根就是这个公式：info.bitrate 的单位在解码器路径上不一致，
                 *   我按 kbps 用就少 1000 倍、按 bps 用就对不上，
                 *   总之算出的是【放大 1000 倍】的假时长。
                 *   而且拖动时 s_elapsed 也用这个错值 ⇒ 两个数一起爆。
                 * ⇒ 改走 mp3_duration_s()：解析 MP3 帧头里的 Xing/Info 总帧数。
                 *   算不出来就返回 0，界面显示 --:--，绝不显示假数字。*/
                if (!is_url && fsz > 0) {
                    s_total = mp3_duration_s(s_path, fsz);
                    ESP_LOGI(TAG, "duration: %d s (file %ld B, mp3 header scan)", s_total, fsz);
                }
            } else {
                ESP_LOGW(TAG, "get_info failed, assume 44.1k/stereo");
            }
            rate_done = true;
        }

        /* --- 单声道 -> 双声道，再送给 I2S --- */
        if (channels == 1) {
            int frames = (int)(out.decoded_size / 2);
            if (frames > 0 && (size_t)frames * 4 <= ST_CHUNK) {
                const int16_t *s = (const int16_t *)pcmbuf;
                int16_t       *d = (int16_t *)stbuf;
                for (int i = 0; i < frames; i++) {
                    d[i * 2]     = s[i];
                    d[i * 2 + 1] = s[i];
                }
                level_update(d, frames * 2);
                app_audio_write(stbuf, (size_t)frames * 4);
            }
        } else {
            level_update((const int16_t *)pcmbuf, (int)(out.decoded_size / 2));
            app_audio_write(pcmbuf, out.decoded_size);
        }

        pcm_total  += out.decoded_size;
        s_elapsed   = (int)(pcm_total / (4ULL * (uint64_t)(s_rate ? s_rate : 44100)));

        /* 文件读完且缓冲已空 -> 正常收工（直播永远走不到这里）*/
        if (eos && in_len == 0) break;
    }

    if (pcm_total == 0 && s_err[0] == '\0') {
        snprintf(s_err, sizeof(s_err), "解不出音频数据");
        ESP_LOGE(TAG, "no PCM produced (n_err=%d) - wrong format or dead stream", n_err);
    }

    ESP_LOGI(TAG, "stopped (played %d s, %u MB PCM, err_frames=%d, reconnects=%d, stalls=%d)",
             s_elapsed, (unsigned)(pcm_total / (1024 * 1024)), n_err, n_recon, n_stall);

done:
    /* ★ 第十八次：必须让 fetch 任务先退出，再动它用过的资源。
     *   s_stop=1 会让它跳出循环，但 s_fetch_task=NULL 是它自己写的，
     *   所以这里要用标志 + 轮询，不能只看一次。*/
    s_stop = true;
    for (int i = 0; i < 200 && s_fetch_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_fetch_task) {
        ESP_LOGW(TAG, "fetch task did not exit, force kill");
        vTaskDelete(s_fetch_task);
        s_fetch_task = NULL;
    }

    if (dec) esp_audio_simple_dec_close(dec);
    if (hls) { hls_stop(hls); free(hls); }     /* list + seg_buf 有几百 KB，必须还 */
    if (http) { esp_http_client_close(http); esp_http_client_cleanup(http); }
    if (fp)  fclose(fp);
    if (inbuf)  free(inbuf);
    if (pcmbuf) free(pcmbuf);
    if (stbuf)  free(stbuf);

    s_now[0]   = '\0';
    s_elapsed  = 0;
    s_total    = 0;
    s_paused   = false;
    s_level    = 0;
    s_is_url   = false;
    s_station  = -1;
    /* 频谱也要归零：停播后 UI 若还画着上一次的高柱，看着像还在响 */
    for (int b = 0; b < SPEC_BINS; b++) {
        s_spec_f[b] = 0.0f;
        s_spec_peak[b] = 0.0f;
        s_spec[b]   = 0;
    }
    s_spec_fill = 0;
    s_task     = NULL;
    s_stop     = false;
    vTaskDelete(NULL);
}

/* ============================================================
 *  对外控制
 * ============================================================ */
void app_radio_stop(void)
{
    if (s_task == NULL) return;
    s_stop   = true;                    /* 任务自己会在下一轮循环退出 */
    s_paused = false;                   /* 别让它卡在暂停分支里不退出 */
}

void app_radio_set_paused(bool paused)
{
    if (s_task == NULL) return;
    s_paused = paused;
    ESP_LOGI(TAG, "%s", paused ? "paused" : "resumed");
}

void app_radio_toggle_pause(void)
{
    app_radio_set_paused(!s_paused);
}

/* 统一入口：停旧的 -> 等它退出 -> 起新的 */
static esp_err_t radio_start(const char *target, const char *display,
                             bool is_url, int station)
{
    if (target == NULL || target[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (!app_audio_ready()) {
        snprintf(s_err, sizeof(s_err), "音频未就绪");
        ESP_LOGW(TAG, "audio not ready, refuse to play");
        return ESP_ERR_INVALID_STATE;
    }
    if (is_url) {
        /* 电台必须有网 */
        extern bool app_net_connected(void);
        if (!app_net_connected()) {
            snprintf(s_err, sizeof(s_err), "WiFi 没连上");
            ESP_LOGW(TAG, "no network, cannot play station");
            return ESP_ERR_INVALID_STATE;
        }
        /* ★★★ 2026-10-03 第十七次删掉这段拦截（ironic 的教训）：
         *   第十五版我写了完整的 HLS 拉流器（app_hls.c），却忘了删第十三次
         *   留在这里的「HLS 放不了」返回码 —— 于是点央视台时
         *   根本没进 player_task，日志只留一行
         *       W XSRADIO: HLS stream not supported yet: ...
         *   ⇒ 表现为「央视还是不能播」，而拉流器根本没被调用过。
         *   ★ 教训：加了新能力，必须把「旧能力不存在」的那道门一起拆掉，
         *     否则新代码是死代码，日志还会把你引到「以为网络/证书有问题」。
         *   现在 m3u8 交给 player_task 里的 is_hls 分支处理。*/
    }

    /* 停掉当前播放，并等旧任务真正退出（否则两路会同时往 I2S 写，声音会糊）*/
    app_radio_stop();
    for (int i = 0; i < 150 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));       /* 最多等 3 秒（直播关连接要时间）*/
    }
    if (s_task != NULL) {
        snprintf(s_err, sizeof(s_err), "上一个还在收尾");
        ESP_LOGW(TAG, "previous player still busy, abort new request");
        return ESP_ERR_TIMEOUT;
    }

    s_err[0] = '\0';
    strncpy(s_arg_path, target, sizeof(s_arg_path) - 1);
    s_arg_path[sizeof(s_arg_path) - 1] = '\0';

    const char *disp = display;
    if (disp == NULL || disp[0] == '\0') {
        const char *base = strrchr(target, '/');
        disp = base ? base + 1 : target;
    }
    strncpy(s_now, disp, sizeof(s_now) - 1);
    s_now[sizeof(s_now) - 1] = '\0';

    s_elapsed = 0;
    s_total   = 0;
    s_paused  = false;
    s_stop    = false;
    s_level   = 0;
    s_is_url  = is_url;
    s_station = station;
    /* ★ 记住本地文件路径 —— 进度条 seek 要靠它重新打开文件。
     *   换台/换曲时一并清掉，免得残留上一首的路径。*/
    if (is_url) {
        s_path[0] = '\0';
    } else {
        strncpy(s_path, target, sizeof(s_path) - 1);
        s_path[sizeof(s_path) - 1] = '\0';
    }
    /* ★ 第十九次：换台/换曲时清掉 seek 请求与回执。
     *   旧代码这里是 `s_seek_req = -1;`，那是「秒数 = -1」的旧语义；
     *   新语义 s_seek_req 是标志位（1=有请求），复位要写 0。
     *   s_seek_done 也必须清 0，否则上一首的回执会让新的一首一上来就重建解码器。*/
    s_seek_req  = 0;
    s_seek_done = 0;
    s_seek_sec  = 0;
    /* 换台/换曲时频谱也要归零（不然上一首的余柱会闪一下）*/
    for (int b = 0; b < SPEC_BINS; b++) {
        s_spec_f[b] = 0.0f;
        s_spec_peak[b] = 0.0f;
        s_spec[b]   = 0;
    }
    s_spec_fill = 0;

    /* 栈给足：http 客户端 + 解码器都有较大的局部结构 */
    BaseType_t ok = xTaskCreate(player_task, "xs_play", 9216, NULL, 8, &s_task);
    if (ok != pdPASS) {
        s_task    = NULL;
        s_now[0]  = '\0';
        s_is_url  = false;
        s_station = -1;
        snprintf(s_err, sizeof(s_err), "任务创建失败");
        ESP_LOGE(TAG, "create player task failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t app_radio_play_file(const char *path)
{
    return radio_start(path, NULL, false, -1);
}

esp_err_t app_radio_play_url(const char *name, const char *url)
{
    return radio_start(url, name, true, -1);
}

esp_err_t app_radio_play_station(int idx)
{
    if (idx < 0 || idx >= app_st_count()) return ESP_ERR_INVALID_ARG;
    ESP_LOGI(TAG, "station %d/%d: %s", idx, app_st_count(), app_st_get(idx)->name);

    /* ★★ 记播放历史（兰兰：「如果有历史播放最好」）。
     *   放在这里而不是 UI 层：切台入口有 4 个
     *   （首页网格 / 播放页上下台 / 台单步进 / 收藏历史页），
     *   放 UI 层必然漏一个；放这儿只要走 play_station 就一定记到。
     *   ★ 只在 idx >= 0 时才记 —— app_radio_play_url 传的 idx 是 -1，
     *     那是「按 URL 播」不是「按台单播」，记进去解析不出台名。*/
    if (idx >= 0) app_hist_note(app_st_get(idx)->name);

    return radio_start(app_st_get(idx)->url, app_st_get(idx)->name, true, idx);
}

int app_radio_station_step(int step)
{
    if (app_st_count() <= 0) return -1;
    if (step == 0) step = 1;

    int cur = s_station;
    int next;
    if (cur < 0) {
        next = (step > 0) ? 0 : app_st_count() - 1;
    } else {
        next = ((cur + step) % app_st_count() + app_st_count()) % app_st_count();
    }
    if (app_radio_play_station(next) != ESP_OK) return -1;
    return next;
}

esp_err_t app_radio_play_next(const char *dir, int step, char *out_name, int out_name_sz)
{
    if (!dir || !dir[0]) return ESP_ERR_INVALID_ARG;
    if (step == 0) step = 1;

    char full[600];
    if (app_sd_dir_audio_nav(dir, app_radio_now_playing(), step, full, sizeof(full)) != 0) {
        snprintf(s_err, sizeof(s_err), "这一目录没有别的音频");
        return ESP_ERR_NOT_FOUND;
    }

    if (out_name && out_name_sz > 0) {
        const char *b = strrchr(full, '/');
        snprintf(out_name, (size_t)out_name_sz, "%s", b ? b + 1 : full);
    }
    return app_radio_play_file(full);
}
