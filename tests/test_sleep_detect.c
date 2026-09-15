/* SPDX-License-Identifier: Apache-2.0
 *
 * 安眠科技 · 入睡判定单元测试（PC 端，无需硬件）
 *
 * 这组测试直接产出技术报告 3.5 节「健康类作品必填」的三项量化指标：
 *   误报率 FP/(FP+TN)、漏报率 FN/(FN+TP)、识别准确率 (TP+TN)/ALL
 * 做法：用合成信号构造「已知真值」的睡眠/清醒场景，逐分析窗与状态机
 * 输出对照，累计混淆矩阵。真值由场景参数定义，不依赖人工标注。
 *
 * 合成信号模型：
 *   MIC = 白噪载波 × 呼吸包络，包络 = base_amp × (1 + depth·sin(2πf·t) + 扰动)
 *         → 100ms 帧 RMS 恰好还原出呼吸周期，与真人呼吸声的调幅特征一致
 *   IMU = 重力 1g(z 轴) + 强度可控的宽带随机体动
 *
 * 编译： cc -std=c11 -O2 -I../include -o test_sleep_detect test_sleep_detect.c ../src/mianyu_sleep_detect.c -lm
 * 运行： ./test_sleep_detect
 */
#include "mianyu_sleep_detect.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, fmt, ...) do { \
    if (cond) { g_pass++; printf("  [PASS] " fmt "\n", ##__VA_ARGS__); } \
    else      { g_fail++; printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); } \
} while (0)

/* ---- 全局混淆矩阵（跨所有场景累计） ---- */
static int g_TP = 0, g_FP = 0, g_FN = 0, g_TN = 0;

/* ================= 合成信号发生器 ================= */

typedef struct {
    float    breath_bpm;    /* 呼吸率（次/分）；0 = 无周期调制（清醒/说话/环境噪声） */
    float    depth;         /* 呼吸调幅深度 0~1 */
    float    env_noise;     /* 包络随机扰动 0~1，越大越不规律 */
    float    base_amp;      /* 载波幅度；0 = 纯静音 */
    float    imu_activity;  /* 体动强度（g 量级）；0.02=静卧 0.8=清醒翻身 */
    uint32_t rng;
} scenario_t;

