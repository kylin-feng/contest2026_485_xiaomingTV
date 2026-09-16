/* SPDX-License-Identifier: Apache-2.0
 *
 * 安眠科技 · 双模态入睡判定实现
 * 算法说明见 mianyu_sleep_detect.h。
 *
 * 计算量核算（对位技术报告 3.3 "算力占用"）：
 *   自相关：300 帧窗 × 60 个滞后 ≈ 18000 次乘加 / 每 2 秒一次
 *   M33 @240MHz 带 FPU，单次分析 < 1ms，占空比 < 0.05%，
 *   可由低功耗核在毫秒级唤醒主核完成，符合"整夜监测不耗电"设计。
 */
#include "mianyu_sleep_detect.h"
#include <math.h>
#include <string.h>

/* ---- 默认阈值（经 tests/test_sleep_detect.c 合成信号标定） ---- */
static const my_sleep_cfg_t k_default_cfg = {
    .regularity_enter  = 0.45f,
    .regularity_asleep = 0.60f,
    .regularity_wake   = 0.35f,
    .movement_still    = 0.12f,
    .movement_active   = 0.35f,
    .sustain_drowsy    = 2,
    .sustain_asleep    = 3,
    .wake_movement_win = 2,
};

/* 环形缓冲读取：k=0 为窗内最旧帧，k=MY_ENV_WINDOW-1 为最新帧。
 * 仅在 env_count >= MY_ENV_WINDOW（窗满）后调用。 */
static inline float env_at(const my_sleep_detector_t *d, int k)
{
    int pos = d->env_head + k;
    if (pos >= MY_ENV_WINDOW) pos -= MY_ENV_WINDOW;
    return d->env[pos];
}

my_err_t my_sleep_init(my_sleep_detector_t *d)
{
    return my_sleep_init_cfg(d, &k_default_cfg);
}

my_err_t my_sleep_init_cfg(my_sleep_detector_t *d, const my_sleep_cfg_t *cfg)
{
    if (!d) return MY_ERR_PARAM;
    memset(d, 0, sizeof(*d));
    d->cfg = cfg ? *cfg : k_default_cfg;
    d->state = MY_SLEEP_AWAKE;
    /* 重力初值设为 0，前若干帧由低通收敛；喂入静止数据时很快逼近真实重力 */
    return MY_OK;
}

void my_sleep_reset_state(my_sleep_detector_t *d)
{
    if (!d) return;
    d->state = MY_SLEEP_AWAKE;
    d->drowsy_streak = 0;
    d->asleep_streak = 0;
    d->wake_streak = 0;
    d->env_count = 0;
    d->env_head = 0;
    d->frame_acc = 0;
    d->frame_n = 0;
    d->hop_counter = 0;
    d->elapsed_sec = 0;
    d->ever_asleep = false;
    memset(&d->last, 0, sizeof(d->last));
    memset(&d->stats, 0, sizeof(d->stats));
}

/* ---- 自相关分析：从包络窗提取呼吸规律度与呼吸率 ---- */
static void analyze_breath(my_sleep_detector_t *d)
{
    const int N = MY_ENV_WINDOW;
    /* 1) 去均值 */
    float mean = 0.0f;
    for (int k = 0; k < N; k++) mean += env_at(d, k);
    mean /= (float)N;

    /* 2) 零滞后能量 r0 = Σ(x-mean)²，作归一化分母 */
    float r0 = 0.0f;
    for (int k = 0; k < N; k++) {
        float v = env_at(d, k) - mean;
        r0 += v * v;
    }
    if (r0 < 1e-9f) {
        /* 窗内几乎无波动（死寂或恒定）：不判为规律呼吸 */
        d->last.regularity = 0.0f;
        d->last.breath_rate = 0.0f;
        d->last.peak_valid = false;
        return;
    }

    /* 3) 在 [MY_LAG_MIN, MY_LAG_MAX] 找归一化自相关最大值。
     * 取区间全局最大而非局部极大：呼吸是准周期信号，该区间内只有一个
     * 主峰，全局最大即呼吸基频（谐波滞后落在区间外，无需额外去谐波）。 */
    float best_r = -1.0f;
    int   best_lag = 0;

    for (int lag = MY_LAG_MIN; lag <= MY_LAG_MAX; lag++) {
        float r = 0.0f;
        /* 归一化自相关：Σ(x[k]-mean)(x[k+lag]-mean) / r0
         * k 范围取 [0, N-lag) 避免越界（有偏估计，对本用途足够） */
        int limit = N - lag;
        for (int k = 0; k < limit; k++) {
            r += (env_at(d, k) - mean) * (env_at(d, k + lag) - mean);
        }
        r /= r0;
        if (r > best_r) { best_r = r; best_lag = lag; }
    }

    /* 峰有效性：峰高需高于噪声底（随机信号自相关在此区间通常 <0.3） */
    d->last.regularity = MY_CLAMP(best_r, 0.0f, 1.0f);
    d->last.peak_valid = (best_r > 0.35f) && (best_lag >= MY_LAG_MIN) && (best_lag <= MY_LAG_MAX);
    /* 呼吸率（次/分）= 60 / 周期(s) = 60 / (lag / ENV_RATE) = 600 / lag */
    d->last.breath_rate = d->last.peak_valid ? (600.0f / (float)best_lag) : 0.0f;
}

