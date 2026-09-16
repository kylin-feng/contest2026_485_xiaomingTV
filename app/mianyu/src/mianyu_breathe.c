/* SPDX-License-Identifier: Apache-2.0
 *
 * 安眠科技 · 呼吸节律灯光引擎实现
 * 设计动机与产品逻辑见 mianyu_breathe.h。
 *
 * 亮度曲线：
 *   以 0..1 归一化"s"在四段上移动，亮度 = trough + (peak-trough)·smoothstep(s)：
 *     INHALE [0,inhale)        s: 0→1     光渐亮（带呼吸）
 *     HOLD   [inhale, +hold)   s = 1       光保持峰值（屏息）
 *     EXHALE [+hold, +exhale)  s: 1→0     光渐暗（呼气，比吸气慢）
 *     REST   [+exhale, +rest)  s = 0       光保持谷值
 *   smoothstep(x)=x²(3-2x) 保证段边界处曲线首尾导数都为 0，
 *   光在段与段之间是平滑过渡的，不会出现"突然变亮/变灭"的阶跃。
 *
 * 时间处理：tick 累加 elapsed_ms，按周期取模定位到段内进度。
 *   周期 = inhale+hold+exhale+rest；rest=0 时吸气段紧接着呼气段
 *   （谷值瞬时不间断，符合 4-7-8 无缝循环）。
 *
 * 无任何 OS 时间依赖：所有时间由 my_breathe_tick(delta_ms) 喂入，
 *   PC 上可直接单元测试，换平台只换喂 tick 的地方。
 */
#include "mianyu_breathe.h"
#include <string.h>

/* ---- 默认配置：4-7-8 呼吸法 ---- */
void my_breathe_default_cfg(my_breathe_cfg_t *cfg)
{
    if (!cfg) return;
    cfg->inhale_ms  = 4000;
    cfg->hold_ms    = 7000;
    cfg->exhale_ms  = 8000;
    cfg->rest_ms    = 0;
    cfg->peak_pct   = 60;
    cfg->trough_pct = 4;
}

my_err_t my_breathe_init(my_breathe_t *b, const my_breathe_cfg_t *cfg)
{
    if (!b) return MY_ERR_PARAM;
    if (cfg) {
        /* 参数校验：时长非负、吸/呼至少一个 >0（否则灯不会动）、
         * 亮度在 0..100、peak >= trough（谷值不能高于峰值）。 */
        if (cfg->inhale_ms < 0 || cfg->hold_ms < 0 || cfg->exhale_ms < 0 ||
            cfg->rest_ms < 0)
            return MY_ERR_PARAM;
        if (cfg->inhale_ms == 0 && cfg->exhale_ms == 0)
            return MY_ERR_PARAM;          /* 全静止配置无意义 */
        if (cfg->peak_pct > 100 || cfg->trough_pct > 100)
            return MY_ERR_PARAM;
        if (cfg->peak_pct < cfg->trough_pct)
            return MY_ERR_PARAM;
        b->cfg = *cfg;
    } else {
        my_breathe_default_cfg(&b->cfg);
    }
    b->elapsed_ms = 0;
    return MY_OK;
}

my_err_t my_breathe_tick(my_breathe_t *b, int32_t delta_ms)
{
    if (!b || delta_ms < 0) return MY_ERR_PARAM;
    b->elapsed_ms += delta_ms;
    return MY_OK;
}

void my_breathe_reset(my_breathe_t *b)
{
    if (!b) return;
    b->elapsed_ms = 0;
}

int32_t my_breathe_cycle_ms(const my_breathe_t *b)
{
    if (!b) return 0;
    return b->cfg.inhale_ms + b->cfg.hold_ms +
           b->cfg.exhale_ms + b->cfg.rest_ms;
}

/* smoothstep：0..1 输入 → 0..1 输出，首尾导数为 0 */
static float smoothstep01(float x)
{
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return x * x * (3.0f - 2.0f * x);
}

/* 由周期内进度（毫秒）算出归一化亮度 t 0..1 与相位 */
static void eval(const my_breathe_t *b, int64_t in_cycle_ms,
                 float *t_out, my_breath_phase_t *phase_out)
{
    const my_breathe_cfg_t *c = &b->cfg;
    int32_t inh = c->inhale_ms, hold = c->hold_ms;
    int32_t exh = c->exhale_ms;

    if (inh > 0 && in_cycle_ms < inh) {
        *t_out = smoothstep01((float)in_cycle_ms / (float)inh);
        *phase_out = MY_BREATH_INHALE;
    } else if (hold > 0 && in_cycle_ms < (int64_t)inh + hold) {
        *t_out = 1.0f;
        *phase_out = MY_BREATH_HOLD;
    } else if (exh > 0 && in_cycle_ms < (int64_t)inh + hold + exh) {
        float x = (float)(in_cycle_ms - inh - hold) / (float)exh;
        *t_out = 1.0f - smoothstep01(x);
        *phase_out = MY_BREATH_EXHALE;
    } else {
        *t_out = 0.0f;                     /* rest 段（可能为 0 长度） */
        *phase_out = MY_BREATH_REST;
    }
}

int my_breathe_level_pct(const my_breathe_t *b)
{
    if (!b) return 0;
    int64_t cycle = my_breathe_cycle_ms(b);
    if (cycle <= 0) return b->cfg.trough_pct;
    int64_t in_cycle = b->elapsed_ms % cycle;
    float t;
    my_breath_phase_t ph;
    eval(b, in_cycle, &t, &ph);
    (void)ph;
    int lo = b->cfg.trough_pct, hi = b->cfg.peak_pct;
    int lvl = lo + (int)((float)(hi - lo) * t + 0.5f);
    if (lvl < 0) lvl = 0;
    if (lvl > 100) lvl = 100;
    return lvl;
}

my_breath_phase_t my_breathe_get_phase(const my_breathe_t *b)
{
    if (!b) return MY_BREATH_REST;
    int64_t cycle = my_breathe_cycle_ms(b);
    if (cycle <= 0) return MY_BREATH_REST;
    int64_t in_cycle = b->elapsed_ms % cycle;
    float t;
    my_breath_phase_t ph;
    eval(b, in_cycle, &t, &ph);
    (void)t;
    return ph;
}

float my_breathe_cycle_progress(const my_breathe_t *b)
{
    if (!b) return 0.0f;
    int64_t cycle = my_breathe_cycle_ms(b);
    if (cycle <= 0) return 0.0f;
    int64_t in_cycle = b->elapsed_ms % cycle;
    return (float)in_cycle / (float)cycle;
}