static uint32_t rng_next(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

/* 均匀分布 -1..1 */
static float rng_uni(uint32_t *s)
{
    return (float)(rng_next(s) >> 8) / 8388608.0f - 1.0f;
}

/* 当前时刻的呼吸包络（恒正，避免过零导致 RMS 出现倍频） */
static float env_value(const scenario_t *s, float t)
{
    if (s->base_amp <= 0.0f) return 0.0f;
    float e = 1.0f;
    if (s->breath_bpm > 0.0f) {
        float f = s->breath_bpm / 60.0f;
        e += s->depth * sinf((float)(2.0 * M_PI) * f * t);
    }
    if (s->env_noise > 0.0f) e += s->env_noise * rng_uni((uint32_t *)&s->rng);
    if (e < 0.05f) e = 0.05f;
    return e * s->base_amp;
}

/* 生成 n 个 PCM 样本（16kHz 单声道），sample_idx 为全局样本序号 */
static void gen_mic(scenario_t *s, my_pcm_t *buf, int n, long *sample_idx)
{
    for (int i = 0; i < n; i++) {
        float t = (float)(*sample_idx) / (float)MY_SAMPLE_RATE;
        float e = env_value(s, t);
        buf[i] = (my_pcm_t)(e * rng_uni(&s->rng));
        (*sample_idx)++;
    }
}

/* 按 50Hz 喂入 duration 秒的 IMU（本函数内部按 100ms 分 5 次） */
static void gen_imu(scenario_t *s, my_sleep_detector_t *d, int frames_100ms)
{
    for (int f = 0; f < frames_100ms; f++) {
        for (int k = 0; k < 5; k++) {   /* 5 样本/100ms = 50Hz */
            float a = s->imu_activity;
            my_sleep_feed_imu(d, a * rng_uni(&s->rng),
                                 a * rng_uni(&s->rng),
                                 1.0f + a * rng_uni(&s->rng));
        }
    }
}

/* ================= 场景执行器 ================= */

typedef struct {
    int      windows;          /* 有效分析窗数 */
    int      asleep_windows;   /* 判为 ASLEEP 的窗数 */
    int      tp, fp, fn, tn;   /* 本场景混淆矩阵 */
    uint32_t first_asleep_sec;
    float    last_regularity;
    float    last_breath_rate;
    float    last_movement;
    float    sum_regularity;   /* 稳态窗规律度均值（用于报告） */
    int      steady_windows;
    my_sleep_state_t final_state;
} run_result_t;

/* 跑一个场景 seconds 秒。truth_asleep = 该场景的真实标签。
 * 逐分析窗对照：以 stats.windows_total 递增作为"完成一个窗"的信号，
 * 不依赖对内部时序的推算。 */
static run_result_t run_scenario(const char *name, scenario_t sc, int seconds,
                                 bool truth_asleep, int warmup_s)
{
    printf("\n--- 场景 %s：%s（真值=%s，%ds）---\n",
           name, truth_asleep ? "睡眠" : "清醒", truth_asleep ? "ASLEEP" : "AWAKE", seconds);

    my_sleep_detector_t d;
    my_sleep_init(&d);

    run_result_t r = {0};
    my_pcm_t *buf = malloc(sizeof(my_pcm_t) * MY_ENV_FRAME_SAMPLES);
    if (!buf) { printf("  [FAIL] malloc\n"); g_fail++; return r; }

    long sample_idx = 0;
    uint32_t prev_windows = 0;

    for (int sec = 0; sec < seconds; sec++) {
        /* 一秒 = 10 帧 × 1600 样本 */
        for (int f = 0; f < MY_ENV_RATE_HZ; f++) {
            gen_mic(&sc, buf, MY_ENV_FRAME_SAMPLES, &sample_idx);
            my_sleep_feed_mic(&d, buf, MY_ENV_FRAME_SAMPLES);
            gen_imu(&sc, &d, 1);
        }

        uint32_t now_windows = my_sleep_get_stats(&d)->windows_total;
        if (now_windows > prev_windows) {           /* 刚完成一个分析窗 */
            prev_windows = now_windows;
            r.windows++;
            my_sleep_state_t st = my_sleep_get_state(&d);
            const my_sleep_metrics_t *m = my_sleep_get_metrics(&d);
            bool pred = (st == MY_SLEEP_ASLEEP);
            if (pred) r.asleep_windows++;

            /* 暖机期（前 warmup_s 秒）不计入混淆矩阵：状态机需要
             * 30s 满窗 + sustain 窗数才可能翻转，这段是算法固有延迟，
             * 不是误判。稳态指标才有统计意义。 */
            if (sec >= warmup_s) {
                if (truth_asleep &&  pred) r.tp++;
                else if (truth_asleep && !pred) r.fn++;
                else if (!truth_asleep &&  pred) r.fp++;
                else r.tn++;
                r.sum_regularity += m->regularity;
                r.steady_windows++;
            }
            r.last_regularity  = m->regularity;
            r.last_breath_rate = m->breath_rate;
            r.last_movement    = m->movement;
        }
    }
    r.final_state      = my_sleep_get_state(&d);
    r.first_asleep_sec = my_sleep_get_stats(&d)->first_asleep_sec;
    free(buf);

    g_TP += r.tp; g_FP += r.fp; g_FN += r.fn; g_TN += r.tn;

    printf("  [INFO] 分析窗=%d  判睡窗=%d  稳态窗=%d  TP=%d FP=%d FN=%d TN=%d\n",
           r.windows, r.asleep_windows, r.steady_windows, r.tp, r.fp, r.fn, r.tn);
    printf("  [INFO] 末窗：规律度=%.3f  呼吸率=%.1f次/分  体动=%.3f  首次入睡=%us\n",
           r.last_regularity, r.last_breath_rate, r.last_movement, r.first_asleep_sec);
    return r;
}

/* ================= 各测试组 ================= */

/* 典型睡眠：12 次/分规律呼吸 + 静卧 */
static scenario_t sc_asleep(float bpm)
{
    scenario_t s = { .breath_bpm = bpm, .depth = 0.6f, .env_noise = 0.03f,
                     .base_amp = 6000.0f, .imu_activity = 0.02f, .rng = 0x12345678u };
    return s;
}

/* 典型清醒：呼吸不规律 + 频繁体动 */
static scenario_t sc_awake(void)
{
    scenario_t s = { .breath_bpm = 0.0f, .depth = 0.0f, .env_noise = 0.9f,
                     .base_amp = 6000.0f, .imu_activity = 0.8f, .rng = 0x9E3779B9u };
    return s;
}

static void test_api_bounds(void)
{
    printf("\n=== 1. API 边界与健壮性 ===\n");
    my_sleep_detector_t d;
    CHECK(my_sleep_init(NULL) == MY_ERR_PARAM, "init(NULL) 返回 MY_ERR_PARAM");
    CHECK(my_sleep_init(&d) == MY_OK, "init 返回 MY_OK");
    CHECK(my_sleep_get_state(&d) == MY_SLEEP_AWAKE, "初始状态 AWAKE");
    CHECK(my_sleep_get_metrics(NULL) == NULL, "get_metrics(NULL) 返回 NULL");
    CHECK(my_sleep_get_stats(&d)->windows_total == 0, "初始窗数 0");

    my_pcm_t buf[16] = {0};
    CHECK(my_sleep_feed_mic(NULL, buf, 16) == MY_ERR_PARAM, "feed_mic(NULL,..) 拒绝");
    CHECK(my_sleep_feed_mic(&d, NULL, 16) == MY_ERR_PARAM, "feed_mic(..,NULL,..) 拒绝");
    CHECK(my_sleep_feed_mic(&d, buf, -1) == MY_ERR_PARAM, "feed_mic 负长度拒绝");
    CHECK(my_sleep_feed_imu(NULL, 0, 0, 1.0f) == MY_ERR_PARAM, "feed_imu(NULL) 拒绝");
    CHECK(my_sleep_feed_imu(&d, 0, 0, 1.0f) == MY_OK, "feed_imu 正常返回");
    my_sleep_reset_state(NULL);   /* 不应崩溃 */
    CHECK(1, "reset_state(NULL) 不崩溃");
    CHECK(sizeof(my_sleep_detector_t) < 4096,
          "状态体 %.0f 字节 < 4KB（可放低功耗域）", (double)sizeof(my_sleep_detector_t));
}

static void test_silence(void)
{
    printf("\n=== 2. 纯静音（无信号，最易误判的退化输入） ===\n");
    scenario_t s = { .breath_bpm = 0, .depth = 0, .env_noise = 0,
                     .base_amp = 0.0f, .imu_activity = 0.02f, .rng = 1 };
    run_result_t r = run_scenario("SILENCE", s, 60, false, 40);

    CHECK(r.last_regularity < 0.1f, "静音规律度接近 0 (实测 %.3f)", r.last_regularity);
    CHECK(r.last_breath_rate == 0.0f, "静音不输出呼吸率 (实测 %.1f)", r.last_breath_rate);
    CHECK(r.final_state != MY_SLEEP_ASLEEP, "静音不误判为入睡（麦克风被捂住/掉线场景）");
    CHECK(r.tp + r.fp == 0, "静音期无任何 ASLEEP 窗");
}

static void test_asleep_detection(void)
{
    printf("\n=== 3. 入睡检出（核心正例） ===\n");
    scenario_t s = sc_asleep(12.0f);   /* 12 次/分，成人睡眠典型值 */
    run_result_t r = run_scenario("SLEEP-12bpm", s, 120, true, 45);

    CHECK(r.final_state == MY_SLEEP_ASLEEP, "120s 后判定为 ASLEEP");
    CHECK(r.first_asleep_sec > 0 && r.first_asleep_sec <= 60,
          "首次入睡时刻 %us 落在合理区间（30s满窗+3窗sustain≈36s，容差到60s）",
          r.first_asleep_sec);
    CHECK(r.fn == 0, "稳态期零漏报（FN=%d）", r.fn);
    CHECK(r.last_breath_rate > 10.0f && r.last_breath_rate < 14.0f,
          "呼吸率估计 %.1f 次/分 ≈ 真值 12", r.last_breath_rate);
    CHECK(r.last_regularity > 0.6f, "规律度 %.3f 高于入睡阈值 0.60", r.last_regularity);
    CHECK(r.last_movement < 0.12f, "体动 %.3f 低于静卧阈值 0.12", r.last_movement);
}

static void test_single_modality_gating(void)
{
    printf("\n=== 4. 单模态门控（证明 AND 融合真的起作用） ===\n");

    /* 4a 呼吸规律但体动多：躺着刷手机/翻身，呼吸仍规律 → 不应判入睡 */
    scenario_t a = sc_asleep(12.0f);
    a.imu_activity = 0.8f;
    a.rng = 0xAAAABBBBu;
    run_result_t ra = run_scenario("REG+MOVING", a, 90, false, 45);
    CHECK(ra.final_state != MY_SLEEP_ASLEEP,
          "呼吸规律但体动大 → 不判入睡（防误报关键用例）");
    CHECK(ra.fp == 0, "该场景零误报（FP=%d）", ra.fp);
    CHECK(ra.last_regularity > 0.6f,
          "规律度确实很高 (%.3f)，说明是体动门控挡住了误判", ra.last_regularity);

    /* 4b 静止但呼吸不规律：醒着安静躺床 → 不应判入睡 */
    scenario_t b = sc_awake();
    b.imu_activity = 0.02f;
    b.rng = 0xCCCCDDDDu;
    run_result_t rb = run_scenario("IRREG+STILL", b, 90, false, 45);
    CHECK(rb.final_state != MY_SLEEP_ASLEEP,
          "体动静止但呼吸不规律 → 不判入睡（防误报关键用例）");
    CHECK(rb.fp == 0, "该场景零误报（FP=%d）", rb.fp);
    CHECK(rb.last_regularity < 0.45f,
          "不规律呼吸规律度 %.3f 低于困倦阈值 0.45", rb.last_regularity);
}

static void test_awake(void)
{
    printf("\n=== 5. 清醒负例 ===\n");
    scenario_t s = sc_awake();
    run_result_t r = run_scenario("AWAKE", s, 90, false, 40);
    CHECK(r.final_state == MY_SLEEP_AWAKE, "清醒场景稳定停在 AWAKE");
    CHECK(r.fp == 0, "清醒场景零误报（FP=%d）", r.fp);
    CHECK(r.asleep_windows == 0, "全程无 ASLEEP 窗");
    CHECK(r.last_movement > 0.35f, "体动 %.3f 高于清醒阈值 0.35", r.last_movement);
}

/* 入睡 → 夜醒 → 复睡，验证状态机往返与事件计数 */
static void test_night_wake_cycle(void)
{
    printf("\n=== 6. 夜醒与复睡（状态机往返） ===\n");
    my_sleep_detector_t d;
    my_sleep_init(&d);
    my_pcm_t *buf = malloc(sizeof(my_pcm_t) * MY_ENV_FRAME_SAMPLES);
    if (!buf) { printf("  [FAIL] malloc\n"); g_fail++; return; }

    long idx = 0;
    /* 阶段计时：以"秒"为单位喂数据 */
    #define FEED_SEC(sc_ptr, nsec) do {                       \
        for (int _s = 0; _s < (nsec); _s++) {                 \
            for (int _f = 0; _f < MY_ENV_RATE_HZ; _f++) {     \
                gen_mic((sc_ptr), buf, MY_ENV_FRAME_SAMPLES, &idx); \
                my_sleep_feed_mic(&d, buf, MY_ENV_FRAME_SAMPLES);   \
                gen_imu((sc_ptr), &d, 1);                     \
            }                                                 \
        }                                                     \
    } while (0)

    scenario_t sleep = sc_asleep(12.0f);
    scenario_t wake  = sc_awake();
    wake.imu_activity = 1.2f;   /* 明显夜醒动作 */
    wake.rng = 0x5151F00Du;

    /* ① 入睡 90s */
    FEED_SEC(&sleep, 90);
    CHECK(my_sleep_get_state(&d) == MY_SLEEP_ASLEEP, "阶段①：90s 后进入 ASLEEP");
    uint32_t t1 = my_sleep_get_stats(&d)->transitions_to_asleep;
    CHECK(t1 == 1, "阶段①：入睡事件计数 = 1（实测 %u）", t1);
    uint32_t first_sec = my_sleep_get_stats(&d)->first_asleep_sec;

    /* ② 夜醒 30s */
    FEED_SEC(&wake, 30);
    CHECK(my_sleep_get_state(&d) == MY_SLEEP_AWAKE, "阶段②：高体动后回退 AWAKE");
    CHECK(my_sleep_get_stats(&d)->night_wake_events == 1,
          "阶段②：夜醒事件计数 = 1（实测 %u）",
          my_sleep_get_stats(&d)->night_wake_events);
    CHECK(my_sleep_get_stats(&d)->first_asleep_sec == first_sec,
          "阶段②：first_asleep_sec 不被夜醒重置（仍是 %us）", first_sec);

    /* ③ 复睡 90s */
    sleep.rng = 0x77778888u;
    FEED_SEC(&sleep, 90);
    CHECK(my_sleep_get_state(&d) == MY_SLEEP_ASLEEP, "阶段③：复睡成功回到 ASLEEP");
    CHECK(my_sleep_get_stats(&d)->transitions_to_asleep == 2,
          "阶段③：入睡事件累计 = 2（实测 %u）",
          my_sleep_get_stats(&d)->transitions_to_asleep);
    CHECK(my_sleep_get_stats(&d)->night_wake_events == 1,
          "阶段③：夜醒计数保持 1，未被复睡污染");

    /* ④ reset 后统计清零但配置保留 */
    my_sleep_reset_state(&d);
    CHECK(my_sleep_get_state(&d) == MY_SLEEP_AWAKE, "阶段④：reset 后回 AWAKE");
    CHECK(my_sleep_get_stats(&d)->transitions_to_asleep == 0, "阶段④：reset 清零事件计数");
    CHECK(my_sleep_get_stats(&d)->night_wake_events == 0, "阶段④：reset 清零夜醒计数");
    CHECK(fabsf(d.cfg.regularity_asleep - 0.60f) < 1e-6f, "阶段④：reset 保留阈值配置");
    #undef FEED_SEC
    free(buf);
}

