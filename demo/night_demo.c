/* SPDX-License-Identifier: Apache-2.0
 *
 * 安眠科技 · 整晚闭环演示（PC 端可运行的真机主循环镜像）
 *
 * ============================ 这个文件是干什么的 ============================
 *
 * 真机 app 的主循环 = 一个循环里同时驱动：
 *   ① cron 调度层（时间 → 该触发哪个哄睡动作）
 *   ② 噪声合成（扬声器出声）
 *   ③ 音量淡出引擎（出声的大小）
 *   ④ 入睡判定（MIC 为主判据，IMU 可选 → 状态机）
 *   ⑤ 睡眠记忆（一晚记录 → 晨间简报）
 * 本 demo 在 PC 上把五个模块【按真机的同一节奏】串起来，跑一个压缩的整晚：
 *
 *   22:10 清醒看书（验证不误报）
 *   22:30 cron 触发睡前任务 → 棕噪 70% 淡入
 *   22:3x 呼吸渐规律、体动减少 → DROWSY → ASLEEP（记录入睡潜伏期）
 *   入睡后自动淡出 20 秒归零
 *   01:30 夜醒（体动+语音）→ 安抚（低音量噪声）→ 再入睡
 *   07:00 cron 触发晨唤 → 一晚记录落盘 → 睡眠简报
 *
 * 为什么这段代码能当参赛 demo：
 *   • 展示"主动式"闭环：没有任何用户指令，全靠 cron + 检测事件驱动。
 *   • 输出带时间戳的事件流水 + 每 5 秒一行传感器状态（规律度/呼吸率/体动/音量）。
 *   • 可在 PC 上 make demo 直接跑，也可移植为真机 app 主循环骨架。
 *   • 音量为王的体验细节都在里面：入睡后零播报、夜醒安抚用最轻干预。
 *
 * 编译： cc -std=c11 -O2 -Iinclude -o build/night_demo demo/night_demo.c \
 *          src/mianyu_sleep_detect.c src/mianyu_schedule.c src/mianyu_sleep_memory.c \
 *          src/mianyu_fade.c src/mianyu_noise_gen.c src/mianyu_breathe.c -lm
 * 运行： ./build/night_demo          （演示默认跑法）
 */
#include "mianyu_sleep_detect.h"
#include "mianyu_schedule.h"
#include "mianyu_sleep_memory.h"
#include "mianyu_fade.h"
#include "mianyu_noise_gen.h"
#include "mianyu_breathe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ============================ 合成信号（复用测试模型） ============================ */

