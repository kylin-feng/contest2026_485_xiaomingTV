/* SPDX-License-Identifier: Apache-2.0
 *
 * 安眠科技 · 音量淡出曲线引擎实现
 * 设计动机与三条翻车对策见 mianyu_fade.h 头部注释。
 *
 * 计算量：apply() 每样本 1 次 int64 乘法 + 移位，320 样本(20ms@16kHz) < 5us。
 * 场景增益是「时间点的纯函数」而非增量累积 —— 浮点误差不会随播放时长漂移，
 * 也便于测试直接对任意时刻求值验证单调性。
 */
#include "mianyu_fade.h"
#include <math.h>
#include <string.h>

/* dB ↔ 线性增益换算，用 2 的幂避免 pow(10,..) 的开销：
 *   gain = FULL * 10^(db/20) = FULL * exp2(db * log2(10)/20)
 *   log2(10)/20 = 0.16609640474
 *   db   = 20*log10(gain/FULL) = log2(gain/FULL) * 20/log2(10)
 *   20/log2(10) = 6.02059991328                                   */
#define DB_TO_EXP2_K   0.16609640474f
#define GAIN_TO_DB_K   6.02059991328f

static inline int32_t db_to_gain_q15(float db)
{
    if (db <= -96.0f) return 0;                     /* 低于 -96dB 视为静音 */
    float lin = exp2f(db * DB_TO_EXP2_K);
    return (int32_t)(lin * (float)MY_GAIN_FULL_Q15);
}

static inline float gain_q15_to_db(int32_t gain_q15)
{
    if (gain_q15 <= 0) return -INFINITY;
    return log2f((float)gain_q15 / (float)MY_GAIN_FULL_Q15) * GAIN_TO_DB_K;
}

/* smoothstep: 3x²-2x³，首尾导数为 0 → 淡入无阶跃冲击，听感"自然浮现" */
static inline float smoothstep(float x)
{
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return x * x * (3.0f - 2.0f * x);
}

/* start_fade_out 需要读当前总增益来"烘焙"乘子，故前向声明（定义在后） */
int32_t my_fade_gain_q15(const my_fade_t *f);

void my_fade_default_cfg(my_fade_cfg_t *cfg)
{
    if (!cfg) return;
    cfg->fade_in_ms    = 2000;
    cfg->hold_ms       = 0;          /* 0 = 无限保持，由入睡判定显式触发淡出 */
    cfg->fade_out_ms   = 900000;     /* 15 分钟，产品设定的哄睡淡出 */
    cfg->tail_ms       = 3000;
    cfg->floor_db      = -60;
    cfg->duck_gain_q15 = 8192;       /* -12dB：背景音压低但语音可懂 */
    cfg->duck_ms       = 300;
    cfg->target_q15    = 22938;      /* 70%，与噪声模块无削顶标定一致 */
    cfg->dither_tpdf   = true;
}

/* 乘子复位到「中性 = 1.0 且斜坡已到位」。
 *
 * 必须同时复位 _from_q15 和 _ms_elapsed 三个字段，否则 ramp_step 会
 * 从 0 起点重新爬一遍斜坡 —— 表现为每次播放的头 duck_ms 毫秒里
 * 总增益从近 0 慢慢升到满幅，既破坏淡出单调性，又产生几十 dB 的
 * 增益跳变（tests/test_fade.c 的测试组 3/5 就是抓这个 bug 的）。 */
static void reset_multipliers(my_fade_t *f)
{
    f->duck_active      = false;
    f->duck_gain_q15    = MY_GAIN_FULL_Q15;
    f->duck_from_q15    = MY_GAIN_FULL_Q15;
    f->duck_ms_elapsed  = f->cfg.duck_ms;      /* 已到终点 */
    f->paused           = false;
    f->pause_gain_q15   = MY_GAIN_FULL_Q15;
    f->pause_from_q15   = MY_GAIN_FULL_Q15;
    f->pause_ms_elapsed = f->cfg.duck_ms;
}

