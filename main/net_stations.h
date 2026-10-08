#pragma once

/* ============================================================
 *  拾声 (XianDial) 硬件版 · 转发头（发布版专用，合成件）
 * ============================================================
 *  为什么有这个文件：
 *    源码里 5 处写死 #include "net_stations.h" ——
 *      app_stlist.h / app_fav.c / app_hist.c / app_radio.c / ui_xiandial.c
 *    而发布版只有 net_stations_pub.h（0 条内置台单）。
 *    两个头的声明【逐行完全相同】（net_station_t / g_stations /
 *    g_station_count / g_cat_name / g_prov_name），所以转发即可。
 *
 *  ⚠️ 本文件不是工作树里的那个同名文件。
 *     工作树里的 main/net_stations.h 是【自用版实体】（含作者自己的台单），
 *     被 .gitignore 与发布脚本排除名单双重挡着，永不进仓库。
 *     本文件由 firmware/_gh_publish.py 在上传时【以内存字节合成】；
 *     GitHub 上这份从没在磁盘上存在过。
 *
 *  ⚠️ 两者同名、内容完全不同 ⇒ 判据要落在【来源】上，不能落在文件名上。
 */
#include "net_stations_pub.h"
