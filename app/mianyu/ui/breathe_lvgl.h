/* SPDX-License-Identifier: Apache-2.0
 *
 * 安眠科技 · 呼吸光晕 UI 接口
 *
 * breathe_lvgl.c 是纯渲染壳，只做「把亮度画成呼吸光晕」。
 * 本头文件把它的接口暴露给 HAL 后端（hal/sf32lb52/mianyu_hal_vela.c），
 * 让主循环的 my_hal_light_set(level) 能真正落到屏上。
 *
 * 线程约定（重要）：
 *   breathe_create / breathe_start / breathe_set_level 都【只能在 LVGL 线程】
 *   调用。跨线程投递请用 lv_async_call 或 lvgl 线程自己的定时器——
 *   LVGL 不是线程安全的。
 */
#ifndef MIANYU_BREATHE_LVGL_H
#define MIANYU_BREATHE_LVGL_H

#include <lvgl/lvgl.h>
#include <stdbool.h>

#include "mianyu_breathe.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 呼吸光晕控件上下文（结构体定义在 breathe_lvgl.c 内，外部只持指针） */
typedef struct breathe_widget_s breathe_widget_t;

/**
 * @brief 在全屏容器上创建呼吸光晕（三层同心圆 + 相位文字 + 底部提示）。
 *
 * @param parent 父对象（通常是 lv_scr_act()），函数内会把父对象底色刷成近黑。
 * @param cfg    呼吸引擎配置；传 NULL 用引擎默认（4-7-8）。
 * @return 控件句柄，失败返回 NULL。
 */
breathe_widget_t *breathe_create(lv_obj_t *parent, const my_breathe_cfg_t *cfg);

/**
 * @brief 启动渲染定时器（100ms 一拍，与主循环 TICK_MS 对齐）。
 * @return LVGL 定时器句柄，可用 lv_timer_del 停止；失败返回 NULL。
 */
lv_timer_t *breathe_start(breathe_widget_t *w);

/**
 * @brief 外部亮度驱动：把主循环算出的亮度（0..100）显示出来。
 *
 * 调用后控件进入「外部驱动」模式：渲染定时器不再使用内部引擎的亮度，
 * 只保留引擎用于推进相位文字。这让 UI 的节奏完全跟随主循环的
 * my_breathe 实例，避免两套引擎各跑各的导致光影与音频、日志对不上。
 *
 * @param w         控件句柄（可为 NULL，空操作）。
 * @param level_pct 亮度百分比，0..100。0 表示灯灭（光晕收到最低透明度）。
 */
void breathe_set_level(breathe_widget_t *w, int level_pct);

/**
 * @brief 切回内部引擎驱动（一般不用，保留给独立 demo 场景）。
 */
void breathe_set_level_auto(breathe_widget_t *w);

/**
 * @brief 切换夜间模式：峰值压暗 + 谷值更低（只改引擎配置，不打断节奏）。
 */
void breathe_set_night_mode(breathe_widget_t *w, bool night);

#ifdef __cplusplus
}
#endif

#endif /* MIANYU_BREATHE_LVGL_H */
