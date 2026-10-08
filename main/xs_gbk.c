#include "xs_gbk.h"

#include <stdint.h>

/* 由 _gen_gbk.py 生成的表：xs_gbk_uni[ (hi-0xA1)*94 + (lo-0xA1) ] */
extern const uint16_t xs_gbk_uni[];
extern const int      xs_gbk_uni_len;

#define GB_LO  0xA1
#define GB_HI  0xFE
#define GB_W   94

static int put_utf8(uint32_t cp, char *dst, size_t o, size_t cap)
{
    if (o + 4 > cap) return -1;
    if (cp < 0x80) {
        dst[o++] = (char)cp;
    } else if (cp < 0x800) {
        dst[o++] = (char)(0xC0 | (cp >> 6));
        dst[o++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        dst[o++] = (char)(0xE0 | (cp >> 12));
        dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[o++] = (char)(0x80 | (cp & 0x3F));
    } else {
        dst[o++] = (char)(0xF0 | (cp >> 18));
        dst[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[o++] = (char)(0x80 | (cp & 0x3F));
    }
    return (int)o;
}

int xs_gbk_to_utf8(const char *src, int n, char *dst, size_t cap)
{
    if (!dst || cap == 0) return 0;
    size_t o = 0;

    for (int i = 0; i < n && src[i]; ) {
        unsigned char c = (unsigned char)src[i];
        uint32_t cp = 0;
        int adv = 1;

        if (c < 0x80) {
            cp = c;                          /* ASCII 原样透传 */
        } else if (c >= GB_LO && c <= GB_HI && i + 1 < n) {
            unsigned char c2 = (unsigned char)src[i + 1];
            if (c2 >= GB_LO && c2 <= GB_HI) {
                int idx = (c - GB_LO) * GB_W + (c2 - GB_LO);
                cp = (idx >= 0 && idx < xs_gbk_uni_len) ? xs_gbk_uni[idx] : 0;
                if (cp == 0) cp = '?';
                adv = 2;
            } else {
                cp = '?';                    /* 孤立的引导字节 */
            }
        } else {
            cp = '?';                        /* 落不到任何合法区间 */
        }

        int r = put_utf8(cp, dst, o, cap);
        if (r < 0) break;                    /* dst 满了就停，不越界 */
        o = (size_t)r;
        i += adv;
    }

    dst[o] = '\0';
    return (int)o;
}