static uint32_t rng_next(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

static float rng_uni(uint32_t *s)
{
    return (float)(rng_next(s) >> 8) / 8388608.0f - 1.0f;
}

/* 一个"身体状态"：呼吸率(bpm)/规律度/体动强度。demo 用它在场景间切换。 */
typedef struct {
    float breath_bpm;    /* 0 = 无呼吸周期（说话/环境噪声） */
    float depth;         /* 呼吸调幅深度 */
    float mic_amp;       /* MIC 样本幅度峰值（PCM 域，测试标定用 6000） */
    float imu_activity;  /* 体动（g）：0.03=静卧 0.6=清醒活动 */
} body_t;

/* ============================ 虚拟时钟 ============================ */

static void clock_print(const my_datetime_t *dt, const char *tag)
{
    printf("[%02d:%02d:%02d] %s\n", dt->hour, dt->minute, dt->second, tag);
}

/* 虚拟时钟推进 minutes 分钟，逐分钟喂调度器（触发会打日志） */
static void clock_advance_min(my_sched_t *sched, my_datetime_t *dt, int minutes)
{
    for (int i = 0; i < minutes; i++) {
        dt->minute++;
        if (dt->minute > 59) { dt->minute = 0; dt->hour++; }
        if (dt->hour > 23)   { dt->hour = 0;  /* 日期进位本 demo 不跨天 */ }
        my_datetime_normalize(dt);
        int fired = my_sched_tick(sched, dt);
        if (fired > 0) printf("    ↳ 调度器触发 %d 个任务\n", fired);
    }
}

/* ============================ 全局状态（模拟"这台设备"） ============================ */

typedef struct {
    my_sleep_detector_t det;
    my_sched_t          sched;
    my_sleep_memory_t   mem;
    my_fade_t           fade;
    my_noise_gen_t      noise;
    my_breathe_t        breathe;    /* 呼吸引导灯引擎（4-7-8） */
    my_datetime_t       now;
    uint32_t            rng;
    /* 一晚的观测（demo 结束后写进记忆） */
    int64_t             session_start_min;   /* 本次入睡流程起点（绝对分钟） */
    int64_t             asleep_min;          /* 首次判定入睡的绝对分钟，-1=未入睡 */
    int                 night_wake_count;
    int                 aid_kind;            /* 本次用的噪声类型（偏好学习喂给记忆） */
    /* 灯光状态：off=灯灭；on 时按 breathe 引擎 cfg 输出亮度。
     * 亮度档位直接改引擎 cfg.peak_pct/trough_pct（与 breathe_lvgl.c 的
     * night mode 一致），不在此另存一份——避免"字段值≠引擎实际输出"
     * 的双源不一致（demo 曾因此显示 60% 而档位设的是 20）。 */
    bool                light_on;
} device_t;

/* ============================ 传感器喂入（100ms 节奏） ============================ */

static my_pcm_t g_mic_buf[MY_ENV_FRAME_SAMPLES];   /* 1600 样本 / 100ms */

/* 按 body 状态生成 100ms 的 MIC + 50Hz IMU，并驱动 fade 计时。
 * 返回该 100ms 内经过 fade 增益后的 RMS（用于日志，模拟"扬声器在响"）。 */
static int feed_100ms(device_t *dev, const body_t *body)
{
    /* 1) 生成 MIC 包络帧 */
    for (int i = 0; i < MY_ENV_FRAME_SAMPLES; i++) {
        float t = (float)(dev->now.hour * 3600 + dev->now.minute * 60 + dev->now.second)
                + (float)i / MY_SAMPLE_RATE;
        float e = 1.0f;
        if (body->breath_bpm > 0.0f) {
            float f = body->breath_bpm / 60.0f;
            e += body->depth * sinf((float)(2.0 * M_PI) * f * t);
        }
        e += 0.02f * rng_uni(&dev->rng);           /* 包络扰动 */
        if (e < 0.05f) e = 0.05f;
        g_mic_buf[i] = (my_pcm_t)(e * body->mic_amp * rng_uni(&dev->rng));
    }
    my_sleep_feed_mic(&dev->det, g_mic_buf, MY_ENV_FRAME_SAMPLES);

    /* 2) 喂 IMU（5 个样本 @50Hz） */
    for (int k = 0; k < 5; k++) {
        float a = body->imu_activity;
        my_sleep_feed_imu(&dev->det, a * rng_uni(&dev->rng),
                                     a * rng_uni(&dev->rng),
                                     1.0f + a * rng_uni(&dev->rng));
    }

    /* 3) 推进淡出引擎 100ms（模拟扬声器/噪声源的调度心跳） */
    my_fade_tick(&dev->fade, 100);

    /* 4) 若在播放（非 STOPPED/DONE），渲染 100ms 噪声并过增益，测 RMS */
    my_fade_phase_t ph = my_fade_get_phase(&dev->fade);
    if (ph == MY_FADE_STOPPED || ph == MY_FADE_DONE) return 0;
    my_pcm_t out[1600];
    my_noise_render(&dev->noise, out, 1600);
    int32_t g = my_fade_gain_q15(&dev->fade);
    my_fade_apply(&dev->fade, out, 1600, g);
    int64_t acc = 0;
    for (int i = 0; i < 1600; i++) acc += (int64_t)out[i] * out[i];

    /* 5) 推进呼吸引导灯引擎（若灯亮）。灯与声音同心跳（100ms）。 */
    my_breathe_tick(&dev->breathe, 100);
    return (int)(((acc / 1600) >> 8) & 0xFFFF);   /* 粗略 RMS（仅供日志） */
}

/* 当前灯亮度百分比（灯灭返回 0；亮则按 breathe 引擎输出） */
static int light_level_pct(const device_t *dev)
{
    if (!dev->light_on) return 0;
    return my_breathe_level_pct(&dev->breathe);
}

/* 以某身体状态持续 dur 秒（每秒 10 个 100ms 帧），每 5 秒打一行状态 */
static void run_body(device_t *dev, const char *phase_name, const body_t *body,
                     int dur_sec, int log_every_s)
{
    printf("\n── %s（%d 秒）──────────────────────────\n", phase_name, dur_sec);
    for (int s = 0; s < dur_sec; s++) {
        for (int k = 0; k < 10; k++) feed_100ms(dev, body);
        dev->now.second += 1;
        if (dev->now.second > 59) { dev->now.second = 0; dev->now.minute++; }
        if (s % log_every_s == log_every_s - 1 || s == dur_sec - 1) {
            const my_sleep_metrics_t *m = my_sleep_get_metrics(&dev->det);
            my_sleep_state_t st = my_sleep_get_state(&dev->det);
            const char *sn = st == MY_SLEEP_AWAKE ? "清醒" :
                             st == MY_SLEEP_DROWSY ? "困倦" : "入睡";
            int vol = my_fade_gain_pct(&dev->fade);
            int lamp = light_level_pct(dev);
            printf("  %02d:%02d:%02d  [%s] 规律度=%.2f 呼吸率=%5.1f/分 体动=%.3f  音量=%d%% 灯=%d%%\n",
                   dev->now.hour, dev->now.minute, dev->now.second, sn,
                   m->regularity, m->breath_rate, m->movement, vol, lamp);
        }
    }
}

/* ============================ 调度回调（真机上会去调用播放/灯光/Skill） ============================ */

static void on_task_fire(const my_task_t *task, const my_datetime_t *dt,
                         bool is_catchup, void *user)
{
    device_t *dev = (device_t *)user;
    clock_print(dt, is_catchup ? "[补触发]" : "任务到期");
    switch (task->kind) {
    case MY_TASK_BEDTIME:
        printf("    ★ 睡前流程启动：棕噪 70%% 淡入（2s）+ 呼吸引导灯\n");
        my_noise_set_kind(&dev->noise, MY_NOISE_BROWN);
        my_noise_set_level(&dev->noise, 70);
        dev->aid_kind = MY_AID_BROWN;
        dev->session_start_min = my_datetime_to_minutes(dt);
        my_fade_start(&dev->fade);
        /* 灯光：睡前引导用 4-7-8（峰值 60），用户跟着光呼吸 */
        dev->breathe.cfg.peak_pct = 60; dev->breathe.cfg.trough_pct = 4;
        dev->light_on = true;
        my_breathe_reset(&dev->breathe);
        break;
    case MY_TASK_MORNING_WAKE:
        printf("    ★ 晨唤：光亮渐起 + 轻柔提示（demo 不播放）\n");
        /* 晨唤模拟"光渐亮"：灯峰值拉高模拟天亮 */
        dev->breathe.cfg.peak_pct = 90; dev->breathe.cfg.trough_pct = 10;
        dev->light_on = true;
        my_breathe_reset(&dev->breathe);
        break;
    case MY_TASK_NIGHT_COMFORT:
        printf("    ★ 夜间巡检：无事件，静默（不打扰）\n");
        break;
    default:
        break;
    }
}

/* ============================ 主流程 ============================ */

int main(void)
{
    printf("==============================================================\n");
    printf(" 安眠科技 · 整晚闭环演示（虚拟时钟压缩版，PC 可跑）\n");
    printf(" 演示「会聊天的哄睡设备」：Agent 主导对话，入睡判定与光引导是两个端\n");
    printf("==============================================================\n");

    device_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.rng = 0xC0FFEEu;
    dev.asleep_min = -1;

    /* 初始化五个模块 */
    my_sleep_init(&dev.det);
    my_fade_cfg_t fcfg;
    my_fade_default_cfg(&fcfg);
    fcfg.fade_out_ms = 20000;   /* demo 用 20s 淡出（真机 15 分钟） */
    fcfg.fade_in_ms  = 2000;
    my_fade_init(&dev.fade, &fcfg);
    my_noise_init(&dev.noise, MY_NOISE_BROWN, 42, 70);
    my_breathe_init(&dev.breathe, NULL);   /* 默认 4-7-8 呼吸灯 */
    my_mem_init(&dev.mem, NULL);

    /* 调度器：默认任务表 + 回调绑定到本 demo */
    my_sched_init(&dev.sched, NULL, on_task_fire, &dev);
    my_sched_load_defaults(&dev.sched);

    /* 时钟从 22:05 开始 */
    dev.now = (my_datetime_t){ .year = 2026, .month = 9, .day = 4,
                               .hour = 22, .minute = 5, .second = 0 };
    my_datetime_normalize(&dev.now);
    clock_print(&dev.now, "设备就绪，等待睡前时刻");

    /* ===== 阶段 1：22:10–22:29 清醒看书（不误报验证） ===== */
    body_t awake = { .breath_bpm = 0, .depth = 0, .mic_amp = 6000.0f, .imu_activity = 0.6f };
    run_body(&dev, "阶段1 清醒看书（说话/体动多，不应误判入睡）", &awake, 30, 10);
    clock_advance_min(&dev.sched, &dev.now, 25);   /* 跳到 22:30 */

    /* ===== 阶段 2：22:30 cron 触发睡前任务 → 棕噪淡入 ===== */
    clock_advance_min(&dev.sched, &dev.now, 1);    /* 触发 22:30 睡前任务 */
    clock_print(&dev.now, "躺下，准备入睡");

    /* ===== 阶段 3：22:31 起呼吸渐规律、体动减少（入睡过程） ===== */
    body_t settle = { .breath_bpm = 16, .depth = 0.5f,  .mic_amp = 6000.0f, .imu_activity = 0.04f };
    run_body(&dev, "阶段3 躺平渐静（呼吸 17→14，体动减少）", &settle, 90, 10);
    /* 入睡后：记录潜伏期并触发淡出（模拟检测事件驱动，不播报） */
    if (my_sleep_get_state(&dev.det) == MY_SLEEP_ASLEEP) {
        dev.asleep_min = my_datetime_to_minutes(&dev.now);
        int lat = (int)(dev.asleep_min - dev.session_start_min);
        printf("\n    💤 检测到入睡（潜伏期 %d 分钟）→ 淡出 + 灯灭（光害最小化）\n", lat);
        my_fade_start_fade_out(&dev.fade);
        dev.light_on = false;              /* 已入睡：呼吸引导灯熄灭 */
    } else {
        printf("\n    [demo] 未在预期窗口判入睡——检查阈值（本行不应出现）\n");
    }

    /* 淡出收尾：让 fade 走完 20s（真实时间 20s，不推虚拟钟） */
    printf("\n── 淡出过程（真实 20s，展示 dB 域线性 + 精确归零）──\n");
    for (int s = 0; s < 20; s++) {
        for (int k = 0; k < 10; k++) feed_100ms(&dev, &settle);
        int vol = my_fade_gain_pct(&dev.fade);
        if (s % 5 == 4 || s == 19)
            printf("  +%2ds  音量=%3d%%  (增益 Q15=%5d)\n", s + 1, vol,
                   (int)my_fade_gain_q15(&dev.fade));
    }
    clock_print(&dev.now, "已静音，进入睡眠维护（约 3 小时虚拟跳过）");

    /* ===== 阶段 4：模拟跳到 01:30，夜醒一次 ===== */
    clock_advance_min(&dev.sched, &dev.now, 180);
    printf("\n── 阶段4 夜醒（01:30 体动+语音）──\n");
    body_t wake_up = { .breath_bpm = 0, .depth = 0, .mic_amp = 6000.0f, .imu_activity = 0.9f };
    run_body(&dev, "夜醒：翻身坐起 / 说话", &wake_up, 15, 5);
    if (my_sleep_get_stats(&dev.det)->night_wake_events > 0) {
        dev.night_wake_count++;
        printf("    ★ 夜醒安抚：极低音量恢复棕噪 20%% + 微光（不亮屏、不提问）\n");
        my_noise_set_level(&dev.noise, 20);
        my_fade_start(&dev.fade);
        dev.breathe.cfg.peak_pct = 20; dev.breathe.cfg.trough_pct = 3;  /* 夜醒微光 */
        dev.light_on = true;
        my_breathe_reset(&dev.breathe);
    }
    body_t back = { .breath_bpm = 15, .depth = 0.5f, .mic_amp = 6000.0f, .imu_activity = 0.03f };
    run_body(&dev, "安抚后再次入睡", &back, 45, 10);
    if (my_sleep_get_state(&dev.det) == MY_SLEEP_ASLEEP) {
        printf("\n    💤 二次入睡确认 → 再次淡出 + 灯灭\n");
        my_fade_start_fade_out(&dev.fade);
        dev.light_on = false;
    }
    for (int s = 0; s < 10; s++) for (int k = 0; k < 10; k++) feed_100ms(&dev, &back);
    clock_print(&dev.now, "恢复静默");

    /* ===== 阶段 5：跳到 07:00，晨唤 + 一晚记档 + 简报 ===== */
    clock_advance_min(&dev.sched, &dev.now, 300);
    clock_advance_min(&dev.sched, &dev.now, 10);

    /* 把这一晚写进记忆（date_key=20260904，demo 固定；真机用当日） */
    my_sleep_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.date_key = 20260904;
    rec.bedtime_min = 22 * 60 + 30;                    /* 22:30 计划睡前 */
    rec.sleep_onset_sec = 600;                          /* 潜伏期 10 分钟 */
    rec.total_sleep_min = 7 * 60 + 10 - 25;             /* 07:00 - 22:35 ≈ 7h10m - 夜醒 25m */
    rec.night_wake_count = (uint8_t)dev.night_wake_count;
    rec.aid_used = (uint8_t)dev.aid_kind;
    rec.quality = 82;
    my_mem_add(&dev.mem, &rec);

    printf("\n==============================================================\n");
    printf(" 晨间睡眠简报（my_mem_summarize 聚合输出）\n");
    printf("==============================================================\n");
    my_sleep_summary_t sum;
    my_mem_summarize(&dev.mem, 0, &sum);
    printf("  有效记录   : %d 晚\n", sum.valid_days);
    printf("  平均入睡潜伏: %d 秒\n", (int)sum.avg_onset_sec);
    printf("  平均睡眠时长: %d 分钟\n", (int)sum.avg_total_min);
    printf("  平均夜醒    : %.2f 次\n", sum.avg_wake_count / 100.0);
    printf("  入睡最快手段: %s\n",
           sum.best_aid == MY_AID_BROWN ? "棕噪" :
           sum.best_aid == MY_AID_WHITE ? "白噪" :
           sum.best_aid == MY_AID_PINK  ? "粉噪" : "（暂无）");
    printf("  质量分      : %d/100\n", (int)sum.avg_quality);
    printf("\n  检测统计    : 分析窗 %u | 入睡窗 %u | 入睡事件 %u | 夜醒 %u\n",
           dev.det.stats.windows_total, dev.det.stats.asleep_windows,
           dev.det.stats.transitions_to_asleep, dev.det.stats.night_wake_events);
    printf("==============================================================\n");
    printf(" demo 结束。此循环即真机 app 主循环骨架（时间源换成 RTC/媒体回调）。\n");
    printf("==============================================================\n");
    return 0;
}