/* ---- 融合置信度：规律呼吸 AND 低体动 ---- */
static float fuse_confidence(const my_sleep_detector_t *d)
{
    const my_sleep_cfg_t *c = &d->cfg;
    /* 呼吸规律度贡献：0.3 以下算 0，0.7 以上算 1 */
    float conf_reg = MY_CLAMP((d->last.regularity - 0.30f) / (0.70f - 0.30f), 0.0f, 1.0f);
    /* 静卧贡献：体动达到 active 阈值即归零 */
    float conf_mov = MY_CLAMP(1.0f - d->movement / c->movement_active, 0.0f, 1.0f);
    /* AND 融合：两者都好才高置信 */
    return conf_reg * conf_mov;
}

/* ---- 状态机推进（每分析窗调用一次） ---- */
static void advance_state(my_sleep_detector_t *d)
{
    const my_sleep_cfg_t *c = &d->cfg;
    float reg = d->last.regularity;
    float mov = d->movement;

    bool ev_asleep = (reg >= c->regularity_asleep) && (mov < c->movement_still);
    bool ev_drowsy = (reg >= c->regularity_enter)  && (mov < c->movement_active);
    /* 醒转证据 = 高体动（IMU 判据）OR 呼吸规律度塌缩（MIC 判据）。
     * MIC 判据让无 IMU 的 DevKit-LCD 也能在「醒来说话/翻身」时通过
     * 呼吸节律消失检测到夜醒。两条路都走 wake_streak 迟滞，防单窗抖动。 */
    bool ev_wake   = (mov >= c->movement_active) || (reg < c->regularity_wake);

    d->stats.windows_total++;
    if (d->state == MY_SLEEP_ASLEEP) d->stats.asleep_windows++;

    switch (d->state) {
    case MY_SLEEP_AWAKE:
        d->asleep_streak = ev_asleep ? d->asleep_streak + 1 : 0;
        d->drowsy_streak = ev_drowsy ? d->drowsy_streak + 1 : 0;
        if (d->asleep_streak >= c->sustain_asleep) {
            d->state = MY_SLEEP_ASLEEP;
            d->stats.transitions_to_asleep++;
            if (!d->ever_asleep) { d->stats.first_asleep_sec = d->elapsed_sec; d->ever_asleep = true; }
            d->asleep_streak = 0; d->drowsy_streak = 0; d->wake_streak = 0;
        } else if (d->drowsy_streak >= c->sustain_drowsy) {
            d->state = MY_SLEEP_DROWSY;
            d->drowsy_streak = 0;
        }
        break;

    case MY_SLEEP_DROWSY:
        d->asleep_streak = ev_asleep ? d->asleep_streak + 1 : 0;
        if (ev_wake) { d->state = MY_SLEEP_AWAKE; d->asleep_streak = 0; d->wake_streak = 0; }
        else if (d->asleep_streak >= c->sustain_asleep) {
            d->state = MY_SLEEP_ASLEEP;
            d->stats.transitions_to_asleep++;
            if (!d->ever_asleep) { d->stats.first_asleep_sec = d->elapsed_sec; d->ever_asleep = true; }
            d->asleep_streak = 0;
        }
        break;

    case MY_SLEEP_ASLEEP:
        /* 入睡后若出现持续高体动 → 夜醒事件，回 AWAKE */
        d->wake_streak = ev_wake ? d->wake_streak + 1 : 0;
        if (d->wake_streak >= c->wake_movement_win) {
            d->state = MY_SLEEP_AWAKE;
            d->stats.night_wake_events++;
            d->wake_streak = 0;
            d->asleep_streak = 0;
            /* 注意：ever_asleep 保持 true，first_asleep_sec 不重置 */
        }
        break;
    }

    d->last.confidence = fuse_confidence(d);
    d->last.movement = mov;
}

