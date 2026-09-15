/* SPDX-License-Identifier: Apache-2.0
 *
 * 安眠科技 · 呼吸光晕 LVGL 渲染层（openvela 移植资产）
 *
 * ============================ 职责边界 ============================
 *
 * 本文件是【纯渲染壳】：节奏来自 mianyu_breathe 引擎（include/src，
 * 平台无关、已在 PC 上 41 项单测验证），本层只负责把引擎输出的
 * level_pct(0..100) 画成一个"呼吸光晕"：
 *
 *   ┌──────────────────────────┐
 *   │    (外晕 obj：暖橙、低透明度)   │
 *   │      (中光 obj：较大、中透明)   │
 *   │        (内核 obj：小、高亮)     │
 *   │    [相位文字：吸气/屏息/呼气]    │
 *   └──────────────────────────┘
 *
 * 三层同心圆全部居中叠放，通过"外大内小 + 透明度差异"叠出光晕的
 * 渐变质感（LVGL 无径向渐变对象时的可靠做法）。亮度变化 = 整组 obj
 * 的 opacity 随引擎走，同时内核 obj 微微放大模拟"呼吸鼓起"。
 *
 * 设计要点（对位技术报告产品差异点）：
 *   • 深夜卧室光污染最小化：主界面纯黑底 + 单色暖光（约 2700K 琥珀），
 *     不渲染任何文字以外的元素。文字用极低亮度灰，相位指示靠光的
 *     明暗本身（用户闭眼也能跟——这正是"节拍器"的意义）。
 *   • 亮度上限由引擎 trough/peak 管，渲染层只做线性映射，不做额外增益，
 *     保证夜间模式峰值压暗后不会在这里被"提亮"回去。
 *
 * 编译位置：本文件含 <lvgl/lvgl.h>，只在 openvela 工程内编译（PC 不编）。
 * 移植时把 engine(include/mianyu_breathe.h + src/mianyu_breathe.c) 一起带过去，
 * 节奏算法不进本文件，避免 UI 线程抖动影响节奏精度。
 */
#include <lvgl/lvgl.h>

#include "mianyu_breathe.h"

/* ---- 颜色与布局（黄山派 AMOLED 390×450；模拟器按同一相对比例） ---- */
#define MB_WARM_AMBER       lv_color_hex(0xFFB26B)   /* ~2700K 暖光 */
#define MB_BG_NEAR_BLACK    lv_color_hex(0x050505)   /* AMOLED 纯黑省电且零光晕 */
#define MB_TEXT_DIM         lv_color_hex(0x555555)   /* 低亮度灰（不抢眼） */

typedef struct {
    /* 引擎实例（节奏源） */
    my_breathe_t      engine;
    /* LVGL 对象：三层光晕 + 相位文字 */
    lv_obj_t         *halo_outer;    /* 最大，最暗 */
    lv_obj_t         *halo_mid;
    lv_obj_t         *halo_core;     /* 最小，最亮 */
    lv_obj_t         *label_phase;
    lv_obj_t         *label_hint;    /* 底部：呼吸提示 */
    /* 布局缓存 */
    lv_coord_t        cx, cy;        /* 光晕圆心 */
    lv_coord_t        r_base;        /* 内核基准半径 */
} breathe_widget_t;

/* 相位 → 中文提示 */
static const char *phase_text(my_breath_phase_t p)
{
    switch (p) {
    case MY_BREATH_INHALE: return "吸气……";
    case MY_BREATH_HOLD:   return "屏息……";
    case MY_BREATH_EXHALE: return "呼气……";
    default:               return "放松";
    }
}

/* 让三层圆叠出一个同心光晕：外大内小、内亮外暗 */
static lv_obj_t *make_halo_layer(lv_obj_t *parent, lv_coord_t size,
                                 lv_opa_t opa, lv_color_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);                    /* 去掉默认边框背景 */
    lv_obj_set_size(o, size, size);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_align(o, LV_ALIGN_CENTER, 0, 0);
    return o;
}