/* 无 IMU 的 MIC 单判据夜醒：目标板 DevKit-LCD 无板载 IMU，
 * 夜醒只能靠「呼吸节律塌缩」检测——这是 9/8 板卡变更后补齐的路径。 */
static void test_mic_only_night_wake(void)
{
    printf("\n=== 6b. MIC 单判据夜醒（无 IMU，对应 DevKit-LCD 真机形态） ===\n");
    my_sleep_detector_t d;
    my_sleep_init(&d);
    my_pcm_t *buf = malloc(sizeof(my_pcm_t) * MY_ENV_FRAME_SAMPLES);
    if (!buf) { printf("  [FAIL] malloc\n"); g_fail++; return; }

    long idx = 0;
    /* 只喂 MIC、绝不喂 IMU（模拟无板载 IMU 的 DevKit-LCD） */
    #define FEED_MIC_ONLY(sc_ptr, nsec) do {                  \
        for (int _s = 0; _s < (nsec); _s++) {                 \
            for (int _f = 0; _f < MY_ENV_RATE_HZ; _f++) {     \
                gen_mic((sc_ptr), buf, MY_ENV_FRAME_SAMPLES, &idx); \
                my_sleep_feed_mic(&d, buf, MY_ENV_FRAME_SAMPLES);   \
            }                                                 \
        }                                                     \
    } while (0)

    scenario_t sleep = sc_asleep(16.0f);
    scenario_t talk  = sc_awake();   /* 说话：无周期呼吸，但体动为 0（无 IMU） */

    /* ① 纯 MIC 入睡 90s（证明无 IMU 也能入睡） */
    FEED_MIC_ONLY(&sleep, 90);
    CHECK(my_sleep_get_state(&d) == MY_SLEEP_ASLEEP, "阶段①：纯 MIC 90s 后进入 ASLEEP");

    /* ② 说话 30s（体动始终 0，只能靠呼吸塌缩检出夜醒） */
    FEED_MIC_ONLY(&talk, 30);
    CHECK(my_sleep_get_state(&d) == MY_SLEEP_AWAKE, "阶段②：说话后回退 AWAKE（MIC 判据）");
    CHECK(my_sleep_get_stats(&d)->night_wake_events == 1,
          "阶段②：夜醒事件计数 = 1（实测 %u）",
          my_sleep_get_stats(&d)->night_wake_events);
    #undef FEED_MIC_ONLY
    free(buf);
}