/* ---- 触发一次完整分析（包络自相关 + 体动结算 + 状态机） ---- */
static void run_analysis(my_sleep_detector_t *d)
{
    /* 体动结算：把窗口累计的去重力偏差均值作为本窗活动量 */
    if (d->mov_n > 0) {
        d->movement = MY_CLAMP(d->mov_acc / (float)d->mov_n, 0.0f, 1.0f);
        d->mov_acc = 0.0f;
        d->mov_n = 0;
    }
    /* 若这段时间没有 IMU 数据（如纯 MIC 测试），movement 保持上次值 */

    analyze_breath(d);
    advance_state(d);
}

my_err_t my_sleep_feed_mic(my_sleep_detector_t *d, const my_pcm_t *pcm, int count)
{
    if (!d || !pcm || count < 0) return MY_ERR_PARAM;

    for (int i = 0; i < count; i++) {
        int32_t v = pcm[i];
        d->frame_acc += (int32_t)((v * v) >> 8);   /* >>8 防 100ms 平方和溢出 int32 */
        d->frame_n++;

        if (d->frame_n >= MY_ENV_FRAME_SAMPLES) {
            /* 满一帧（100ms）：算 RMS 能量，归一到 0~1，写入环形缓冲 */
            float ms = (float)d->frame_acc / (float)d->frame_n;   /* 已 >>8 的均方 */
            float rms = sqrtf(ms) * 16.0f;   /* ×16 把 >>8(=÷256) 的尺度拉回并放大到可辨范围 */
            d->env[d->env_head] = rms;
            d->env_head = (d->env_head + 1) % MY_ENV_WINDOW;
            if (d->env_count < MY_ENV_WINDOW) d->env_count++;

            d->frame_acc = 0;
            d->frame_n = 0;

            /* 每帧 = 100ms */
            d->hop_counter++;
            if (d->hop_counter % 10 == 0) d->elapsed_sec++;   /* 10 帧 = 1 秒 */

            /* 窗满且到达分析 hop → 跑一次分析 */
            if (d->env_count >= MY_ENV_WINDOW && d->hop_counter >= MY_ANALYSIS_HOP) {
                d->hop_counter = 0;
                run_analysis(d);
            }
        }
    }
    return MY_OK;
}

my_err_t my_sleep_feed_imu(my_sleep_detector_t *d, float ax, float ay, float az)
{
    if (!d) return MY_ERR_PARAM;
    /* 重力低通估计：g = g*0.98 + a*0.02（截止约 0.16Hz@50Hz 采样） */
    const float alpha = 0.02f;
    d->grav[0] += alpha * (ax - d->grav[0]);
    d->grav[1] += alpha * (ay - d->grav[1]);
    d->grav[2] += alpha * (az - d->grav[2]);
    /* 去重力后的线性加速度幅值 */
    float lx = ax - d->grav[0];
    float ly = ay - d->grav[1];
    float lz = az - d->grav[2];
    float mag = sqrtf(lx * lx + ly * ly + lz * lz);
    /* 累计绝对偏差作为活动量（静止时≈0，翻身/挪动时升高） */
    d->mov_acc += mag;
    d->mov_n++;
    return MY_OK;
}

my_sleep_state_t my_sleep_get_state(const my_sleep_detector_t *d)
{
    return d ? d->state : MY_SLEEP_AWAKE;
}

const my_sleep_metrics_t *my_sleep_get_metrics(const my_sleep_detector_t *d)
{
    return d ? &d->last : NULL;
}

const my_sleep_stats_t *my_sleep_get_stats(const my_sleep_detector_t *d)
{
    return d ? &d->stats : NULL;
}
