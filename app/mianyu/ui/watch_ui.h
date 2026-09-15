/* SPDX-License-Identifier: Apache-2.0
 *
 * 安眠科技 · 手表界面入口
 *
 * 只有一个对外函数：把界面建起来。必须从 LVGL 线程调用
 * （真机上的做法是主循环第一次推快照时用 lv_async_call 投过来，
 * 见 hal/sf32lb52/mianyu_hal_vela.c 的 my_hal_ui_update）。
 */
#ifndef WATCH_UI_H
#define WATCH_UI_H

#ifdef __cplusplus
extern "C" {
#endif

/* 建表盘页和哄睡页，开 100ms 刷新定时器。重复调用无副作用。 */
void watch_ui_start(void);

#ifdef __cplusplus
}
#endif
#endif /* WATCH_UI_H */