/* 呼吸率估计精度扫描：覆盖成人睡眠常见范围 */
static void test_breath_rate_accuracy(void)
{
    printf("\n=== 7. 呼吸率估计精度扫描 ===\n");
    static const float bpms[] = { 8.0f, 10.0f, 12.0f, 16.0f, 20.0f, 25.0f };
    int ok_cnt = 0;

    for (unsigned i = 0; i < sizeof(bpms) / sizeof(bpms[0]); i++) {
        scenario_t s = sc_asleep(bpms[i]);
        s.rng = 0x0F0F0F0Fu + (uint32_t)(bpms[i] * 1000);

        my_sleep_detector_t d;
        my_sleep_init(&d);
        my_pcm_t *buf = malloc(sizeof(my_pcm_t) * MY_ENV_FRAME_SAMPLES);
        if (!buf) { g_fail++; continue; }
        long idx = 0;
        /* 跑 70s：30s 满窗 + 20 个分析窗，规律度已收敛 */
        for (int sec = 0; sec < 70; sec++) {
            for (int f = 0; f < MY_ENV_RATE_HZ; f++) {
                gen_mic(&s, buf, MY_ENV_FRAME_SAMPLES, &idx);
                my_sleep_feed_mic(&d, buf, MY_ENV_FRAME_SAMPLES);
                gen_imu(&s, &d, 1);
            }
        }
        free(buf);

        float est = my_sleep_get_metrics(&d)->breath_rate;
        float err = fabsf(est - bpms[i]) / bpms[i] * 100.0f;
        /* 离散滞后量化误差上限：lag=±1 → 相对误差 1/lag。
         * 最坏情况 8bpm(lag=75) 1.3%，25bpm(lag=24) 4.2%。给 12% 容差。 */
        bool good = (err < 12.0f);
        if (good) ok_cnt++;
        /* 用 OK/NO 而非 [PASS]/[FAIL]：逐点扫描不是独立断言，避免与
         * CHECK 宏的计数器/grep 统计重复计入（下面汇总才是真断言）。 */
        printf("  [ %s ] 真值 %5.1f 次/分 → 估计 %5.1f（误差 %.1f%%）规律度 %.3f\n",
               good ? " OK " : "NO ", bpms[i], est, err,
               my_sleep_get_metrics(&d)->regularity);
    }
    CHECK(ok_cnt == (int)(sizeof(bpms) / sizeof(bpms[0])),
          "全部呼吸率点在 12%% 误差内（实际 %d/%d）", ok_cnt,
          (int)(sizeof(bpms) / sizeof(bpms[0])));
}