/* 创建呼吸光晕控件。parent 为全屏容器。返回上下文指针。 */
breathe_widget_t *breathe_create(lv_obj_t *parent, const my_breathe_cfg_t *cfg)
{
    breathe_widget_t *w = lv_malloc(sizeof(breathe_widget_t));
    LV_ASSERT_MALLOC(w);
    if (!w) return NULL;
    lv_memset(w, 0, sizeof(*w));

    /* 引擎初始化（节奏源，纯 C） */
    my_breathe_init(&w->engine, cfg);

    /* 背景纯黑（AMOLED 像素级熄灭，光晕区域外零光污染） */
    lv_obj_set_style_bg_color(parent, MB_BG_NEAR_BLACK, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    /* 画布尺寸：取父容器较小边，光晕直径约为其 72% */
    lv_coord_t pw = lv_obj_get_width(parent);
    lv_coord_t ph = lv_obj_get_height(parent);
    lv_coord_t d = (pw < ph ? pw : ph) * 72 / 100;

    w->r_base = d / 10;                 /* 内核基准半径 = 光晕直径/10 */

    /* 三层同心光晕（外→内，暗→亮） */
    w->halo_outer = make_halo_layer(parent, d, LV_OPA_20, MB_WARM_AMBER);
    w->halo_mid   = make_halo_layer(parent, d * 2 / 3, LV_OPA_50, MB_WARM_AMBER);
    w->halo_core  = make_halo_layer(parent, d * 2 / 5, LV_OPA_90, MB_WARM_AMBER);

    /* 相位文字（居中的大字 + 底部小字提示） */
    w->label_phase = lv_label_create(parent);
    lv_obj_set_style_text_color(w->label_phase, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(w->label_phase, &lv_font_montserrat_28, 0);
    lv_label_set_text(w->label_phase, "放松");
    lv_obj_align(w->label_phase, LV_ALIGN_CENTER, 0, 0);

    w->label_hint = lv_label_create(parent);
    lv_obj_set_style_text_color(w->label_hint, MB_TEXT_DIM, 0);
    lv_obj_set_style_text_font(w->label_hint, &lv_font_montserrat_14, 0);
    lv_label_set_text(w->label_hint, "跟着光 吸气·屏息·呼气");
    lv_obj_align(w->label_hint, LV_ALIGN_BOTTOM_MID, 0, -28);

    return w;
}

/* LVGL 定时器回调：喂引擎 + 刷新光晕。周期建议 100ms（与 fade 心跳一致）。 */
static void breathe_timer_cb(lv_timer_t *timer)
{
    breathe_widget_t *w = lv_timer_get_user_data(timer);
    if (!w) return;

    my_breathe_tick(&w->engine, 100);          /* 100ms 一跳 */

    int lvl   = my_breathe_level_pct(&w->engine);
    my_breath_phase_t ph = my_breathe_get_phase(&w->engine);

    /* 亮度映射：引擎 trough..peak → LVGL 透明度 30..255。
     * 映射下限 30（不是 0）：渲染层也遵守"不全灭"约定，避免一暗到底
     * 变成黑屏让用户以为灯坏了。上限 255 = 峰值不削顶。 */
    int lo = w->engine.cfg.trough_pct, hi = w->engine.cfg.peak_pct;
    int span = (hi - lo) > 0 ? (hi - lo) : 1;
    lv_opa_t opa = LV_OPA_30 + (lv_opa_t)((lvl - lo) * 225L / span);
    if (opa > 255) opa = 255;

    lv_obj_set_style_bg_opa(w->halo_outer, opa / 5, 0);   /* 外层最淡 */
    lv_obj_set_style_bg_opa(w->halo_mid,   opa / 2, 0);
    lv_obj_set_style_bg_opa(w->halo_core,  opa, 0);

    /* 内核随呼吸"鼓起"：吸气放大一点，呼气回缩（模拟肺） */
    float prog = my_breathe_cycle_progress(&w->engine);
    lv_coord_t grow = w->r_base / 4;                     /* 最多 +25% */
    lv_coord_t size = w->r_base * 2 +
        (lv_coord_t)(grow * 2 * (prog < 0.21f ? prog * 5 : 1.0f - prog * 4.8f));
    lv_obj_set_size(w->halo_core, size, size);

    /* 相位文字 */
    lv_label_set_text(w->label_phase, phase_text(ph));

    /* 用一个很淡的小字显示当前亮度%，演示/调参时可见；量产可去掉 */
    lv_label_set_text_fmt(w->label_hint, "亮度 %d%% · %s",
                          lvl, ph == MY_BREATH_INHALE ? "跟上吸气" :
                               ph == MY_BREATH_EXHALE ? "慢慢呼" : "稳住");
}

/* 启动呼吸灯。返回 LVGL 定时器句柄（可 lv_timer_del 停止）。 */
lv_timer_t *breathe_start(breathe_widget_t *w)
{
    return lv_timer_create(breathe_timer_cb, 100, w);
}

/* 切换夜间模式：峰值压暗 + 谷值更低（不打断节奏，只改引擎配置） */
void breathe_set_night_mode(breathe_widget_t *w, bool night)
{
    if (night) {
        w->engine.cfg.peak_pct   = 30;    /* 夜间峰值压到 30%（不刺眼） */
        w->engine.cfg.trough_pct = 3;
    } else {
        w->engine.cfg.peak_pct   = 60;
        w->engine.cfg.trough_pct = 4;
    }
}
