/* SPDX-License-Identifier: Apache-2.0
 *
 * 安眠科技 · 呼吸节律灯光引擎（哄睡灯的"节拍器"）
 *
 * ====================== 为什么灯光需要一个独立引擎 ======================
 *
 * 哄睡灯不是"装饰灯"，是【节拍器】：用户跟着光的明暗来调整呼吸节奏，
 * 把注意力从"我睡不着"的念头转移到"光在带我呼吸"上——这是多种放松
 * 疗法里"引导式呼吸"的视觉化。灯光的节奏必须严格、可重复、不刺眼，
 * 这三条决定了它需要一个专门的包络引擎，而不是 UI 里随手写个动画：
 *
 *   ① 严格：4-7-8 呼吸法要求"吸 4 秒、屏 7 秒、呼 8 秒"。一个周期 19 秒，
 *      连续循环。节奏乱了，用户就要睁眼看屏幕核对——违背"闭眼可用"。
 *   ② 可重复：同一配置每次启动的明暗曲线必须一致（合成信号同理，便于
 *      测试与"记住用户偏好"）。
 *   ③ 不刺眼：深夜卧室里灯突然全灭/全亮都是惊吓。本引擎保证曲线在
 *      段边界处连续（smoothstep 首尾导数为 0），并允许配置"谷值亮度"
 *      （呼气底不全灭，保持微光，避免"灯灭了"的突兀感）。
 *
 * ====================== 光是怎么带呼吸的（产品逻辑） ======================
 *
 *   吸气（4s）→ 光从谷值渐亮到峰值    （你跟着光吸气）
 *   屏息（7s）→ 光保持峰值            （你跟着光的"停"保持）
 *   呼气（8s）→ 光从峰值渐暗到谷值    （你缓缓吐气，比吸气更慢）
 *   停顿（rest，可 0）→ 光保持谷值
 * 峰值/谷值都是可配置百分比——夜间模式会把峰值压到 ~30%、谷值 ~4%，
 * 既带节奏又不刺眼；晨唤模式反过来用高一点的峰值模拟"光唤"。
 *
 * ====================== 可移植性（沿用 fade/schedule 的验证模式） ======================
 *
 * 引擎【不调用任何 OS 时间函数】：时间由调用方按 tick(delta_ms) 喂入。
 * LVGL 光晕层只是把引擎输出的 level_pct 画成一个呼吸圆环（见
 * app/ui/breathe_lvgl.c），换平台只换画法，节奏算法一字不改。
 * 因此可以在 PC 上完整单元测试（tests/test_breathe.c），
 * 也能写 HTML 预览把曲线画出来给评审/用户看（docs/breathe_preview.html）。
 */
#ifndef MIANYU_BREATHE_H
#define MIANYU_BREATHE_H

#include "mianyu_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 呼吸相位 ---- */
typedef enum {
    MY_BREATH_INHALE = 0,   /* 吸气段：亮度上升 */
    MY_BREATH_HOLD,         /* 屏息段：亮度保持峰值（引导"停住"） */
    MY_BREATH_EXHALE,       /* 呼气段：亮度下降（比吸气慢） */
    MY_BREATH_REST,         /* 呼气后停顿：亮度保持谷值 */
} my_breath_phase_t;

/* ---- 配置（全时长毫秒 + 亮度百分比） ---- */
typedef struct {
    int32_t inhale_ms;      /* 吸气时长。默认 4000 */
    int32_t hold_ms;        /* 屏息时长（光保持峰值）。默认 7000 */
    int32_t exhale_ms;      /* 呼气时长。默认 8000 */
    int32_t rest_ms;        /* 呼气后停顿。默认 0（4-7-8 无缝循环） */
    uint8_t peak_pct;       /* 峰值亮度 0..100。默认 60（睡前低亮） */
    uint8_t trough_pct;     /* 谷值亮度 0..100。默认 4（不全灭，防"灯灭了"突兀） */
} my_breathe_cfg_t;

/* ---- 引擎状态 ---- */
typedef struct {
    my_breathe_cfg_t cfg;
    int64_t          elapsed_ms;   /* 距启动的累计时间（毫秒） */
} my_breathe_t;

MY_STATIC_ASSERT(sizeof(my_breathe_t) < 64, breathe_state_is_tiny);

/* ---- 默认配置：4-7-8 呼吸法（19s 周期） ---- */
void my_breathe_default_cfg(my_breathe_cfg_t *cfg);

/* 初始化。cfg 传 NULL 用默认 4-7-8。 */
my_err_t my_breathe_init(my_breathe_t *b, const my_breathe_cfg_t *cfg);

/* 推进 delta_ms。返回 MY_OK；delta<0 返回 MY_ERR_PARAM。 */
my_err_t my_breathe_tick(my_breathe_t *b, int32_t delta_ms);

/* 复位到 0 时刻（重新开始一个周期）。保留配置。 */
void my_breathe_reset(my_breathe_t *b);

/* ---- 查询 ---- */
/* 当前亮度 0..100（peak_pct/trough_pct 之间的插值） */
int my_breathe_level_pct(const my_breathe_t *b);

/* 当前相位 */
my_breath_phase_t my_breathe_get_phase(const my_breathe_t *b);

/* 本周期内的进度 0..1（周期 = 吸+屏+呼+停 总长） */
float my_breathe_cycle_progress(const my_breathe_t *b);

/* 一个完整周期的毫秒数 */
int32_t my_breathe_cycle_ms(const my_breathe_t *b);

#ifdef __cplusplus
}
#endif
#endif /* MIANYU_BREATHE_H */