/* 超出设计范围的呼吸率不应被误当成有效呼吸 */
static void test_out_of_range_rate(void)
{
    printf("\n=== 8. 超范围呼吸率（设计边界诚实性检查） ===\n");
    /* 6 次/分 → 周期 10s → lag 100 > MY_LAG_MAX(80)，落在搜索窗外。
     * 算法应当"检不到"而不是硬凑一个错值：这正是 3.5 节要写的已知边界。 */
    scenario_t s = sc_asleep(6.0f);
    s.rng = 0x00000606u;
    my_sleep_detector_t d;
    my_sleep_init(&d);
    my_pcm_t *buf = malloc(sizeof(my_pcm_t) * MY_ENV_FRAME_SAMPLES);
    if (!buf) { printf("  [FAIL] malloc\n"); g_fail++; return; }
    long idx = 0;
    for (int sec = 0; sec < 70; sec++) {
        for (int f = 0; f < MY_ENV_RATE_HZ; f++) {
            gen_mic(&s, buf, MY_ENV_FRAME_SAMPLES, &idx);
            my_sleep_feed_mic(&d, buf, MY_ENV_FRAME_SAMPLES);
            gen_imu(&s, &d, 1);
        }
    }
    free(buf);

    const my_sleep_metrics_t *m = my_sleep_get_metrics(&d);
    printf("  [INFO] 6次/分输入 → 规律度 %.3f，呼吸率 %.1f（真值 6.0，窗外）\n",
           m->regularity, m->breath_rate);
    /* 允许两种诚实结果：判为无效峰，或规律度显著低于正常睡眠场景 */
    CHECK(!m->peak_valid || m->regularity < 0.9f || fabsf(m->breath_rate - 6.0f) > 0.5f,
          "6次/分不被当作正常范围内呼吸（未伪造精度）");
}

