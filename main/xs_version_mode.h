#pragma once
/*
 * 拾声 · 版本形态开关（发布版 / 自用版）的公共定义
 * ================================================================
 * ★★ 为什么需要这个头文件（10-06）：
 *   XS_FAV_PUBLISH 现在有三个文件要用：
 *     app_fav.c      收藏播种名单（发布版不播种）
 *     app_stlist.c   st_net 的 ?insecure 开关（发布版编译期去掉）
 *     app_version.h  版本排障笔记（发布版只留一句话）
 *   而它的定义处是 CMakeLists.txt 末尾的 target_compile_definitions ——
 *   【编译命令行上的宏，不是头文件】，所以每个 .c 要用就得
 *     ① 自己 include app_version.h，或者
 *     ② 各自写一遍 #ifndef 兜底。
 *   ② 会出现「漏了一个文件」的隐患，而漏了的后果是
 *      【发布版悄悄按自用版编】—— 正好是这次要根除的那类事故（铁律 42/43）。
 *   ⇒ 统一在这里兜底，各 .c 只 include 这一个头。
 *
 *   ⚠ 兜底值必须是 0（自用版）：拿不到定义时按「多播种、留排障信息」
 *     处理更安全，发布形态一定会由 CMake 显式传 1。
 *
 * ★ 权威定义处仍是 CMakeLists.txt 的那个 if/else —— 那里同时决定
 *   台单、字库、播种名单、insecure 开关。四件事必须同源（铁律 43）。
 */

/* 发布版 = 1：空台单 + 不播种 + 无 ?insecure + 简短版本说明 */
#ifndef XS_FAV_PUBLISH
#  define XS_FAV_PUBLISH 0
#endif