my_err_t my_fade_init(my_fade_t *f, const my_fade_cfg_t *cfg)
{
    if (!f) return MY_ERR_PARAM;
    memset(f, 0, sizeof(*f));

    my_fade_cfg_t c;
    if (cfg) c = *cfg; else my_fade_default_cfg(&c);

    /* ---- 参数校验与自愈（真机上配置来自 cJSON，可能被写坏） ---- */
    if (c.fade_in_ms  < 0) c.fade_in_ms  = 0;
    if (c.hold_ms     < 0) c.hold_ms     = 0;
    if (c.fade_out_ms <= 0) c.fade_out_ms = 900000;
    if (c.tail_ms     < 0) c.tail_ms     = 0;
    /* 末段必须短于总长，否则指数段不存在、淡出退化成一条直线 */
    if (c.tail_ms >= c.fade_out_ms) c.tail_ms = c.fade_out_ms / 2;
    if (c.floor_db  > -6)  c.floor_db  = -6;      /* 太低会让淡出前段听不出变化 */
    if (c.duck_gain_q15 < 0) c.duck_gain_q15 = 0;
    if (c.duck_gain_q15 > MY_GAIN_FULL_Q15) c.duck_gain_q15 = MY_GAIN_FULL_Q15;
    if (c.duck_ms   < 0) c.duck_ms   = 0;
    if (c.target_q15 < 0) c.target_q15 = 0;
    if (c.target_q15 > MY_GAIN_FULL_Q15) c.target_q15 = MY_GAIN_FULL_Q15;

    f->cfg = c;
    f->phase = MY_FADE_STOPPED;
    f->scene_gain_q15 = 0;
    f->rng = 0x5EED1234u;
    reset_multipliers(f);   /* 须在 f->cfg 赋值之后（用到 duck_ms） */
    return MY_OK;
}

/* 场景包络：给定阶段与阶段内时间，求纯函数增益 */
static int32_t scene_gain_at(const my_fade_t *f, my_fade_phase_t phase, int32_t t_ms)
{
    switch (phase) {
    case MY_FADE_STOPPED:
    case MY_FADE_DONE:
        return 0;

    case MY_FADE_HOLD:
        return f->cfg.target_q15;

    case MY_FADE_IN: {
        if (f->cfg.fade_in_ms <= 0) return f->cfg.target_q15;
        float x = (float)t_ms / (float)f->cfg.fade_in_ms;
        return (int32_t)(smoothstep(x) * (float)f->cfg.target_q15);
    }

    case MY_FADE_OUT: {
        if (f->fo_exp_ms > 0 && t_ms < f->fo_exp_ms) {
            /* 指数段：dB 域线性插值 → 感知匀速变轻 */
            float x = (float)t_ms / (float)f->fo_exp_ms;
            float db = f->fo_start_db + ((float)f->cfg.floor_db - f->fo_start_db) * x;
            return db_to_gain_q15(db);
        }
        /* 线性收尾段：从 floor_db 对应增益精确归零，杜绝指数拖尾 */
        int32_t lt = t_ms - f->fo_exp_ms;
        if (f->fo_lin_ms <= 0 || lt >= f->fo_lin_ms) return 0;
        int64_t g = (int64_t)f->fo_lin_start_gain_q15 * (int64_t)(f->fo_lin_ms - lt)
                    / (int64_t)f->fo_lin_ms;
        return (int32_t)g;
    }

    default:
        return 0;
    }
}

my_err_t my_fade_start(my_fade_t *f)
{
    if (!f) return MY_ERR_PARAM;
    f->phase = (f->cfg.fade_in_ms > 0) ? MY_FADE_IN : MY_FADE_HOLD;
    f->scene_ms = 0;
    f->played_ms = 0;
    f->fo_exp_ms = 0;
    f->fo_lin_ms = 0;
    reset_multipliers(f);   /* 重播应清掉遗留的暂停/闪避状态 */
    f->scene_gain_q15 = scene_gain_at(f, f->phase, 0);
    return MY_OK;
}

