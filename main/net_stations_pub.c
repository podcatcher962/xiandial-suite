/* 自动生成，勿手改 ---- firmware/_gen_stations_all.py --publish */
#include "net_stations_pub.h"

const char *const g_cat_name[NET_CAT_N] = {
    "新闻综合", "交通台", "音乐", "文艺", "说书", "戏曲", "怀旧老歌", "网络台", "教育台", "电视伴音", "综合", "宗教", "境外新闻",
};

const char *const g_prov_name[NET_PROV_N] = {
    "北京", "天津", "河北", "山西", "内蒙古", "辽宁", "吉林", "黑龙江", "上海", "江苏", "浙江", "安徽", "福建", "江西", "山东", "河南", "湖北", "湖南", "广东", "广西", "海南", "重庆", "四川", "贵州", "云南", "西藏", "陕西", "甘肃", "青海", "宁夏", "新疆", "中国台湾", "中国香港", "中国澳门", "其他华语", "北美", "欧洲", "日韩", "新马", "东南亚", "大洋洲", "海外中文",
};

/* ★ 台数为 0（发布版）。UI 与 app_stlist.c 都要用 g_station_count，
 *   少这个符号会在链接期报 undefined reference。 */
const int g_station_count = 0;

/* ★ 发布版：0 条台源。
 *   数组不能为空（C 里零长数组是非标准），给一个占位元素，
 *   但 g_station_count = 0，它永远不会被访问到。 */
const net_station_t g_stations[1] = {
    { "", "", 0, 0 },
};