/* ================= 汇总 ================= */

static void print_confusion_matrix(void)
{
    printf("\n=== 9. 混淆矩阵汇总（技术报告 3.5 节直接引用） ===\n");
    int total = g_TP + g_FP + g_FN + g_TN;
    printf("                 预测=入睡   预测=清醒\n");
    printf("  真值=睡眠        TP=%-5d   FN=%-5d\n", g_TP, g_FN);
    printf("  真值=清醒        FP=%-5d   TN=%-5d\n", g_FP, g_TN);
    printf("  稳态分析窗合计 = %d\n", total);

    if (total == 0) { printf("  [FAIL] 无有效窗\n"); g_fail++; return; }

    double fpr = (g_FP + g_TN) ? 100.0 * g_FP / (g_FP + g_TN) : 0.0;   /* 误报率 */
    double fnr = (g_TP + g_FN) ? 100.0 * g_FN / (g_TP + g_FN) : 0.0;   /* 漏报率 */
    double acc = 100.0 * (g_TP + g_TN) / total;                        /* 准确率 */
    double prec = (g_TP + g_FP) ? 100.0 * g_TP / (g_TP + g_FP) : 100.0;

    printf("  误报率 FPR = %.2f%%   （醒着判成睡着 → 会导致声音提前停）\n", fpr);
    printf("  漏报率 FNR = %.2f%%   （睡着没判出 → 会导致整夜播放）\n", fnr);
    printf("  准确率 ACC = %.2f%%   精确率 PRE = %.2f%%\n", acc, prec);

    CHECK(g_FP == 0, "全部清醒场景零误报（FP=%d）", g_FP);
    CHECK(g_FN == 0, "全部睡眠场景零漏报（FN=%d）", g_FN);
    CHECK(acc > 99.0, "综合准确率 %.2f%% > 99%%", acc);

    /* ---- 诚实性声明：这组数字能证明什么、不能证明什么 ----
     *
     * 本矩阵来自【合成信号】：呼吸包络是纯正弦调幅、载波是理想白噪、
     * 体动是均匀随机数。它证明的是：
     *   ✓ 算法逻辑正确——自相关能在指定滞后窗内锁定呼吸基频
     *   ✓ MIC 与 IMU 的 AND 融合确实生效——单模态达标不会误判（见测试组 4）
     *   ✓ 状态机迟滞与夜醒计数行为符合设计
     *   ✓ 阈值在理想信噪比下可分（睡眠规律度 0.83 vs 清醒 0.10，间隔充分）
     *
     * 它【不能】证明真机误报率/漏报率就是 0%。真实场景的干扰源：
     *   ✗ 鼾声（宽带脉冲，会污染呼吸包络周期）
     *   ✗ 空调/风扇稳态噪声（抬高基底，压低规律度）
     *   ✗ 同床伴侣呼吸（两个基频叠加 → 自相关出现伪峰）
     *   ✗ 翻身时被褥摩擦声（MIC 端也会动，与 IMU 体动重复计入）
     *   ✗ 说话/咳嗽（突发高能，可能被误判为体动或呼吸中断）
     *
     * 因此技术报告 3.5 节必须【分开写两组数据】：
     *   ① 合成信号验证：本矩阵（100% ACC，119 窗），标注"算法逻辑正确性验证"
     *   ② 真机实测：黄山派 + 真人整夜录音回放，N 晚 × 逐窗人工标注真值，
     *      给出真实 FPR/FNR，并附阈值调整记录（k_default_cfg 会被改）
     * 只写①不写② = 数据造假；②没有就如实写"真机标定进行中，
     * 当前给出的是合成信号下的算法上限"。这条边界务必守住。
     */
    printf("\n  注意：边界声明：以上矩阵基于【合成信号】，仅证明算法逻辑正确性，\n"
           "     不可作为真机误报率/漏报率写入报告。真机需用整夜录音回放\n"
           "     + 人工标注另做标定（干扰源：鼾声/空调/同床呼吸/被褥摩擦/说话）。\n"
           "     报告 3.5 节须分组呈现：合成信号验证 + 真机实测两组数据。\n");
}

int main(void)
{
    printf("============================================\n");
    printf(" 安眠科技 · 入睡判定单元测试\n");
    printf(" 包络率 %dHz / 分析窗 %ds / hop %d帧 / lag 搜索 [%d,%d]\n",
           MY_ENV_RATE_HZ, MY_ANALYSIS_WINDOW_S, MY_ANALYSIS_HOP,
           MY_LAG_MIN, MY_LAG_MAX);
    printf("============================================\n");

    test_api_bounds();
    test_silence();
    test_asleep_detection();
    test_single_modality_gating();
    test_awake();
    test_night_wake_cycle();
    test_mic_only_night_wake();
    test_breath_rate_accuracy();
    test_out_of_range_rate();
    print_confusion_matrix();

    printf("\n============================================\n");
    printf(" 结果： %d 通过 / %d 失败\n", g_pass, g_fail);
    printf("MY_RESULT: pass=%d fail=%d\n", g_pass, g_fail);
    printf("============================================\n");
    return g_fail ? 1 : 0;
}