my_err_t my_fade_start_fade_out(my_fade_t *f)
{
    if (!f) return MY_ERR_PARAM;
    /* 已停止/已完成：无害空操作（避免重复触发把时间线清零） */
    if (f->phase == MY_FADE_STOPPED || f->phase == MY_FADE_DONE) return MY_OK;
    if (f->phase == MY_FADE_OUT) return MY_OK;    /* 正在淡出，不重启曲线 */

    /* 关键：淡出从【当前实际可闻音量】起算 = 场景增益 × 闪避 × 暂停，
     * 并把这两个乘子"烘焙"进曲线起点后置为中性、解除闪避/暂停。
     *
     * 为什么必须烘焙（tests/test_fade.c 组 8 抓的就是这个 bug）：
     *   若淡出途中智能体说完话释放闪避，duck 乘子会从 8192 拉回 32768，
     *   总增益瞬间反弹（实测 5734 → 22160，约 +12dB）—— 夜里音量突然
     *   变大 = 惊醒用户，是哄睡产品最致命的翻车。
     *   烘焙后淡出曲线直接驱动总增益，乘子恒为 1.0，总增益严格单调不增。
     *
     * 副作用（可接受并记录）：在暂停途中触发淡出会被解除暂停、从当前
     *   可闻音量继续淡出。语义上"都要淡出关机了"覆盖"暂停"，合理。 */
    int32_t start = my_fade_gain_q15(f);
    if (start <= 0) {
        f->phase = MY_FADE_DONE;
        f->scene_gain_q15 = 0;
        return MY_OK;
    }

    /* 烘焙：解除闪避与暂停，乘子归中性并标记斜坡已到位 */
    f->duck_active      = false;
    f->duck_gain_q15    = MY_GAIN_FULL_Q15;
    f->duck_from_q15    = MY_GAIN_FULL_Q15;
    f->duck_ms_elapsed  = f->cfg.duck_ms;
    f->paused           = false;
    f->pause_gain_q15   = MY_GAIN_FULL_Q15;
    f->pause_from_q15   = MY_GAIN_FULL_Q15;
    f->pause_ms_elapsed = f->cfg.duck_ms;

    f->fo_start_gain_q15 = start;
    f->fo_start_db = gain_q15_to_db(start);
    f->scene_ms = 0;
    f->phase = MY_FADE_OUT;

    if (f->fo_start_db <= (float)f->cfg.floor_db) {
        /* 起点已低于/等于地板：无指数段可走，整段线性归零。
         * 这保证「从很轻的音量淡出」同样在 fade_out_ms 内精确到 0。 */
        f->fo_exp_ms = 0;
        f->fo_lin_start_gain_q15 = start;
        f->fo_lin_ms = f->cfg.fade_out_ms;
    } else {
        f->fo_exp_ms = f->cfg.fade_out_ms - f->cfg.tail_ms;
        if (f->fo_exp_ms <= 0) f->fo_exp_ms = 1;
        f->fo_lin_start_gain_q15 = db_to_gain_q15((float)f->cfg.floor_db);
        f->fo_lin_ms = f->cfg.fade_out_ms - f->fo_exp_ms;
    }
    f->scene_gain_q15 = scene_gain_at(f, MY_FADE_OUT, 0);
    return MY_OK;
}

/* 斜坡推进：从 from 朝 dest 以 ramp_ms 为总时长，按已流逝时间求值。
 * 纯函数（不依赖当前值）→ 反复 tick 不会累积误差，也不会因 delta 抖动而漂移。 */
static int32_t ramp_step(int32_t from, int32_t dest,
                         int32_t elapsed_ms, int32_t ramp_ms)
{
    if (ramp_ms <= 0) return dest;
    if (elapsed_ms >= ramp_ms) return dest;
    int64_t span = (int64_t)dest - (int64_t)from;
    return (int32_t)((int64_t)from + span * (int64_t)elapsed_ms / (int64_t)ramp_ms);
}

/* 斜坡计时器推进：到位后钳住不再增长。
 * 整夜播放（8h = 28800000ms）虽不会溢出 int32，但钳住可杜绝
 * 超长运行（>24天）的溢出隐患，也让"斜坡已完成"状态一目了然。 */
static inline void ramp_advance(int32_t *elapsed, int32_t delta_ms, int32_t ramp_ms)
{
    *elapsed += delta_ms;
    if (ramp_ms > 0 && *elapsed > ramp_ms) *elapsed = ramp_ms;
}

