/* SPDX-License-Identifier: Apache-2.0
 *
 * 安眠科技 · 手表 UI 状态接口
 *
 * ===================== 这层是干什么的 =====================
 *
 * 界面要显示的东西（几点了、睡着没有、音量多少），来源是主循环里的
 * 那几个核心模块。主循环和 LVGL 是两个线程，直接互相调用对象会崩。
 * 所以这里定一个【纯数据快照】：主循环每 100ms 往里写一次，UI 定时器
 * 每 100ms 读一次。两边只碰这一个结构体，不碰对方的对象。
 *
 * 结构体里全是 int（浮点量按 x10 / x100 定点传），这样拷贝在两个线程
 * 之间是安全的，也不用担心 ABI 上的浮点对齐差异。
 *
 * 数据流：
 *   主循环 ── my_hal_ui_update(快照) ──▶ 平台层暂存
 *   LVGL 定时器 ── my_hal_ui_state() ──▶ 拿到快照，刷新界面
 *   界面点按钮 ── my_hal_ui_request_start() ──▶ 主循环 take 走并执行
 */
#ifndef MIANYU_UI_H
#define MIANYU_UI_H

#include "mianyu_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 哄睡流程的阶段。IDLE 之外的四步就是产品上能看到的四个状态。 */
typedef enum {
    MY_UI_PHASE_IDLE = 0,    /* 还没开始今晚（表盘待机） */
    MY_UI_PHASE_GUIDING,     /* 引导中：光晕带着呼吸，噪声已起 */
    MY_UI_PHASE_DETECTING,   /* 判定中：在听呼吸节律，判断睡没睡 */
    MY_UI_PHASE_ASLEEP,      /* 已睡着：屏幕暗下去，只留微光 */
    MY_UI_PHASE_NIGHT_WAKE,  /* 夜醒安抚：微光 + 轻声，不吵醒 */
} my_ui_phase_t;

/* 界面用的一次快照 */
typedef struct {
    my_ui_phase_t phase;

    /* 时间 */
    int  hour, minute;
    int  month, day, weekday;      /* weekday: 0=周日, 1=周一 ... */

    /* 今晚计划（主动式调度的体现：不用点，到点自己开始） */
    bool plan_valid;
    int  plan_hour, plan_min;

    /* 正在哄睡时的实时量 */
    int  light_pct;                /* 呼吸灯亮度 0..100 */
    int  volume_pct;               /* 噪声音量 0..100 */
    int  aid_kind;                 /* 0=棕噪 1=白噪 2=粉噪 */
    int  elapsed_sec;              /* 本次开始到现在 */
    int  night_wakes;              /* 今夜夜醒次数 */

    /* 上一晚的结果（表盘上那行简报） */
    bool last_valid;
    int  last_total_min;
    int  last_onset_sec;
    int  last_wakes;
} my_ui_state_t;

/* ---- 平台层实现（真机在 hal/sf32lb52，PC 在 hal/sim）---- */

/* 主循环写：推一份新快照。首次调用会顺便把界面创建出来。 */
void my_hal_ui_update(const my_ui_state_t *st);

/* UI 线程读：拿最近一份快照。永不为 NULL。 */
const my_ui_state_t *my_hal_ui_state(void);

/* 界面上点了「开始哄睡」 */
void my_hal_ui_request_start(void);

/* 主循环取走这个请求；没有则返回 false，取走后自动清掉 */
bool my_hal_ui_take_start_request(void);

/* 界面上长按「返回」= 结束今晚（停止声音、复位状态） */
void my_hal_ui_request_stop(void);
bool my_hal_ui_take_stop_request(void);

#ifdef __cplusplus
}
#endif
#endif /* MIANYU_UI_H */
