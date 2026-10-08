/*
 * 灵签 · 签文数据接口
 *
 * ★ 数据本体由生成器产出（lingqian_data.c），本文件只定接口。
 *   换签文体系（观音灵签 / 关帝灵签 / 黄大仙 / 月老）时，
 *   只改数据源 + 重跑生成器，界面代码一行不动。
 */
#pragma once

/* 签诗固定 4 句（传统观音灵签是 4 句 × 7 言，100 签无例外） */
#define LQ_POEM_LINES 4

/*
 * 一条签文 —— 字段与【传统观音灵签】的签条结构一一对应：
 *   吉凶 · 宫位 · 签诗 · 诗意 · 解曰 · 典故名 · 典故详述
 *
 * 数据来源与正确性核对见 firmware/_gen_lingqian.py 顶部注释。
 * ★ gushi（典故详述）有 5 条为空 —— 界面侧遇空串【不要建控件】。
 */
typedef struct {
    const char *level;                 /* 吉凶：上 / 中 / 下 */
    const char *gong;                  /* 宫位：子宫 / 丑宫 …（十二宫） */
    const char *poem[LQ_POEM_LINES];   /* 签诗，4 句 × 7 字（UTF-8） */
    const char *story;                 /* 典故名，如「钟离成道」 */
    const char *shiyi;                 /* 诗意：此卦……之象。凡事……也。 */
    const char *jieyi;                 /* 解曰：八字骈句，如「急速兆速。年未值时…」*/
    const char *gushi;                 /* 典故详述（约 90 字，可能为空串）*/
} lq_entry_t;

/* 取第 idx 签（0 起）。越界返回第 0 签，永不返回 NULL。
 * ★ 字段本身可能为空串（尤其 gushi）。*/
const lq_entry_t *lq_get(int idx);

/* 签的总数（本签筒共几签） */
int lq_count(void);