my_err_t my_fade_pause(my_fade_t *f)
{
    if (!f) return MY_ERR_PARAM;
    if (f->phase == MY_FADE_STOPPED || f->phase == MY_FADE_DONE) return MY_ERR_STATE;
    if (f->paused) return MY_OK;
    f->paused = true;
    f->pause_from_q15 = f->pause_gain_q15;   /* 从中途暂停也平滑 */
    f->pause_ms_elapsed = 0;
    return MY_OK;
}

my_err_t my_fade_resume(my_fade_t *f)
{
    if (!f) return MY_ERR_PARAM;
    if (!f->paused) return MY_OK;
    f->paused = false;
    f->pause_from_q15 = f->pause_gain_q15;
    f->pause_ms_elapsed = 0;
    return MY_OK;
}

my_err_t my_fade_set_duck(my_fade_t *f, bool active)
{
    if (!f) return MY_ERR_PARAM;
    if (f->duck_active == active) return MY_OK;
    f->duck_active = active;
    f->duck_from_q15 = f->duck_gain_q15;
    f->duck_ms_elapsed = 0;
    return MY_OK;
}

my_err_t my_fade_tick(my_fade_t *f, int32_t delta_ms)
{
    if (!f) return MY_ERR_PARAM;
    if (delta_ms < 0) return MY_ERR_PARAM;
    if (delta_ms == 0) return MY_OK;

    /* 1) 闪避斜坡（与暂停独立推进） */
    ramp_advance(&f->duck_ms_elapsed, delta_ms, f->cfg.duck_ms);
    if (f->duck_active) {
        f->duck_gain_q15 = ramp_step(f->duck_from_q15, f->cfg.duck_gain_q15,
                                     f->duck_ms_elapsed, f->cfg.duck_ms);
    } else {
        f->duck_gain_q15 = ramp_step(f->duck_from_q15, MY_GAIN_FULL_Q15,
                                     f->duck_ms_elapsed, f->cfg.duck_ms);
    }

    /* 2) 暂停斜坡 */
    ramp_advance(&f->pause_ms_elapsed, delta_ms, f->cfg.duck_ms);
    if (f->paused) {
        f->pause_gain_q15 = ramp_step(f->pause_from_q15, 0,
                                      f->pause_ms_elapsed, f->cfg.duck_ms);
    } else {
        f->pause_gain_q15 = ramp_step(f->pause_from_q15, MY_GAIN_FULL_Q15,
                                      f->pause_ms_elapsed, f->cfg.duck_ms);
    }

    /* 3) 场景时间线：暂停期间冻结（暂停语义 = 进度不丢） */
    if (f->paused) return MY_OK;
    if (f->phase == MY_FADE_STOPPED || f->phase == MY_FADE_DONE) return MY_OK;

    f->scene_ms  += delta_ms;
    f->played_ms += delta_ms;

    switch (f->phase) {
    case MY_FADE_IN:
        if (f->cfg.fade_in_ms > 0 && f->scene_ms >= f->cfg.fade_in_ms) {
            /* 淡入到位 → 进入保持，scene_ms 归零重新计时（hold_ms 从此处算） */
            f->phase = MY_FADE_HOLD;
            f->scene_ms = 0;
        }
        break;

    case MY_FADE_HOLD:
        if (f->cfg.hold_ms > 0) {
            if (f->scene_ms >= f->cfg.hold_ms) {
                my_fade_start_fade_out(f);   /* 自动淡出（定时关闭场景） */
                return MY_OK;                /* 内部已重算 scene_gain */
            }
        } else {
            /* 无限保持：钳住 scene_ms 防溢出。设备整夜不关可跑数天，
             * int32 毫秒约 24.8 天溢出；钳在 1 天处对行为无任何影响
             * （HOLD 的增益是常量，与 scene_ms 无关）。 */
            if (f->scene_ms > 86400000) f->scene_ms = 86400000;
        }
        break;

    case MY_FADE_OUT:
        if (f->scene_ms >= f->cfg.fade_out_ms) {
            f->scene_ms = f->cfg.fade_out_ms;
            f->phase = MY_FADE_DONE;
        }
        break;

    default:
        break;
    }

    f->scene_gain_q15 = scene_gain_at(f, f->phase, f->scene_ms);
    return MY_OK;
}

/* 总增益 = 场景 × 闪避 × 暂停（Q15 × Q15 × Q15 → Q15） */
int32_t my_fade_gain_q15(const my_fade_t *f)
{
    if (!f) return 0;
    if (f->scene_gain_q15 <= 0) return 0;
    int64_t g = (int64_t)f->scene_gain_q15 * (int64_t)f->duck_gain_q15;
    g >>= 15;
    g = g * (int64_t)f->pause_gain_q15;
    g >>= 15;
    return (int32_t)MY_CLAMP(g, 0, (int64_t)MY_GAIN_FULL_Q15);
}

int my_fade_gain_pct(const my_fade_t *f)
{
    /* 四舍五入到 0..100，供只吃整数百分比的硬件驱动做粗调 */
    return (int)((my_fade_gain_q15(f) * 100 + MY_GAIN_FULL_Q15 / 2) / MY_GAIN_FULL_Q15);
}

float my_fade_gain_db(const my_fade_t *f)
{
    return gain_q15_to_db(my_fade_gain_q15(f));
}

my_fade_phase_t my_fade_get_phase(const my_fade_t *f)
{
    return f ? f->phase : MY_FADE_STOPPED;
}

int32_t my_fade_remaining_ms(const my_fade_t *f)
{
    if (!f || f->phase != MY_FADE_OUT) return 0;
    int32_t r = f->cfg.fade_out_ms - f->scene_ms;
    return r > 0 ? r : 0;
}

int64_t my_fade_played_ms(const my_fade_t *f)
{
    return f ? f->played_ms : 0;
}

/* TPDF 抖动：两个均匀分布之和 → 三角分布，范围 ±1 LSB（Q15 域 ±32767） */
static inline int32_t tpdf_dither(uint32_t *rng)
{
    uint32_t a = *rng;
    a ^= a << 13; a ^= a >> 17; a ^= a << 5;
    *rng = a;
    int32_t r1 = (int32_t)(a & 0x7FFF);
    uint32_t b = a;
    b ^= b << 13; b ^= b >> 17; b ^= b << 5;
    *rng = b;
    int32_t r2 = (int32_t)(b & 0x7FFF);
    return r1 + r2 - 32767;
}

int my_fade_apply(my_fade_t *f, my_pcm_t *buf, int count, int32_t gain_q15)
{
    /* f 是本引擎的方法句柄（提供抖动配置与 RNG），必须有效 */
    if (!f || !buf || count < 0) return MY_ERR_PARAM;
    if (gain_q15 < 0) gain_q15 = 0;
    if (gain_q15 > MY_GAIN_FULL_Q15) gain_q15 = MY_GAIN_FULL_Q15;

    /* 静音快路径：直接写零，不做乘法也不加抖动 —— 保证彻底无声。
     * 这一条对哄睡场景是硬要求：淡出结束后残留 ±1 LSB 的本底噪声
     * 在深夜安静卧室里是可闻的。 */
    if (gain_q15 == 0) {
        memset(buf, 0, (size_t)count * sizeof(my_pcm_t));
        return count;
    }
    /* 满幅快路径：unity 增益必须【逐位透传】。
     * 此时没有重定量化（不丢位），抖动反而会污染信号：满幅 ±32767 的
     * 方波加了 TPDF 抖动就变成 ±32766/±32767 的失真。故 unity 时不抖动、
     * 不做乘法，原样返回。这是 tests/test_fade.c 组 10 抓的第二个 bug。 */
    if (gain_q15 == MY_GAIN_FULL_Q15) return count;

    bool dither = f->cfg.dither_tpdf;
    for (int i = 0; i < count; i++) {
        int64_t v = (int64_t)buf[i] * (int64_t)gain_q15;
        if (dither) v += tpdf_dither(&f->rng);
        v >>= 15;                              /* 算术右移 = 向 -inf 取整，配抖动是正确做法 */
        buf[i] = (my_pcm_t)MY_CLAMP(v, (int64_t)MY_PCM_MIN, (int64_t)MY_PCM_MAX);
    }
    return count;
}

int my_fade_apply_current(my_fade_t *f, my_pcm_t *buf, int count)
{
    if (!f) return MY_ERR_PARAM;
    return my_fade_apply(f, buf, count, my_fade_gain_q15(f));
}
